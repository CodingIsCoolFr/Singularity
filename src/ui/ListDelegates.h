#pragma once

#include "core/Cosmetics.h"
#include "ui/NameIcons.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QPair>
#include <QRect>
#include <QSet>
#include <QStyledItemDelegate>

// Roles hung off list rows. Shared so the window and the painters agree.
namespace SingularityRoles {
constexpr int Id = Qt::UserRole + 1;

// "guild", "channel" (text), "voice", "voicemember", "dm", or "header".
constexpr int Kind = Qt::UserRole + 2;

// Direct message rows only: the second line, and the online state that
// colours the little bubble on the avatar.
constexpr int Subtitle = Qt::UserRole + 3;
constexpr int Status = Qt::UserRole + 4;

// Server rail only: which folder a tile sits in, and whether a folder tile is
// open.
constexpr int Folder = Qt::UserRole + 5;
constexpr int FolderOpen = Qt::UserRole + 6;

// Voice rows only. On a person: sharing a screen, camera on, muted, deafened.
// On the channel itself: how long the call has been running, already written
// out as text.
constexpr int Streaming = Qt::UserRole + 7;
constexpr int Video = Qt::UserRole + 8;
constexpr int VoiceMuted = Qt::UserRole + 9;
constexpr int VoiceDeafened = Qt::UserRole + 10;
constexpr int Elapsed = Qt::UserRole + 11;

// Member list only: the colour a role gives a name, and whether a row is one
// of the headings rather than a person.
constexpr int NameColour = Qt::UserRole + 12;
constexpr int Heading = Qt::UserRole + 13;

// Unread is a dot. Mentions is a count, drawn in place of the dot.
constexpr int Unread = Qt::UserRole + 14;
constexpr int Mentions = Qt::UserRole + 15;

// Voice channel rows: how many people are in it, and the cap. Zero cap means
// no limit. A count at the cap is full.
constexpr int VoiceCount = Qt::UserRole + 16;
constexpr int VoiceLimit = Qt::UserRole + 17;

// Folder tiles on the server rail: small pictures of the first four servers
// inside (a QVariantList of QPixmap), and the folder's colour (a QColor,
// invalid when it has none).
constexpr int FolderIcons = Qt::UserRole + 18;
constexpr int FolderColor = Qt::UserRole + 19;

// Friends list only: Discord's relationship type. 1 friend, 2 blocked,
// 3 a request sent to you, 4 a request you sent.
constexpr int Relationship = Qt::UserRole + 20;

// The person a row is about, and the server it is seen in, for what they
// are dressed in there: nameplate, decoration, server tag, name style and
// role colours (LookPainter). A direct message row's Id is the channel, so
// this carries the person; the server is empty outside one.
constexpr int UserId = Qt::UserRole + 21;
constexpr int GuildId = Qt::UserRole + 22;

// The address of an animated avatar (an "a_" hash). The row's picture is its
// first frame; while the row is hovered or open it plays, as Discord's lists
// do. Empty for a still one.
constexpr int AvatarUrl = Qt::UserRole + 23;

// Member list only: a line of small print instead of a person, saying who is
// not listed and why. Its words are the row's DisplayRole.
constexpr int Note = Qt::UserRole + 24;
// Member list only: a person in the Offline group, drawn faded.
constexpr int Faded = Qt::UserRole + 25;
} // namespace SingularityRoles

// Shared easing used by both delegates below.
//
// Each row keeps a number between 0 and 1 for "hovered" and "selected". Every
// paint nudges that number toward its target and, while it is still moving,
// asks for another paint. That gives a smooth slide with no timers to manage
// and nothing to clean up when rows come and go.
class AnimatedDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit AnimatedDelegate(QAbstractItemView *view, QObject *parent = nullptr);

    // Tooltips for the pictures plugins put on a row; the row's own otherwise.
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

protected:
    // Returns the eased value for this row and schedules a repaint if it is
    // still settling.
    qreal progress(QHash<int, qreal> &store, int row, bool on) const;

    // Another frame soon, for something on a row that is moving: a nameplate
    // or decoration playing, a name effect, a gradient role sliding.
    void requestFrame() const { scheduleRepaint(); }

    // What follows a name in a row: the server tag, then the pictures plugins
    // put there (NameIcons). Worked out before the name is drawn, because the
    // name gives way to them, then the pictures to the tag, and nothing to a
    // name squeezed under 40 pixels.
    struct NameTrail
    {
        Cosmetics::GuildTag tag;
        QList<NameIcon> icons;
        int tagSpace = 0;    // with its gap; 0 when it is not shown
        int iconSpace = 0;
        int width() const { return tagSpace + iconSpace; }
    };
    NameTrail nameTrail(const QString &userId, const Cosmetics::GuildTag &tag, NamePlace place, int room,
                        qreal ratio) const;
    // Draws it after a name ending at `x`, centred on `middle`.
    void paintNameTrail(QPainter *painter, int row, const NameTrail &trail, int x, int middle) const;

    // A plugin's picture in place of the status dot (NamePlace::Avatar),
    // cut out of the picture by an outline of `backdrop`. False when there is
    // none, for the dot to be drawn instead.
    bool paintAvatarIcon(QPainter *painter, int row, const QRect &avatar, const QString &userId,
                         const QColor &backdrop) const;

    // The row's picture: its still (DecorationRole), or while `playing` the
    // current frame of its animated avatar (AvatarUrl), as Discord's lists
    // play one only on the row under the pointer or the one open.
    void paintAvatar(QPainter *painter, const QRect &avatar, const QModelIndex &index, bool playing) const;

    QAbstractItemView *m_view = nullptr;

private:
    void scheduleRepaint() const;

    mutable QHash<const void *, QHash<int, qreal>> m_velocity;

    // Where each row's plugin pictures were drawn, and what they say.
    mutable QHash<int, QList<QPair<QRect, QString>>> m_nameTips;
    mutable QHash<int, QPair<QRect, QString>> m_avatarTips;
};

// The server rail on the far left.
//
// Idle icons are circles. Hovering or selecting one squares it off and slides
// a pill out on the left edge, the way the real client does.
class GuildRailDelegate : public AnimatedDelegate
{
    Q_OBJECT

public:
    using AnimatedDelegate::AnimatedDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    // The tile a dragged server would be dropped onto, lit so it is clear a
    // folder is about to be made. -1 for none.
    void setMergeRow(int row) { m_mergeRow = row; }

    // Where a dragged server would land between tiles: a line along the top
    // of `row`, or along its bottom when `bottom` is set. -1 for none.
    void setInsertLine(int row, bool bottom)
    {
        m_insertRow = row;
        m_insertBottom = bottom;
    }

private:
    mutable QHash<int, qreal> m_hover;
    mutable QHash<int, qreal> m_select;
    int m_mergeRow = -1;
    int m_insertRow = -1;
    bool m_insertBottom = false;
};

// One row of the friends list: picture, name, what they are doing, and two
// small round buttons that fade in when the mouse is over the row.
class FriendDelegate : public AnimatedDelegate
{
    Q_OBJECT

public:
    using AnimatedDelegate::AnimatedDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;

signals:
    void messageRequested(const QString &userId);
    void profileRequested(const QString &userId);
    // A pending request: accept one sent to you, or remove (ignore an incoming
    // one, cancel an outgoing one, unblock a blocked person).
    void acceptRequested(const QString &userId);
    void removeRequested(const QString &userId);

private:
    enum class Button { Message, Profile, Accept, Remove, None };

    static QList<Button> buttonsFor(const QModelIndex &index);
    static QRect buttonRect(const QRect &row, int slotFromRight);
    static Button buttonAt(const QRect &row, const QModelIndex &index, const QPoint &point);

    mutable QHash<int, qreal> m_hover;
    mutable QHash<int, qreal> m_select;
};

// One row of a server's member list on the right.
//
// Two shapes in one delegate, because they are one list: a heading carrying a
// role name and a count, and a person carrying a picture, a name in their role
// colour, and whatever they are doing. Keeping them in one list rather than in
// a tree is what lets Discord's own ordering be used unchanged.
class MemberDelegate : public AnimatedDelegate
{
    Q_OBJECT

public:
    using AnimatedDelegate::AnimatedDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    mutable QHash<int, qreal> m_hover;
};

// The channel list.
//
// Selected and hovered rows fade a rounded panel in behind the text, and the
// selected row grows a short accent bar on its left edge.
class ChannelDelegate : public AnimatedDelegate
{
    Q_OBJECT

public:
    using AnimatedDelegate::AnimatedDelegate;

    // Which channel is currently joined, so its row can show "Leave".
    void setJoinedVoiceChannel(const QString &channelId);

    // The voice channel a dragged person is hovering, so that row lights up.
    void setDropTarget(const QString &channelId);

    // Whoever is talking right now gets a green ring round their picture.
    void setSpeakingUsers(const QSet<QString> &userIds);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;

signals:
    // Raised by the small buttons that appear on a voice row when hovered.
    void joinVoiceRequested(const QString &channelId);
    void leaveVoiceRequested(const QString &channelId);
    void openChatRequested(const QString &channelId);
    void inviteRequested(const QString &channelId);
    void channelSettingsRequested(const QString &channelId);

private:
    // The three buttons, left to right, inside one row.
    enum class Button { Chat, Invite, Settings, None };

    static QRect buttonRect(const QRect &row, Button button);
    static Button buttonAt(const QRect &row, const QPoint &point);

    mutable QHash<int, qreal> m_hover;
    mutable QHash<int, qreal> m_select;
    QString m_joinedChannelId;
    QString m_dropChannelId;
    QSet<QString> m_speaking;
};
