#pragma once

#include <QElapsedTimer>
#include <QHash>
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

protected:
    // Returns the eased value for this row and schedules a repaint if it is
    // still settling.
    qreal progress(QHash<int, qreal> &store, int row, bool on) const;

    QAbstractItemView *m_view = nullptr;

private:
    void scheduleRepaint() const;

    mutable bool m_repaintQueued = false;
    mutable QHash<const void *, QHash<int, qreal>> m_velocity;
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

private:
    mutable QHash<int, qreal> m_hover;
    mutable QHash<int, qreal> m_select;
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

private:
    enum class Button { Message, Profile, None };

    static QRect buttonRect(const QRect &row, Button button);
    static Button buttonAt(const QRect &row, const QPoint &point);

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
    QSet<QString> m_speaking;
};
