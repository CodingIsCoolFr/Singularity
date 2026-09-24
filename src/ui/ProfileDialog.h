#pragma once

#include "core/MessageStore.h"

#include <QColor>
#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>

class RestClient;

class FlowLayout;
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
    void refreshArtwork();
    void rebuildBadges(int publicFlags);
    void rebuildRoles();
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
    QLabel *m_displayName = nullptr;
    QLabel *m_handle = nullptr;

    // One green line, shown only when the Presence hints plugin has something
    // to say about someone Discord reports as offline.
    QLabel *m_meta = nullptr;
    FlowLayout *m_badgeFlow = nullptr;
    QWidget *m_badgeHost = nullptr;
    QPushButton *m_messageButton = nullptr;
    QPushButton *m_friendButton = nullptr;
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

    // The press that closed the window was eaten, so its release is too.
    bool m_swallowRelease = false;

    // Straight from the profile endpoint.
    QJsonObject m_profile;
    QString m_bannerHash;
    QString m_decorationAsset;
    int m_accentColour = 0;
    QJsonArray m_mutualFriends;
    QJsonArray m_mutualGuilds;
};
