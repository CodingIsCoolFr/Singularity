#include "ui/MainWindow.h"

#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/RestClient.h"
#include "plugin/PluginHost.h"
#include "ui/ChatView.h"
#include "ui/FriendsPage.h"
#include "ui/ImageViewer.h"
#include "ui/ListDelegates.h"
#include "ui/LogDialog.h"
#include "ui/MediaCache.h"
#include "ui/MediaCache.h"
#include "ui/PluginsDialog.h"
#include "ui/ProfileDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QMessageBox>
#include <QDesktopServices>
#include <QFrame>
#include <QInputDialog>
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QScrollBar>
#include <QStatusBar>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// Roles hung off list rows, shared with the painters.
constexpr int IdRole = WispRoles::Id;
constexpr int KindRole = WispRoles::Kind;
constexpr int FolderRole = WispRoles::Folder;
constexpr int FolderOpenRole = WispRoles::FolderOpen;

constexpr int GuildIconPixels = 48;
constexpr int RailWidth = 72;
constexpr int SidebarWidth = 240;

// Discord asks clients to repeat the typing signal at most every 8 seconds.
constexpr qint64 TypingIntervalMs = 8000;
constexpr int HistoryLimit = 50;

// Messages from the same person within this gap join one block.
constexpr qint64 GroupWindowSeconds = 7 * 60;

// The one line shown under a name in the direct message list.
//
// A game or song wins over a typed status, because that is what the real
// client puts first. Extra activities are counted rather than listed.
QString describePresence(const PresenceInfo &presence, const QString &hint = QString())
{
    QString custom;
    QString primary;
    int extras = 0;

    for (const ActivityInfo &activity : presence.activities) {
        if (activity.isCustomStatus()) {
            custom = activity.emoji.isEmpty()
                ? activity.state
                : QStringLiteral("%1 %2").arg(activity.emoji, activity.state).trimmed();
            continue;
        }

        if (primary.isEmpty()) {
            // A song reads better as the track than as "Spotify".
            if (activity.type == 2 && !activity.details.isEmpty())
                primary = activity.details;
            else
                primary = activity.name;
        } else {
            ++extras;
        }
    }

    if (!primary.isEmpty())
        return extras > 0 ? QStringLiteral("%1  +%2").arg(primary).arg(extras) : primary;

    if (!custom.isEmpty())
        return custom;

    // Nothing reported, so fall back to what we worked out ourselves.
    return hint;
}

// "1.4 MB" for a file row, or nothing when the size is unknown.
QString humanSize(qint64 bytes)
{
    if (bytes <= 0)
        return {};
    static const QStringList units{QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"),
                                   QStringLiteral("GB")};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < units.size() - 1) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral(" <span class=\"meta\">%1 %2</span>")
        .arg(value, 0, 'f', value < 10.0 && unit > 0 ? 1 : 0)
        .arg(units.at(unit));
}

// How long somebody has been sitting in a call, in words rather than digits.
//
// Seconds are only interesting for the first minute; after that a ticking
// second counter in a sidebar is noise.
QString elapsedWords(qint64 seconds)
{
    if (seconds < 60)
        return QStringLiteral("just now");

    const qint64 minutes = seconds / 60;
    if (minutes < 60)
        return QStringLiteral("%1 min").arg(minutes);

    const qint64 hours = minutes / 60;
    const qint64 spare = minutes % 60;
    if (hours < 24)
        return spare == 0 ? QStringLiteral("%1 hr").arg(hours)
                          : QStringLiteral("%1 hr %2 min").arg(hours).arg(spare);

    return QStringLiteral("%1 days").arg(hours / 24);
}

} // namespace

MainWindow::MainWindow(RestClient *rest, GatewayClient *gateway, MessageStore *store, PluginHost *plugins,
                       QWidget *parent)
    : QMainWindow(parent)
    , m_rest(rest)
    , m_gateway(gateway)
    , m_store(store)
    , m_plugins(plugins)
    , m_voice(new VoiceConnection(this))
{
    setWindowTitle(QStringLiteral("Wisp"));
    resize(1280, 820);

    buildUi();
    buildMenu();

    m_typingClearTimer.setSingleShot(true);
    connect(&m_typingClearTimer, &QTimer::timeout, this, [this]() { setTypingHint(QString()); });

    connect(m_gateway, &GatewayClient::ready, this, &MainWindow::onGatewayReady);
    connect(m_gateway, &GatewayClient::dispatch, this, &MainWindow::onGatewayDispatch);
    connect(m_gateway, &GatewayClient::stateChanged, this, &MainWindow::onGatewayState);
    connect(m_gateway, &GatewayClient::logLine, this, [this](const QString &line) {
        statusBar()->showMessage(line, 6000);
    });
    connect(m_gateway, &GatewayClient::fatalAuthError, this, [this]() {
        statusBar()->showMessage(QStringLiteral("Discord refused this session. Log out and sign in again."), 0);

        if (m_selfStatus) {
            m_selfStatus->setText(QStringLiteral("signed out"));
            m_selfStatus->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                            .arg(QLatin1String(Theme::Accent)));
        }

        m_messageView->setHtml(QStringLiteral(
            "<p class=\"system\"><b>Discord will not accept this session any more.</b>"
            "<br><br>This nearly always means the saved token is stale. Discord retires a token "
            "when you change your password, sign out elsewhere, or when it decides a sign-in looked "
            "unusual."
            "<br><br>Fix it: <b>Wisp &gt; Log out</b>, then sign in again."
            "<br><br><b>Wisp &gt; Log</b> shows the exact reason, on the line beginning "
            "\"giving up\".</p>"));
    });

    connect(m_plugins, &PluginHost::repaintRequested, this, [this]() { renderChannel(); });
    connect(m_plugins, &PluginHost::pluginLogged, this, [this](const QString &id, const QString &line) {
        statusBar()->showMessage(QStringLiteral("[%1] %2").arg(id, line), 5000);
    });

    connect(m_store, &MessageStore::channelHistoryChanged, this, [this](const QString &channelId) {
        if (channelId == m_currentChannelId)
            renderChannel();
    });
    connect(m_store, &MessageStore::messageChanged, this, [this](const QString &channelId, const QString &) {
        if (channelId == m_currentChannelId)
            renderChannel();
    });

    // A green ring appears round whoever is talking.
    connect(m_voice, &VoiceConnection::speakingChanged, this,
            [this](const QString &userId, bool speaking) {
                const bool had = m_speakingUsers.contains(userId);
                if (speaking == had)
                    return;
                if (speaking)
                    m_speakingUsers.insert(userId);
                else
                    m_speakingUsers.remove(userId);
                m_channelDelegate->setSpeakingUsers(m_speakingUsers);
            });

    connect(m_voice, &VoiceConnection::stateChanged, this, [this](VoiceConnection::State state) {
        if (!m_voiceState)
            return;
        switch (state) {
        case VoiceConnection::State::Idle:        m_voiceState->setText(QStringLiteral("Not in voice")); break;
        case VoiceConnection::State::Connecting:  m_voiceState->setText(QStringLiteral("Connecting...")); break;
        case VoiceConnection::State::Handshaking: m_voiceState->setText(QStringLiteral("Setting up...")); break;
        case VoiceConnection::State::Connected:
            m_voiceState->setText(QStringLiteral("Voice connected"));
            m_voiceRetries = 0;   // a good connection clears the slate
            statusBar()->showMessage(QStringLiteral("Voice connected and encrypted."), 5000);
            break;
        case VoiceConnection::State::Failed:      m_voiceState->setText(QStringLiteral("Voice failed")); break;
        }
    });

    connect(m_voice, &VoiceConnection::failed, this, [this](const QString &reason) {
        statusBar()->showMessage(QStringLiteral("Voice: %1").arg(reason), 10000);

        // Nothing to retry if we are not meant to be in a call any more.
        if (m_voiceChannelId.isEmpty())
            return;

        // A retry that is already on its way must not start another.
        if (m_voiceRetryTimer.isActive())
            return;

        // Marked final by the voice side: trying again would only repeat it.
        if (reason.contains(QStringLiteral("[final]"))) {
            wlog(QStringLiteral("voice"), QStringLiteral("not retrying: %1").arg(reason));
            if (m_voiceState)
                m_voiceState->setText(QStringLiteral("Voice unavailable"));

            if (reason.contains(QStringLiteral("4017"))) {
                // Since March 2026 Discord requires end-to-end encryption on
                // every call, so no other channel will work either. Saying
                // "try another one" would just waste someone's time.
                statusBar()->showMessage(
                    QStringLiteral("Discord now requires end-to-end encryption on every call. "
                                   "Wisp does not implement it yet, so voice cannot connect "
                                   "anywhere. Everything else works."),
                    0);
            }

            m_gateway->leaveVoice(m_voiceGuildId);
            m_voiceChannelId.clear();
            m_voiceGuildId.clear();
            updateVoicePanel();
            return;
        }

        if (m_voiceRetries >= 2) {
            wlog(QStringLiteral("voice"), QStringLiteral("giving up after %1 tries: %2")
                                              .arg(m_voiceRetries).arg(reason));
            if (m_voiceState)
                m_voiceState->setText(QStringLiteral("Voice failed"));

            // This one has a cause outside the client, so say so rather than
            // leaving a bare error on screen.
            if (reason.contains(QStringLiteral("4006"))) {
                statusBar()->showMessage(
                    QStringLiteral("Discord allows one voice connection per account. Close the "
                                   "official Discord app completely, including the tray icon, "
                                   "then try again."),
                    0);
            }
            return;
        }

        // Start the whole handshake again. Leaving first makes Discord throw
        // away the old voice session and issue a fresh server and ticket,
        // which clears the usual causes of a refused one.
        ++m_voiceRetries;
        wlog(QStringLiteral("voice"), QStringLiteral("retry %1 after: %2").arg(m_voiceRetries).arg(reason));

        if (m_voiceState)
            m_voiceState->setText(QStringLiteral("Retrying (%1)...").arg(m_voiceRetries));

        m_voice->disconnectFromVoice();
        m_pendingVoiceToken.clear();
        m_pendingVoiceEndpoint.clear();
        m_voiceSessionId.clear();

        m_gateway->leaveVoice(m_voiceGuildId);
        m_voiceRetryTimer.start(1200);
    });

    // The second half of a retry: rejoin once Discord has let go of the old
    // voice session.
    m_voiceRetryTimer.setSingleShot(true);
    connect(&m_voiceRetryTimer, &QTimer::timeout, this, [this]() {
        if (m_voiceChannelId.isEmpty())
            return;

        AppConfig &config = AppConfig::instance();
        m_gateway->joinVoice(m_voiceGuildId, m_voiceChannelId,
                             config.value(QStringLiteral("voice/joinMuted"), false).toBool(),
                             config.value(QStringLiteral("voice/joinDeafened"), false).toBool());
    });

    // Discord normally answers a join within a second. Ten is generous.
    m_voiceWatchdog.setSingleShot(true);
    connect(&m_voiceWatchdog, &QTimer::timeout, this, [this]() {
        if (m_voiceChannelId.isEmpty())
            return;

        QStringList missing;
        if (m_voiceSessionId.isEmpty())
            missing << QStringLiteral("our own voice state");
        if (m_pendingVoiceEndpoint.isEmpty())
            missing << QStringLiteral("the voice server");

        if (missing.isEmpty())
            return;

        wlog(QStringLiteral("voice"),
             QStringLiteral("Discord did not answer the join after 10 seconds. Still waiting for: %1. "
                            "channel=%2 guild=%3")
                 .arg(missing.join(QStringLiteral(" and ")), m_voiceChannelId, m_voiceGuildId));

        if (m_voiceState)
            m_voiceState->setText(QStringLiteral("Discord did not answer"));
        statusBar()->showMessage(
            QStringLiteral("Discord never answered the join. It may not allow this channel."), 8000);
    });

    // People joining or leaving voice changes the sidebar, but only redraw
    // when a server is actually on screen.
    // Voice comings and goings change a server sidebar; presence changes the
    // direct message list. Both are coalesced through one timer, because a
    // busy account fires dozens of these at once.
    const auto scheduleSidebarRefresh = [this]() {
        if (!m_voiceRefreshTimer.isActive())
            m_voiceRefreshTimer.start(400);
    };

    connect(m_store, &MessageStore::voiceStatesChanged, this, scheduleSidebarRefresh);
    connect(m_store, &MessageStore::userChanged, this, [this, scheduleSidebarRefresh](const QString &) {
        // Only the direct message list shows someone's status and activity.
        if (m_currentGuildId.isEmpty())
            scheduleSidebarRefresh();
    });

    m_voiceRefreshTimer.setSingleShot(true);
    connect(&m_voiceRefreshTimer, &QTimer::timeout, this, [this]() {
        // Direct messages only change their pictures and their second line, so
        // the rows are edited where they stand. Nothing moves.
        if (m_currentGuildId.isEmpty()) {
            refreshDirectRows();
            return;
        }

        // A server list can gain and lose rows as people join voice, so it has
        // to be rebuilt. Keep the place and the selection across it, and do
        // not let the rebuild open a different channel.
        const int scroll = m_channelList->verticalScrollBar()->value();
        const QString keep = m_currentChannelId;

        populateChannelList(false);

        {
            QSignalBlocker blocker(m_channelList);
            for (int row = 0; row < m_channelList->count(); ++row) {
                if (m_channelList->item(row)->data(IdRole).toString() == keep) {
                    m_channelList->setCurrentRow(row);
                    break;
                }
            }
        }

        // Last, because selecting a row scrolls to it on its own.
        m_channelList->verticalScrollBar()->setValue(scroll);
    });

    // Server icons arrive after the rail is already on screen.
    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &url) {
        if (url.path().startsWith(QLatin1String("/icons/"))) {
            refreshGuildIcons();
            return;
        }

        if (url.path().startsWith(QLatin1String("/avatars/"))
            || url.path().startsWith(QLatin1String("/embed/avatars/"))) {
            updateUserPanel();

            // A late avatar belongs in whichever list is on screen: the direct
            // messages, or the people sitting under a voice channel. This used
            // to run only in the direct message list, which is why a face in a
            // server stayed a grey circle until something else forced a
            // redraw. The timer already knows how to refresh either one.
            if (!m_voiceRefreshTimer.isActive())
                m_voiceRefreshTimer.start(400);
        }
    });
}

// ---------------------------------------------------------------------------
// Building the window
// ---------------------------------------------------------------------------

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    rootLayout->addWidget(buildGuildRail(central));
    rootLayout->addWidget(buildSidebar(central));

    // The right hand side is either a conversation or the friends list.
    m_chatStack = new QStackedWidget(central);
    m_chatPage = buildChatColumn(m_chatStack);
    m_chatStack->addWidget(m_chatPage);

    m_friends = new FriendsPage(m_store, m_rest, m_chatStack);
    connect(m_friends, &FriendsPage::openDirectMessage, this, [this](const QString &channelId) {
        selectChannelEverywhere(channelId);
    });
    connect(m_friends, &FriendsPage::openProfile, this, [this](const QString &userId) {
        showProfile(userId, QCursor::pos());
    });
    m_chatStack->addWidget(m_friends);

    rootLayout->addWidget(m_chatStack, 1);

    setCentralWidget(central);

    m_statusDot = new QLabel(QStringLiteral("offline"), this);
    m_statusDot->setStyleSheet(QStringLiteral("color: %1; padding-right: 10px;").arg(QLatin1String(Theme::TextFaint)));
    statusBar()->addPermanentWidget(m_statusDot);
}

QWidget *MainWindow::buildGuildRail(QWidget *parent)
{
    auto *rail = new QWidget(parent);
    rail->setObjectName(QStringLiteral("GuildRail"));
    rail->setFixedWidth(RailWidth);

    auto *layout = new QVBoxLayout(rail);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(0);

    m_guildRail = new QListWidget(rail);
    m_guildRail->setIconSize(QSize(GuildIconPixels, GuildIconPixels));
    m_guildRail->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_guildRail->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_guildRail->setUniformItemSizes(true);
    // The delegate draws the rows itself, so the stylesheet must not.
    m_guildRail->setItemDelegate(new GuildRailDelegate(m_guildRail, m_guildRail));

    // Drag a tile to reorder it. The arrangement is remembered on this machine.
    m_guildRail->setDragDropMode(QAbstractItemView::InternalMove);
    m_guildRail->setDefaultDropAction(Qt::MoveAction);
    m_guildRail->setContextMenuPolicy(Qt::CustomContextMenu);

    layout->addWidget(m_guildRail, 1);

    connect(m_guildRail, &QListWidget::currentRowChanged, this, &MainWindow::onGuildSelected);
    connect(m_guildRail, &QListWidget::customContextMenuRequested, this, &MainWindow::showRailMenu);

    // After a drag, read the tiles back and save the new order.
    connect(m_guildRail->model(), &QAbstractItemModel::rowsMoved, this, [this]() {
        if (m_rebuildingRail)
            return;
        saveRailOrderFromView();
    });

    m_rail.load();
    return rail;
}

QWidget *MainWindow::buildSidebar(QWidget *parent)
{
    auto *sidebar = new QWidget(parent);
    sidebar->setObjectName(QStringLiteral("Sidebar"));
    sidebar->setFixedWidth(SidebarWidth);

    auto *layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_sidebarHeader = new QLabel(QStringLiteral("Direct messages"), sidebar);
    m_sidebarHeader->setObjectName(QStringLiteral("SidebarHeader"));
    m_sidebarHeader->setWordWrap(false);
    layout->addWidget(m_sidebarHeader);

    m_channelList = new QListWidget(sidebar);
    m_channelList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_channelDelegate = new ChannelDelegate(m_channelList, m_channelList);
    m_channelList->setItemDelegate(m_channelDelegate);
    layout->addWidget(m_channelList, 1);

    connect(m_channelDelegate, &ChannelDelegate::joinVoiceRequested, this, &MainWindow::joinVoice);
    connect(m_channelDelegate, &ChannelDelegate::leaveVoiceRequested, this,
            [this](const QString &) { leaveVoice(); });
    connect(m_channelDelegate, &ChannelDelegate::openChatRequested, this,
            [this](const QString &channelId) { selectChannelEverywhere(channelId); });
    connect(m_channelDelegate, &ChannelDelegate::inviteRequested, this, &MainWindow::createInvite);
    connect(m_channelDelegate, &ChannelDelegate::channelSettingsRequested, this,
            &MainWindow::showChannelSettings);

    m_voicePanel = buildVoicePanel(sidebar);
    layout->addWidget(m_voicePanel);

    m_userPanel = buildUserPanel(sidebar);
    layout->addWidget(m_userPanel);

    connect(m_channelList, &QListWidget::currentRowChanged, this, &MainWindow::onChannelSelected);

    // People under a voice channel are not selectable, so they need their own
    // click handler to open a profile.
    connect(m_channelList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (item && item->data(KindRole).toString() == QLatin1String("voicemember"))
            showProfile(item->data(IdRole).toString(), QCursor::pos());
    });

    return sidebar;
}

QWidget *MainWindow::buildUserPanel(QWidget *parent)
{
    auto *panel = new QWidget(parent);
    panel->setObjectName(QStringLiteral("UserPanel"));
    panel->setFixedHeight(56);
    panel->setCursor(Qt::PointingHandCursor);
    panel->setToolTip(QStringLiteral("Your profile"));
    panel->installEventFilter(this);

    auto *layout = new QHBoxLayout(panel);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(10);

    m_selfAvatar = new QLabel(panel);
    m_selfAvatar->setFixedSize(32, 32);
    layout->addWidget(m_selfAvatar);

    auto *text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(0);

    m_selfName = new QLabel(QStringLiteral("Signing in..."), panel);
    m_selfName->setObjectName(QStringLiteral("SelfName"));
    text->addWidget(m_selfName);

    m_selfStatus = new QLabel(QStringLiteral("connecting"), panel);
    m_selfStatus->setObjectName(QStringLiteral("SelfStatus"));
    text->addWidget(m_selfStatus);

    layout->addLayout(text, 1);
    return panel;
}

QWidget *MainWindow::buildVoicePanel(QWidget *parent)
{
    auto *panel = new QFrame(parent);
    panel->setObjectName(QStringLiteral("VoicePanel"));
    panel->setVisible(false);
    panel->setFixedHeight(82);

    // Two rows, because three buttons and a channel name do not fit across a
    // 240 pixel sidebar.
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 8, 12, 10);
    layout->setSpacing(6);

    auto *text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(0);

    m_voiceState = new QLabel(QStringLiteral("Connecting..."), panel);
    m_voiceState->setObjectName(QStringLiteral("VoiceHeading"));
    text->addWidget(m_voiceState);

    m_voiceChannelLabel = new QLabel(panel);
    m_voiceChannelLabel->setObjectName(QStringLiteral("VoiceChannel"));
    // Ignored width lets a long server name shrink instead of shoving the
    // button off the edge of the sidebar.
    m_voiceChannelLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    text->addWidget(m_voiceChannelLabel);

    layout->addLayout(text);

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(6);

    m_muteButton = new QPushButton(QStringLiteral("Mute"), panel);
    m_muteButton->setMinimumWidth(56);
    m_muteButton->setFixedHeight(26);
    m_muteButton->setCheckable(true);
    m_muteButton->setToolTip(QStringLiteral("Stop sending your voice"));
    connect(m_muteButton, &QPushButton::toggled, this, [this](bool on) {
        m_voice->setMuted(on);
        m_muteButton->setText(on ? QStringLiteral("Muted") : QStringLiteral("Mute"));
        // Tell the server too, so other people see the crossed out microphone.
        m_gateway->joinVoice(m_voiceGuildId, m_voiceChannelId, on, m_deafenButton->isChecked());
    });
    buttons->addWidget(m_muteButton);

    m_deafenButton = new QPushButton(QStringLiteral("Deafen"), panel);
    m_deafenButton->setMinimumWidth(62);
    m_deafenButton->setFixedHeight(26);
    m_deafenButton->setCheckable(true);
    m_deafenButton->setToolTip(QStringLiteral("Stop hearing everyone"));
    connect(m_deafenButton, &QPushButton::toggled, this, [this](bool on) {
        m_voice->setDeafened(on);
        m_deafenButton->setText(on ? QStringLiteral("Deaf") : QStringLiteral("Deafen"));
        m_gateway->joinVoice(m_voiceGuildId, m_voiceChannelId, m_muteButton->isChecked(), on);
    });
    buttons->addWidget(m_deafenButton);

    auto *leave = new QPushButton(QStringLiteral("Leave"), panel);
    leave->setMinimumWidth(56);
    leave->setFixedHeight(26);
    connect(leave, &QPushButton::clicked, this, &MainWindow::leaveVoice);
    buttons->addWidget(leave);

    buttons->addStretch(1);
    layout->addLayout(buttons);

    return panel;
}

QWidget *MainWindow::buildChatColumn(QWidget *parent)
{
    auto *chat = new QWidget(parent);
    auto *layout = new QVBoxLayout(chat);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new QWidget(chat);
    header->setObjectName(QStringLiteral("ChatHeader"));
    header->setFixedHeight(56);
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(20, 8, 20, 8);
    headerLayout->setSpacing(1);

    m_channelTitle = new QLabel(QStringLiteral("Pick a channel"), header);
    m_channelTitle->setObjectName(QStringLiteral("ChannelTitle"));
    headerLayout->addWidget(m_channelTitle);

    m_channelTopic = new QLabel(QString(), header);
    m_channelTopic->setObjectName(QStringLiteral("ChannelTopic"));
    m_channelTopic->setVisible(false);
    headerLayout->addWidget(m_channelTopic);
    layout->addWidget(header);

    m_messageView = new ChatView(chat);
    m_messageView->document()->setDefaultStyleSheet(Theme::messageViewCss(
        AppConfig::instance().value(QStringLiteral("appearance/fontSize"), 14).toInt()));
    m_messageView->document()->setDocumentMargin(0);
    m_messageView->setAnimationsEnabled(
        AppConfig::instance().value(QStringLiteral("appearance/playAnimations"), true).toBool());
    // Links are handled here so profile links can be told apart from web ones.
    m_messageView->setOpenLinks(false);
    m_messageView->setOpenExternalLinks(false);
    connect(m_messageView, &ChatView::anchorClicked, this, &MainWindow::handleAnchor);
    layout->addWidget(m_messageView, 1);

    m_typingLabel = new QLabel(QString(), chat);
    m_typingLabel->setObjectName(QStringLiteral("TypingLabel"));
    m_typingLabel->setFixedHeight(18);
    layout->addWidget(m_typingLabel);

    auto *composerWrap = new QWidget(chat);
    composerWrap->setObjectName(QStringLiteral("Composer"));
    auto *composerLayout = new QVBoxLayout(composerWrap);
    composerLayout->setContentsMargins(20, 0, 20, 16);

    auto *composerBox = new QFrame(composerWrap);
    composerBox->setObjectName(QStringLiteral("ComposerBox"));
    auto *boxLayout = new QVBoxLayout(composerBox);
    boxLayout->setContentsMargins(0, 0, 0, 0);

    m_composer = new QTextEdit(composerBox);
    m_composer->setObjectName(QStringLiteral("MessageInput"));
    m_composer->setPlaceholderText(QStringLiteral("Write a message"));
    m_composer->setFixedHeight(48);
    m_composer->installEventFilter(this);
    m_composer->setEnabled(false);
    boxLayout->addWidget(m_composer);

    composerLayout->addWidget(composerBox);
    layout->addWidget(composerWrap);

    connect(m_composer, &QTextEdit::textChanged, this, &MainWindow::onComposerChanged);
    return chat;
}

void MainWindow::buildMenu()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&Wisp"));

    auto *settingsAction = fileMenu->addAction(QStringLiteral("Settings..."));
    settingsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::openSettings);

    auto *pluginsAction = fileMenu->addAction(QStringLiteral("Plugins..."));
    pluginsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+P")));
    connect(pluginsAction, &QAction::triggered, this, &MainWindow::openPlugins);

    auto *logAction = fileMenu->addAction(QStringLiteral("Log..."));
    logAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    connect(logAction, &QAction::triggered, this, &MainWindow::openLog);

    fileMenu->addSeparator();

    auto *logOutAction = fileMenu->addAction(QStringLiteral("Log out"));
    connect(logOutAction, &QAction::triggered, this, &MainWindow::logOut);

    auto *quitAction = fileMenu->addAction(QStringLiteral("Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);
}

void MainWindow::startSession(const QString &token)
{
    m_gateway->start(token);
    statusBar()->showMessage(QStringLiteral("Connecting..."));
}

// ---------------------------------------------------------------------------
// Gateway
// ---------------------------------------------------------------------------

void MainWindow::onGatewayState(GatewayClient::State state)
{
    QString text;
    QString colour = Theme::TextFaint;
    switch (state) {
    case GatewayClient::State::Disconnected: text = QStringLiteral("offline"); break;
    case GatewayClient::State::Connecting:   text = QStringLiteral("connecting"); colour = Theme::Highlight; break;
    case GatewayClient::State::Identifying:  text = QStringLiteral("signing in"); colour = Theme::Highlight; break;
    case GatewayClient::State::Ready:        text = QStringLiteral("online"); colour = Theme::Green; break;
    case GatewayClient::State::Reconnecting: text = QStringLiteral("reconnecting"); colour = Theme::Accent; break;
    }

    m_statusDot->setText(text);
    m_statusDot->setStyleSheet(QStringLiteral("color: %1; padding-right: 10px;").arg(colour));

    if (m_selfStatus) {
        m_selfStatus->setText(text);
        m_selfStatus->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(colour));
    }
}

void MainWindow::onGatewayReady(const QJsonObject &payload)
{
    const QJsonObject user = payload.value(QStringLiteral("user")).toObject();
    m_selfUserId = user.value(QStringLiteral("id")).toString();
    m_selfAvatarHash = user.value(QStringLiteral("avatar")).toString();
    m_selfDisplayName = user.value(QStringLiteral("global_name")).toString();
    if (m_selfDisplayName.isEmpty())
        m_selfDisplayName = user.value(QStringLiteral("username")).toString();

    // A new session makes any voice connection built on the old one invalid.
    // Dropping it here avoids a call that looks joined but carries nothing.
    if (!m_voiceChannelId.isEmpty()) {
        wlog(QStringLiteral("voice"), QStringLiteral("signed in again, so the call must be rejoined"));
        m_voice->disconnectFromVoice();
        m_pendingVoiceToken.clear();
        m_pendingVoiceEndpoint.clear();
        m_voiceSessionId.clear();
        m_voiceChannelId.clear();
        m_voiceGuildId.clear();
        updateVoicePanel();
    }

    // A reconnect rebuilds everything. Without this you get dropped back to
    // direct messages every time the connection hiccups.
    const QString wasInGuild = m_currentGuildId;
    const QString wasInChannel = m_currentChannelId;

    m_store->ingestReady(payload);
    populateGuildRail();
    updateUserPanel();

    if (!wasInGuild.isEmpty()) {
        for (int row = 0; row < m_guildRail->count(); ++row) {
            if (m_guildRail->item(row)->data(IdRole).toString() != wasInGuild)
                continue;
            m_guildRail->setCurrentRow(row);
            if (!wasInChannel.isEmpty())
                selectChannelEverywhere(wasInChannel);
            break;
        }
    }

    wlog(QStringLiteral("ui"), QStringLiteral("rail built: %1 servers, %2 direct chats")
                                   .arg(m_store->guilds().size())
                                   .arg(m_store->directChannels().size()));
    statusBar()->showMessage(QStringLiteral("Connected."), 4000);
}

void MainWindow::onGatewayDispatch(const QString &eventType, const QJsonObject &data)
{
    // Plugins see every event before the client reacts to it.
    m_plugins->dispatchGatewayEvent(eventType, data);

    if (eventType == QLatin1String("MESSAGE_CREATE")) {
        m_store->appendMessage(data);
        const QString channelId = data.value(QStringLiteral("channel_id")).toString();
        if (channelId == m_currentChannelId && m_store->hasHistory(channelId)) {
            const MessageInfo message = MessageStore::parseMessage(data);
            const bool grouped = m_hasLastRendered && shouldGroup(m_lastRendered, message);
            appendMessageToView(message, grouped);
            m_lastRendered = message;
            m_hasLastRendered = true;
        }
        return;
    }

    if (eventType == QLatin1String("CHANNEL_CREATE")) {
        m_store->ingestChannelObject(data);
        // A new direct chat should appear in the sidebar at once.
        if (m_currentGuildId.isEmpty() && data.value(QStringLiteral("guild_id")).toString().isEmpty()) {
            const int scroll = m_channelList->verticalScrollBar()->value();
            const QString keep = m_currentChannelId;

            populateChannelList(false);

            // Put the selection back without reopening the channel.
            {
                QSignalBlocker blocker(m_channelList);
                for (int row = 0; row < m_channelList->count(); ++row) {
                    if (m_channelList->item(row)->data(IdRole).toString() == keep) {
                        m_channelList->setCurrentRow(row);
                        break;
                    }
                }
            }
            m_channelList->verticalScrollBar()->setValue(scroll);
        }
        return;
    }

    if (eventType == QLatin1String("GUILD_MEMBER_LIST_UPDATE")) {
        m_store->ingestMemberListUpdate(data);
        return;
    }

    if (eventType == QLatin1String("PRESENCE_UPDATE")) {
        const QJsonObject user = data.value(QStringLiteral("user")).toObject();
        const QString userId = user.value(QStringLiteral("id")).toString();
        m_store->rememberUser(user);
        m_store->setPresence(userId, data);
        return;
    }

    if (eventType == QLatin1String("READY_SUPPLEMENTAL")) {
        // Voice states and presences for other people live here, not in READY.
        m_store->ingestReadySupplemental(data);
        return;
    }

    if (eventType == QLatin1String("VOICE_STATE_UPDATE")) {
        m_store->setVoiceState(data);

        // While a join is still outstanding, write down anyone arriving in the
        // channel we are aiming for. If our own name appears here but the
        // branch below stays quiet, the fault is in matching our own id, not
        // in Discord's reply.
        if (m_voiceWatchdog.isActive()
            && data.value(QStringLiteral("channel_id")).toString() == m_voiceChannelId) {
            wlog(QStringLiteral("voice"),
                 QStringLiteral("someone entered the target channel: user %1 (we are %2)")
                     .arg(data.value(QStringLiteral("user_id")).toString(), m_selfUserId));
        }

        // Discord has the final say on where we are sitting. Trusting the
        // event rather than our own button press means the panel cannot get
        // stuck, even if we joined or left from another device.
        const QString userId = data.value(QStringLiteral("user_id")).toString();
        if (!userId.isEmpty() && userId == m_selfUserId) {
            const QString nowIn = data.value(QStringLiteral("channel_id")).toString();
            wlog(QStringLiteral("voice"), QStringLiteral("own state: %1")
                                              .arg(nowIn.isEmpty() ? QStringLiteral("(left)") : nowIn));

            // This is the half of the handshake that carries the session id.
            // Only keep it while we are actually in a channel, so it can never
            // be paired with a later call's server details.
            m_voiceSessionId = nowIn.isEmpty()
                ? QString()
                : data.value(QStringLiteral("session_id")).toString();

            if (nowIn != m_voiceChannelId) {
                m_voiceChannelId = nowIn;
                updateVoicePanel();
            }
            if (nowIn.isEmpty())
                m_voice->disconnectFromVoice();
            else
                tryStartVoice();
        }
        return;
    }

    if (eventType == QLatin1String("VOICE_SERVER_UPDATE")) {
        // The other half: which server to talk to, and the password for it.
        m_pendingVoiceToken = data.value(QStringLiteral("token")).toString();
        m_pendingVoiceEndpoint = data.value(QStringLiteral("endpoint")).toString();
        wlog(QStringLiteral("voice"), QStringLiteral("server update: %1").arg(m_pendingVoiceEndpoint));
        tryStartVoice();
        return;
    }

    if (eventType == QLatin1String("RELATIONSHIP_ADD")) {
        m_store->rememberUser(data.value(QStringLiteral("user")).toObject());
        m_store->setRelationship(data.value(QStringLiteral("id")).toString(),
                                 data.value(QStringLiteral("type")).toInt());
        return;
    }

    if (eventType == QLatin1String("RELATIONSHIP_REMOVE")) {
        m_store->setRelationship(data.value(QStringLiteral("id")).toString(), 0);
        return;
    }

    if (eventType == QLatin1String("MESSAGE_UPDATE")) {
        m_store->updateMessage(data);
        return;
    }

    if (eventType == QLatin1String("MESSAGE_DELETE")) {
        const QString channelId = data.value(QStringLiteral("channel_id")).toString();
        const QString messageId = data.value(QStringLiteral("id")).toString();
        if (m_plugins->runBeforeMessageDelete(channelId, messageId))
            m_store->removeMessage(channelId, messageId);
        else
            m_store->markDeleted(channelId, messageId);
        return;
    }

    if (eventType == QLatin1String("TYPING_START")) {
        const QString channelId = data.value(QStringLiteral("channel_id")).toString();
        if (channelId != m_currentChannelId)
            return;
        const QString userId = data.value(QStringLiteral("user_id")).toString();
        if (userId == m_selfUserId)
            return;

        QString name = m_store->userName(userId);
        if (name.isEmpty()) {
            const QJsonObject member = data.value(QStringLiteral("member")).toObject();
            const QJsonObject user = member.value(QStringLiteral("user")).toObject();
            m_store->rememberUser(user);
            name = m_store->userName(userId);
        }
        if (name.isEmpty())
            name = QStringLiteral("Someone");

        setTypingHint(QStringLiteral("%1 is typing...").arg(name));
        m_typingClearTimer.start(9000);
    }
}

// ---------------------------------------------------------------------------
// Server rail
// ---------------------------------------------------------------------------

void MainWindow::populateGuildRail()
{
    const QString keepSelected = m_currentGuildId;

    // Work out what the arrangement should be, dropping servers we have left
    // and adding ones we have joined.
    QStringList knownIds;
    const QList<GuildInfo> guilds = m_store->guilds();
    for (const GuildInfo &guild : guilds)
        knownIds.append(guild.id);

    m_rail.reconcile(knownIds);
    m_rail.save();

    m_guildRail->blockSignals(true);
    m_guildRail->clear();

    // Direct messages always sit at the top and cannot be moved.
    auto *dmItem = new QListWidgetItem;
    dmItem->setData(IdRole, QString());
    dmItem->setData(KindRole, QStringLiteral("guild"));
    dmItem->setToolTip(QStringLiteral("Direct messages"));
    dmItem->setIcon(MediaCache::initialsAvatar(QStringLiteral("DM"), GuildIconPixels));
    dmItem->setFlags(dmItem->flags() & ~Qt::ItemIsDragEnabled);
    m_guildRail->addItem(dmItem);

    const auto addGuildTile = [this](const QString &guildId, const QString &folderId) {
        const GuildInfo guild = m_store->guild(guildId);
        const QString name = guild.name.isEmpty() ? QStringLiteral("Server") : guild.name;

        auto *item = new QListWidgetItem;
        item->setData(IdRole, guildId);
        item->setData(KindRole, QStringLiteral("guild"));
        item->setData(FolderRole, folderId);
        item->setToolTip(name);
        item->setIcon(MediaCache::initialsAvatar(name, GuildIconPixels));
        m_guildRail->addItem(item);
    };

    const QList<RailLayout::Entry> entries = m_rail.entries();
    for (const RailLayout::Entry &entry : entries) {
        if (!entry.isFolder) {
            addGuildTile(entry.guildId, QString());
            continue;
        }

        auto *item = new QListWidgetItem;
        item->setData(IdRole, entry.folder.id);
        item->setData(KindRole, QStringLiteral("folder"));
        item->setData(FolderOpenRole, entry.folder.open);
        item->setToolTip(QStringLiteral("%1 (%2 servers)")
                             .arg(entry.folder.name)
                             .arg(entry.folder.guildIds.size()));
        m_guildRail->addItem(item);

        if (!entry.folder.open)
            continue;

        for (const QString &guildId : entry.folder.guildIds)
            addGuildTile(guildId, entry.folder.id);
    }

    m_guildRail->blockSignals(false);
    refreshGuildIcons();

    // Stay where we were if that server is still on screen.
    int wanted = 0;
    if (!keepSelected.isEmpty()) {
        for (int row = 0; row < m_guildRail->count(); ++row) {
            if (m_guildRail->item(row)->data(IdRole).toString() == keepSelected
                && m_guildRail->item(row)->data(KindRole).toString() == QLatin1String("guild")) {
                wanted = row;
                break;
            }
        }
    }
    m_guildRail->setCurrentRow(wanted);
}

void MainWindow::saveRailOrderFromView()
{
    // Read the tiles back in the order they now sit, and rebuild the saved
    // arrangement from that. A server dragged under an open folder joins it,
    // which is the behaviour people expect from dragging.
    QList<RailLayout::Entry> rebuilt;
    QString currentFolder;

    for (int row = 0; row < m_guildRail->count(); ++row) {
        QListWidgetItem *item = m_guildRail->item(row);
        const QString kind = item->data(KindRole).toString();
        const QString id = item->data(IdRole).toString();

        if (kind == QLatin1String("folder")) {
            const RailLayout::Folder *existing = m_rail.folder(id);
            RailLayout::Entry entry;
            entry.isFolder = true;
            entry.folder.id = id;
            entry.folder.name = existing ? existing->name : QStringLiteral("Folder");
            entry.folder.open = existing ? existing->open : true;
            rebuilt.append(entry);
            currentFolder = entry.folder.open ? id : QString();
            continue;
        }

        if (id.isEmpty())
            continue;   // direct messages

        // Only an open folder can swallow the tiles drawn under it.
        if (!currentFolder.isEmpty() && !rebuilt.isEmpty() && rebuilt.last().isFolder
            && rebuilt.last().folder.id == currentFolder) {
            rebuilt.last().folder.guildIds.append(id);
            continue;
        }

        currentFolder.clear();
        RailLayout::Entry entry;
        entry.guildId = id;
        rebuilt.append(entry);
    }

    m_rail.setEntries(rebuilt);
    m_rail.save();

    // Redraw so folder membership shows correctly straight away.
    m_rebuildingRail = true;
    populateGuildRail();
    m_rebuildingRail = false;
}

void MainWindow::showRailMenu(const QPoint &where)
{
    QListWidgetItem *item = m_guildRail->itemAt(where);
    if (!item)
        return;

    const QString kind = item->data(KindRole).toString();
    const QString id = item->data(IdRole).toString();
    if (id.isEmpty())
        return;   // the direct messages tile

    QMenu menu(this);

    if (kind == QLatin1String("folder")) {
        QAction *rename = menu.addAction(QStringLiteral("Rename folder..."));
        QAction *dissolve = menu.addAction(QStringLiteral("Remove folder (keep servers)"));

        QAction *chosen = menu.exec(m_guildRail->mapToGlobal(where));
        if (chosen == rename) {
            const RailLayout::Folder *folder = m_rail.folder(id);
            bool ok = false;
            const QString name = QInputDialog::getText(this, QStringLiteral("Rename folder"),
                                                       QStringLiteral("Folder name"), QLineEdit::Normal,
                                                       folder ? folder->name : QString(), &ok);
            if (ok)
                m_rail.renameFolder(id, name);
        } else if (chosen == dissolve) {
            m_rail.dissolveFolder(id);
        } else {
            return;
        }

        populateGuildRail();
        return;
    }

    // A server tile.
    const QString inFolder = item->data(FolderRole).toString();

    QAction *newFolder = menu.addAction(QStringLiteral("New folder with this server..."));

    QMenu *moveTo = nullptr;
    QHash<QAction *, QString> moveTargets;
    const QList<RailLayout::Entry> entries = m_rail.entries();
    for (const RailLayout::Entry &entry : entries) {
        if (!entry.isFolder || entry.folder.id == inFolder)
            continue;
        if (!moveTo)
            moveTo = menu.addMenu(QStringLiteral("Move to folder"));
        moveTargets.insert(moveTo->addAction(entry.folder.name), entry.folder.id);
    }

    QAction *takeOut = nullptr;
    if (!inFolder.isEmpty())
        takeOut = menu.addAction(QStringLiteral("Take out of folder"));

    QAction *chosen = menu.exec(m_guildRail->mapToGlobal(where));
    if (!chosen)
        return;

    if (chosen == newFolder) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("New folder"),
                                                   QStringLiteral("Folder name"), QLineEdit::Normal,
                                                   QStringLiteral("Folder"), &ok);
        if (!ok)
            return;
        m_rail.createFolder(name, id);
    } else if (chosen == takeOut) {
        m_rail.moveGuildOutOfFolders(id);
    } else if (moveTargets.contains(chosen)) {
        m_rail.moveGuildIntoFolder(id, moveTargets.value(chosen));
    } else {
        return;
    }

    populateGuildRail();
}

void MainWindow::refreshGuildIcons()
{
    for (int row = 0; row < m_guildRail->count(); ++row) {
        QListWidgetItem *item = m_guildRail->item(row);
        const QString guildId = item->data(IdRole).toString();
        if (guildId.isEmpty())
            continue;

        const GuildInfo guild = m_store->guild(guildId);
        const QUrl url = MediaCache::guildIconUrl(guildId, guild.iconHash, 96);
        if (url.isEmpty())
            continue;

        // Asking starts the download. The cache tells us when it lands.
        const QImage picture = MediaCache::instance().image(url);
        if (!picture.isNull())
            item->setIcon(MediaCache::circular(picture, GuildIconPixels));
    }
}

void MainWindow::onGuildSelected(int row)
{
    if (row < 0)
        return;
    QListWidgetItem *item = m_guildRail->item(row);
    if (!item)
        return;

    // A folder tile is not a server: clicking it opens or closes the folder.
    if (item->data(KindRole).toString() == QLatin1String("folder")) {
        const QString folderId = item->data(IdRole).toString();
        const RailLayout::Folder *folder = m_rail.folder(folderId);
        m_rail.setFolderOpen(folderId, folder ? !folder->open : true);
        populateGuildRail();
        return;
    }

    m_currentGuildId = item->data(IdRole).toString();

    if (!m_currentGuildId.isEmpty()) {
        // Discord only sends member statuses when asked for a specific
        // channel's member list, so pick the first readable text channel.
        QString anyTextChannel;
        const QList<ChannelGroup> groups = m_store->groupedChannels(m_currentGuildId);
        for (const ChannelGroup &group : groups) {
            for (const ChannelInfo &channel : group.channels) {
                if (!channel.isVoice()) {
                    anyTextChannel = channel.id;
                    break;
                }
            }
            if (!anyTextChannel.isEmpty())
                break;
        }
        m_gateway->subscribeToGuild(m_currentGuildId, anyTextChannel);
    }

    populateChannelList();
}

// ---------------------------------------------------------------------------
// Channel sidebar
// ---------------------------------------------------------------------------

void MainWindow::refreshDirectRows()
{
    for (int row = 0; row < m_channelList->count(); ++row) {
        QListWidgetItem *item = m_channelList->item(row);
        if (!item || item->data(KindRole).toString() != QLatin1String("dm"))
            continue;

        const ChannelInfo channel = m_store->channel(item->data(IdRole).toString());
        if (channel.type != 1 || channel.recipientIds.size() != 1)
            continue;

        const QString otherId = channel.recipientIds.first();
        const UserInfo info = m_store->user(otherId);
        const PresenceInfo presence = m_store->presence(otherId);

        item->setData(WispRoles::Status, m_store->presenceBubble(otherId));
        item->setData(WispRoles::Subtitle, describePresence(presence, m_store->activityHint(otherId)));

        const QUrl url = MediaCache::avatarUrl(otherId, info.avatarHash, 64);
        const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
        if (!picture.isNull())
            item->setIcon(MediaCache::circular(picture, 32));
    }

    m_channelList->viewport()->update();
}

void MainWindow::populateChannelList(bool autoSelectFirst)
{
    m_sidebarHeader->setText(m_currentGuildId.isEmpty() ? QStringLiteral("Direct messages")
                                                        : m_store->guild(m_currentGuildId).name);

    m_channelList->blockSignals(true);
    m_channelList->clear();

    int firstSelectable = -1;

    // A direct message row carries a picture, an online bubble and a line
    // saying what the person is doing, the way the real client shows it.
    const auto addDirectRow = [&](const ChannelInfo &channel) {
        auto *item = new QListWidgetItem(channel.name);
        item->setData(IdRole, channel.id);
        item->setData(KindRole, QStringLiteral("dm"));

        // A one to one chat follows that person. A group has no single owner.
        const bool oneToOne = channel.type == 1 && channel.recipientIds.size() == 1;
        const QString otherId = oneToOne ? channel.recipientIds.first() : QString();

        QImage picture;
        if (oneToOne) {
            const UserInfo info = m_store->user(otherId);
            const QUrl url = MediaCache::avatarUrl(otherId, info.avatarHash, 64);
            if (!url.isEmpty())
                picture = MediaCache::instance().image(url);

            const PresenceInfo presence = m_store->presence(otherId);
            item->setData(WispRoles::Status, m_store->presenceBubble(otherId));
            item->setData(WispRoles::Subtitle,
                          describePresence(presence, m_store->activityHint(otherId)));
        } else {
            item->setData(WispRoles::Status, QStringLiteral("offline"));
            item->setData(WispRoles::Subtitle,
                          QStringLiteral("%1 people").arg(channel.recipientIds.size() + 1));
        }

        item->setIcon(picture.isNull() ? MediaCache::initialsAvatar(channel.name, 32)
                                       : MediaCache::circular(picture, 32));
        item->setToolTip(channel.name);

        m_channelList->addItem(item);
        if (firstSelectable < 0)
            firstSelectable = m_channelList->count() - 1;
    };

    const auto addChannelRow = [&](const ChannelInfo &channel) {
        // The painter draws the # or the speaker, so the text is the bare name.
        auto *item = new QListWidgetItem(channel.name);
        item->setData(IdRole, channel.id);
        item->setData(KindRole, channel.isVoice() ? QStringLiteral("voice") : QStringLiteral("channel"));
        item->setToolTip(channel.topic.isEmpty() ? channel.name : channel.topic);
        m_channelList->addItem(item);
        if (firstSelectable < 0 && !channel.isVoice())
            firstSelectable = m_channelList->count() - 1;

        if (!channel.isVoice())
            return;

        // Anyone sitting in this voice channel is listed under it.
        const QStringList members = m_store->voiceMembers(channel.id);
        for (const QString &memberId : members) {
            const UserInfo info = m_store->user(memberId);

            // Voice states carry an id and nothing else. Somebody who has not
            // spoken in a channel we have open is a stranger to us, and a bare
            // number is no use to anyone, so ask Discord who they are. Once
            // each: the answer lands in the store and the list redraws itself.
            if (info.displayName().isEmpty())
                requestUnknownName(memberId);

            const QString name = info.displayName().isEmpty() ? memberId : info.displayName();

            auto *memberItem = new QListWidgetItem(name);
            memberItem->setData(IdRole, memberId);
            memberItem->setData(KindRole, QStringLiteral("voicemember"));
            memberItem->setFlags(Qt::ItemIsEnabled);

            const VoiceStateInfo state = m_store->voiceState(memberId);
            memberItem->setData(WispRoles::Streaming, state.streaming);
            memberItem->setData(WispRoles::Video, state.video);
            memberItem->setData(WispRoles::VoiceMuted, state.muted);
            memberItem->setData(WispRoles::VoiceDeafened, state.deafened);

            QStringList marks;
            if (state.streaming)
                marks << QStringLiteral("sharing a screen");
            if (state.video)
                marks << QStringLiteral("camera on");
            if (state.deafened)
                marks << QStringLiteral("cannot hear anyone");
            else if (state.muted)
                marks << QStringLiteral("muted");
            if (state.since.isValid()) {
                marks << QStringLiteral("here %1")
                             .arg(elapsedWords(state.since.secsTo(QDateTime::currentDateTimeUtc())));
            }
            if (!marks.isEmpty())
                memberItem->setToolTip(QStringLiteral("%1 — %2").arg(name, marks.join(QStringLiteral(", "))));

            const QUrl url = MediaCache::avatarUrl(memberId, info.avatarHash, 64);
            const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
            memberItem->setIcon(picture.isNull() ? MediaCache::initialsAvatar(name, 18)
                                                 : MediaCache::circular(picture, 18));

            m_channelList->addItem(memberItem);
        }
    };

    if (m_currentGuildId.isEmpty()) {
        // The Friends row sits above the chats, as in the real client.
        auto *friends = new QListWidgetItem(QStringLiteral("Friends"));
        friends->setData(IdRole, QStringLiteral("wisp:friends"));
        friends->setData(KindRole, QStringLiteral("channel"));
        friends->setToolTip(QStringLiteral("Everyone you are friends with"));
        m_channelList->addItem(friends);
        firstSelectable = m_channelList->count() - 1;

        auto *spacer = new QListWidgetItem(QStringLiteral("Direct messages"));
        spacer->setData(KindRole, QStringLiteral("header"));
        spacer->setFlags(Qt::NoItemFlags);
        m_channelList->addItem(spacer);

        const QList<ChannelInfo> channels = m_store->directChannels();
        for (const ChannelInfo &channel : channels)
            addDirectRow(channel);
    } else {
        const QList<ChannelGroup> groups = m_store->groupedChannels(m_currentGuildId);
        for (const ChannelGroup &group : groups) {
            if (!group.name.isEmpty()) {
                // A heading, not a destination.
                auto *header = new QListWidgetItem(group.name.toUpper());
                header->setData(KindRole, QStringLiteral("header"));
                header->setFlags(Qt::NoItemFlags);
                header->setForeground(QColor(Theme::TextFaint));
                m_channelList->addItem(header);
            }
            for (const ChannelInfo &channel : group.channels)
                addChannelRow(channel);
        }
    }

    m_channelList->blockSignals(false);

    if (!autoSelectFirst)
        return;

    if (firstSelectable >= 0)
        m_channelList->setCurrentRow(firstSelectable);
    else
        openChannel(QString());
}

void MainWindow::onChannelSelected(int row)
{
    if (row < 0)
        return;
    QListWidgetItem *item = m_channelList->item(row);
    if (!item)
        return;

    // Every kind of row that holds messages opens the same way. Voice channels
    // carry their own text chat, and a direct message is just a channel with a
    // person's name on it.
    //
    // Keep this list in step with the kinds set in populateChannelList: a row
    // kind missing here looks exactly like a dead, unclickable list.
    static const QStringList openable{
        QStringLiteral("channel"),
        QStringLiteral("voice"),
        QStringLiteral("dm"),
    };

    const QString kind = item->data(KindRole).toString();
    if (!openable.contains(kind)) {
        // A person under a voice channel is meant to be unopenable: clicking
        // one shows their profile instead, handled elsewhere.
        if (kind != QLatin1String("voicemember"))
            wlog(QStringLiteral("ui"), QStringLiteral("row kind \"%1\" cannot be opened").arg(kind));
        return;
    }

    openChannel(item->data(IdRole).toString());
}

void MainWindow::openChannel(const QString &channelId)
{
    // The Friends row is not a channel, it swaps the whole chat area.
    if (channelId == QLatin1String("wisp:friends")) {
        m_currentChannelId.clear();
        m_friends->refresh();
        m_chatStack->setCurrentWidget(m_friends);
        m_composer->setEnabled(false);
        return;
    }

    m_chatStack->setCurrentWidget(m_chatPage);

    m_currentChannelId = channelId;
    m_hasLastRendered = false;
    setTypingHint(QString());

    if (channelId.isEmpty()) {
        m_channelTitle->setText(QStringLiteral("Pick a channel"));
        m_channelTopic->setVisible(false);
        m_messageView->clear();
        m_composer->setEnabled(false);
        return;
    }

    const ChannelInfo channel = m_store->channel(channelId);
    m_channelTitle->setText(channel.isDirect() ? channel.name : QStringLiteral("# ") + channel.name);
    m_channelTopic->setText(channel.topic);
    m_channelTopic->setVisible(!channel.topic.isEmpty());
    m_composer->setEnabled(true);
    m_composer->setPlaceholderText(
        QStringLiteral("Message %1").arg(channel.isDirect() ? channel.name : QStringLiteral("#") + channel.name));
    m_composer->setFocus();

    if (m_store->hasHistory(channelId)) {
        renderChannel();
        return;
    }

    m_messageView->setHtml(QStringLiteral("<p class=\"system\">Loading messages...</p>"));
    const int limit =
        AppConfig::instance().value(QStringLiteral("appearance/historyLimit"), HistoryLimit).toInt();
    m_rest->fetchMessages(
        channelId, limit,
        [this, channelId](const QJsonArray &messages) { m_store->setHistory(channelId, messages); },
        [this, channelId](const RestClient::Error &error) {
            wlog(QStringLiteral("rest"), QStringLiteral("history failed for %1: HTTP %2 %3")
                                             .arg(channelId).arg(error.httpStatus).arg(error.message));
            if (channelId != m_currentChannelId)
                return;
            QString reason = error.message;
            if (error.httpStatus == 403)
                reason = QStringLiteral("No permission to read this channel.");
            else if (error.isRateLimit())
                reason = QStringLiteral("Rate limited. Try again in a moment.");
            m_messageView->setHtml(QStringLiteral("<p class=\"system\">Could not load messages: %1</p>")
                                       .arg(reason.toHtmlEscaped()));
        });
}

// ---------------------------------------------------------------------------
// Drawing messages
// ---------------------------------------------------------------------------

bool MainWindow::shouldGroup(const MessageInfo &previous, const MessageInfo &current)
{
    if (previous.authorId.isEmpty() || previous.authorId != current.authorId)
        return false;
    if (!previous.timestamp.isValid() || !current.timestamp.isValid())
        return false;

    const int minutes =
        AppConfig::instance().value(QStringLiteral("appearance/groupMinutes"), 7).toInt();
    if (minutes <= 0)
        return false;

    return previous.timestamp.secsTo(current.timestamp) < minutes * 60;
}

void MainWindow::renderChannel()
{
    if (m_currentChannelId.isEmpty())
        return;

    // Redrawing happens for small reasons too: a message edited far above, a
    // plugin toggled, a late picture. Reading half way up the channel should
    // survive all of those, so only jump to the bottom if that is where the
    // view already was.
    QScrollBar *bar = m_messageView->verticalScrollBar();
    const bool wasAtBottom = bar->value() >= bar->maximum() - 8 || bar->maximum() == 0;
    const int previousPosition = bar->value();
    const bool sameChannel = (m_renderedChannelId == m_currentChannelId);

    const QList<MessageInfo> messages = m_store->messages(m_currentChannelId);
    if (messages.isEmpty()) {
        m_messageView->setHtml(QStringLiteral("<p class=\"system\">No messages here yet.</p>"));
        m_hasLastRendered = false;
        m_renderedChannelId = m_currentChannelId;
        return;
    }

    QString html;
    html.reserve(messages.size() * 400);

    MessageInfo previous;
    bool havePrevious = false;
    for (const MessageInfo &message : messages) {
        const bool grouped = havePrevious && shouldGroup(previous, message);
        html += messageHtml(message, grouped);
        previous = message;
        havePrevious = true;
    }

    m_lastRendered = previous;
    m_hasLastRendered = havePrevious;

    m_messageView->setHtml(html);
    m_renderedChannelId = m_currentChannelId;

    // A freshly opened channel always starts at the newest message.
    if (!sameChannel || wasAtBottom)
        bar->setValue(bar->maximum());
    else
        bar->setValue(qMin(previousPosition, bar->maximum()));
}

void MainWindow::appendMessageToView(const MessageInfo &message, bool grouped)
{
    QScrollBar *bar = m_messageView->verticalScrollBar();
    const bool wasAtBottom = bar->value() >= bar->maximum() - 40;

    QTextCursor cursor(m_messageView->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertHtml(messageHtml(message, grouped));

    if (wasAtBottom)
        bar->setValue(bar->maximum());
}

QString MainWindow::messageHtml(const MessageInfo &message, bool grouped)
{
    const QString decorations = m_plugins->runDecorateHeader(message);
    const bool isSelf = !m_selfUserId.isEmpty() && message.authorId == m_selfUserId;

    // Body -----------------------------------------------------------------
    QString body = renderContent(message.content);

    for (const Attachment &attachment : message.attachments) {
        const QString safeUrl = attachment.url.toHtmlEscaped();
        const QString label = attachment.filename.isEmpty() ? QStringLiteral("file")
                                                            : attachment.filename.toHtmlEscaped();

        if (attachment.isImage() && ChatView::isAllowedImageHost(QUrl(attachment.url))) {
            // Wrapped so a click opens the big view rather than a browser.
            body += QStringLiteral("<div class=\"attach\"><a href=\"wisp-image:%1\">"
                                   "<img src=\"%1\"></a></div>")
                        .arg(safeUrl);
        } else {
            body += QStringLiteral("<div class=\"file\"><a href=\"%1\">%2</a>%3</div>")
                        .arg(safeUrl, label, humanSize(attachment.size));
        }
    }

    body += stickersHtml(message);
    body += embedsHtml(message);

    if (body.isEmpty())
        body = QStringLiteral("<span class=\"system\">(no text)</span>");

    const QString bodyClass = message.deleted ? QStringLiteral("body deleted") : QStringLiteral("body");

    // A grouped message has no avatar and no name, only the text, lined up
    // under the block it belongs to. The narrow strip on the left carries the
    // clock time, the way the real client shows it on hover.
    if (grouped) {
        return QStringLiteral(
                   "<table class=\"row\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">"
                   "<tr><td width=\"56\" valign=\"top\" class=\"gut\">%1</td>"
                   "<td valign=\"top\"><div class=\"%2\">%3</div></td></tr></table>")
            .arg(m_plugins->runDecorateGutter(message), bodyClass, body);
    }

    // Header ---------------------------------------------------------------
    const QString displayName = message.authorName.isEmpty() ? QStringLiteral("Unknown")
                                                             : message.authorName.toHtmlEscaped();
    const QString authorClass = isSelf ? QStringLiteral("author author-self") : QStringLiteral("author");

    // Both the picture and the name open the profile card.
    const QString profileLink = QStringLiteral("wisp-user:%1").arg(message.authorId);

    QString avatarCell;
    if (!grouped) {
        const QUrl avatar = MediaCache::avatarUrl(message.authorId, message.authorAvatar, 80);
        if (!avatar.isEmpty()) {
            avatarCell = QStringLiteral("<a href=\"%1\"><img src=\"%2\"></a>")
                             .arg(profileLink, avatar.toString().toHtmlEscaped());
        }
    }

    return QStringLiteral(
               "<table class=\"row\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">"
               "<tr>"
               "<td width=\"56\" valign=\"top\" class=\"ava\">%1</td>"
               "<td valign=\"top\"><div class=\"hdr\">"
               "<a class=\"namelink\" href=\"%2\"><span class=\"%3\">%4</span></a>%5</div>"
               "<div class=\"%6\">%7</div></td>"
               "</tr></table>")
        .arg(avatarCell, profileLink, authorClass, displayName, decorations, bodyClass, body);
}

QString MainWindow::stickersHtml(const MessageInfo &message) const
{
    QString html;
    for (const StickerInfo &sticker : message.stickers) {
        if (!sticker.isDrawable()) {
            // Lottie stickers are vector animations with no Qt reader.
            html += QStringLiteral("<div class=\"file\"><span class=\"system\">sticker: %1</span></div>")
                        .arg(sticker.name.toHtmlEscaped());
            continue;
        }

        const QString ext = sticker.formatType == 4 ? QStringLiteral("gif") : QStringLiteral("png");
        const QString url = QStringLiteral("https://media.discordapp.net/stickers/%1.%2?size=240")
                                .arg(sticker.id, ext);
        html += QStringLiteral("<div class=\"attach\"><a href=\"wisp-image:%1\">"
                               "<img src=\"%1\" alt=\"%2\" title=\"%2\"></a></div>")
                    .arg(url, sticker.name.toHtmlEscaped());
    }
    return html;
}

QString MainWindow::embedsHtml(const MessageInfo &message) const
{
    QString html;

    for (const EmbedInfo &embed : message.embeds) {
        const bool pictureReachable =
            !embed.imageUrl.isEmpty() && ChatView::isAllowedImageHost(QUrl(embed.imageUrl));

        // A Tenor or Giphy link is only a picture, so skip the card frame.
        if (embed.isPictureOnly()) {
            if (!pictureReachable)
                continue;
            html += QStringLiteral("<div class=\"attach\"><a href=\"wisp-image:%1\">"
                                   "<img src=\"%1\"></a></div>")
                        .arg(embed.imageUrl.toHtmlEscaped());
            continue;
        }

        // Everything else is drawn as a card with a coloured edge.
        QString inner;

        if (!embed.providerName.isEmpty()) {
            inner += QStringLiteral("<div class=\"embed-provider\">%1</div>")
                         .arg(embed.providerName.toHtmlEscaped());
        }
        if (!embed.authorName.isEmpty()) {
            inner += QStringLiteral("<div class=\"embed-author\">%1</div>")
                         .arg(embed.authorName.toHtmlEscaped());
        }
        if (!embed.title.isEmpty()) {
            const QString title = embed.title.toHtmlEscaped();
            inner += embed.url.isEmpty()
                ? QStringLiteral("<div class=\"embed-title\">%1</div>").arg(title)
                : QStringLiteral("<div class=\"embed-title\"><a href=\"%1\">%2</a></div>")
                      .arg(embed.url.toHtmlEscaped(), title);
        }
        if (!embed.description.isEmpty()) {
            QString description = embed.description.toHtmlEscaped();
            if (description.size() > 400)
                description = description.left(397) + QStringLiteral("...");
            description.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
            inner += QStringLiteral("<div class=\"embed-body\">%1</div>").arg(description);
        }
        if (pictureReachable) {
            inner += QStringLiteral("<div class=\"attach\"><a href=\"wisp-image:%1\">"
                                    "<img src=\"%1\"></a></div>")
                         .arg(embed.imageUrl.toHtmlEscaped());
        }

        if (inner.isEmpty())
            continue;

        const QColor edge = embed.colour != 0 ? QColor::fromRgb(static_cast<QRgb>(embed.colour))
                                              : QColor(Theme::Highlight);

        // Qt's rich text has no left border on a div, so a one cell table
        // stands in for the coloured strip Discord draws.
        html += QStringLiteral(
                    "<table class=\"embed\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                    "<td width=\"4\" bgcolor=\"%1\"></td>"
                    "<td class=\"embed-inner\">%2</td></tr></table>")
                    .arg(edge.name(), inner);
    }

    return html;
}

QString MainWindow::renderContent(const QString &raw) const
{
    if (raw.isEmpty())
        return {};

    QString text = raw.toHtmlEscaped();

    // Custom emoji: <:name:id> and <a:name:id> become real little pictures.
    // Animated ones are asked for as .gif so they keep moving.
    static const QRegularExpression emojiRe(QStringLiteral("&lt;(a?):([A-Za-z0-9_]+):(\\d+)&gt;"));
    {
        QString rebuilt;
        int last = 0;
        auto it = emojiRe.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const bool animated = match.captured(1) == QLatin1String("a");
            const QString name = match.captured(2);
            const QString id = match.captured(3);

            rebuilt += text.mid(last, match.capturedStart() - last);
            rebuilt += QStringLiteral("<img src=\"https://cdn.discordapp.com/emojis/%1.%2?size=48\" "
                                      "width=\"22\" height=\"22\" alt=\":%3:\" title=\":%3:\">")
                           .arg(id, animated ? QStringLiteral("gif") : QStringLiteral("png"),
                                name.toHtmlEscaped());
            last = match.capturedEnd();
        }
        rebuilt += text.mid(last);
        text = rebuilt;
    }

    static const QRegularExpression roleRe(QStringLiteral("&lt;@&amp;(\\d+)&gt;"));
    text.replace(roleRe, QStringLiteral("<span class=\"mention\">@role</span>"));

    // Channel mentions resolve against the store.
    static const QRegularExpression channelRe(QStringLiteral("&lt;#(\\d+)&gt;"));
    {
        QString rebuilt;
        int last = 0;
        auto it = channelRe.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const ChannelInfo channel = m_store->channel(match.captured(1));
            const QString label = channel.name.isEmpty() ? QStringLiteral("channel") : channel.name;
            rebuilt += text.mid(last, match.capturedStart() - last);
            rebuilt += QStringLiteral("<span class=\"mention\">#%1</span>").arg(label.toHtmlEscaped());
            last = match.capturedEnd();
        }
        rebuilt += text.mid(last);
        text = rebuilt;
    }

    // User mentions use the names learned from READY and from messages.
    static const QRegularExpression userRe(QStringLiteral("&lt;@!?(\\d+)&gt;"));
    {
        QString rebuilt;
        int last = 0;
        auto it = userRe.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const QString name = m_store->userName(match.captured(1));
            rebuilt += text.mid(last, match.capturedStart() - last);
            rebuilt += QStringLiteral("<span class=\"mention\">@%1</span>")
                           .arg((name.isEmpty() ? QStringLiteral("user") : name).toHtmlEscaped());
            last = match.capturedEnd();
        }
        rebuilt += text.mid(last);
        text = rebuilt;
    }

    // Small markdown subset. Code first so its content is left alone.
    static const QRegularExpression codeRe(QStringLiteral("`([^`\\n]+)`"));
    text.replace(codeRe, QStringLiteral("<code>\\1</code>"));
    static const QRegularExpression boldRe(QStringLiteral("\\*\\*([^*\\n]+)\\*\\*"));
    text.replace(boldRe, QStringLiteral("<b>\\1</b>"));
    static const QRegularExpression italicRe(QStringLiteral("(?<![*\\w])\\*([^*\\n]+)\\*(?![*\\w])"));
    text.replace(italicRe, QStringLiteral("<i>\\1</i>"));
    static const QRegularExpression strikeRe(QStringLiteral("~~([^~\\n]+)~~"));
    text.replace(strikeRe, QStringLiteral("<s>\\1</s>"));

    // Plain links become clickable, shortened so one long address cannot
    // stretch the column.
    static const QRegularExpression linkRe(QStringLiteral("(https?://[^\\s<]+)"));
    {
        QString rebuilt;
        int last = 0;
        auto it = linkRe.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const QString url = match.captured(1);
            QString label = url;
            if (label.size() > 64)
                label = label.left(61) + QStringLiteral("...");
            rebuilt += text.mid(last, match.capturedStart() - last);
            rebuilt += QStringLiteral("<a href=\"%1\">%2</a>").arg(url, label);
            last = match.capturedEnd();
        }
        rebuilt += text.mid(last);
        text = rebuilt;
    }

    text.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return text;
}

void MainWindow::updateUserPanel()
{
    if (!m_selfName)
        return;

    m_selfName->setText(m_selfDisplayName.isEmpty() ? QStringLiteral("Signed in") : m_selfDisplayName);

    const QUrl url = MediaCache::avatarUrl(m_selfUserId, m_selfAvatarHash, 80);
    const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);

    if (!picture.isNull())
        m_selfAvatar->setPixmap(MediaCache::circular(picture, 32));
    else
        m_selfAvatar->setPixmap(MediaCache::initialsAvatar(m_selfDisplayName, 32));
}

void MainWindow::setTypingHint(const QString &text)
{
    m_typingLabel->setText(text);
}

// ---------------------------------------------------------------------------
// Profiles and links
// ---------------------------------------------------------------------------

void MainWindow::handleAnchor(const QUrl &url)
{
    const QString whole = url.toString();

    // Our own scheme: open the profile card for that person.
    if (url.scheme() == QLatin1String("wisp-user")) {
        const QString userId = url.path().isEmpty() ? whole.mid(10) : url.path();
        showProfile(userId, QCursor::pos());
        return;
    }

    // A picture in chat opens the big view instead of a browser. The address
    // is taken from the raw text, because a full web address inside another
    // address does not survive being parsed into parts.
    if (url.scheme() == QLatin1String("wisp-image")) {
        const QString target = whole.mid(QStringLiteral("wisp-image:").size());
        if (target.isEmpty())
            return;

        if (!m_imageViewer)
            m_imageViewer = new ImageViewer(this);
        m_imageViewer->showImage(QUrl(target));
        return;
    }

    // Anything else is a real web link, opened in the normal browser.
    if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
        QDesktopServices::openUrl(url);
}

void MainWindow::showProfile(const QString &userId, const QPoint &globalPos)
{
    if (userId.isEmpty())
        return;

    Q_UNUSED(globalPos)

    if (!m_profileDialog) {
        m_profileDialog = new ProfileDialog(m_store, m_rest, this);
        connect(m_profileDialog, &ProfileDialog::openDirectMessage, this, [this](const QString &channelId) {
            selectChannelEverywhere(channelId);
        });
    }

    // The server matters: roles, nickname and the join date come from it.
    m_profileDialog->showFor(userId, m_currentGuildId, userId == m_selfUserId);
}

void MainWindow::selectChannelEverywhere(const QString &channelId)
{
    if (channelId.isEmpty())
        return;

    const ChannelInfo channel = m_store->channel(channelId);

    // Jump to the right server first, so the sidebar holds the channel.
    const QString wantedGuild = channel.guildId;
    if (wantedGuild != m_currentGuildId) {
        for (int row = 0; row < m_guildRail->count(); ++row) {
            if (m_guildRail->item(row)->data(IdRole).toString() == wantedGuild) {
                m_guildRail->setCurrentRow(row);
                break;
            }
        }
    }

    for (int row = 0; row < m_channelList->count(); ++row) {
        if (m_channelList->item(row)->data(IdRole).toString() == channelId) {
            m_channelList->setCurrentRow(row);
            return;
        }
    }

    // The sidebar has not caught up yet, so open it directly.
    openChannel(channelId);
}

// ---------------------------------------------------------------------------
// Composer
// ---------------------------------------------------------------------------

void MainWindow::onComposerChanged()
{
    if (m_currentChannelId.isEmpty() || m_composer->toPlainText().trimmed().isEmpty())
        return;

    if (m_typingSentTimer.isValid() && m_typingSentTimer.elapsed() < TypingIntervalMs)
        return;
    m_typingSentTimer.restart();

    if (m_plugins->runBeforeTyping(m_currentChannelId))
        m_rest->sendTyping(m_currentChannelId);
}

void MainWindow::sendCurrentMessage()
{
    if (m_currentChannelId.isEmpty())
        return;

    QString content = m_composer->toPlainText();
    if (content.trimmed().isEmpty())
        return;

    if (!m_plugins->runOutgoingMessage(content, m_currentChannelId)) {
        statusBar()->showMessage(QStringLiteral("A plugin cancelled that message."), 4000);
        m_composer->clear();
        return;
    }

    if (content.length() > 2000) {
        statusBar()->showMessage(QStringLiteral("Discord caps messages at 2000 characters."), 5000);
        return;
    }

    const QString channelId = m_currentChannelId;
    m_composer->clear();

    m_rest->sendMessage(
        channelId, content,
        [](const QJsonObject &) {
            // The gateway echoes the message back, so nothing to do here.
        },
        [this, content](const RestClient::Error &error) {
            QString reason = error.message;
            if (error.isRateLimit())
                reason = QStringLiteral("rate limited");
            statusBar()->showMessage(
                QStringLiteral("Send failed (%1). Your text: %2").arg(reason, content.left(60)), 8000);
        });
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Clicking your own panel opens your own profile card.
    if (watched == m_userPanel && event->type() == QEvent::MouseButtonRelease) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton && !m_selfUserId.isEmpty()) {
            const QPoint corner = m_userPanel->mapToGlobal(QPoint(m_userPanel->width() + 6, -200));
            showProfile(m_selfUserId, corner);
            return true;
        }
    }

    if (watched == m_composer && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const bool isEnter = keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter;
        if (isEnter && !(keyEvent->modifiers() & Qt::ShiftModifier)) {
            sendCurrentMessage();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// Menu actions
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------

// Asks Discord who somebody is, at most once per person per run.
//
// A voice state names only an id. Without this a channel full of people you
// have never spoken to is a list of eighteen digit numbers, which is what it
// was. The reply goes into the store, the store tells the window, and the row
// redraws with a name and a face.
void MainWindow::requestUnknownName(const QString &userId)
{
    if (userId.isEmpty() || m_namesRequested.contains(userId) || !m_rest)
        return;
    m_namesRequested.insert(userId);

    m_rest->fetchUser(
        userId,
        [this](const QJsonObject &user) {
            m_store->rememberUser(user);
            if (!m_voiceRefreshTimer.isActive())
                m_voiceRefreshTimer.start(400);
        },
        [userId](const RestClient::Error &error) {
            // Deleted accounts and the like. The number stays, which is honest.
            wlog(QStringLiteral("ui"), QStringLiteral("could not look up user %1: HTTP %2")
                                           .arg(userId)
                                           .arg(error.httpStatus));
        });
}

void MainWindow::joinVoice(const QString &channelId)
{
    const ChannelInfo channel = m_store->channel(channelId);
    if (!channel.isVoice())
        return;

    AppConfig &config = AppConfig::instance();
    const bool muted = config.value(QStringLiteral("voice/joinMuted"), false).toBool();
    const bool deafened = config.value(QStringLiteral("voice/joinDeafened"), false).toBool();

    // Anything left from a previous call is stale the moment a new one starts.
    // Pairing a fresh voice server with an old session is what Discord refuses
    // with "session no longer valid".
    m_voice->disconnectFromVoice();
    m_pendingVoiceToken.clear();
    m_pendingVoiceEndpoint.clear();
    m_voiceSessionId.clear();
    m_speakingUsers.clear();

    m_voiceRetries = 0;
    m_voiceRetryTimer.stop();

    m_voiceChannelId = channelId;
    m_voiceGuildId = channel.guildId;
    m_gateway->joinVoice(channel.guildId, channelId, muted, deafened);
    updateVoicePanel();

    m_voiceWatchdog.start(10000);
    statusBar()->showMessage(QStringLiteral("Joining %1...").arg(channel.name), 4000);
}

void MainWindow::leaveVoice()
{
    if (m_voiceChannelId.isEmpty())
        return;

    // Use the guild remembered at join time. Looking it up again can come back
    // empty, and a leave with no guild is ignored by Discord, which is how the
    // panel used to get stuck saying "connected".
    QString guildId = m_voiceGuildId;
    if (guildId.isEmpty())
        guildId = m_store->channel(m_voiceChannelId).guildId;

    m_voiceRetryTimer.stop();
    m_voiceRetries = 0;

    m_voiceWatchdog.stop();
    m_gateway->leaveVoice(guildId);
    m_voice->disconnectFromVoice();
    m_speakingUsers.clear();

    // Clear straight away so the window responds, then let the gateway's own
    // event confirm it.
    m_voiceChannelId.clear();
    m_voiceGuildId.clear();
    updateVoicePanel();
}

void MainWindow::tryStartVoice()
{
    // Both halves arrive as separate events and either can be first, so this
    // runs on both and only acts once everything is in hand.
    if (m_voiceChannelId.isEmpty() || m_pendingVoiceToken.isEmpty()
        || m_pendingVoiceEndpoint.isEmpty()) {
        return;
    }

    // The session id has to be the one carried by our own voice state, not the
    // one from signing in. They usually look alike, and are not always the
    // same; the voice server only accepts its own.
    //
    // Both are logged so a future "session no longer valid" can be settled by
    // comparing them rather than by guessing again.
    QString sessionId = m_voiceSessionId;
    if (sessionId.isEmpty()) {
        wlog(QStringLiteral("voice"), QStringLiteral("no voice session yet, waiting for our own state"));
        return;
    }

    wlog(QStringLiteral("voice"), QStringLiteral("sessions: voice=%1 gateway=%2 (%3)")
                                      .arg(sessionId, m_gateway->sessionId(),
                                           sessionId == m_gateway->sessionId()
                                               ? QStringLiteral("same")
                                               : QStringLiteral("different")));

    // Both halves are in hand, so the join was answered.
    m_voiceWatchdog.stop();

    applyVoiceSettings();

    m_voice->connectToVoice(m_voiceGuildId.isEmpty() ? m_store->channel(m_voiceChannelId).guildId
                                                     : m_voiceGuildId,
                            m_voiceChannelId, m_selfUserId, sessionId, m_pendingVoiceToken,
                            m_pendingVoiceEndpoint);

    // Used once, so a later reconnect waits for a fresh pair.
    m_pendingVoiceToken.clear();
    m_pendingVoiceEndpoint.clear();
}

void MainWindow::applyVoiceSettings()
{
    AppConfig &config = AppConfig::instance();
    m_voice->setInputDevice(config.value(QStringLiteral("voice/inputDevice")).toByteArray());
    m_voice->setOutputDevice(config.value(QStringLiteral("voice/outputDevice")).toByteArray());
    m_voice->setInputVolume(config.value(QStringLiteral("voice/inputVolume"), 100).toInt());
    m_voice->setOutputVolume(config.value(QStringLiteral("voice/outputVolume"), 100).toInt());
    m_voice->setSensitivity(config.value(QStringLiteral("voice/sensitivity"), 15).toInt());
}

void MainWindow::updateVoicePanel()
{
    const bool connected = !m_voiceChannelId.isEmpty();
    m_voicePanel->setVisible(connected);
    m_channelDelegate->setJoinedVoiceChannel(m_voiceChannelId);

    if (!connected)
        return;

    const ChannelInfo channel = m_store->channel(m_voiceChannelId);
    const GuildInfo guild = m_store->guild(channel.guildId);
    const QString full = guild.name.isEmpty() ? channel.name
                                              : QStringLiteral("%1 / %2").arg(guild.name, channel.name);

    // Cut it to fit rather than letting it push the Leave button out of view.
    const QFontMetrics metrics(m_voiceChannelLabel->font());
    m_voiceChannelLabel->setText(metrics.elidedText(full, Qt::ElideRight, 150));
    m_voiceChannelLabel->setToolTip(full);
}

void MainWindow::createInvite(const QString &channelId)
{
    m_rest->createInvite(
        channelId,
        [this](const QJsonObject &invite) {
            const QString code = invite.value(QStringLiteral("code")).toString();
            if (code.isEmpty())
                return;
            const QString link = QStringLiteral("https://discord.gg/%1").arg(code);
            QApplication::clipboard()->setText(link);
            statusBar()->showMessage(QStringLiteral("Invite copied: %1").arg(link), 10000);
        },
        [this](const RestClient::Error &error) {
            wlog(QStringLiteral("invite"), QStringLiteral("failed: HTTP %1 %2")
                                               .arg(error.httpStatus).arg(error.message));
            statusBar()->showMessage(error.httpStatus == 403
                                         ? QStringLiteral("You cannot make invites for that channel.")
                                         : QStringLiteral("Could not make an invite."),
                                     6000);
        });
}

void MainWindow::showChannelSettings(const QString &channelId)
{
    const ChannelInfo channel = m_store->channel(channelId);

    QStringList lines;
    lines << QStringLiteral("<b>%1</b>").arg(channel.name.toHtmlEscaped());
    if (!channel.topic.isEmpty())
        lines << channel.topic.toHtmlEscaped();
    if (channel.bitrate > 0)
        lines << QStringLiteral("Bitrate: %1 kbps").arg(channel.bitrate / 1000);
    lines << (channel.userLimit > 0 ? QStringLiteral("User limit: %1").arg(channel.userLimit)
                                    : QStringLiteral("User limit: none"));
    if (!channel.rtcRegion.isEmpty())
        lines << QStringLiteral("Region: %1").arg(channel.rtcRegion.toHtmlEscaped());
    else
        lines << QStringLiteral("Region: automatic");
    lines << QStringLiteral("<span style=\"color:%1\">Channel id %2</span>")
                 .arg(QLatin1String(Theme::TextFaint), channelId);

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Channel settings"));
    box.setTextFormat(Qt::RichText);
    box.setText(lines.join(QStringLiteral("<br>")));
    box.setInformativeText(QStringLiteral("These are read only for now. Editing a channel is not built yet."));
    box.exec();
}

void MainWindow::openPlugins()
{
    PluginsDialog dialog(m_plugins, this);
    dialog.exec();
}

void MainWindow::openSettings()
{
    SettingsDialog dialog(m_store, m_rest, m_plugins, m_selfUserId, this);
    connect(&dialog, &SettingsDialog::appearanceChanged, this, &MainWindow::applyAppearance);
    connect(&dialog, &SettingsDialog::logOutRequested, this, &MainWindow::logOut);
    dialog.exec();
}

void MainWindow::applyAppearance()
{
    AppConfig &config = AppConfig::instance();

    m_messageView->document()->setDefaultStyleSheet(
        Theme::messageViewCss(config.value(QStringLiteral("appearance/fontSize"), 14).toInt()));
    m_messageView->setAnimationsEnabled(
        config.value(QStringLiteral("appearance/playAnimations"), true).toBool());

    renderChannel();
}

void MainWindow::openLog()
{
    auto *dialog = new LogDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::logOut()
{
    m_gateway->stop();
    AppConfig::instance().clearToken();
    m_store->clear();
    emit loggedOut();
    close();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_gateway->stop();
    QMainWindow::closeEvent(event);
}
