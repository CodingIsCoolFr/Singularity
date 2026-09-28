#include "ui/CallView.h"

#include "core/MessageStore.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QCursor>
#include <QEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

namespace {

constexpr int Pad = 14;
constexpr int Gap = 10;
constexpr int StripHeight = 148;
constexpr int StripMinTile = 168;
constexpr int FeaturedAvatar = 128;
constexpr int StripAvatar = 56;
constexpr int ArrowSize = 34;

// Above this many people the strip shows only those with a camera or a
// stream, until the user asks for everyone. Discord hides non-video
// participants in big calls for the same reason: a row of forty avatars is
// not something anyone is looking for.
constexpr int BigCall = 12;

// What a tile asks the server for. The strip used to take Discord's 180p
// copy (rid 50). That layer arrives as stripes, and the same camera is
// fine the moment it is clicked onto the stage, which asks for the full
// picture. Both sizes now ask for that.
constexpr int SmallViewPixels = 1280 * 720;
constexpr int LargeViewPixels = 1280 * 720;

QColor withAlpha(const char *hex, int alpha)
{
    QColor colour(hex);
    colour.setAlpha(alpha);
    return colour;
}

// The main window sets this while a picture is the background. An empty
// call tile is then just a face on that picture, not a dark plate over it.
bool pictureBehind(const QWidget *widget)
{
    for (const QWidget *w = widget; w; w = w->parentWidget()) {
        if (w->property("glass").toBool())
            return true;
    }
    return false;
}

} // namespace

CallView::CallView(MessageStore *store, QWidget *parent)
    : QFrame(parent)
    , m_store(store)
{
    setObjectName(QStringLiteral("CallView"));
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(0);
    setVisible(false);
    setMouseTracking(true);
}

QSize CallView::sizeHint() const
{
    if (m_stageSuppressed || m_tiles.isEmpty())
        return QSize(640, 0);

    const bool cinema = !m_cameraFrames.isEmpty() || !m_shareFrames.isEmpty();
    return QSize(960, cinema ? 560 : 360);
}

QSize CallView::minimumSizeHint() const
{
    return (m_stageSuppressed || m_tiles.isEmpty()) ? QSize(0, 0) : QSize(320, 240);
}

void CallView::setStageSuppressed(bool suppressed)
{
    if (m_stageSuppressed == suppressed)
        return;
    m_stageSuppressed = suppressed;
    refresh();
}

void CallView::setChannel(const QString &channelId)
{
    if (m_channelId == channelId)
        return;

    m_channelId = channelId;
    m_focusedUser.clear();
    m_focusedSurface = Surface::Camera;
    m_hoverUserId.clear();
    m_hoverSurface = Surface::Camera;
    m_cameraFrames.clear();
    m_shareFrames.clear();
    m_stripOffset = 0;
    if (channelId.isEmpty()) {
        m_tiles.clear();
        publishViews();
        if (isVisible()) {
            setVisible(false);
            emit visibilityChanged(false);
        }
        return;
    }

    refresh();
}

void CallView::setSpeaking(const QSet<QString> &userIds)
{
    if (m_speaking == userIds)
        return;

    m_speaking = userIds;
    for (Tile &tile : m_tiles)
        tile.speaking = m_speaking.contains(tile.userId);
    update();
}

void CallView::setVideoHidden(const QSet<QString> &userIds)
{
    if (m_videoHidden == userIds)
        return;
    m_videoHidden = userIds;
    for (const QString &userId : userIds)
        m_cameraFrames.remove(userId);
    refresh();
    publishViews();
}

void CallView::setStreamHidden(const QSet<QString> &userIds)
{
    if (m_streamHidden == userIds)
        return;
    m_streamHidden = userIds;
    for (const QString &userId : userIds)
        m_shareFrames.remove(userId);
    update();
}

void CallView::setFocusedUser(const QString &userId, Surface surface)
{
    if (m_focusedUser == userId && m_focusedSurface == surface)
        return;
    m_focusedUser = userId;
    m_focusedSurface = surface;
    layoutTiles();
    update();
}

void CallView::setFrame(const QString &userId, const QImage &image, Surface surface)
{
    if (userId.isEmpty() || image.isNull())
        return;

    if (surface == Surface::Share && m_streamHidden.contains(userId))
        return;   // a picture still on its way after the stream video was turned off
    if (surface == Surface::Share)
        m_shareFrames.insert(userId, image);
    else if (m_videoHidden.contains(userId))
        return;   // a picture still on its way after Turn Off Video
    else
        m_cameraFrames.insert(userId, image);

    for (const Tile &tile : m_tiles) {
        if (tile.userId == userId && tile.surface == surface) {
            update(tile.box);
            return;
        }
    }
}

void CallView::dropFrames(const QString &userId, Surface surface)
{
    const bool removed = surface == Surface::Share ? m_shareFrames.remove(userId)
                                                   : m_cameraFrames.remove(userId);
    if (removed)
        update();
}

void CallView::refresh()
{
    if (m_channelId.isEmpty() || !m_store) {
        m_tiles.clear();
        publishViews();
        if (isVisible()) {
            setVisible(false);
            emit visibilityChanged(false);
        }
        return;
    }

    const QStringList members = m_store->voiceMembers(m_channelId);

    m_tiles.clear();
    m_tiles.reserve(members.size() * 2);

    m_peopleInCall = int(members.size());
    m_peopleWithVideo = 0;
    for (const QString &userId : members) {
        const VoiceStateInfo state = m_store->voiceState(userId);
        if (state.video || state.streaming)
            ++m_peopleWithVideo;
    }
    // With nobody on camera, "video only" would leave an empty stage, so
    // everyone shows regardless.
    const bool onlyVideo = videoOnly() && m_peopleWithVideo > 0;

    for (const QString &userId : members) {
        const UserInfo user = m_store->user(userId);
        const VoiceStateInfo state = m_store->voiceState(userId);
        const QString name = user.displayName().isEmpty() ? userId : user.displayName();

        Tile person;
        person.userId = userId;
        person.name = name;
        person.surface = Surface::Camera;
        // A camera turned off here is drawn as the person's picture.
        person.video = state.video && !m_videoHidden.contains(userId);
        person.muted = state.muted;
        person.deafened = state.deafened;
        person.speaking = m_speaking.contains(userId);
        // Someone the user clicked stays, camera or not.
        if (!onlyVideo || state.video || userId == m_focusedUser)
            m_tiles.append(person);

        // A share is its own tile so it never overwrites the camera.
        if (state.streaming) {
            Tile share;
            share.userId = userId;
            share.name = name;
            share.surface = Surface::Share;
            share.streaming = true;
            share.muted = state.muted;
            share.deafened = state.deafened;
            share.speaking = person.speaking;
            m_tiles.append(share);
        }
    }

    // Streams first, then cameras, then everyone else, each group in the
    // order Discord gave. Not by who is talking: tiles that jump around
    // every time somebody speaks are impossible to follow.
    const auto rank = [](const Tile &tile) {
        if (tile.surface == Surface::Share)
            return 0;
        return tile.video ? 1 : 2;
    };
    std::stable_sort(m_tiles.begin(), m_tiles.end(),
                     [&rank](const Tile &a, const Tile &b) { return rank(a) < rank(b); });

    const bool show = !m_tiles.isEmpty() && !m_stageSuppressed;
    const bool wasVisible = isVisible();
    setVisible(show);
    layoutTiles();
    update();
    if (show != wasVisible)
        emit visibilityChanged(show);
}

void CallView::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    layoutTiles();
}

bool CallView::hasTile(const QString &userId, Surface surface) const
{
    for (const Tile &tile : m_tiles) {
        if (tile.userId == userId && tile.surface == surface)
            return true;
    }
    return false;
}

const QHash<QString, QImage> &CallView::framesFor(Surface surface) const
{
    return surface == Surface::Share ? m_shareFrames : m_cameraFrames;
}

CallView::Focus CallView::pickFocus() const
{
    if (m_tiles.isEmpty())
        return {};

    if (!m_focusedUser.isEmpty() && hasTile(m_focusedUser, m_focusedSurface))
        return {m_focusedUser, m_focusedSurface};
    if (!m_focusedUser.isEmpty() && hasTile(m_focusedUser, Surface::Share))
        return {m_focusedUser, Surface::Share};
    if (!m_focusedUser.isEmpty() && hasTile(m_focusedUser, Surface::Camera))
        return {m_focusedUser, Surface::Camera};

    for (const Tile &tile : m_tiles) {
        if (tile.surface == Surface::Share && m_shareFrames.contains(tile.userId))
            return {tile.userId, Surface::Share};
    }
    for (const Tile &tile : m_tiles) {
        if (tile.surface == Surface::Camera && tile.video && m_cameraFrames.contains(tile.userId))
            return {tile.userId, Surface::Camera};
    }
    for (const Tile &tile : m_tiles) {
        if (tile.surface == Surface::Share)
            return {tile.userId, Surface::Share};
    }
    for (const Tile &tile : m_tiles) {
        if (tile.video)
            return {tile.userId, Surface::Camera};
    }
    for (const Tile &tile : m_tiles) {
        if (tile.speaking)
            return {tile.userId, tile.surface};
    }
    return {m_tiles.first().userId, m_tiles.first().surface};
}

bool CallView::videoOnly() const
{
    if (m_videoOnlyChoice >= 0)
        return m_videoOnlyChoice == 1;
    return m_peopleInCall > BigCall;
}

// The stage on top, one row underneath.
//
// This used to fit every other tile into the strip by adding rows. With
// forty people that meant eight rows in 150 pixels, each one a sliver too
// thin to show a face. Discord never shrinks a tile below a size you can
// read: it keeps one row of 16:9 tiles and lets it scroll, and so does this.
void CallView::layoutTiles()
{
    m_stripRect = {};
    m_leftArrow = {};
    m_rightArrow = {};
    m_stripMaxOffset = 0;

    if (m_tiles.isEmpty()) {
        publishViews();
        return;
    }

    const Focus focus = pickFocus();
    m_focusedUser = focus.userId;
    m_focusedSurface = focus.surface;

    const bool many = m_tiles.size() > 1;
    const int stripH = many ? qBound(110, qMin(StripHeight, height() / 4), 170) : 0;
    const QRect featured(Pad, Pad, qMax(1, width() - Pad * 2),
                         qMax(1, height() - Pad * 2 - (many ? stripH + Gap : 0)));

    QList<int> others;
    others.reserve(m_tiles.size());
    for (int i = 0; i < m_tiles.size(); ++i) {
        m_tiles[i].featured = (m_tiles[i].userId == focus.userId
                               && m_tiles[i].surface == focus.surface);
        m_tiles[i].visible = m_tiles[i].featured;
        if (!m_tiles[i].featured)
            others.append(i);
        else
            m_tiles[i].box = featured;
    }

    if (!others.isEmpty()) {
        const int usable = qMax(1, width() - Pad * 2);
        m_stripRect = QRect(Pad, featured.bottom() + Gap + 1, usable, stripH);

        const int tileH = stripH;
        const int tileW = qMax(StripMinTile, tileH * 16 / 9);
        m_stripStep = tileW + Gap;

        const int count = int(others.size());
        const int content = count * tileW + (count - 1) * Gap;
        m_stripMaxOffset = qMax(0, content - usable);
        m_stripOffset = qBound(0, m_stripOffset, m_stripMaxOffset);

        // A row that fits sits in the middle, like Discord's.
        const int startX = content <= usable ? m_stripRect.left() + (usable - content) / 2
                                             : m_stripRect.left() - m_stripOffset;

        for (int n = 0; n < count; ++n) {
            Tile &tile = m_tiles[others[n]];
            tile.box = QRect(startX + n * m_stripStep, m_stripRect.top(), tileW, tileH);
            tile.visible = tile.box.intersects(m_stripRect);
        }

        if (m_stripOffset > 0) {
            m_leftArrow = QRect(m_stripRect.left() + 6, m_stripRect.center().y() - ArrowSize / 2,
                                ArrowSize, ArrowSize);
        }
        if (m_stripOffset < m_stripMaxOffset) {
            m_rightArrow = QRect(m_stripRect.right() - 6 - ArrowSize,
                                 m_stripRect.center().y() - ArrowSize / 2, ArrowSize, ArrowSize);
        }
    }

    publishViews();
}

void CallView::scrollStrip(int pixels)
{
    const int next = qBound(0, m_stripOffset + pixels, m_stripMaxOffset);
    if (next == m_stripOffset)
        return;
    m_stripOffset = next;
    layoutTiles();
    update();
}

// Tells the voice connection which cameras are worth downloading.
//
// The server sends only what it is asked for. Asking for every camera at full
// size in a call of forty is forty video streams to receive and decode, most
// of them for tiles nobody can see. Discord asks for the ones on screen, at
// the size they are drawn, and this does the same.
void CallView::publishViews()
{
    QString stageUser;
    Surface stageSurface = Surface::Camera;
    if (!m_stageSuppressed && !isHidden()) {
        for (const Tile &tile : m_tiles) {
            if (tile.featured) {
                stageUser = tile.userId;
                stageSurface = tile.surface;
                break;
            }
        }
    }
    if (stageUser != m_lastStageUser || stageSurface != m_lastStageSurface) {
        m_lastStageUser = stageUser;
        m_lastStageSurface = stageSurface;
        emit stageChanged(stageUser, stageSurface);
    }

    QHash<QString, int> views;
    if (!m_stageSuppressed && !isHidden()) {
        for (const Tile &tile : m_tiles) {
            if (tile.surface != Surface::Camera || !tile.video || !tile.visible)
                continue;
            views.insert(tile.userId, tile.featured ? LargeViewPixels : SmallViewPixels);
        }
    }

    if (m_viewsPublished && views == m_lastViews)
        return;
    m_viewsPublished = true;
    m_lastViews = views;
    emit videoViewsChanged(views);
}

const CallView::Tile *CallView::tileAt(const QPoint &pos) const
{
    for (const Tile &tile : m_tiles) {
        if (!tile.visible || !tile.box.contains(pos))
            continue;
        if (!tile.featured && !m_stripRect.contains(pos))
            continue;
        return &tile;
    }
    return nullptr;
}

void CallView::paintEvent(QPaintEvent *event)
{
    QFrame::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    for (const Tile &tile : m_tiles) {
        if (!tile.visible || !tile.box.intersects(event->rect()))
            continue;
        painter.save();
        // Strip tiles are cut off at the strip's edges as they scroll past.
        if (!tile.featured)
            painter.setClipRect(m_stripRect);
        paintTile(painter, tile);
        painter.restore();
    }

    paintStripControls(painter);
}

void CallView::paintStripControls(QPainter &painter) const
{
    const auto arrow = [&painter](const QRect &box, bool left) {
        if (box.isNull())
            return;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 170));
        painter.drawEllipse(box);
        QPen chevron{QColor(Theme::Light)};
        chevron.setWidthF(2.2);
        chevron.setCapStyle(Qt::RoundCap);
        chevron.setJoinStyle(Qt::RoundJoin);
        painter.setPen(chevron);
        painter.setBrush(Qt::NoBrush);
        const QPointF c = QRectF(box).center();
        const qreal d = left ? 3.0 : -3.0;
        QPolygonF line;
        line << QPointF(c.x() + d, c.y() - 6) << QPointF(c.x() - d, c.y())
             << QPointF(c.x() + d, c.y() + 6);
        painter.drawPolyline(line);
    };
    arrow(m_leftArrow, true);
    arrow(m_rightArrow, false);

    // The filter, only where it matters: a call big enough to have one.
    QRect &chip = m_filterChip;
    chip = {};
    if (m_tiles.isEmpty() || m_peopleWithVideo == 0
        || (m_peopleInCall <= BigCall && m_videoOnlyChoice < 0))
        return;

    const bool only = videoOnly();
    const QString text = only ? QStringLiteral("Video only · show all %1").arg(m_peopleInCall)
                              : QStringLiteral("Everyone · show video only");
    QFont chipFont = font();
    chipFont.setPixelSize(12);
    chipFont.setWeight(QFont::DemiBold);
    painter.setFont(chipFont);
    const int w = painter.fontMetrics().horizontalAdvance(text) + 22;
    chip = QRect(Pad + 12, Pad + 12, w, 24);

    const bool hovered = chip.contains(mapFromGlobal(QCursor::pos()));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, hovered ? 210 : 160));
    painter.drawRoundedRect(chip, 12, 12);
    painter.setPen(QColor(hovered ? Theme::Light : Theme::LightGray));
    painter.drawText(chip, Qt::AlignCenter, text);
}

void CallView::wheelEvent(QWheelEvent *event)
{
    if (m_stripMaxOffset <= 0 || !m_stripRect.contains(event->position().toPoint())) {
        QFrame::wheelEvent(event);
        return;
    }

    // A touchpad gives pixels; a mouse wheel gives notches of 120, and one
    // notch moves one tile.
    const QPoint pixels = event->pixelDelta();
    int move = 0;
    if (!pixels.isNull()) {
        move = -(qAbs(pixels.x()) > qAbs(pixels.y()) ? pixels.x() : pixels.y());
    } else {
        const QPoint angle = event->angleDelta();
        const int notches = qAbs(angle.x()) > qAbs(angle.y()) ? angle.x() : angle.y();
        move = -notches * m_stripStep / 120;
    }
    scrollStrip(move);
    event->accept();
}

void CallView::paintTile(QPainter &painter, const Tile &tile) const
{
    const qreal radius = tile.featured ? 16.0 : 12.0;
    QPainterPath clip;
    clip.addRoundedRect(QRectF(tile.box), radius, radius);

    painter.save();
    painter.setClipPath(clip, painter.hasClipping() ? Qt::IntersectClip : Qt::ReplaceClip);

    const QImage live = framesFor(tile.surface).value(tile.userId);
    // No camera yet: the rounded plate was a box sitting on the picture.
    const bool clearPlate = pictureBehind(this) && live.isNull();
    if (!clearPlate)
        painter.fillPath(clip, withAlpha(Theme::SurfaceChat, 230));

    if (!live.isNull()) {
        // Shared screens keep their whole picture. Cameras fill the tile.
        const Qt::AspectRatioMode mode =
            tile.surface == Surface::Share ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding;
        const QSize fitted = live.size().scaled(tile.box.size(), mode);
        const QRect target(tile.box.center().x() - fitted.width() / 2,
                           tile.box.center().y() - fitted.height() / 2, fitted.width(),
                           fitted.height());
        painter.drawImage(target, live);
    } else {
        const UserInfo user = m_store->user(tile.userId);
        const int faceSize = tile.featured ? FeaturedAvatar : StripAvatar;
        const QUrl url = MediaCache::avatarUrl(tile.userId, user.avatarHash, 256);
        const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
        const QPixmap face = picture.isNull() ? MediaCache::initialsAvatar(tile.name, faceSize)
                                              : MediaCache::circular(picture, faceSize);
        const QRect avatarBox(tile.box.center().x() - faceSize / 2,
                              tile.box.center().y() - faceSize / 2 - (tile.featured ? 18 : 8),
                              faceSize, faceSize);
        painter.drawPixmap(avatarBox, face);

        const bool streamOff = tile.surface == Surface::Share && m_streamHidden.contains(tile.userId);
        if (((tile.streaming || tile.video) && tile.featured) || streamOff) {
            QFont note = font();
            note.setPixelSize(tile.featured ? 13 : 11);
            painter.setFont(note);
            painter.setPen(QColor(Theme::TextMuted));
            painter.drawText(QRect(tile.box.left() + 20, avatarBox.bottom() + 6, tile.box.width() - 40, 20),
                             Qt::AlignHCenter | Qt::AlignVCenter,
                             streamOff ? QStringLiteral("stream video turned off")
                             : tile.surface == Surface::Share ? QStringLiteral("waiting for the screen...")
                                                              : QStringLiteral("waiting for the camera..."));
        }
    }

    // Name plate, darker at the bottom so white text on a bright share still reads.
    // Skipped on a clear plate: the fade is itself a dark box.
    if (!clearPlate) {
        QLinearGradient fade(tile.box.bottomLeft(), QPoint(tile.box.left(), tile.box.bottom() - 72));
        fade.setColorAt(0.0, QColor(0, 0, 0, tile.featured ? 170 : 140));
        fade.setColorAt(1.0, QColor(0, 0, 0, 0));
        painter.fillRect(QRect(tile.box.left(), tile.box.bottom() - 72, tile.box.width(), 72), fade);
    }

    QFont nameFont = font();
    nameFont.setPixelSize(tile.featured ? 16 : 12);
    nameFont.setWeight(QFont::DemiBold);
    painter.setFont(nameFont);
    painter.setPen(QColor(tile.speaking ? Theme::TextPrimary : Theme::LightGray));
    const int nameH = tile.featured ? 22 : 16;
    const QRect nameBox(tile.box.left() + 14, tile.box.bottom() - nameH - 12,
                        tile.box.width() - 52, nameH);
    const QString label = tile.surface == Surface::Share
        ? QStringLiteral("%1's screen").arg(tile.name)
        : tile.name;
    painter.drawText(nameBox, Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(label, Qt::ElideRight, nameBox.width()));

    painter.restore();

    const bool hovered = tile.userId == m_hoverUserId && tile.surface == m_hoverSurface;
    const bool focused = tile.userId == m_focusedUser && tile.surface == m_focusedSurface;
    // A clear plate keeps an edge only while it means something: speaking,
    // focused, or under the pointer. Otherwise the outline is the box again.
    if (!clearPlate || tile.speaking || focused || hovered) {
        QPen edge(tile.speaking ? QColor(Theme::Green)
                                : (focused ? QColor(Theme::Accent)
                                           : withAlpha(Theme::Border, 180)));
        if (hovered && !tile.speaking)
            edge.setColor(QColor(Theme::AccentHover));
        edge.setWidthF(tile.featured ? 2.4 : 1.6);
        painter.setPen(edge);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(tile.box).adjusted(1, 1, -1, -1), radius, radius);
    }

    paintMarks(painter, tile);
}

void CallView::paintMarks(QPainter &painter, const Tile &tile) const
{
    int edgeX = tile.box.right() - 10;

    if (tile.streaming || tile.video) {
        QFont badgeFont = font();
        badgeFont.setPixelSize(tile.featured ? 10 : 8);
        badgeFont.setWeight(QFont::Bold);
        painter.setFont(badgeFont);

        const QString word = tile.surface == Surface::Share ? QStringLiteral("LIVE")
                                                            : QStringLiteral("CAM");
        const int badgeWidth = painter.fontMetrics().horizontalAdvance(word) + 14;
        const int badgeHeight = tile.featured ? 18 : 14;
        const QRect badge(edgeX - badgeWidth, tile.box.top() + 10, badgeWidth, badgeHeight);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(tile.surface == Surface::Share ? Theme::Red : Theme::Highlight));
        painter.drawRoundedRect(badge, 4, 4);
        painter.setPen(QColor(Theme::Light));
        painter.drawText(badge, Qt::AlignCenter, word);
        edgeX = badge.left() - 8;
    }

    const int markSize = tile.featured ? 16 : 13;
    int markX = tile.box.right() - 12 - markSize;
    const int markY = tile.box.bottom() - 14 - markSize;

    const auto strike = [&](const QRectF &box, const QColor &colour) {
        QPen bar(colour);
        bar.setWidthF(1.8);
        bar.setCapStyle(Qt::RoundCap);
        painter.setPen(bar);
        painter.drawLine(box.topLeft() + QPointF(1.5, 1.5), box.bottomRight() - QPointF(1.5, 1.5));
    };

    if (tile.deafened) {
        const QRectF box(markX, markY, markSize, markSize);
        const QColor colour(Theme::Red);
        QPen band(colour);
        band.setWidthF(1.6);
        painter.setPen(band);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(box.left() + 1, box.top() + 2, box.width() - 2, box.height() - 3),
                        0 * 16, 180 * 16);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawRoundedRect(QRectF(box.left() + 0.5, box.center().y(), 3, box.height() / 2 - 1),
                                1.2, 1.2);
        painter.drawRoundedRect(QRectF(box.right() - 3.5, box.center().y(), 3, box.height() / 2 - 1),
                                1.2, 1.2);
        strike(box, colour);
    } else if (tile.muted) {
        const QRectF box(markX, markY, markSize, markSize);
        const QColor colour(Theme::Red);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawRoundedRect(QRectF(box.center().x() - 2, box.top() + 1.5, 4, 7), 2, 2);
        QPen stem(colour);
        stem.setWidthF(1.4);
        stem.setCapStyle(Qt::RoundCap);
        painter.setPen(stem);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(box.center().x() - 4, box.top() + 4, 8, 8), 180 * 16, 180 * 16);
        painter.drawLine(QPointF(box.center().x(), box.top() + 11.5),
                         QPointF(box.center().x(), box.bottom() - 1));
        strike(box, colour);
    }
}

void CallView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const QPoint pos = event->pos();
        if (m_filterChip.contains(pos)) {
            m_videoOnlyChoice = videoOnly() ? 0 : 1;
            m_stripOffset = 0;
            refresh();
            return;
        }
        if (m_leftArrow.contains(pos)) {
            scrollStrip(-qMax(m_stripStep, m_stripRect.width() - m_stripStep));
            return;
        }
        if (m_rightArrow.contains(pos)) {
            scrollStrip(qMax(m_stripStep, m_stripRect.width() - m_stripStep));
            return;
        }
    }

    const Tile *tile = tileAt(event->pos());
    if (!tile) {
        QFrame::mousePressEvent(event);
        return;
    }

    if (event->button() == Qt::RightButton) {
        emit volumeMenuRequested(tile->userId, event->globalPosition().toPoint());
        return;
    }

    emit focusRequested(tile->userId, tile->surface);
    if (tile->surface == Surface::Share)
        emit watchAttempted(tile->userId);
    else if (!tile->video)
        emit profileRequested(tile->userId);
}

void CallView::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->pos();
    const bool onControl = m_filterChip.contains(pos) || m_leftArrow.contains(pos)
        || m_rightArrow.contains(pos);
    const Tile *tile = onControl ? nullptr : tileAt(pos);
    setCursor(tile || onControl ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (!m_filterChip.isNull())
        update(m_filterChip);

    const QString id = tile ? tile->userId : QString();
    const Surface surface = tile ? tile->surface : Surface::Camera;
    if (id == m_hoverUserId && surface == m_hoverSurface)
        return;
    m_hoverUserId = id;
    m_hoverSurface = surface;
    update();
}

void CallView::leaveEvent(QEvent *event)
{
    QFrame::leaveEvent(event);
    if (m_hoverUserId.isEmpty())
        return;
    m_hoverUserId.clear();
    m_hoverSurface = Surface::Camera;
    setCursor(Qt::ArrowCursor);
    update();
}
