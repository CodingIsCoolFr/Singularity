#pragma once

#include <QPoint>
#include <QSet>
#include <QTimer>
#include <QWidget>

class MessageStore;
class RestClient;

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

private:
    enum class Tab { Online, All, Pending, Blocked };

    QWidget *buildTabBar();
    void setTab(Tab tab);
    void startDirectMessage(const QString &userId);
    void rebuildActivity();
    void fitActivityWidth();
    bool eventFilter(QObject *watched, QEvent *event) override;

    MessageStore *m_store = nullptr;
    RestClient *m_rest = nullptr;

    QPushButton *m_onlineTab = nullptr;
    QPushButton *m_allTab = nullptr;
    QPushButton *m_pendingTab = nullptr;
    QPushButton *m_blockedTab = nullptr;

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
