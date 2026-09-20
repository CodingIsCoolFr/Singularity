#include "ui/CallView.h"

#include "core/MessageStore.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

#include <cmath>

namespace {

constexpr int TileGap = 10;
constexpr int TileMinWidth = 190;
constexpr int AvatarPixels = 72;

} // namespace

CallView::CallView(MessageStore *store, QWidget *parent)
    : QFrame(parent)
    , m_store(store)
{
    setObjectName(QStringLiteral("CallView"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("#CallView { background-color: %1; border-bottom: 1px solid %2; }")
                      .arg(QLatin1String(Theme::SurfaceRail), QLatin1String(Theme::Border)));
    setMinimumHeight(0);
    setVisible(false);
    setMouseTracking(true);
}

void CallView::setChannel(const QString &channelId)
{
    if (m_channelId == channelId)
        return;

    m_channelId = channelId;
    if (channelId.isEmpty()) {
        m_tiles.clear();
        setVisible(false);
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

void CallView::setFrame(const QString &userId, const QImage &image)
{
    if (userId.isEmpty() || image.isNull())
        return;

    m_frames.insert(userId, image);

    // Repaint only that tile. Frames arrive thirty times a second and
    // redrawing the whole grid each time would be wasteful.
    for (const Tile &tile : m_tiles) {
        if (tile.userId == userId) {
            update(tile.box);
            return;
        }
    }
}

void CallView::dropFrames(const QString &userId)
{
    if (m_frames.remove(userId) > 0)
        update();
}

void CallView::refresh()
{
    if (m_channelId.isEmpty() || !m_store) {
        setVisible(false);
        return;
    }

    const QStringList members = m_store->voiceMembers(m_channelId);

    m_tiles.clear();
    m_tiles.reserve(members.size());

    for (const QString &userId : members) {
        const UserInfo user = m_store->user(userId);
        const VoiceStateInfo state = m_store->voiceState(userId);

        Tile tile;
        tile.userId = userId;
        tile.name = user.displayName().isEmpty() ? userId : user.displayName();
        tile.streaming = state.streaming;
        tile.video = state.video;
        tile.muted = state.muted;
        tile.deafened = state.deafened;
        tile.speaking = m_speaking.contains(userId);
        m_tiles.append(tile);
    }

    setVisible(!m_tiles.isEmpty());
    layoutTiles();
    update();
}

void CallView::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    layoutTiles();
}

// Works out how many tiles fit across, then how tall the whole thing has to
// be, and asks for that height. The view grows and shrinks with the number of
// people rather than being a fixed band with gaps in it.
void CallView::layoutTiles()
{
    if (m_tiles.isEmpty())
        return;

    const int usable = qMax(TileMinWidth, width() - TileGap * 2);
    int columns = qMax(1, (usable + TileGap) / (TileMinWidth + TileGap));
    columns = qMin(columns, m_tiles.size());

    const int rows = (m_tiles.size() + columns - 1) / columns;
    const int tileWidth = (usable - TileGap * (columns - 1)) / columns;
    const int tileHeight = static_cast<int>(tileWidth * 0.62);

    for (int index = 0; index < m_tiles.size(); ++index) {
        const int row = index / columns;
        const int column = index % columns;
        m_tiles[index].box = QRect(TileGap + column * (tileWidth + TileGap),
                                   TileGap + row * (tileHeight + TileGap), tileWidth, tileHeight);
    }

    // Four rows is plenty on screen at once; beyond that the conversation
    // underneath would be squeezed out entirely.
    const int wanted = TileGap + qMin(rows, 4) * (tileHeight + TileGap);
    setFixedHeight(wanted);
}

void CallView::paintEvent(QPaintEvent *event)
{
    QFrame::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    for (const Tile &tile : m_tiles) {
        if (!tile.box.intersects(event->rect()))
            continue;

        // The tile.
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(Theme::SurfaceChat));
        painter.drawRoundedRect(tile.box, 10, 10);

        // Talking shows as an edge, the way it does in the real client.
        if (tile.speaking) {
            QPen edge{QColor(Theme::Green)};
            edge.setWidthF(2.0);
            painter.setPen(edge);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(tile.box).adjusted(1, 1, -1, -1), 10, 10);
        }

        // A live picture fills the tile. Anything else falls back to the
        // person's photo in the middle.
        const QImage live = m_frames.value(tile.userId);

        if (!live.isNull()) {
            // Kept to its own shape inside the tile, so a wide screen share is
            // letterboxed rather than stretched into the wrong proportions.
            const QSize fitted = live.size().scaled(tile.box.size(), Qt::KeepAspectRatio);
            const QRect target(tile.box.center().x() - fitted.width() / 2,
                               tile.box.center().y() - fitted.height() / 2, fitted.width(),
                               fitted.height());

            painter.save();
            QPainterPath clip;
            clip.addRoundedRect(tile.box, 10, 10);
            painter.setClipPath(clip);
            painter.drawImage(target, live);
            painter.restore();
        } else {
            const UserInfo user = m_store->user(tile.userId);
            const QUrl url = MediaCache::avatarUrl(tile.userId, user.avatarHash, 160);
            const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);

            const QRect avatarBox(tile.box.center().x() - AvatarPixels / 2,
                                  tile.box.center().y() - AvatarPixels / 2 - 6, AvatarPixels,
                                  AvatarPixels);

            const QPixmap face = picture.isNull()
                ? MediaCache::initialsAvatar(tile.name, AvatarPixels)
                : MediaCache::circular(picture, AvatarPixels);
            painter.drawPixmap(avatarBox, face);
        }

        // Name along the bottom.
        QFont nameFont = font();
        nameFont.setPixelSize(11);
        painter.setFont(nameFont);
        painter.setPen(QColor(tile.speaking ? Theme::TextPrimary : Theme::TextMuted));

        const QRect nameBox(tile.box.left() + 10, tile.box.bottom() - 22, tile.box.width() - 20, 16);
        painter.drawText(nameBox, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(tile.name, Qt::ElideRight, nameBox.width()));

        // What they are doing, top right.
        int edgeX = tile.box.right() - 8;

        if (tile.streaming || tile.video) {
            QFont badgeFont = font();
            badgeFont.setPixelSize(8);
            badgeFont.setWeight(QFont::Bold);
            painter.setFont(badgeFont);

            const QString word = tile.streaming ? QStringLiteral("LIVE") : QStringLiteral("CAM");
            const int badgeWidth = painter.fontMetrics().horizontalAdvance(word) + 10;
            const QRect badge(edgeX - badgeWidth, tile.box.top() + 8, badgeWidth, 13);

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(tile.streaming ? Theme::Red : Theme::Highlight));
            painter.drawRoundedRect(badge, 3, 3);

            painter.setPen(QColor(tile.streaming ? Theme::TextPrimary : Theme::Dark));
            painter.drawText(badge, Qt::AlignCenter, word);

            edgeX = badge.left() - 6;

            // While the picture has not arrived, say what is being waited for
            // rather than leaving an empty rectangle.
            if (live.isNull()) {
                QFont noteFont = font();
                noteFont.setPixelSize(10);
                painter.setFont(noteFont);
                painter.setPen(QColor(Theme::TextFaint));
                painter.drawText(QRect(tile.box.left() + 10, tile.box.bottom() - 38,
                                       tile.box.width() - 20, 14),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 QStringLiteral("waiting for the picture..."));
            }
        }

        // Muted and deafened, bottom right, drawn rather than written.
        const int markSize = 13;
        int markX = tile.box.right() - 10 - markSize;
        const int markY = tile.box.bottom() - 22;

        const auto strike = [&](const QRectF &box, const QColor &colour) {
            QPen bar(colour);
            bar.setWidthF(1.6);
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
            painter.drawRoundedRect(QRectF(box.center().x() - 2, box.top() + 1.5, 4, 6.5), 2, 2);

            QPen stem(colour);
            stem.setWidthF(1.4);
            stem.setCapStyle(Qt::RoundCap);
            painter.setPen(stem);
            painter.setBrush(Qt::NoBrush);
            painter.drawArc(QRectF(box.center().x() - 4, box.top() + 4, 8, 7), 180 * 16, 180 * 16);
            painter.drawLine(QPointF(box.center().x(), box.top() + 10.5),
                             QPointF(box.center().x(), box.bottom() - 1));
            strike(box, colour);
        }
    }
}

void CallView::mousePressEvent(QMouseEvent *event)
{
    for (const Tile &tile : m_tiles) {
        if (!tile.box.contains(event->pos()))
            continue;

        if (tile.streaming || tile.video)
            emit watchAttempted(tile.userId);
        else
            emit profileRequested(tile.userId);
        return;
    }

    QFrame::mousePressEvent(event);
}
