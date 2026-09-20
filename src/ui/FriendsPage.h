#pragma once

#include <QWidget>

class MessageStore;
class RestClient;

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

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

private:
    enum class Tab { Online, All, Pending, Blocked };

    QWidget *buildTabBar();
    void setTab(Tab tab);
    void startDirectMessage(const QString &userId);

    MessageStore *m_store = nullptr;
    RestClient *m_rest = nullptr;

    QPushButton *m_onlineTab = nullptr;
    QPushButton *m_allTab = nullptr;
    QPushButton *m_pendingTab = nullptr;
    QPushButton *m_blockedTab = nullptr;

    QLineEdit *m_search = nullptr;
    QLabel *m_heading = nullptr;
    QListWidget *m_list = nullptr;

    Tab m_tab = Tab::Online;
};
