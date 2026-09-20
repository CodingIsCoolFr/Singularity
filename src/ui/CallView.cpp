#include "ui/CallView.h"

#include "core/MessageStore.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QEvent>
#include <QMouseEvent>
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

QColor withAlpha(const char *hex, int alpha)
{
    QColor colour(hex);
    colour.setAlpha(alpha);
    return colour;
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
    if (m_tiles.isEmpty())
        return QSize(640, 0);

    const bool cinema = !m_cameraFrames.isEmpty() || !m_shareFrames.isEmpty();
    return QSize(960, cinema ? 560 : 360);
}

QSize CallView::minimumSizeHint() const
{
    return m_tiles.isEmpty() ? QSize(200, 0) : QSize(320, 240);
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
    if (channelId.isEmpty()) {
        m_tiles.clear();
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

    if (surface == Surface::Share)
        m_shareFrames.insert(userId, image);
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
        if (isVisible()) {
            setVisible(false);
            emit visibilityChanged(false);
        }
        return;
    }

    const QStringList members = m_store->voiceMembers(m_channelId);

    m_tiles.clear();
    m_tiles.reserve(members.size() * 2);

    for (const QString &userId : members) {
        const UserInfo user = m_store->user(userId);
        const VoiceStateInfo state = m_store->voiceState(userId);
        const QString name = user.displayName().isEmpty() ? userId : user.displayName();

        Tile person;
        person.userId = userId;
        person.name = name;
        person.surface = Surface::Camera;
        person.video = state.video;
        person.muted = state.muted;
        person.deafened = state.deafened;
        person.speaking = m_speaking.contains(userId);
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

    const bool show = !m_tiles.isEmpty();
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

void CallView::layoutTiles()
{
    if (m_tiles.isEmpty())
        return;

    const Focus focus = pickFocus();
    m_focusedUser = focus.userId;
    m_focusedSurface = focus.surface;

    const bool many = m_tiles.size() > 1;
    const int stripH = many ? qBound(120, qMin(StripHeight, height() / 4), 170) : 0;
    const QRect featured(Pad, Pad, qMax(1, width() - Pad * 2),
                         qMax(1, height() - Pad * 2 - (many ? stripH + Gap : 0)));

    QList<int> others;
    others.reserve(m_tiles.size());
    for (int i = 0; i < m_tiles.size(); ++i) {
        m_tiles[i].featured = (m_tiles[i].userId == focus.userId
                               && m_tiles[i].surface == focus.surface);
        if (!m_tiles[i].featured)
            others.append(i);
        else
            m_tiles[i].box = featured;
    }

    if (others.isEmpty())
        return;

    const int stripY = featured.bottom() + Gap + 1;
    const int usable = qMax(StripMinTile, width() - Pad * 2);
    const int count = others.size();
    int columns = qMax(1, (usable + Gap) / (StripMinTile + Gap));
    columns = qMin(columns, count);
    const int rows = (count + columns - 1) / columns;
    const int tileW = (usable - Gap * (columns - 1)) / columns;
    const int tileH = rows > 0 ? (stripH - Gap * (rows - 1)) / rows : stripH;

    for (int n = 0; n < others.size(); ++n) {
        const int row = n / columns;
        const int col = n % columns;
        m_tiles[others[n]].box = QRect(Pad + col * (tileW + Gap), stripY + row * (tileH + Gap),
                                       tileW, tileH);
    }
}

const CallView::Tile *CallView::tileAt(const QPoint &pos) const
{
    for (const Tile &tile : m_tiles) {
        if (tile.box.contains(pos))
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
        if (!tile.box.intersects(event->rect()))
            continue;
        paintTile(painter, tile);
    }
}

void CallView::paintTile(QPainter &painter, const Tile &tile) const
{
    const qreal radius = tile.featured ? 16.0 : 12.0;
    QPainterPath clip;
    clip.addRoundedRect(QRectF(tile.box), radius, radius);

    painter.save();
    painter.setClipPath(clip);
    painter.fillPath(clip, withAlpha(Theme::SurfaceChat, 230));

    const QImage live = framesFor(tile.surface).value(tile.userId);
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

        if ((tile.streaming || tile.video) && tile.featured) {
            QFont note = font();
            note.setPixelSize(13);
            painter.setFont(note);
            painter.setPen(QColor(Theme::TextMuted));
            painter.drawText(QRect(tile.box.left() + 20, avatarBox.bottom() + 10, tile.box.width() - 40, 20),
                             Qt::AlignHCenter | Qt::AlignVCenter,
                             tile.surface == Surface::Share ? QStringLiteral("waiting for the screen...")
                                                            : QStringLiteral("waiting for the camera..."));
        }
    }

    // Name plate, darker at the bottom so white text on a bright share still reads.
    QLinearGradient fade(tile.box.bottomLeft(), QPoint(tile.box.left(), tile.box.bottom() - 72));
    fade.setColorAt(0.0, QColor(0, 0, 0, tile.featured ? 170 : 140));
    fade.setColorAt(1.0, QColor(0, 0, 0, 0));
    painter.fillRect(QRect(tile.box.left(), tile.box.bottom() - 72, tile.box.width(), 72), fade);

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
    QPen edge(tile.speaking ? QColor(Theme::Green)
                            : ((tile.userId == m_focusedUser && tile.surface == m_focusedSurface)
                                   ? QColor(Theme::Accent)
                                   : withAlpha(Theme::Border, 180)));
    if (hovered && !tile.speaking)
        edge.setColor(QColor(Theme::AccentHover));
    edge.setWidthF(tile.featured ? 2.4 : 1.6);
    painter.setPen(edge);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(tile.box).adjusted(1, 1, -1, -1), radius, radius);

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
    const Tile *tile = tileAt(event->pos());
    if (!tile) {
        QFrame::mousePressEvent(event);
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
    const Tile *tile = tileAt(event->pos());
    const QString id = tile ? tile->userId : QString();
    const Surface surface = tile ? tile->surface : Surface::Camera;
    if (id == m_hoverUserId && surface == m_hoverSurface)
        return;
    m_hoverUserId = id;
    m_hoverSurface = surface;
    setCursor(tile ? Qt::PointingHandCursor : Qt::ArrowCursor);
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
