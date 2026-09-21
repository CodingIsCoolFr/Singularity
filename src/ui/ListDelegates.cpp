#include "ui/ListDelegates.h"

#include "ui/Theme.h"

#include <QAbstractItemView>
#include <QCursor>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QTimer>
#include <QToolTip>

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr qreal Settled = 0.004;
constexpr int FrameMs = 16;

constexpr int GuildIcon = 48;
constexpr int GuildRowHeight = 66;
constexpr int PillWidth = 4;

constexpr int ChannelRowHeight = 34;
constexpr int HeaderRowHeight = 32;
constexpr int VoiceMemberRowHeight = 28;
constexpr int DirectRowHeight = 46;
constexpr int DirectAvatar = 32;

QColor statusColourFor(const QString &status)
{
    if (status == QLatin1String("online"))
        return QColor(Theme::Green);
    if (status == QLatin1String("idle"))
        return QColor(Theme::Yellow);
    if (status == QLatin1String("dnd"))
        return QColor(Theme::Red);
    // Offline, which is a grey rather than a status colour.
    return QColor(Theme::TextFaint);
}

// The little bubble on the corner of an avatar, cut out of the picture the
// same way the real client does it.
void drawStatusBubble(QPainter *painter, const QRect &avatar, const QString &status,
                      const QColor &backdrop)
{
    const int bubble = qMax(10, avatar.width() / 3);
    const QRect area(avatar.right() - bubble + 1, avatar.bottom() - bubble + 1, bubble, bubble);

    painter->setPen(Qt::NoPen);
    painter->setBrush(backdrop);
    painter->drawEllipse(area.adjusted(-2, -2, 2, 2));

    // A guess is drawn hollow, so it never passes for something Discord said.
    if (status == QLatin1String("hint")) {
        QPen ring{QColor(Theme::Green)};
        ring.setWidthF(2.0);
        painter->setPen(ring);
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(area.adjusted(1, 1, -1, -1));
        return;
    }

    painter->setBrush(statusColourFor(status));
    painter->drawEllipse(area);

    painter->setBrush(backdrop);
    if (status == QLatin1String("dnd")) {
        const int barHeight = qMax(2, bubble / 4);
        painter->drawRect(QRect(area.left() + bubble / 4, area.center().y() - barHeight / 2,
                                bubble / 2, barHeight));
    } else if (status == QLatin1String("idle")) {
        painter->drawEllipse(area.adjusted(-bubble / 5, -bubble / 5, -bubble / 2, -bubble / 2));
    } else if (status != QLatin1String("online")) {
        const int hole = qMax(3, bubble / 3);
        painter->drawEllipse(QRect(area.center().x() - hole / 2, area.center().y() - hole / 2,
                                   hole, hole));
    }
}

// Draws the "#" that marks a text channel.
void drawHashMark(QPainter *painter, const QRectF &box, const QColor &colour)
{
    QPen pen(colour);
    pen.setWidthF(1.4);
    painter->setPen(pen);

    const qreal x = box.center().x();
    const qreal y = box.center().y();
    const qreal arm = 5.0;

    painter->drawLine(QPointF(x - arm, y - 2), QPointF(x + arm, y - 2));
    painter->drawLine(QPointF(x - arm, y + 2), QPointF(x + arm, y + 2));
    painter->drawLine(QPointF(x - 1.5, y - arm), QPointF(x - 3.5, y + arm));
    painter->drawLine(QPointF(x + 3.5, y - arm), QPointF(x + 1.5, y + arm));
}

constexpr int ButtonSize = 22;
constexpr int ButtonGap = 2;

// --- the three hover glyphs ------------------------------------------------

void drawChatGlyph(QPainter *painter, const QRectF &box, const QColor &colour)
{
    QPen pen(colour);
    pen.setWidthF(1.3);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const QRectF bubble(box.center().x() - 6, box.center().y() - 5, 12, 9);
    painter->drawRoundedRect(bubble, 3, 3);

    // Little tail at the bottom left.
    QPolygonF tail;
    tail << QPointF(bubble.left() + 3, bubble.bottom())
         << QPointF(bubble.left() + 2, bubble.bottom() + 3)
         << QPointF(bubble.left() + 6, bubble.bottom());
    painter->drawPolyline(tail);
}

void drawInviteGlyph(QPainter *painter, const QRectF &box, const QColor &colour)
{
    QPen pen(colour);
    pen.setWidthF(1.3);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const qreal x = box.center().x() - 2;
    const qreal y = box.center().y();

    // Head and shoulders.
    painter->drawEllipse(QRectF(x - 5, y - 6, 6, 6));
    painter->drawArc(QRectF(x - 7, y + 0.5, 10, 9), 0, 180 * 16);

    // Plus sign.
    painter->drawLine(QPointF(x + 5, y - 2), QPointF(x + 5, y + 3));
    painter->drawLine(QPointF(x + 2.5, y + 0.5), QPointF(x + 7.5, y + 0.5));
}

// A head and shoulders, used for the "open their profile" button.
void drawPersonGlyph(QPainter *painter, const QRectF &box, const QColor &colour)
{
    QPen pen(colour);
    pen.setWidthF(1.3);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const QPointF centre = box.center();
    painter->drawEllipse(QRectF(centre.x() - 3.5, centre.y() - 6.0, 7.0, 7.0));
    painter->drawArc(QRectF(centre.x() - 6.0, centre.y() + 0.5, 12.0, 11.0), 0, 180 * 16);
}

void drawGearGlyph(QPainter *painter, const QRectF &box, const QColor &colour)
{
    QPen pen(colour);
    pen.setWidthF(1.3);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const QPointF centre = box.center();
    painter->drawEllipse(centre, 3.0, 3.0);
    painter->drawEllipse(centre, 6.0, 6.0);

    // Six teeth around the rim.
    for (int i = 0; i < 6; ++i) {
        const qreal angle = i * M_PI / 3.0;
        const QPointF inner(centre.x() + std::cos(angle) * 5.5, centre.y() + std::sin(angle) * 5.5);
        const QPointF outer(centre.x() + std::cos(angle) * 8.0, centre.y() + std::sin(angle) * 8.0);
        painter->drawLine(inner, outer);
    }
}

// Draws the little speaker that marks a voice channel.
void drawSpeaker(QPainter *painter, const QRectF &box, const QColor &colour)
{
    painter->setPen(Qt::NoPen);
    painter->setBrush(colour);

    const qreal x = box.center().x() - 3.0;
    const qreal y = box.center().y();

    // Body plus cone.
    QPainterPath body;
    body.addRect(QRectF(x - 4.0, y - 2.0, 3.0, 4.0));
    QPolygonF cone;
    cone << QPointF(x - 1.0, y - 2.0) << QPointF(x + 2.0, y - 5.0) << QPointF(x + 2.0, y + 5.0)
         << QPointF(x - 1.0, y + 2.0);
    body.addPolygon(cone);
    painter->drawPath(body);

    // Two sound arcs.
    QPen pen(colour);
    pen.setWidthF(1.2);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawArc(QRectF(x + 2.0, y - 4.0, 4.0, 8.0), -60 * 16, 120 * 16);
    painter->drawArc(QRectF(x + 2.0, y - 6.5, 7.0, 13.0), -55 * 16, 110 * 16);
}

QColor withAlpha(const char *hex, qreal alpha)
{
    QColor colour(hex);
    colour.setAlphaF(alpha);
    return colour;
}

} // namespace

AnimatedDelegate::AnimatedDelegate(QAbstractItemView *view, QObject *parent)
    : QStyledItemDelegate(parent)
    , m_view(view)
{
    if (m_view)
        m_view->viewport()->setMouseTracking(true);
}

void AnimatedDelegate::scheduleRepaint() const
{
    if (m_repaintQueued || !m_view)
        return;

    m_repaintQueued = true;
    QTimer::singleShot(FrameMs, m_view->viewport(), [this]() {
        m_repaintQueued = false;
        if (m_view)
            m_view->viewport()->update();
    });
}

qreal AnimatedDelegate::progress(QHash<int, qreal> &store, int row, bool on) const
{
    const qreal target = on ? 1.0 : 0.0;
    qreal current = store.value(row, target);
    qreal &vel = m_velocity[static_cast<const void *>(&store)][row];

    // Spring instead of a linear ease: it overshoots a little then settles,
    // which reads as a press rather than a fade.
    const qreal stiffness = 220.0;
    const qreal damping = 18.0;
    const qreal dt = FrameMs / 1000.0;
    const qreal force = (target - current) * stiffness - vel * damping;
    vel += force * dt;
    current += vel * dt;

    if (qAbs(target - current) < Settled && qAbs(vel) < 0.02) {
        store.insert(row, target);
        vel = 0.0;
        return target;
    }

    current = qBound(0.0, current, 1.15);
    store.insert(row, current);
    scheduleRepaint();
    return current;
}

// ---------------------------------------------------------------------------
// Server rail
// ---------------------------------------------------------------------------

QSize GuildRailDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(option) Q_UNUSED(index)
    return QSize(GuildIcon + 16, GuildRowHeight);
}

void GuildRailDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    const int row = index.row();
    const bool hovered = option.state & QStyle::State_MouseOver;
    const bool selected = option.state & QStyle::State_Selected;
    const bool isFolder = index.data(SingularityRoles::Kind).toString() == QLatin1String("folder");
    const bool inFolder = !index.data(SingularityRoles::Folder).toString().isEmpty();

    const qreal hoverAmount = progress(m_hover, row, hovered);
    const qreal selectAmount = progress(m_select, row, selected);
    // Selection always wins, hover only tops it up.
    const qreal lift = qMax(selectAmount, hoverAmount);

    // --- the icon ---------------------------------------------------------
    const QRect cell = option.rect;

    // Tiles inside a folder are drawn a little smaller and tucked to the
    // right, so the grouping reads at a glance.
    const int iconSize = inFolder ? GuildIcon - 10 : GuildIcon;
    const int nudge = inFolder ? 6 : 2;
    const QRect iconRect(cell.center().x() - iconSize / 2 + nudge, cell.center().y() - iconSize / 2,
                         iconSize, iconSize);

    if (isFolder) {
        const bool open = index.data(SingularityRoles::FolderOpen).toBool();
        const QRectF box(iconRect);

        painter->setPen(Qt::NoPen);
        painter->setBrush(withAlpha(Theme::SurfaceHover, 0.6 + 0.4 * lift));
        painter->drawRoundedRect(box, 14, 14);

        // A simple folder shape: a body with a tab on its top left.
        QPainterPath shape;
        const qreal x = box.center().x() - 11;
        const qreal y = box.center().y() - 8;
        shape.addRoundedRect(QRectF(x, y + 3, 22, 13), 2.5, 2.5);
        shape.addRoundedRect(QRectF(x, y, 9, 5), 1.5, 1.5);

        painter->setBrush(QColor(open ? Theme::Accent : Theme::TextMuted));
        painter->drawPath(shape);

        painter->restore();
        return;
    }

    // A circle when idle, a rounded square when lit up. The spring can
    // overshoot 1.0, which punches the morph a little past the rest pose.
    const qreal morph = qBound(0.0, lift, 1.0);
    const qreal radius = iconSize / 2.0 - (iconSize / 2.0 - 15.0) * morph;

    if (lift > 0.01) {
        QRadialGradient glow(iconRect.center(), iconSize * 0.95);
        QColor ac(Theme::Accent);
        ac.setAlphaF(0.38 * qBound(0.0, lift, 1.0));
        glow.setColorAt(0.0, ac);
        ac.setAlphaF(0.0);
        glow.setColorAt(1.0, ac);
        painter->setPen(Qt::NoPen);
        painter->setBrush(glow);
        painter->drawEllipse(QRectF(iconRect).adjusted(-8, -8, 8, 8));
    }

    painter->save();
    const QPointF c = iconRect.center();
    painter->translate(c);
    painter->scale(1.0 + 0.07 * lift, 1.0 + 0.07 * lift);
    painter->translate(-c);

    QPainterPath clip;
    clip.addRoundedRect(iconRect, radius, radius);

    // A soft plate behind the icon so transparent pictures still read.
    painter->fillPath(clip, withAlpha(Theme::SurfaceHover, 0.55 + 0.45 * morph));

    const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
    if (!icon.isNull()) {
        painter->save();
        painter->setClipPath(clip);
        icon.paint(painter, iconRect, Qt::AlignCenter);
        painter->restore();
    }
    painter->restore();

    // --- the pill on the left --------------------------------------------
    if (lift > 0.01) {
        // Hover shows a short stub, selection grows it to most of the icon.
        const qreal stub = GuildIcon * 0.42;
        const qreal full = GuildIcon * 0.84;
        const qreal height = stub * hoverAmount + (full - stub) * selectAmount;

        if (height > 1.0) {
            const QRectF pill(cell.left() + 2.0, cell.center().y() - height / 2.0, PillWidth, height);
            painter->setPen(Qt::NoPen);
            QColor pillColor(Theme::Accent);
            pillColor.setAlphaF(0.55 + 0.45 * qBound(0.0, selectAmount, 1.0));
            painter->setBrush(pillColor);
            painter->drawRoundedRect(pill, PillWidth / 2.0, PillWidth / 2.0);
        }
    }

    painter->restore();
}

// ---------------------------------------------------------------------------
// Friends list
// ---------------------------------------------------------------------------

namespace {
constexpr int FriendRowHeight = 50;
constexpr int FriendAvatar = 34;
constexpr int FriendButton = 30;
} // namespace

QSize FriendDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(index)
    return QSize(option.rect.width(), FriendRowHeight);
}

QRect FriendDelegate::buttonRect(const QRect &row, Button button)
{
    const int index = button == Button::Profile ? 0 : 1;
    const int right = row.right() - 14 - index * (FriendButton + 6);
    return QRect(right - FriendButton, row.center().y() - FriendButton / 2, FriendButton, FriendButton);
}

FriendDelegate::Button FriendDelegate::buttonAt(const QRect &row, const QPoint &point)
{
    if (buttonRect(row, Button::Message).contains(point))
        return Button::Message;
    if (buttonRect(row, Button::Profile).contains(point))
        return Button::Profile;
    return Button::None;
}

// ---------------------------------------------------------------------------
// Member list
// ---------------------------------------------------------------------------

namespace {
constexpr int MemberAvatar = 32;
}

QSize MemberDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(option)

    // A heading gets room above it as well as below, which is what separates
    // one group from the next without drawing a line between them.
    if (index.data(SingularityRoles::Heading).toBool())
        return QSize(0, index.row() == 0 ? 26 : 34);

    return QSize(0, 42);
}

void MemberDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                           const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect cell = option.rect;

    if (index.data(SingularityRoles::Heading).toBool()) {
        QFont font = option.font;
        font.setPixelSize(11);
        font.setWeight(QFont::Bold);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
        painter->setFont(font);
        painter->setPen(QColor(Theme::TextFaint));
        painter->drawText(QRect(cell.left() + 14, cell.bottom() - 18, cell.width() - 24, 16),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(Qt::DisplayRole).toString().toUpper());
        painter->restore();
        return;
    }

    const qreal hoverAmount = progress(m_hover, index.row(), option.state & QStyle::State_MouseOver);

    if (hoverAmount > 0.01) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(withAlpha(Theme::SurfaceHover, hoverAmount * 0.9));
        painter->drawRoundedRect(QRectF(cell).adjusted(6, 2, -6, -2), 7, 7);
    }

    const QRect avatarRect(cell.left() + 14, cell.center().y() - MemberAvatar / 2,
                           MemberAvatar, MemberAvatar);
    const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
    if (!icon.isNull())
        icon.paint(painter, avatarRect, Qt::AlignCenter);

    drawStatusBubble(painter, avatarRect, index.data(SingularityRoles::Status).toString(),
                     QColor(Theme::SurfaceSidebar));

    const QString subtitle = index.data(SingularityRoles::Subtitle).toString();
    const int textLeft = avatarRect.right() + 10;
    const int textWidth = qMax(30, cell.right() - textLeft - 12);

    QFont nameFont = option.font;
    nameFont.setPixelSize(13.5);
    nameFont.setWeight(QFont::DemiBold);
    painter->setFont(nameFont);

    // A role colour when they have one, ordinary text when they do not. This
    // is the one place in the client where a colour is not the theme's to
    // choose: it is the server's, and people recognise each other by it.
    const QVariant colour = index.data(SingularityRoles::NameColour);
    painter->setPen(colour.isValid() && colour.value<QColor>().isValid()
                        ? colour.value<QColor>()
                        : QColor(Theme::TextMuted));

    const QString name = index.data(Qt::DisplayRole).toString();
    const QRect nameBox = subtitle.isEmpty()
        ? QRect(textLeft, cell.top(), textWidth, cell.height())
        : QRect(textLeft, cell.top() + 5, textWidth, 17);
    painter->drawText(nameBox, Qt::AlignLeft | Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(name, Qt::ElideRight, textWidth));

    if (!subtitle.isEmpty()) {
        QFont small = option.font;
        small.setPixelSize(11);
        painter->setFont(small);
        painter->setPen(QColor(Theme::TextFaint));
        painter->drawText(QRect(textLeft, cell.top() + 21, textWidth, 15),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(subtitle, Qt::ElideRight, textWidth));
    }

    painter->restore();
}

void FriendDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                           const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect cell = option.rect;
    const int row = index.row();
    const qreal hoverAmount = progress(m_hover, row, option.state & QStyle::State_MouseOver);

    // Background.
    const QRectF panel = QRectF(cell).adjusted(8, 2, -8, -2);
    if (hoverAmount > 0.01) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(withAlpha(Theme::SurfaceHover, hoverAmount * 0.9));
        painter->drawRoundedRect(panel, 8, 8);
        painter->setBrush(withAlpha(Theme::Accent, 0.10 * qBound(0.0, hoverAmount, 1.0)));
        painter->drawRoundedRect(panel, 8, 8);
    }

    // Picture, with the online bubble drawn on top rather than baked into it,
    // so nothing has to be composed per row.
    const QRect avatarRect(cell.left() + 20, cell.center().y() - FriendAvatar / 2,
                           FriendAvatar, FriendAvatar);
    const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
    if (!icon.isNull())
        icon.paint(painter, avatarRect, Qt::AlignCenter);

    drawStatusBubble(painter, avatarRect, index.data(SingularityRoles::Status).toString(),
                     QColor(Theme::SurfaceChat));

    // Buttons appear on hover, the way the real client does it.
    int textRight = 20;
    if (hoverAmount > 0.15) {
        const QPoint cursor = m_view ? m_view->viewport()->mapFromGlobal(QCursor::pos()) : QPoint(-1, -1);

        for (const Button which : {Button::Message, Button::Profile}) {
            const QRect box = buttonRect(cell, which);
            const bool lit = box.contains(cursor);

            painter->setPen(Qt::NoPen);
            painter->setBrush(withAlpha(lit ? Theme::SurfaceRail : Theme::SurfaceInput,
                                        hoverAmount));
            painter->drawEllipse(box);

            const QColor glyph(lit ? Theme::TextPrimary : Theme::TextMuted);
            painter->save();
            if (which == Button::Message)
                drawChatGlyph(painter, box, glyph);
            else
                drawPersonGlyph(painter, box, glyph);
            painter->restore();
        }

        textRight = 2 * (FriendButton + 6) + 22;
    }

    // Name and the line under it.
    const int textLeft = avatarRect.right() + 12;
    const int textWidth = qMax(40, cell.right() - textLeft - textRight);

    QFont nameFont = option.font;
    nameFont.setPixelSize(14);
    nameFont.setWeight(QFont::DemiBold);
    painter->setFont(nameFont);
    painter->setPen(QColor(Theme::TextPrimary));

    const QString name = index.data(Qt::DisplayRole).toString();
    painter->drawText(QRect(textLeft, cell.top() + 7, textWidth, 18), Qt::AlignLeft | Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(name, Qt::ElideRight, textWidth));

    const QString subtitle = index.data(SingularityRoles::Subtitle).toString();
    if (!subtitle.isEmpty()) {
        QFont subFont = option.font;
        subFont.setPixelSize(11);
        painter->setFont(subFont);
        painter->setPen(QColor(Theme::TextFaint));
        painter->drawText(QRect(textLeft, cell.center().y() + 2, textWidth, 16),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(subtitle, Qt::ElideRight, textWidth));
    }

    painter->restore();
}

bool FriendDelegate::editorEvent(QEvent *event, QAbstractItemModel *model,
                                 const QStyleOptionViewItem &option, const QModelIndex &index)
{
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        const Button which = buttonAt(option.rect, mouse->pos());
        if (which == Button::None)
            return AnimatedDelegate::editorEvent(event, model, option, index);

        if (event->type() == QEvent::MouseButtonPress)
            return true;   // swallow, so the row is not also selected

        const QString userId = index.data(SingularityRoles::Id).toString();
        if (which == Button::Message)
            emit messageRequested(userId);
        else
            emit profileRequested(userId);
        return true;
    }

    return AnimatedDelegate::editorEvent(event, model, option, index);
}

// ---------------------------------------------------------------------------
// Channel list
// ---------------------------------------------------------------------------

void ChannelDelegate::setJoinedVoiceChannel(const QString &channelId)
{
    if (m_joinedChannelId == channelId)
        return;
    m_joinedChannelId = channelId;
    if (m_view)
        m_view->viewport()->update();
}

void ChannelDelegate::setSpeakingUsers(const QSet<QString> &userIds)
{
    if (m_speaking == userIds)
        return;
    m_speaking = userIds;
    if (m_view)
        m_view->viewport()->update();
}

QRect ChannelDelegate::buttonRect(const QRect &row, Button button)
{
    // Laid out right to left: settings, invite, chat.
    int index = 0;
    switch (button) {
    case Button::Settings: index = 0; break;
    case Button::Invite:   index = 1; break;
    case Button::Chat:     index = 2; break;
    case Button::None:     return {};
    }

    const int right = row.right() - 10 - index * (ButtonSize + ButtonGap);
    return QRect(right - ButtonSize, row.center().y() - ButtonSize / 2, ButtonSize, ButtonSize);
}

ChannelDelegate::Button ChannelDelegate::buttonAt(const QRect &row, const QPoint &point)
{
    for (const Button which : {Button::Chat, Button::Invite, Button::Settings}) {
        if (buttonRect(row, which).contains(point))
            return which;
    }
    return Button::None;
}

bool ChannelDelegate::editorEvent(QEvent *event, QAbstractItemModel *model,
                                  const QStyleOptionViewItem &option, const QModelIndex &index)
{
    const QString kind = index.data(SingularityRoles::Kind).toString();
    if (kind != QLatin1String("voice"))
        return AnimatedDelegate::editorEvent(event, model, option, index);

    const QString channelId = index.data(SingularityRoles::Id).toString();

    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        const Button which = buttonAt(option.rect, mouse->pos());
        if (which == Button::None)
            return AnimatedDelegate::editorEvent(event, model, option, index);

        // Swallow the press so the row does not also get selected.
        if (event->type() == QEvent::MouseButtonPress)
            return true;

        switch (which) {
        case Button::Chat:     emit openChatRequested(channelId); break;
        case Button::Invite:   emit inviteRequested(channelId); break;
        case Button::Settings: emit channelSettingsRequested(channelId); break;
        case Button::None:     break;
        }
        return true;
    }

    // A double click on the row itself joins or leaves the call.
    if (event->type() == QEvent::MouseButtonDblClick) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (buttonAt(option.rect, mouse->pos()) == Button::None) {
            if (channelId == m_joinedChannelId)
                emit leaveVoiceRequested(channelId);
            else
                emit joinVoiceRequested(channelId);
            return true;
        }
    }

    return AnimatedDelegate::editorEvent(event, model, option, index);
}

QSize ChannelDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const QString kind = index.data(SingularityRoles::Kind).toString();
    if (kind == QLatin1String("header"))
        return QSize(option.rect.width(), HeaderRowHeight);
    if (kind == QLatin1String("voicemember"))
        return QSize(option.rect.width(), VoiceMemberRowHeight);
    if (kind == QLatin1String("dm"))
        return QSize(option.rect.width(), DirectRowHeight);
    return QSize(option.rect.width(), ChannelRowHeight);
}

void ChannelDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QRect cell = option.rect;
    const QString text = index.data(Qt::DisplayRole).toString();
    const QString kind = index.data(SingularityRoles::Kind).toString();

    if (kind == QLatin1String("header")) {
        QFont font = option.font;
        font.setPixelSize(10);
        font.setWeight(QFont::Bold);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
        painter->setFont(font);
        painter->setPen(QColor(Theme::TextFaint));
        painter->drawText(cell.adjusted(14, 8, -10, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
        painter->restore();
        return;
    }

    // A person sitting in a voice channel: small avatar, indented under it.
    if (kind == QLatin1String("voicemember")) {
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const QRect avatarRect(cell.left() + 34, cell.center().y() - 9, 18, 18);

        const bool talking = m_speaking.contains(index.data(SingularityRoles::Id).toString());
        if (talking) {
            // Braces, not parentheses: with parentheses the compiler reads
            // this as a function declaration rather than a variable.
            QPen ring{QColor(Theme::Green)};
            ring.setWidthF(2.0);
            painter->setPen(ring);
            painter->setBrush(Qt::NoBrush);
            painter->drawEllipse(QRectF(avatarRect).adjusted(-2.5, -2.5, 2.5, 2.5));
        }

        if (!icon.isNull())
            icon.paint(painter, avatarRect, Qt::AlignCenter);

        QFont font = option.font;
        font.setPixelSize(12);
        painter->setFont(font);
        painter->setPen(QColor(talking ? Theme::TextPrimary : Theme::TextMuted));

        // Badges sit hard against the right edge, so the name shortens rather
        // than running underneath them.
        int rightEdge = cell.right() - 10;

        if (index.data(SingularityRoles::Streaming).toBool()) {
            // The same red LIVE tag the real client uses, because everybody
            // already knows what it means.
            QFont badgeFont = option.font;
            badgeFont.setPixelSize(8);
            badgeFont.setWeight(QFont::Bold);
            painter->setFont(badgeFont);

            const int badgeWidth = painter->fontMetrics().horizontalAdvance(QStringLiteral("LIVE")) + 8;
            const QRect badge(rightEdge - badgeWidth, cell.center().y() - 6, badgeWidth, 12);

            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(Theme::Red));
            painter->drawRoundedRect(badge, 3, 3);

            painter->setPen(QColor(Theme::TextPrimary));
            painter->drawText(badge, Qt::AlignCenter, QStringLiteral("LIVE"));

            rightEdge = badge.left() - 6;
        }

        // Microphone, headphones and camera, drawn by hand.
        //
        // These were plain characters first: a cross for muted and a crossed
        // circle for deafened. Both were misread the moment somebody saw one,
        // because a cross beside a name reads as a button that removes them.
        // A shape that looks like the thing it is about does not need
        // explaining, so they are drawn like the # and speaker marks above.
        const int markSize = 13;

        const auto reserve = [&]() {
            const QRect box(rightEdge - markSize, cell.center().y() - markSize / 2, markSize, markSize);
            rightEdge -= markSize + 5;
            return box;
        };

        // The diagonal bar that means "off", drawn across whatever it crosses.
        const auto strikeThrough = [&](const QRectF &box, const QColor &colour) {
            QPen bar(colour);
            bar.setWidthF(1.6);
            bar.setCapStyle(Qt::RoundCap);
            painter->setPen(bar);
            painter->drawLine(box.topLeft() + QPointF(1.5, 1.5),
                              box.bottomRight() - QPointF(1.5, 1.5));
        };

        if (index.data(SingularityRoles::Video).toBool()) {
            // A camera: a rounded body with a lens barrel on its side.
            const QRectF box = reserve();
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(Theme::TextMuted));
            painter->drawRoundedRect(QRectF(box.left(), box.top() + 3.5, box.width() - 4.5, box.height() - 7),
                                     2, 2);
            QPolygonF barrel;
            barrel << QPointF(box.right() - 3.5, box.center().y() - 1.5)
                   << QPointF(box.right(), box.center().y() - 3.5)
                   << QPointF(box.right(), box.center().y() + 3.5)
                   << QPointF(box.right() - 3.5, box.center().y() + 1.5);
            painter->drawPolygon(barrel);
        }

        if (index.data(SingularityRoles::VoiceDeafened).toBool()) {
            // Headphones: a band over two earpieces, struck through.
            const QRectF box = reserve();
            const QColor colour(Theme::Red);

            QPen band(colour);
            band.setWidthF(1.6);
            painter->setPen(band);
            painter->setBrush(Qt::NoBrush);
            painter->drawArc(QRectF(box.left() + 1, box.top() + 2, box.width() - 2, box.height() - 3),
                             0 * 16, 180 * 16);

            painter->setPen(Qt::NoPen);
            painter->setBrush(colour);
            painter->drawRoundedRect(QRectF(box.left() + 0.5, box.center().y(), 3, box.height() / 2 - 1),
                                     1.2, 1.2);
            painter->drawRoundedRect(QRectF(box.right() - 3.5, box.center().y(), 3, box.height() / 2 - 1),
                                     1.2, 1.2);

            strikeThrough(box, colour);
        } else if (index.data(SingularityRoles::VoiceMuted).toBool()) {
            // A microphone: a capsule on a stem, struck through.
            const QRectF box = reserve();
            const QColor colour(Theme::Red);

            painter->setPen(Qt::NoPen);
            painter->setBrush(colour);
            painter->drawRoundedRect(QRectF(box.center().x() - 2, box.top() + 1.5, 4, 6.5), 2, 2);

            QPen stem(colour);
            stem.setWidthF(1.4);
            stem.setCapStyle(Qt::RoundCap);
            painter->setPen(stem);
            painter->setBrush(Qt::NoBrush);
            painter->drawArc(QRectF(box.center().x() - 4, box.top() + 4, 8, 7), 180 * 16, 180 * 16);
            painter->drawLine(QPointF(box.center().x(), box.top() + 10.5),
                              QPointF(box.center().x(), box.bottom() - 1));

            strikeThrough(box, colour);
        }

        painter->setFont(font);
        painter->setPen(QColor(talking ? Theme::TextPrimary : Theme::TextMuted));

        const QRect textRect(avatarRect.right() + 8, cell.top(),
                             qMax(0, rightEdge - avatarRect.right() - 10), cell.height());
        const QString elided = painter->fontMetrics().elidedText(text, Qt::ElideRight, textRect.width());
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);

        painter->restore();
        return;
    }

    const int row = index.row();
    const qreal hoverAmount = progress(m_hover, row, option.state & QStyle::State_MouseOver);
    const qreal selectAmount = progress(m_select, row, option.state & QStyle::State_Selected);

    // A direct message row: picture, name, and what they are doing under it.
    if (kind == QLatin1String("dm")) {
        const QRectF panel = QRectF(cell).adjusted(6, 1, -6, -1);
        const qreal backdrop = qMax(selectAmount, hoverAmount * 0.65);
        if (backdrop > 0.01) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(withAlpha(Theme::SurfaceHover, backdrop));
            painter->drawRoundedRect(panel, 7, 7);
        }

        const QRect avatarRect(cell.left() + 12, cell.center().y() - DirectAvatar / 2,
                               DirectAvatar, DirectAvatar);

        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        if (!icon.isNull())
            icon.paint(painter, avatarRect, Qt::AlignCenter);

        drawStatusBubble(painter, avatarRect, index.data(SingularityRoles::Status).toString(),
                         QColor(Theme::SurfaceSidebar));

        const QString subtitle = index.data(SingularityRoles::Subtitle).toString();
        const int textLeft = avatarRect.right() + 10;
        const int textWidth = cell.right() - textLeft - 10;

        QFont nameFont = option.font;
        nameFont.setPixelSize(13);
        nameFont.setWeight(selectAmount > 0.5 ? QFont::DemiBold : QFont::Normal);
        painter->setFont(nameFont);
        painter->setPen(QColor(selectAmount > 0.5 || hoverAmount > 0.3 ? Theme::TextPrimary
                                                                       : Theme::TextMuted));

        // With no second line the name sits in the middle instead.
        const QRect nameRect = subtitle.isEmpty()
            ? QRect(textLeft, cell.top(), textWidth, cell.height())
            : QRect(textLeft, cell.top() + 5, textWidth, cell.height() / 2);
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(text, Qt::ElideRight, textWidth));

        if (!subtitle.isEmpty()) {
            QFont subFont = option.font;
            subFont.setPixelSize(11);
            painter->setFont(subFont);
            painter->setPen(QColor(Theme::TextFaint));

            const QRect subRect(textLeft, cell.center().y(), textWidth, cell.height() / 2 - 4);
            painter->drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter,
                              painter->fontMetrics().elidedText(subtitle, Qt::ElideRight, textWidth));
        }

        painter->restore();
        return;
    }

    // --- background panel -------------------------------------------------
    const QRectF panel = QRectF(cell).adjusted(8, 1, -8, -1);
    const qreal backdrop = qMax(selectAmount, hoverAmount * 0.65);
    if (backdrop > 0.01) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(withAlpha(Theme::SurfaceHover, backdrop));
        painter->drawRoundedRect(panel, 7, 7);
        if (hoverAmount > 0.01) {
            painter->setBrush(withAlpha(Theme::Accent, 0.10 * qBound(0.0, hoverAmount, 1.0)));
            painter->drawRoundedRect(panel, 7, 7);
        }
    }

    // --- accent bar on the selected row -----------------------------------
    if (selectAmount > 0.01) {
        const qreal height = panel.height() * 0.55 * selectAmount;
        const QRectF bar(panel.left() + 1.0, panel.center().y() - height / 2.0, 3.0, height);
        painter->setBrush(QColor(Theme::Accent));
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(bar, 1.5, 1.5);
    }

    // --- hover buttons on a voice row -------------------------------------
    const bool isVoice = kind == QLatin1String("voice");
    const bool showButtons = isVoice && hoverAmount > 0.25;
    int textRightInset = 8;

    if (showButtons) {
        const QPoint cursor = m_view ? m_view->viewport()->mapFromGlobal(QCursor::pos()) : QPoint(-1, -1);

        for (const Button which : {Button::Chat, Button::Invite, Button::Settings}) {
            const QRect box = buttonRect(cell, which);
            const bool lit = box.contains(cursor);

            if (lit) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(withAlpha(Theme::SurfaceInput, 0.9));
                painter->drawRoundedRect(box, 5, 5);
            }

            const QColor glyph(lit ? Theme::TextPrimary : Theme::TextMuted);
            painter->save();
            switch (which) {
            case Button::Chat:     drawChatGlyph(painter, box, glyph); break;
            case Button::Invite:   drawInviteGlyph(painter, box, glyph); break;
            case Button::Settings: drawGearGlyph(painter, box, glyph); break;
            case Button::None:     break;
            }
            painter->restore();
        }

        textRightInset = 3 * (ButtonSize + ButtonGap) + 12;
    }

    // --- label ------------------------------------------------------------
    QColor colour(Theme::TextMuted);
    if (selectAmount > 0.5)
        colour = QColor(Theme::Light);
    else if (hoverAmount > 0.3)
        colour = QColor(Theme::TextPrimary);

    // --- the # or speaker mark -------------------------------------------
    const QRectF markBox(panel.left() + 8, panel.top(), 18, panel.height());
    painter->save();
    if (kind == QLatin1String("voice"))
        drawSpeaker(painter, markBox, colour);
    else
        drawHashMark(painter, markBox, colour);
    painter->restore();

    QFont font = option.font;
    font.setWeight(selectAmount > 0.5 ? QFont::DemiBold : QFont::Normal);
    painter->setFont(font);
    painter->setPen(colour);

    const QRect textRect = panel.toRect().adjusted(30, 0, -textRightInset, 0);
    QString label = text;
    if (isVoice && !m_joinedChannelId.isEmpty()
        && index.data(SingularityRoles::Id).toString() == m_joinedChannelId) {
        label += QStringLiteral("  •  connected");
    }
    const QString elided = painter->fontMetrics().elidedText(label, Qt::ElideRight, textRect.width());
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);

    painter->restore();
}
