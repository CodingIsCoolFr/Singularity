#pragma once

#include "core/GatewayClient.h"
#include "core/MessageStore.h"
#include "core/VoiceConnection.h"
#include "ui/RailLayout.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QTimer>

class RestClient;
class PluginHost;

class QPushButton;
class ChannelDelegate;
class ChatView;
class FriendsPage;
class ImageViewer;
class QStackedWidget;
class ProfileDialog;
class QListWidget;
class QListWidgetItem;
class QTextEdit;
class QLabel;

// The one window: server rail, channel sidebar, message view, composer.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(RestClient *rest, GatewayClient *gateway, MessageStore *store, PluginHost *plugins,
               QWidget *parent = nullptr);

    void startSession(const QString &token);

signals:
    void loggedOut();

private slots:
    void onGatewayReady(const QJsonObject &payload);
    void onGatewayDispatch(const QString &eventType, const QJsonObject &data);
    void onGatewayState(GatewayClient::State state);
    void onGuildSelected(int row);
    void onChannelSelected(int row);
    void onComposerChanged();
    void sendCurrentMessage();
    void openPlugins();
    void openSettings();
    void applyAppearance();
    void openLog();
    void logOut();

private:
    void buildUi();
    void buildMenu();
    QWidget *buildGuildRail(QWidget *parent);
    QWidget *buildSidebar(QWidget *parent);
    QWidget *buildChatColumn(QWidget *parent);
    QWidget *buildUserPanel(QWidget *parent);
    QWidget *buildVoicePanel(QWidget *parent);

    // Voice. Joining puts you in the channel so everyone sees you there.
    // Carrying sound is not built yet.
    void joinVoice(const QString &channelId);
    void leaveVoice();
    void updateVoicePanel();
    void createInvite(const QString &channelId);
    void showChannelSettings(const QString &channelId);

    void populateGuildRail();
    void refreshGuildIcons();
    void showRailMenu(const QPoint &where);
    void saveRailOrderFromView();
    // `autoSelectFirst` is only true when the view genuinely changed, such as
    // picking another server. A background refresh must never move the
    // selection, or clicking a person gets undone a moment later.
    void populateChannelList(bool autoSelectFirst = true);

    // Updates the existing direct message rows in place. Far better than
    // rebuilding: the selection, the scroll and the hover all survive.
    void refreshDirectRows();
    void openChannel(const QString &channelId);
    void renderChannel();
    void appendMessageToView(const MessageInfo &message, bool grouped);
    QString messageHtml(const MessageInfo &message, bool grouped);
    QString renderContent(const QString &raw) const;
    QString stickersHtml(const MessageInfo &message) const;
    QString embedsHtml(const MessageInfo &message) const;
    void updateUserPanel();
    void setTypingHint(const QString &text);
    void handleAnchor(const QUrl &url);
    void showProfile(const QString &userId, const QPoint &globalPos);
    void selectChannelEverywhere(const QString &channelId);

    // Two messages join into one block when the same person sent them close
    // together, the way the real client does it.
    static bool shouldGroup(const MessageInfo &previous, const MessageInfo &current);

    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

    RestClient *m_rest = nullptr;
    GatewayClient *m_gateway = nullptr;
    MessageStore *m_store = nullptr;
    PluginHost *m_plugins = nullptr;

    QListWidget *m_guildRail = nullptr;
    QListWidget *m_channelList = nullptr;
    QLabel *m_sidebarHeader = nullptr;
    ChatView *m_messageView = nullptr;
    QTextEdit *m_composer = nullptr;
    QLabel *m_channelTitle = nullptr;
    QLabel *m_channelTopic = nullptr;
    QLabel *m_typingLabel = nullptr;
    QLabel *m_statusDot = nullptr;
    QLabel *m_selfAvatar = nullptr;
    QLabel *m_selfName = nullptr;
    QLabel *m_selfStatus = nullptr;
    QStackedWidget *m_chatStack = nullptr;
    QWidget *m_chatPage = nullptr;
    FriendsPage *m_friends = nullptr;
    QWidget *m_userPanel = nullptr;
    QWidget *m_voicePanel = nullptr;
    QLabel *m_voiceChannelLabel = nullptr;
    ProfileDialog *m_profileDialog = nullptr;
    ImageViewer *m_imageViewer = nullptr;
    ChannelDelegate *m_channelDelegate = nullptr;

    QString m_voiceChannelId;
    QString m_voiceGuildId;

    // The second connection, the one that actually carries sound.
    VoiceConnection *m_voice = nullptr;
    QString m_voiceSessionId;

    // A failed handshake is retried from scratch a couple of times before
    // giving up, because the usual causes are one-off stale state.
    int m_voiceRetries = 0;
    QTimer m_voiceRetryTimer;

    // Discord answers a join with two separate events. If it answers with
    // neither, nothing else in the client ever complains, and the panel reads
    // "Connecting..." for ever. This says out loud which half never came.
    QTimer m_voiceWatchdog;

    // People we have already asked Discord to name, so a sidebar rebuild does
    // not fire the same lookup again every few seconds.
    void requestUnknownName(const QString &userId);
    QSet<QString> m_namesRequested;
    QString m_pendingVoiceToken;
    QString m_pendingVoiceEndpoint;
    QSet<QString> m_speakingUsers;
    QPushButton *m_muteButton = nullptr;
    QPushButton *m_deafenButton = nullptr;
    QLabel *m_voiceState = nullptr;

    void tryStartVoice();
    void applyVoiceSettings();

    QString m_currentGuildId;   // empty means direct messages
    QString m_currentChannelId;
    QString m_selfUserId;
    QString m_selfAvatarHash;
    QString m_selfDisplayName;

    MessageInfo m_lastRendered;
    bool m_hasLastRendered = false;

    // Which channel the view currently holds, so a redraw of the same channel
    // can keep your place while a new channel starts at the bottom.
    QString m_renderedChannelId;

    // How the server rail is arranged, saved on this machine only.
    RailLayout m_rail;
    bool m_rebuildingRail = false;

    QTimer m_typingClearTimer;
    QTimer m_voiceRefreshTimer;
    QElapsedTimer m_typingSentTimer;
    QHash<QString, QString> m_userNameCache;
};
