#include "ui/ChatView.h"

#include "ui/AnimatedImage.h"
#include "ui/ClipPlayer.h"
#include "ui/MediaCache.h"
#include "core/Logger.h"

#include <QElapsedTimer>
#include <QtMath>
#include <QPalette>
#include <QScrollBar>
#include <QTextDocument>
#include <QTimer>

#include <exception>
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
// At most this many moving pictures at once, and at most this many pixels of
// them redrawn per frame: about four full-size GIFs, or dozens of emoji.
constexpr int MaxConcurrentAnimations = 48;
constexpr qint64 AnimationPixelBudget = 4LL * 340 * 280;

bool looksLikeAvatar(const QUrl &url)
{
    const QString path = url.path();
    return path.startsWith(QLatin1String("/avatars/")) || path.startsWith(QLatin1String("/embed/avatars/"));
}

bool looksLikeEmoji(const QUrl &url)
{
    return url.path().startsWith(QLatin1String("/emojis/"));
}

bool looksLikeVideo(const QUrl &url)
{
    const QString path = url.path().toLower();
    return path.endsWith(QLatin1String(".mp4")) || path.endsWith(QLatin1String(".webm"))
        || path.endsWith(QLatin1String(".mov"));
}

} // namespace

ChatView::ChatView(QWidget *parent)
    : QTextBrowser(parent)
{
    setOpenExternalLinks(true);
    setFrameShape(QFrame::NoFrame);

    // Nobody can type in here, so nobody can undo anything either - but Qt
    // does not know that, and it was keeping a record of every change so it
    // could be undone. Every channel opened, every reaction, every picture
    // that landed, all of it held for the life of the window. There is nothing
    // to undo, so there is nothing to keep.
    document()->setUndoRedoEnabled(false);
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
        // Whatever was prepared before was made from nothing.
        const auto stale = m_prepared.constFind(key);
        if (stale != m_prepared.constEnd()) {
            m_preparedBytes -= qint64(stale.value().width()) * stale.value().height() * 4;
            m_prepared.erase(stale);
        }
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

qint64 ChatView::takePictureWorkMs()
{
    const qint64 ms = m_pictureWorkNs / 1000000;
    m_pictureWorkNs = 0;
    return ms;
}

int ChatView::takePicturesPrepared()
{
    const int count = m_picturesPrepared;
    m_picturesPrepared = 0;
    return count;
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
    dropAnimations();
    qDeleteAll(m_videos);
    m_videos.clear();
}

void ChatView::dropAnimations()
{
    qDeleteAll(m_animations);
    m_animations.clear();
    m_animationOrder.clear();
    m_animationPixels.clear();
    m_animationPixelTotal = 0;
    m_animationsDropped.clear();
}

void ChatView::clearImageCache()
{
    m_wanted.clear();
    m_frameSize.clear();
    m_prepared.clear();
    m_preparedBytes = 0;
    m_arrived.clear();
    m_arrivalTimer.stop();
    dropAnimations();
    qDeleteAll(m_videos);
    m_videos.clear();
    m_animationTimer.stop();
}

QSize ChatView::boxFor(const QUrl &url) const
{
    if (looksLikeAvatar(url))
        return QSize(AvatarPixels, AvatarPixels);
    if (looksLikeEmoji(url)) {
        // The size follows the chat text size, and rides in the address as
        // "#e31" so each size is its own picture. The fragment never leaves
        // this machine. Kept sharp on a scaled screen: the tag's width and
        // height set the room it takes, the pixels only how fine it looks.
        int pixels = EmojiPixels;
        const QString fragment = url.fragment();
        if (fragment.startsWith(QLatin1Char('e')))
            pixels = qBound(12, fragment.mid(1).toInt(), 128);
        pixels = qCeil(pixels * devicePixelRatioF());
        return QSize(pixels, pixels);
    }
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

    QElapsedTimer clock;
    clock.start();
    const QPixmap ready = scaleForDocument(url, MediaCache::instance().image(url));
    m_pictureWorkNs += clock.nsecsElapsed();
    ++m_picturesPrepared;

    if (!ready.isNull()) {
        // Counted in bytes, because pixmaps are not all the same size: an
        // avatar is 40 across and a picture in a message is up to 340 by 280,
        // which is fifty times the memory. A count would have meant either
        // room for almost no pictures or room for far too much.
        m_preparedBytes += qint64(ready.width()) * ready.height() * 4;
        m_prepared.insert(key, ready);

        if (m_preparedBytes > 12 * 1024 * 1024) {
            m_prepared.clear();
            m_preparedBytes = 0;
        }
    }
    return ready;
}

void ChatView::adoptVideo(const QUrl &url)
{
    if (!m_animationsEnabled || !looksLikeVideo(url))
        return;

    const QString key = url.toString();
    if (m_videos.contains(key) || m_videos.size() >= 4 || m_videosGivenUp.contains(key))
        return;

    // Our own player: decoded off the window thread, two helper threads, and
    // every picture already shrunk to the box it is drawn in. See ClipPlayer.h
    // for what Qt's player cost instead.
    auto *player = new ClipPlayer(url, QSize(MaxPictureWidth, MaxPictureHeight), this);
    player->setPaused(!isVisible());

    connect(player, &ClipPlayer::frameReady, this, [this, url](const QImage &frame) {
        if (isVisible())
            showVideoFrame(url, frame);
    });
    connect(player, &ClipPlayer::failed, this, [this, url, key, player](const QString &reason) {
        wlog(QStringLiteral("media"), QStringLiteral("could not play %1: %2").arg(url.toString(), reason));
        m_videosGivenUp.insert(key);
        m_videos.remove(key);
        player->deleteLater();
    });

    m_videos.insert(key, player);
}

void ChatView::showEvent(QShowEvent *event)
{
    QTextBrowser::showEvent(event);
    for (ClipPlayer *player : std::as_const(m_videos))
        player->setPaused(false);
}

void ChatView::hideEvent(QHideEvent *event)
{
    QTextBrowser::hideEvent(event);
    // Nobody sees a clip while the chat is hidden, so nothing is decoded.
    for (ClipPlayer *player : std::as_const(m_videos))
        player->setPaused(true);
}

void ChatView::showVideoFrame(const QUrl &url, const QImage &frame)
{
    const QString key = url.toString();
    const bool first = !m_frameSize.contains(key);
    const QPixmap pixmap = scaleForDocument(url, frame);
    if (pixmap.isNull())
        return;

    document()->addResource(QTextDocument::ImageResource, url, pixmap);
    if (first) {
        // The first frame is what gives the picture a size. Later frames
        // only change pixels, so the page does not get laid out again.
        document()->markContentsDirty(0, document()->characterCount());
    } else {
        viewport()->update();
    }
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

    if (m_animationsDropped.contains(key))
        return;

    const QByteArray bytes = MediaCache::instance().animationData(url);
    if (bytes.isEmpty())
        return;

    auto *animation = new AnimatedImage(this);
    if (!animation->setData(bytes) || !animation->isAnimated()) {
        delete animation;
        return;
    }

    m_animations.insert(key, animation);
    const QSize box = boxFor(url);
    const qint64 pixels = qint64(box.width()) * box.height();
    m_animationOrder.append(key);
    m_animationPixels.insert(key, pixels);
    m_animationPixelTotal += pixels;

    // Too many, or too many pixels a frame: the oldest stop, so the newest -
    // at the bottom, where the reading happens - keep moving. This used to be
    // a flat eight, first come first served, and the first eight were the
    // oldest messages in the channel: a fresh animated emoji at the bottom
    // stood still because a GIF far up the history had its place. Counting
    // pixels lets dozens of emoji move where two GIFs would use the same.
    while (m_animationOrder.size() > 1
           && (m_animationOrder.size() > MaxConcurrentAnimations || m_animationPixelTotal > AnimationPixelBudget)) {
        const QString oldest = m_animationOrder.takeFirst();
        m_animationPixelTotal -= m_animationPixels.take(oldest);
        delete m_animations.take(oldest);
        m_animationsDropped.insert(oldest);
    }

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

    // A gifv card points at an mp4. Treating it as a picture marks it failed
    // and it stays a blank box.
    if (looksLikeVideo(name)) {
        adoptVideo(name);
        const QString key = name.toString();
        const auto locked = m_frameSize.constFind(key);
        if (locked == m_frameSize.constEnd())
            return {};
        const QVariant existing = document()->resource(QTextDocument::ImageResource, name);
        if (existing.isValid())
            return existing;
        return {};
    }

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
