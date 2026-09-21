#include "ui/ChatView.h"

#include "ui/AnimatedImage.h"
#include "ui/MediaCache.h"

#include <QPalette>
#include <QScrollBar>
#include <QTextDocument>
#include <QTimer>

#include <utility>

namespace {

// Discord draws avatars at 40 and caps picture previews at roughly this box.
constexpr int AvatarPixels = 40;
constexpr int EmojiPixels = 22;
constexpr int MaxPictureWidth = 340;
constexpr int MaxPictureHeight = 280;

// Redrawing the document is not free, so animations share one modest tick
// rather than each running at its own speed.
constexpr int AnimationFrameMs = 66;   // about 15 times a second
constexpr int MaxConcurrentAnimations = 8;

bool looksLikeAvatar(const QUrl &url)
{
    const QString path = url.path();
    return path.startsWith(QLatin1String("/avatars/")) || path.startsWith(QLatin1String("/embed/avatars/"));
}

bool looksLikeEmoji(const QUrl &url)
{
    return url.path().startsWith(QLatin1String("/emojis/"));
}

} // namespace

ChatView::ChatView(QWidget *parent)
    : QTextBrowser(parent)
{
    setOpenExternalLinks(true);
    setFrameShape(QFrame::NoFrame);
    viewport()->setAutoFillBackground(false);
    QPalette pal = palette();
    pal.setColor(QPalette::Base, Qt::transparent);
    pal.setColor(QPalette::Window, Qt::transparent);
    setPalette(pal);

    // One timer drives every animation on screen.
    m_animationTimer.setInterval(AnimationFrameMs);
    connect(&m_animationTimer, &QTimer::timeout, this, &ChatView::pumpAnimations);

    // Pictures that land are collected and dealt with together.
    //
    // A picture appearing changes the height of the page, so the conversation
    // really does have to be laid out again. Doing that once per picture meant
    // a hundred full re-layouts while a channel filled in. They now share one.
    m_arrivalTimer.setSingleShot(true);
    m_arrivalTimer.setInterval(90);
    connect(&m_arrivalTimer, &QTimer::timeout, this, &ChatView::flushArrivals);

    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &url) {
        const QString key = url.toString();
        if (!m_wanted.contains(key))
            return;

        // The old pixmap, if any, was drawn from nothing.
        m_prepared.remove(key);
        m_arrived.insert(key);

        if (!m_arrivalTimer.isActive())
            m_arrivalTimer.start();
    });
}

void ChatView::flushArrivals()
{
    if (m_arrived.isEmpty())
        return;

    for (const QString &key : std::as_const(m_arrived)) {
        const QUrl url(key);
        adoptAnimation(url);
        document()->addResource(QTextDocument::ImageResource, url, prepare(url));
    }
    m_arrived.clear();

    QScrollBar *bar = verticalScrollBar();
    const bool wasAtBottom = bar->value() >= bar->maximum() - 4;
    const int position = bar->value();

    document()->markContentsDirty(0, document()->characterCount());

    // Pictures appearing above where you are reading push the text down. Being
    // at the bottom means staying at the bottom.
    if (wasAtBottom)
        bar->setValue(bar->maximum());
    else if (bar->value() != position)
        bar->setValue(position);

    viewport()->update();
}

ChatView::~ChatView() = default;

bool ChatView::isAllowedImageHost(const QUrl &url)
{
    return MediaCache::isAllowedHost(url);
}

void ChatView::setAnimationsEnabled(bool enabled)
{
    if (m_animationsEnabled == enabled)
        return;
    m_animationsEnabled = enabled;

    if (enabled) {
        if (!m_animations.isEmpty())
            m_animationTimer.start();
        return;
    }

    // Let the current frames stand, and stop spending time on new ones.
    m_animationTimer.stop();
    qDeleteAll(m_animations);
    m_animations.clear();
}

void ChatView::clearImageCache()
{
    m_wanted.clear();
    m_frameSize.clear();
    m_prepared.clear();
    m_arrived.clear();
    m_arrivalTimer.stop();
    qDeleteAll(m_animations);
    m_animations.clear();
    m_animationTimer.stop();
}

QSize ChatView::boxFor(const QUrl &url) const
{
    if (looksLikeAvatar(url))
        return QSize(AvatarPixels, AvatarPixels);
    if (looksLikeEmoji(url))
        return QSize(EmojiPixels, EmojiPixels);
    return QSize(MaxPictureWidth, MaxPictureHeight);
}

QPixmap ChatView::scaleForDocument(const QUrl &url, const QImage &source) const
{
    if (source.isNull())
        return {};

    // Avatars become circles, like the real client.
    if (looksLikeAvatar(url))
        return MediaCache::circular(source, AvatarPixels);

    const QString key = url.toString();

    // Every frame of an animation is drawn at exactly the size the first frame
    // settled on. Without this, frames that differ by a pixel change the page
    // height and the view slides around while a picture plays.
    const auto locked = m_frameSize.constFind(key);
    if (locked != m_frameSize.constEnd())
        return QPixmap::fromImage(source.scaled(locked.value(), Qt::IgnoreAspectRatio,
                                                Qt::SmoothTransformation));

    const QSize box = boxFor(url);

    QImage scaled = source;
    if (scaled.width() > box.width())
        scaled = scaled.scaledToWidth(box.width(), Qt::SmoothTransformation);
    if (scaled.height() > box.height())
        scaled = scaled.scaledToHeight(box.height(), Qt::SmoothTransformation);

    m_frameSize.insert(key, scaled.size());
    return QPixmap::fromImage(scaled);
}

QPixmap ChatView::prepare(const QUrl &url) const
{
    const QString key = url.toString();

    // A moving picture draws its current frame instead of the still one, and
    // is never cached - the whole point is that it changes.
    if (AnimatedImage *animation = m_animations.value(key)) {
        const QImage frame = animation->currentFrame();
        if (!frame.isNull())
            return scaleForDocument(url, frame);
    }

    const auto done = m_prepared.constFind(key);
    if (done != m_prepared.constEnd())
        return done.value();

    const QPixmap ready = scaleForDocument(url, MediaCache::instance().image(url));
    if (!ready.isNull()) {
        // Flat ceiling. A long session through many channels would otherwise
        // hold every picture ever shown.
        if (m_prepared.size() > 600)
            m_prepared.clear();
        m_prepared.insert(key, ready);
    }
    return ready;
}

void ChatView::adoptAnimation(const QUrl &url)
{
    if (!m_animationsEnabled)
        return;

    const QString key = url.toString();
    if (m_animations.contains(key))
        return;

    // Avatars in the message list stay still on purpose: one moving picture
    // per row would be a lot of noise and a lot of redrawing.
    if (looksLikeAvatar(url))
        return;

    const QByteArray bytes = MediaCache::instance().animationData(url);
    if (bytes.isEmpty())
        return;

    if (m_animations.size() >= MaxConcurrentAnimations)
        return;

    auto *animation = new AnimatedImage(this);
    if (!animation->setData(bytes) || !animation->isAnimated()) {
        delete animation;
        return;
    }

    m_animations.insert(key, animation);
    if (!m_animationTimer.isActive())
        m_animationTimer.start();
}

void ChatView::pumpAnimations()
{
    if (m_animations.isEmpty()) {
        m_animationTimer.stop();
        return;
    }

    // Nothing to do while the view is hidden.
    if (!isVisible())
        return;

    bool changed = false;
    for (auto it = m_animations.constBegin(); it != m_animations.constEnd(); ++it) {
        const QUrl url(it.key());
        const QPixmap frame = scaleForDocument(url, it.value()->currentFrame());
        if (frame.isNull())
            continue;
        document()->addResource(QTextDocument::ImageResource, url, frame);
        changed = true;
    }

    if (!changed)
        return;

    // Repaint only. No layout.
    //
    // This used to mark the whole document dirty, which laid the entire
    // conversation out again - fifteen times a second, for as long as a single
    // animated emoji was on screen. In a server that uses them that is every
    // second of every channel, and it was the worst of the stutter.
    //
    // Nothing about the page has actually moved: every frame is drawn at the
    // size the first frame settled on, which m_frameSize exists to guarantee.
    // Only the pixels differ, and painting picks those up straight from the
    // document's resources.
    viewport()->update();
}

QVariant ChatView::loadResource(int type, const QUrl &name)
{
    if (type != QTextDocument::ImageResource)
        return QTextBrowser::loadResource(type, name);

    if (!MediaCache::isAllowedHost(name))
        return {};

    // Remember that this view wants the picture, so the `ready` signal knows
    // whether it needs to redraw.
    m_wanted.insert(name.toString());

    adoptAnimation(name);

    const QPixmap ready = prepare(name);
    if (!ready.isNull())
        return ready;

    // Not here yet. The cache has started the download.
    return {};
}
