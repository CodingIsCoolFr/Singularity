#pragma once

#include "core/GatewayClient.h"
#include "core/MessageStore.h"
#include "core/NotificationRules.h"
#include "core/RestClient.h"
#include "core/SettingsProto.h"
#include "core/VoiceConnection.h"
#include "ui/RailLayout.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>

class PluginHost;

class Backdrop;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QPushButton;
class QSlider;
class QSplitter;
class CallView;
class ChannelDelegate;
class ChatView;
class FriendsPage;
class ImageViewer;
class QStackedWidget;
class ProfileDialog;
class SettingsDialog;
class QListWidget;
class QListWidgetItem;
class QHBoxLayout;
class QTextEdit;
class QLabel;
class QToolButton;
class QShowEvent;

// The one window: server rail, channel sidebar, message view, composer.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(RestClient *rest, GatewayClient *gateway, MessageStore *store, PluginHost *plugins,
               QWidget *parent = nullptr);

    // Stops the media thread, after deleting the voice connections on it.
    ~MainWindow() override;

    // The account switcher.
    void switchToAccount(const QString &userId);
    void restartInto(const QStringList &arguments);
    void manageAccounts();

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
    QString composerPayload() const;
    void updateMentionPopup();
    void hideMentionPopup();
    void insertPickedMention();
    bool mentionQuery(int *atPos, QString *query) const;
    void showMessageMenu(const QPoint &pos);
    void beginReply(const QString &messageId);
    // "Forward" on a message: pick up to five places, optionally add a line.
    void forwardMessage(const QString &messageId);
    void beginEdit(const QString &messageId);
    void clearComposerContext();
    void stopEditing();
    void chooseAttachment();

    // Every way a file gets attached - the picker, a drop, a paste - ends here.
    // Files over the Discord limit go to the Big files plugin instead.
    void addAttachments(const QStringList &paths);
    void startBigUpload(const QString &path);
    qint64 uploadLimitBytes() const;
    bool handleFileDrop(QEvent *event);
    void showEmojiMenu();
    void showReactionPicker(const QString &messageId, const QPoint &globalPos);
    void refreshComposerContext();
    QString messageIdAt(const QPoint &viewportPos) const;

    void refreshUnreadMarks();
    void acknowledgeChannel(const QString &channelId);

    void toggleCamera();
    void stopCamera();
    void applyCameraDevice();
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
    void goToConnectedVoice();
    void joinVoiceAt(const QString &guildId, const QString &channelId);
    bool voiceChannelFull(const QString &channelId) const;
    void leaveVoice();
    void updateVoicePanel();
    void createInvite(const QString &channelId);

    // Invites: the "Join a server" box, the card under a message that holds
    // a discord.gg link, and the join itself (with the captcha Discord often
    // asks for, solved by the person in CaptchaDialog).
    // A Windows notification for a new message, when Discord's rules for that
    // chat say so (NotificationRules) and the window is not in front.
    void maybeNotify(const QJsonObject &data, const MessageInfo &message);
    void showDesktopNotification(const QString &title, const QString &text, const QString &channelId);

    // Direct messages with something unread, as pictures with a red count
    // under the home tile, the way Discord puts them there. Kept in step with
    // the read state; clicking one opens that chat.
    void refreshDmTiles();
    bool m_refreshingDmTiles = false;

    // "Read All" above the rail: every unread chat marked read in one go.
    void readAll();

    // The server menu under the server's name, as in Discord: Mark As Read,
    // Copy Server ID, Leave Server.
    void showGuildMenu(const QPoint &globalPos);

    // Telling Discord what was read, the way its own client does it: a queue,
    // sent a hundred at a time with a second between, and a batch Discord
    // says came too fast is sent again when Discord says to. Read All used to
    // fire every batch at once and 33 of 34 were refused.
    void queueAcks(const QList<QPair<QString, QString>> &channelsAndMessages);
    void sendNextAckBatch();
    QList<QPair<QString, QString>> m_ackQueue;
    bool m_ackSending = false;
    int m_ackSent = 0;
    int m_ackGivenUp = 0;
    void markGuildRead(const QString &guildId);
    // Asks first, then leaves. guildGone tidies up after a leave, a kick or a
    // ban (GUILD_DELETE without "unavailable").
    void leaveGuild(const QString &guildId);
    void guildGone(const QString &guildId);

    void showJoinServerDialog(const QString &prefill = {});
    void requestInvite(const QString &code);
    void redrawInviteWaiters(const QString &code);
    void joinInvite(const QString &code, const QByteArray &context,
                    std::function<void(bool joined, const QString &problem)> onDone = {},
                    const RestClient::CaptchaProof &captcha = {});
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

    // Redraws one message in place. False means it could not be done safely
    // and the whole channel has to be drawn again.
    bool replaceMessageInView(const QString &messageId);

    // Somebody scrolled to the top: draw more of what is already here, or ask
    // Discord for more when there is nothing left undrawn.
    void reachedTop();

    // The other direction. Messages that were let go of while reading upwards
    // are put back when the reader comes back down, so the scrollbar keeps
    // moving through the conversation instead of stopping at a cut edge.
    void revealNewer();

    // Inserts a run of messages that sit immediately above or below what is
    // already drawn. The scrollbar moves by exactly the height that was
    // inserted, so the messages on screen stay where they are and the new
    // ones are simply there to scroll into.
    bool insertMessagesIntoView(int storeIndex, int count, bool above);
    bool prependMessagesToView(int storeIndex, int count);
    bool appendMessagesToView(int storeIndex, int count);

    // Drops drawn messages that are far off the end the reader is not looking
    // at, once the document has grown past the ceiling. What is on screen is
    // left alone.
    void trimRenderedStart(int drop);
    void trimRenderedEnd(int keep);

    // How many messages a channel opens with, and how many more each scroll to
    // the top adds. Forty is several screens' worth on any window somebody
    // would actually use.
    static constexpr int RenderWindowStep = 40;

    // Past this the far end of the document, the end nobody is looking at, is
    // dropped. The conversation on screen does not get smaller.
    static constexpr int MaxRenderedMessages = 240;

    // What is drawn, against what is held. They differ because a channel opens
    // showing only its newest messages.
    int m_renderWindow = RenderWindowStep;
    int m_renderedCount = 0;

    // Where the window sits. It follows the newest message until somebody
    // reads far enough back that it has to let go of the end.
    int m_renderFirst = 0;
    bool m_windowAtTail = true;

    // Stops one scroll from widening the window several times over.
    QElapsedTimer m_growCooldown;

    // Whether the person has touched the wheel since the last time more was
    // drawn or fetched. Reaching the top is a position and stays true; this
    // makes it an action, which happens once.
    bool m_scrolledSinceLoad = false;

    // Writes what the program is holding on to into the log.
    void reportMemory();

    // Keeping the reader's place across a redraw, by message rather than by
    // pixel count.
    void captureScrollAnchor();
    bool restoreScrollAnchor();
    QString messageHtml(const MessageInfo &message, bool grouped);
    // `jumbo`: a message's own text, where emoji on their own come out big the
    // way Discord shows them. Embeds pass false.
    QString renderContent(const QString &raw, bool jumbo = false);
    QString stickersHtml(const MessageInfo &message) const;
    QString embedsHtml(const MessageInfo &message);
    // Buttons and Discord's Components V2 layout: containers, text blocks,
    // sections, dividers, pictures. See MessageInfo::components.
    QString componentsHtml(const QJsonArray &components, const QString &messageId,
                           const QString &applicationId, int messageFlags, int depth = 0);
    void pressMessageButton(const QString &messageId, const QString &applicationId, int messageFlags,
                            const QString &customId);
    void pressMessageSelect(const QString &messageId, const QString &applicationId, int messageFlags,
                            const QString &customId);
    void toggleReaction(const QString &messageId, const QString &emoji);
    void showInteractionModal(const QJsonObject &data);
    QString textDisplayHtml(const QString &markdown);
    QString inviteCardsHtml(const MessageInfo &message);
    QString activityInviteHtml(const MessageInfo &message) const;
    QString reactionsHtml(const MessageInfo &message) const;

    // Discord's "Wave to …" on a new 1:1 DM, and "Wave back" when they waved
    // first. Sends the Wumpus Wave sticker; a 👋 if Discord refuses it.
    QString emptyChannelHtml() const;
    QString wavePromptHtml(const QList<MessageInfo> &messages) const;
    void sendWave();
    bool isOneToOneDm() const;
    static bool messageHasWave(const MessageInfo &message);
    QString m_waveSentChannelId;
    void updateUserPanel();
    void setTypingHint(const QString &text);
    void handleAnchor(const QUrl &url);
    void showProfile(const QString &userId, const QPoint &globalPos);
    void selectChannelEverywhere(const QString &channelId);

    // Two messages join into one block when the same person sent them close
    // together, the way the real client does it.
    static bool shouldGroup(const MessageInfo &previous, const MessageInfo &current);

    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;
    void layoutTitleRow();
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif
    void flashStatus(const QString &text, int ms = 0);

    RestClient *m_rest = nullptr;
    GatewayClient *m_gateway = nullptr;
    MessageStore *m_store = nullptr;
    PluginHost *m_plugins = nullptr;

    QListWidget *m_guildRail = nullptr;
    QListWidget *m_channelList = nullptr;
    QString m_voiceDragUser;
    QPoint m_voiceDragOrigin;
    bool m_voiceDragMoved = false;
    class GuildHeader *m_sidebarHeader = nullptr;   // name, banner, boost goal
    class QSystemTrayIcon *m_tray = nullptr;        // made on the first notification
    ChatView *m_messageView = nullptr;
    QTextEdit *m_composer = nullptr;
    QListWidget *m_mentionPopup = nullptr;
    int m_mentionAtPos = -1;
    QTimer m_mentionSearchTimer;
    int m_mentionSearchSerial = 0;
    QString m_mentionSearchedFor;
    QList<QPair<QString, QString>> m_mentionSearchHits;
    QWidget *m_composerContext = nullptr;
    QLabel *m_composerContextText = nullptr;
    QLabel *m_attachmentLabel = nullptr;
    QPushButton *m_removeAttachment = nullptr;
    QPushButton *m_stopEdit = nullptr;
    QString m_replyMessageId;
    QString m_editingMessageId;
    QStringList m_pendingFiles;
    QHash<QString, int> m_uploadsInFlight;   // path -> percent, while going to GoFile

    NotificationRules m_notifyRules;
    QString m_notifyChannelId;   // the chat the last notification was about

    // What each invite code seen in chat leads to, fetched once per run.
    struct InviteCard
    {
        bool loaded = false;
        bool valid = false;
        QString guildId;
        QString guildName;
        QString iconHash;
        QString channelId;
        int channelType = 0;
        int online = -1;
        int members = -1;
    };
    QHash<QString, InviteCard> m_invites;
    QHash<QString, QSet<QString>> m_inviteWaiters;   // code -> message ids to redraw
    QSet<QString> m_invitesJoining;                  // codes with a join in flight

    // Set by a join, cleared when that server's GUILD_CREATE arrives: open
    // it there, at the channel the invite pointed to.
    QString m_pendingJoinGuildId;
    QString m_pendingJoinChannelId;
    int m_selfPremiumType = 0;               // 0 none, 1 Classic, 2 Nitro, 3 Basic
    QLabel *m_channelTitle = nullptr;
    QLabel *m_channelTopic = nullptr;
    QLabel *m_typingLabel = nullptr;
    QLabel *m_statusDot = nullptr;
    QLabel *m_statusMessage = nullptr;
    QToolButton *m_appMenu = nullptr;
    QWidget *m_titleBar = nullptr;

    QPushButton *m_captionMax = nullptr;
    QHBoxLayout *m_titleLayout = nullptr;
    QTimer m_statusClearTimer;
    QLabel *m_selfAvatar = nullptr;
    QLabel *m_selfName = nullptr;
    QLabel *m_selfStatus = nullptr;
    // Stands in front of the window while it fills itself in, so the client is
    // not watched assembling itself.
    class LoadingOverlay *m_loading = nullptr;

    class MemberListPanel *m_members = nullptr;
    QStackedWidget *m_chatStack = nullptr;
    QWidget *m_chatPage = nullptr;
    FriendsPage *m_friends = nullptr;
    QWidget *m_userPanel = nullptr;
    QWidget *m_voicePanel = nullptr;
    QLabel *m_voiceChannelLabel = nullptr;
    ProfileDialog *m_profileDialog = nullptr;
    SettingsDialog *m_settingsDialog = nullptr;
    ImageViewer *m_imageViewer = nullptr;
    ChannelDelegate *m_channelDelegate = nullptr;

    QString m_voiceChannelId;
    QString m_voiceGuildId;
    // Set when Discord drops the voice socket (often because the gateway is
    // reconnecting). We stay in the channel locally and join again once the
    // gateway is Ready. Cleared on a real leave or a confirmed kick.
    bool m_rejoinVoiceAfterGateway = false;

    // The channel we most recently chose to leave, and when, so Discord's
    // late echo of it is not taken for being moved back in.
    QString m_leftVoiceChannelId;
    QElapsedTimer m_leftVoiceAt;

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
    // With a guild id the person is asked about through the gateway, batched
    // (see flushMemberLookups); without one, through a single REST call.
    void requestUnknownName(const QString &userId, const QString &guildId = QString());
    void flushMemberLookups();
    QSet<QString> m_namesRequested;
    QHash<QString, QSet<QString>> m_pendingMemberLookups;   // guild -> users
    QTimer m_memberLookupTimer;

    // Scrolling back through a channel.
    //
    // `m_loadingOlder` stops a second request going out while one is still in
    // flight, which scrolling at the top would otherwise fire every few
    // pixels. Channels known to have no more are remembered so reaching the
    // beginning does not ask again for ever.
    // Watching somebody's shared screen.
    //
    // A Go Live stream is a whole second connection, with its own server, and
    // the call underneath has to stay up while it runs. So there is a second
    // VoiceConnection here, used only as a viewer.
    void watchStream(const QString &userId);
    void stopWatchingStream();
    void tryStartStream();

    // Sharing our own screen.
    //
    // A third voice connection, on the same reasoning as the viewer one: Go
    // Live has its own server and the call underneath has to stay up. This one
    // sends pictures and opens no microphone, because the microphone is
    // already being carried by the call.
    // Reads the background out of settings and puts it on the window: the
    // drawn hole, or a picture of your own.
    void applyBackgroundSettings();

    // Online, idle, do not disturb, invisible - from your own panel at the
    // bottom of the sidebar, which is where people look for it.
    void showStatusMenu();
    // `storeOnDiscord` is true only when the person picked it from the menu.
    // Anything else - signing in, another device changing it - adopts what
    // Discord already has and must never write back, or an old copy of this
    // program overwrites the status someone set somewhere else.
    void setPresenceStatus(const QString &status, bool storeOnDiscord = true);

    void startScreenShare();
    void stopScreenShare();
    void tryBeginBroadcast();

    class ScreenShare *m_share = nullptr;
    VoiceConnection *m_shareVoice = nullptr;
    QPushButton *m_shareButton = nullptr;
    QPushButton *m_updateButton = nullptr;   // Discord's green "update ready" arrow
    QPushButton *m_cameraButton = nullptr;
    class CameraShare *m_camera = nullptr;
    class QCamera *m_webcam = nullptr;
    class QMediaCaptureSession *m_cameraSession = nullptr;
    class QVideoSink *m_cameraSink = nullptr;

    QString m_myStreamKey;
    QString m_myStreamServerId;
    QString m_myStreamChannelId;
    QString m_myStreamToken;
    QString m_myStreamEndpoint;

    // Our Go Live tile's still picture, as the official client keeps it: one
    // a moment after the share starts, then a fresh one every five minutes
    // (every minute after a failure). Without it phones, and anyone not yet
    // watching, see a black tile.
    QImage m_lastSharePicture;
    QTimer m_streamPreviewTimer;
    int m_streamPreviewTries = 0;
    void uploadStreamPreview();

    // What the picker chose, held until the server answers.
    QString m_shareMonitorId;
    int m_shareWidth = 0;
    int m_shareHeight = 0;
    int m_shareFps = 30;
    int m_shareBitrate = 0;

    // The sound that goes with the picture, and which one was picked.
    class ShareAudio *m_shareAudio = nullptr;
    int m_shareSoundSource = 0;   // ShareAudio::Source
    quint32 m_shareSoundPid = 0;
    QString m_shareSoundName;

    VoiceConnection *m_streamVoice = nullptr;

    // Where all three voice connections live: sound, network and video
    // assembly for calls, never on the thread that draws the window.
    QThread m_mediaThread;
    void startMediaThread();

    QString m_watchingUserId;
    QString m_streamKey;
    QString m_streamServerId;
    QString m_streamChannelId;
    QString m_streamToken;
    QString m_streamEndpoint;

    void loadOlderMessages();
    bool m_loadingOlder = false;

    // Set while a history page is being merged into the store, so the
    // "history changed" signal does not throw away the document and draw the
    // newest page again. That redraw was the scroll repeating itself.
    bool m_applyingHistory = false;
    QSet<QString> m_fullyLoaded;
    // The reader's place, kept as a message rather than a pixel count. See
    // captureScrollAnchor for why the pixel count never worked.
    QString m_anchorMessageId;
    int m_anchorOffset = 0;

    // The ids of the messages currently drawn, in order, so the nth frame of
    // the document can be named.
    QStringList m_renderedIds;
    QString m_pendingVoiceToken;
    QString m_pendingVoiceEndpoint;
    QSet<QString> m_speakingUsers;
    QPushButton *m_muteButton = nullptr;
    QPushButton *m_deafenButton = nullptr;
    QPushButton *m_panelActivity = nullptr;
    QPushButton *m_panelMute = nullptr;
    QPushButton *m_panelDeafen = nullptr;
    void setSelfMuted(bool on);
    void setSelfDeafened(bool on);
    // Whether the microphone was muted before Deafen muted it. Undeafen puts
    // it back to this, the way Discord does.
    bool m_mutedBeforeDeafen = false;
    void setActivityShared(bool on);
    QSlider *m_outputVolumeSlider = nullptr;
    QSlider *m_streamVolumeSlider = nullptr;
    QWidget *m_streamControls = nullptr;
    QPushButton *m_stopWatchButton = nullptr;
    QLabel *m_voiceState = nullptr;
    CallView *m_callView = nullptr;
    Backdrop *m_backdrop = nullptr;

    // The frame pacer's beat (SingularityApplication::setFramePace): thirty
    // redraws a second over a picture, sixty while a stream is the big tile,
    // and off behind the black hole (see updateFramePace).
    bool m_paceHole = false;
    bool m_paceStream = false;
    void updateFramePace();
    QSplitter *m_chatSplitter = nullptr;

    void tryStartVoice();
    void rejoinVoiceIfNeeded();
    void applyVoiceSettings();

    // Per-person loudness. Discord is the store; the file is only so a call
    // that starts before READY still uses the last numbers.
    void loadUserAudio();
    void saveUserAudio();
    void syncUserVolumes();
    void rememberUserAudio(const QString &userId, int volume, bool muted, bool publish);
    void flushUserAudio();
    void applyAudioSettingsUpdate(const QJsonObject &data);
    void ingestSettingsProto(const QByteArray &proto, bool partial);
    void showUserVolumeMenu(const QString &userId, const QPoint &globalPos);
    void showPersonMenu(const QString &userId, const QPoint &globalPos);
    void showPersonMenuAt(const QString &userId, const QPoint &globalPos, const QString &closeChannelId);
    void patchVoiceMember(const QString &userId, const QJsonObject &fields, const QString &failed);
    void moveVoiceMember(const QString &userId, const QString &channelId);
    QString voiceGuildOf(const QString &userId) const;
    bool canMoveVoiceMember(const QString &userId) const;
    QString voiceChannelAt(const QPoint &viewportPos) const;
    void startVoiceMemberDrag(const QString &userId);
    bool handleVoiceMemberDrag(QEvent *event, QObject *watched);
    // "Turn Off Video" / "Turn On Video" on a person in the call.
    void setVideoHidden(const QString &userId, bool hidden);
    void setStreamVideoHidden(const QString &userId, bool hidden);
    void closeDirectMessage(const QString &channelId);
    void applyChannelClosed(const QString &channelId);
    void openDirectWith(const QString &userId);
    void ensureClientActivity();
    void findSingularityApplication(const RestClient::CaptchaProof &captcha = {});
    void createSingularityApplication(const RestClient::CaptchaProof &captcha = {});
    // True when Discord asked for a check and the person finished it.
    bool takeCaptcha(const RestClient::Error &error, RestClient::CaptchaProof *proof);
    void proxyClientLogo(const QString &applicationId);
    bool m_openedCentered = false;
    void useClientActivity(const QString &applicationId, const QString &imageKey);
    void refreshVolumePopup();

    QHash<QString, UserAudioLevel> m_userAudio;
    QSet<QString> m_volumeDirty;
    QTimer m_volumePush;
    QPointer<QWidget> m_volumePopup;

    QString m_currentGuildId;   // empty means direct messages
    QString m_listGuild;
    QString m_listChannel;
    void watchGuildChannel(const QString &guildId, const QString &channelId);
    QString m_currentChannelId;
    QString m_selfUserId;
    QString m_selfAvatarHash;
    QString m_selfDisplayName;

    // Following the newest message.
    //
    // A rich text document is measured lazily, so the bottom is not where it
    // appears to be at the moment the text is set. This says "keep going to
    // the bottom as the document settles", and is cleared the moment the
    // reader scrolls away themselves.
    void scrollToBottom();
    bool m_stickToBottom = true;
    bool m_autoScrolling = false;

    // Whether the reader has actually touched this channel's scroll yet. A
    // document growing on its own must never be mistaken for them doing it.
    bool m_readerMoved = false;

    // Redraws, collapsed. Opening a channel draws at once; everything that
    // merely changes what is already on screen waits a moment in case more is
    // coming, because in a busy server it always is.
    void scheduleRender();
    QTimer m_renderTimer;
    // Names learned for mentions. One redraw after they stop arriving, not
    // one redraw per person. The welcome channel was laying itself out
    // twenty-five times.
    QTimer m_mentionRefresh;

    // What a channel shows while it waits for Discord.
    static QString loadingSkeletonHtml();

    // Fetching a channel's history before anybody asks for it.
    //
    // Opening a channel for the first time costs about a third of a second of
    // round trip to Discord, measured, and no amount of local cleverness
    // shortens that. What removes the wait is not doing it while somebody is
    // watching: the history for a channel you are about to open is already
    // here, because it was fetched when you opened the server, or when the
    // pointer crossed its name.
    //
    // One request at a time, spaced out, and never while a channel somebody
    // actually clicked is still loading. This is speculative work, and
    // speculative work that gets in the way of real work is worse than none.
    void prefetchChannel(const QString &channelId);
    void prefetchGuild(const QString &guildId);
    void pumpPrefetch();

    QStringList m_prefetchQueue;
    QSet<QString> m_prefetchAsked;
    QTimer m_prefetchTimer;
    QTimer m_memoryTimer;

    // How often, and by how much, the window's thread was too busy to answer.
    QTimer m_uiPulse;
    QElapsedTimer m_uiPulseClock;
    qint64 m_uiWorstStallMs = 0;
    int m_uiStalls = 0;

    // Which channel is being fetched ahead right now. Clicking that same
    // channel must wait for the request already on its way rather than
    // sending a second one for the same thing.
    QString m_prefetchCurrent;
    bool m_prefetchInFlight = false;

    // Set when Discord says we are asking too often. Speculation stops for
    // the rest of the session rather than spending the allowance somebody
    // else's real request will need.
    bool m_prefetchGaveUp = false;

    MessageInfo m_lastRendered;
    bool m_hasLastRendered = false;

    // Which channel the view currently holds, so a redraw of the same channel
    // can keep your place while a new channel starts at the bottom.
    QString m_renderedChannelId;

    // How the server rail is arranged, saved on this machine only.
    RailLayout m_rail;
    bool m_rebuildingRail = false;

    // This session's token, for adding it to the account switcher. Never logged.
    QString m_sessionToken;

    // The button press that is waiting on the bot, so a popup that comes back
    // can be sent to the same place.
    QString m_buttonChannelId;
    QString m_buttonGuildId;
    QString m_buttonMessageId;
    QString m_buttonApplicationId;
    int m_buttonMessageFlags = 0;

    // Folder sync with Discord. Nothing is ever sent until Discord's own
    // arrangement has been read at sign-in, so an old copy on this machine
    // can never overwrite the real one.
    bool m_railSynced = false;
    bool m_railPushInFlight = false;
    QTimer m_railPushTimer;
    void railChangedByUser();
    void pushRailToDiscord();
    void applyDiscordFolders(const QByteArray &settingsProto, bool partial);

    // Dropping one server onto another, which makes a folder.
    int m_railMergeRow = -1;
    bool handleRailDrag(QEvent *event);

    QTimer m_typingClearTimer;
    QTimer m_voiceRefreshTimer;
    QElapsedTimer m_typingSentTimer;
    QHash<QString, QString> m_userNameCache;
};
