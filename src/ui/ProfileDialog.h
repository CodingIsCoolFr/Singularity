#pragma once

#include "core/MessageStore.h"

#include <QColor>
#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTimer>

class RestClient;

class FlowLayout;
class ProfileEffectView;
class ProfileFrameView;
class ProfileNameView;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class QWidget;

// The full profile window, laid out like the real client: a card on the left
// with who the person is, and a panel on the right with what they are doing
// and what you have in common.
//
// Everything here is started by a click. Nothing acts on its own.
class ProfileDialog : public QDialog
{
    Q_OBJECT

public:
    ProfileDialog(MessageStore *store, RestClient *rest, QWidget *parent = nullptr);

    // `guildId` may be empty. When set, the server specific parts appear:
    // nickname, roles, and the date they joined that server.
    void showFor(const QString &userId, const QString &guildId, bool isSelf);

signals:
    void openDirectMessage(const QString &channelId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Building blocks, each returns the widget it made.
    QWidget *buildLeftCard();
    QWidget *buildRightPane();
    QWidget *buildTabBar();

    // Filling them in.
    void applyLocalData();
    void applyProfilePayload(const QJsonObject &payload);

    // Asks Discord for the profile. With a server first, for the nickname,
    // roles and server bio; without one if that is refused, which it is for
    // somebody who has left the server or was never in it.
    void requestProfile(const QString &userId, const QString &guildId, int request);
    void refreshArtwork();

    // The profile effect and frame named by these SKUs, once the shop says
    // what they are made of. Empty SKUs clear them.
    void showProfileArt(const QString &effectSku, const QString &frameSku);
    void setProfileFrame(const Cosmetics::ProfileFrame &frame);

    // The profile's own badge list when Discord sent one - pictures for every
    // badge, the 2026 levelled ones included - or words from the public flags
    // when it did not. A badge opens the badges window on itself.
    void rebuildBadges(const QJsonArray &badges, int publicFlags);
    void refreshBadgeIcons();
    // The pictures plugins put after the badges (NameIcons).
    void refreshPlatformIcons();
    void openBadges(int badgeId, const QString &iconHash = QString());
    void rebuildRoles();

    // Give or take away a role, when you have Manage Roles and the role is
    // below your own highest one - Discord's own rule, so the × and + only
    // show where Discord would allow them.
    void changeRole(const QString &roleId, bool give);
    void rebuildConnections(const QJsonArray &connections);
    void rebuildActivity();
    void rebuildMutuals();
    void setTab(int index);
    void updateFriendButton();

    // Actions.
    void startDirectMessage();
    void toggleFriend();
    void showMoreMenu();
    void saveNote();
    void setNote(const QString &text, bool isError = false);

    static QString relativeSince(qint64 startMs);
    static QWidget *makePill(const QString &text, const QColor &dot, QWidget *parent);

    MessageStore *m_store = nullptr;
    RestClient *m_rest = nullptr;

    // Left card.
    // Both are custom painters declared inside the .cpp file.
    QWidget *m_banner = nullptr;
    QWidget *m_avatar = nullptr;
    ProfileNameView *m_displayName = nullptr;
    QLabel *m_handle = nullptr;

    // The profile effect over the card, and the frame round it: one view
    // behind the card and one in front. The holder is the card's room, which
    // grows by what the frame reaches past it.
    ProfileEffectView *m_effectView = nullptr;
    ProfileFrameView *m_frameBack = nullptr;
    ProfileFrameView *m_frameFront = nullptr;
    QWidget *m_cardHolder = nullptr;

    // One green line, shown only when the Presence hints plugin has something
    // to say about someone Discord reports as offline.
    QLabel *m_meta = nullptr;
    FlowLayout *m_badgeFlow = nullptr;
    QWidget *m_badgeHost = nullptr;
    QPushButton *m_messageButton = nullptr;
    QPushButton *m_friendButton = nullptr;
    QPushButton *m_badgesButton = nullptr;
    QPushButton *m_moreButton = nullptr;
    QLabel *m_bioLabel = nullptr;
    QWidget *m_bioSection = nullptr;
    QLabel *m_memberSince = nullptr;
    QLabel *m_friendsSince = nullptr;
    QWidget *m_friendsSinceSection = nullptr;
    QWidget *m_rolesSection = nullptr;
    QWidget *m_roleHost = nullptr;
    FlowLayout *m_roleFlow = nullptr;
    QWidget *m_connectionsSection = nullptr;
    QVBoxLayout *m_connectionsLayout = nullptr;
    QLineEdit *m_noteEdit = nullptr;
    QLabel *m_noteStatus = nullptr;

    // Right pane.
    QPushButton *m_activityTab = nullptr;
    QPushButton *m_friendsTab = nullptr;
    QPushButton *m_serversTab = nullptr;
    QStackedWidget *m_tabs = nullptr;
    QVBoxLayout *m_activityLayout = nullptr;
    QVBoxLayout *m_mutualFriendsLayout = nullptr;
    QVBoxLayout *m_mutualServersLayout = nullptr;

    QString m_userId;
    QString m_guildId;
    bool m_isSelf = false;

    // Which opening of the card a reply belongs to. A reply for an earlier
    // one is dropped, even when it is for the same person.
    int m_profileRequest = 0;

    // The press that closed the window was eaten, so its release is too.
    bool m_swallowRelease = false;

    // Straight from the profile endpoint.
    QJsonObject m_profile;
    QString m_bannerHash;
    QString m_decorationAsset;
    int m_accentColour = 0;
    QJsonArray m_mutualFriends;
    QJsonArray m_mutualGuilds;

    // The pictures each part of the card shows, by address. The window
    // downloads hundreds of pictures a minute, and redrawing the whole card for
    // every one of them - decoding its animations again each time - froze it
    // for seconds; now only an arrival the card is waiting for redraws, and
    // only the part it belongs to, once per burst.
    enum LatePart { LateArtwork = 1, LateBadges = 2, LateActivity = 4, LateMutuals = 8 };
    QSet<QString> m_artworkUrls;
    QSet<QString> m_badgeUrls;
    QSet<QString> m_activityUrls;
    QSet<QString> m_mutualUrls;
    int m_lateParts = 0;
    QTimer m_lateTimer;
};
