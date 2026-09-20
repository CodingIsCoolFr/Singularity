#include "ui/ChatView.h"

#include "ui/AnimatedImage.h"
#include "ui/MediaCache.h"

#include <QPalette>
#include <QScrollBar>
#include <QTextDocument>
#include <QTimer>

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

    // When a download lands, hand it to the document and lay out again so it
    // appears without rebuilding the whole view.
    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &url) {
        if (!m_wanted.contains(url.toString()))
            return;
        adoptAnimation(url);
        document()->addResource(QTextDocument::ImageResource, url, prepare(url));
        document()->markContentsDirty(0, document()->characterCount());
        viewport()->update();
    });
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

    // A moving picture draws its current frame instead of the still one.
    if (AnimatedImage *animation = m_animations.value(key)) {
        const QImage frame = animation->currentFrame();
        if (!frame.isNull())
            return scaleForDocument(url, frame);
    }

    return scaleForDocument(url, MediaCache::instance().image(url));
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

    // Laying the page out again can shift where you were reading, so put the
    // scroll back exactly where it was.
    QScrollBar *bar = verticalScrollBar();
    const bool wasAtBottom = bar->value() >= bar->maximum() - 4;
    const int position = bar->value();

    document()->markContentsDirty(0, document()->characterCount());

    if (wasAtBottom)
        bar->setValue(bar->maximum());
    else if (bar->value() != position)
        bar->setValue(position);

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
