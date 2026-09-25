#pragma once

#include "core/RestClient.h"

#include <QPoint>
#include <QSet>
#include <QTimer>
#include <QWidget>

class MessageStore;

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QWidget;

// The page behind the "Friends" row at the top of the direct message list.
//
// Rows are painted by a delegate rather than built out of widgets. With a few
// hundred friends the widget approach meant thousands of objects, which took
// seconds to appear and scrolled badly.
class FriendsPage : public QWidget
{
    Q_OBJECT

public:
    FriendsPage(MessageStore *store, RestClient *rest, QWidget *parent = nullptr);

    void refresh();

signals:
    void openDirectMessage(const QString &channelId);
    void openProfile(const QString &userId);
    void personMenuRequested(const QString &userId, const QPoint &globalPos);
    // A voice card was clicked. guild id and channel id are enough to join
    // even when that channel has never been opened in the sidebar.
    void joinVoiceChannel(const QString &guildId, const QString &channelId);
    // Something short for the status line, such as why a request failed.
    void statusMessage(const QString &text);

private:
    enum class Tab { Online, All, Pending, Blocked, Add };

    QWidget *buildTabBar();

    // The Add Friend tab: a username box and a Send button, the way Discord's
    // own Friends page has it. Discord often asks for a captcha first; the
    // person solves it and the same request goes again with the answer.
    QWidget *buildAddPanel();
    void sendFriendRequest(const RestClient::CaptchaProof &captcha = {});
    void showAddResult(bool ok, const QString &text);
    void setTab(Tab tab);
    void startDirectMessage(const QString &userId);

    // The pending and blocked buttons. Both go to Discord first and change
    // the list only when it agrees, so what shows here is what the official
    // client shows too. Discord then sends RELATIONSHIP_ADD / _REMOVE to
    // every session, which keeps the other devices in step.
    void acceptRequest(const QString &userId);
    void removeRelationship(const QString &userId);
    QSet<QString> m_busy;
    void rebuildActivity();
    void fitActivityWidth();
    bool eventFilter(QObject *watched, QEvent *event) override;

    MessageStore *m_store = nullptr;
    RestClient *m_rest = nullptr;

    QPushButton *m_onlineTab = nullptr;
    QPushButton *m_allTab = nullptr;
    QPushButton *m_pendingTab = nullptr;
    QPushButton *m_blockedTab = nullptr;
    QPushButton *m_addTab = nullptr;

    QWidget *m_top = nullptr;         // search and heading, hidden on Add Friend
    QWidget *m_addPanel = nullptr;
    QLineEdit *m_addField = nullptr;
    QPushButton *m_addSend = nullptr;
    QLabel *m_addResult = nullptr;
    bool m_addBusy = false;

    QLineEdit *m_search = nullptr;
    QLabel *m_heading = nullptr;
    QListWidget *m_list = nullptr;
    QWidget *m_activity = nullptr;
    QScrollArea *m_activityScroll = nullptr;
    QVBoxLayout *m_activityLayout = nullptr;
    QSet<QString> m_namesAsked;

    // Pictures arrive one at a time, long after the rows are on screen, and
    // with a few hundred friends that is a few hundred arrivals. Rebuilding on
    // each one would make the list thrash, so they are gathered up and the
    // page is redrawn once when they stop coming.
    QTimer m_artworkTimer;

    Tab m_tab = Tab::Online;
};
