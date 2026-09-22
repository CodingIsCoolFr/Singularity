#include "ui/MainWindow.h"

#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/RestClient.h"
#include "plugin/PluginHost.h"
#include "ui/ChatView.h"
#include "ui/FriendsPage.h"
#include "ui/ImageViewer.h"
#include "ui/AuroraWidget.h"
#include "core/ScreenShare.h"
#include "ui/LoadingOverlay.h"
#include "ui/MemberListPanel.h"
#include "ui/ShareDialog.h"
#include "ui/UpdateFlow.h"

#include <QProcess>

#include "ui/CallView.h"
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
#include <QEvent>
#include <QIcon>
#include <QMessageBox>
#include <QDesktopServices>
#include <QFrame>
#include <QInputDialog>
#include <QMouseEvent>
#include <QAbstractTextDocumentLayout>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QScrollBar>
#include <QSlider>
#include <QSplitter>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStatusBar>
#include <QTextCursor>
#include <QTextDocument>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

#include <QTextEdit>
#include <QTextFrame>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#ifdef Q_OS_WIN
#    include <dwmapi.h>
#    include <windows.h>
#    include <windowsx.h>
#endif

namespace {

// Roles hung off list rows, shared with the painters.
constexpr int IdRole = SingularityRoles::Id;
constexpr int KindRole = SingularityRoles::Kind;
constexpr int FolderRole = SingularityRoles::Folder;
constexpr int FolderOpenRole = SingularityRoles::FolderOpen;

constexpr int GuildIconPixels = 48;
constexpr int RailWidth = 84;
constexpr int SidebarWidth = 256;

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

QPushButton *captionButton(QWidget *parent, const QString &name, const QString &text)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(name);
    button->setFlat(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(46, 36);
    button->setCursor(Qt::ArrowCursor);
    return button;
}

bool containsGlobal(const QWidget *widget, const QPoint &global)
{
    return widget && widget->isVisible() && widget->rect().contains(widget->mapFromGlobal(global));
}

// The strip along the top that a frameless window can be dragged by.
//
// startSystemMove hands the drag to Windows rather than moving the window by
// hand from mouse deltas. That is what makes it feel like a title bar: snap
// to the screen edges, snap to the half of the screen, and the shake to
// minimise everything else all come for free, and none of them can be
// imitated by setting a position.
class TitleDragArea : public QWidget
{
public:
    TitleDragArea(QWidget *window, QWidget *parent)
        : QWidget(parent)
        , m_window(window)
    {
        setAttribute(Qt::WA_TranslucentBackground, false);
        setAutoFillBackground(false);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !m_window) {
            QWidget::mousePressEvent(event);
            return;
        }
        if (QWindow *handle = m_window->windowHandle())
            handle->startSystemMove();
    }

    // Double click does what double clicking a title bar does.
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !m_window) {
            QWidget::mouseDoubleClickEvent(event);
            return;
        }
        if (m_window->isMaximized())
            m_window->showNormal();
        else
            m_window->showMaximized();
    }

private:
    QWidget *m_window = nullptr;
};

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
    setWindowTitle(QStringLiteral("Singularity"));
    setWindowIcon(QIcon(QStringLiteral(":/brand/singularity.png")));
    resize(1440, 900);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);

    buildUi();
    buildMenu();
    applyAppearance();

    m_statusClearTimer.setSingleShot(true);
    connect(&m_statusClearTimer, &QTimer::timeout, this, [this]() {
        if (m_statusMessage)
            m_statusMessage->clear();
    });

    m_typingClearTimer.setSingleShot(true);
    connect(&m_typingClearTimer, &QTimer::timeout, this, [this]() { setTypingHint(QString()); });

    connect(m_gateway, &GatewayClient::ready, this, &MainWindow::onGatewayReady);
    connect(m_gateway, &GatewayClient::dispatch, this, &MainWindow::onGatewayDispatch);
    connect(m_gateway, &GatewayClient::stateChanged, this, &MainWindow::onGatewayState);
    connect(m_gateway, &GatewayClient::logLine, this, [this](const QString &line) {
        flashStatus(line, 6000);
    });
    connect(m_gateway, &GatewayClient::fatalAuthError, this, [this]() {
        flashStatus(QStringLiteral("Discord refused this session. Log out and sign in again."), 0);

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
            "<br><br>Fix it: <b>Singularity &gt; Log out</b>, then sign in again."
            "<br><br><b>Singularity &gt; Log</b> shows the exact reason, on the line beginning "
            "\"giving up\".</p>"));
    });

    // Redrawing is expensive and arrives in bursts.
    //
    // Every edit, every reaction, every late embed rebuilds the whole
    // conversation and lays it out again. In a busy server those land several
    // to a second, and each one is a full re-layout of a hundred messages -
    // which is the stutter you feel while reading. Collapsing a burst into one
    // redraw costs a few milliseconds of delay nobody can see.
    // Raised from fifty. A busy server sends edits, deletions and reactions
    // in bursts, and fifty milliseconds was short enough that a burst still
    // became two or three full redraws - which the log shows as pairs of
    // identical lines eighty milliseconds apart. A person cannot see the
    // difference between this and instant; they can see the redraws.
    m_renderTimer.setSingleShot(true);
    m_renderTimer.setInterval(120);
    connect(&m_renderTimer, &QTimer::timeout, this, &MainWindow::renderChannel);

    connect(m_plugins, &PluginHost::repaintRequested, this, [this]() { scheduleRender(); });
    connect(m_plugins, &PluginHost::pluginLogged, this, [this](const QString &id, const QString &line) {
        flashStatus(QStringLiteral("[%1] %2").arg(id, line), 5000);
    });

    connect(m_store, &MessageStore::channelHistoryChanged, this, [this](const QString &channelId) {
        // A page of older messages is applied by the scroll itself, which
        // inserts them above the reader. Rebuilding here would replace that
        // with the newest page and the scroll would appear to repeat.
        if (m_applyingHistory)
            return;
        if (channelId == m_currentChannelId)
            scheduleRender();
    });
    connect(m_store, &MessageStore::messageChanged, this,
            [this](const QString &channelId, const QString &messageId) {
                if (channelId != m_currentChannelId)
                    return;
                // A reaction or an edit touches one message. Rebuilding the
                // whole conversation for it is what made a busy channel feel
                // heavy, so try to redraw just that one first.
                if (!replaceMessageInView(messageId))
                    scheduleRender();
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
                if (m_callView)
                    m_callView->setSpeaking(m_speakingUsers);
            });

    // Cameras and shared screens, once they have been decoded.
    connect(m_voice, &VoiceConnection::videoFrame, this,
            [this](const QString &userId, const QImage &image) {
                if (m_callView)
                    m_callView->setFrame(userId, image, CallView::Surface::Camera);
            });

    // The second connection, used only for watching somebody's shared screen.
    m_streamVoice = new VoiceConnection(this);
    m_streamVoice->setViewerOnly(true);
    m_streamVoice->setAudioHost(m_voice);
    m_streamVoice->setOutputVolume(
        AppConfig::instance().value(QStringLiteral("voice/streamVolume"), 80).toInt());

    connect(m_streamVoice, &VoiceConnection::videoFrame, this,
            [this](const QString &, const QImage &image) {
                // A stream carries one picture, and it belongs to whoever we
                // asked to watch rather than to the sender named inside it.
                if (m_callView && !m_watchingUserId.isEmpty())
                    m_callView->setFrame(m_watchingUserId, image, CallView::Surface::Share);
            });

    connect(m_streamVoice, &VoiceConnection::failed, this, [this](const QString &reason) {
        wlog(QStringLiteral("stream"), QStringLiteral("stream connection failed: %1").arg(reason));
        flashStatus(QStringLiteral("Could not watch that stream: %1").arg(reason), 6000);
    });

    // The third connection, which carries our own shared screen out.
    //
    // Viewer-only for the same reason the watching one is: the microphone is
    // already being carried by the call underneath, and opening a second one
    // here would send everything twice.
    m_shareVoice = new VoiceConnection(this);
    m_shareVoice->setViewerOnly(true);

    m_share = new ScreenShare(this);

    // Capture and encoding run on their own thread and hand finished pictures
    // back here, where the keys and the socket are.
    connect(m_share, &ScreenShare::picture, this,
            [this](const QList<QByteArray> &units, bool) {
                if (m_shareVoice)
                    m_shareVoice->sendPicture(units);
            });

    // Our own tile, filled from our own capture. Nothing comes back off the
    // network for a stream we are the one sending.
    connect(m_share, &ScreenShare::preview, this, [this](const QImage &frame) {
        if (m_callView && !m_selfUserId.isEmpty())
            m_callView->setFrame(m_selfUserId, frame, CallView::Surface::Share);
    });

    connect(m_share, &ScreenShare::started, this,
            [this](int width, int height, const QString &encoder, bool hardware) {
                wlog(QStringLiteral("share"),
                     QStringLiteral("encoding with %1 (%2)")
                         .arg(encoder, hardware ? QStringLiteral("hardware")
                                                : QStringLiteral("software")));
                if (m_shareVoice)
                    m_shareVoice->startSendingVideo(width, height);

                flashStatus(QStringLiteral("You are live — %1x%2, %3")
                                .arg(width)
                                .arg(height)
                                .arg(hardware ? QStringLiteral("hardware encoded")
                                              : QStringLiteral("software encoded")),
                            5000);
                updateVoicePanel();
            });

    connect(m_share, &ScreenShare::failed, this, [this](const QString &reason) {
        wlog(QStringLiteral("share"), QStringLiteral("share failed: %1").arg(reason));
        flashStatus(reason, 7000);
        stopScreenShare();
    });

    // A viewer that cannot draw anything asks, and the request arrives on the
    // socket rather than through any UI.
    connect(m_shareVoice, &VoiceConnection::keyframeWanted, this, [this]() {
        if (m_share)
            m_share->requestKeyframe();
    });

    // The capture cannot start until there is somewhere to send it.
    connect(m_shareVoice, &VoiceConnection::stateChanged, this,
            [this](VoiceConnection::State state) {
                if (state != VoiceConnection::State::Connected || m_myStreamKey.isEmpty())
                    return;
                if (m_share && !m_share->isRunning()) {
                    m_share->start(m_shareMonitorId, m_shareWidth, m_shareHeight, m_shareFps,
                                   m_shareBitrate);
                }
            });

    connect(m_shareVoice, &VoiceConnection::failed, this, [this](const QString &reason) {
        wlog(QStringLiteral("share"), QStringLiteral("share connection failed: %1").arg(reason));
        flashStatus(QStringLiteral("Could not start your screen share: %1").arg(reason), 6000);
        stopScreenShare();
    });

    connect(m_voice, &VoiceConnection::videoAvailable, this,
            [this](const QString &userId, bool available) {
                if (!m_callView)
                    return;
                if (!available)
                    m_callView->dropFrames(userId, CallView::Surface::Camera);
                m_callView->refresh();
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
            flashStatus(QStringLiteral("Voice connected and encrypted."), 5000);
            break;
        case VoiceConnection::State::Failed:      m_voiceState->setText(QStringLiteral("Voice failed")); break;
        }
    });

    connect(m_voice, &VoiceConnection::failed, this, [this](const QString &reason) {
        flashStatus(QStringLiteral("Voice: %1").arg(reason), 10000);

        // Nothing to retry if we are not meant to be in a call any more.
        if (m_voiceChannelId.isEmpty())
            return;

        // A retry that is already on its way must not start another.
        if (m_voiceRetryTimer.isActive())
            return;

        // 4014 is Discord tearing down this voice socket. That happens when a
        // mod kicks you, and also — far more often — when the gateway
        // reconnects. Leaving would turn a blip into a real kick.
        if (reason.contains(QStringLiteral("4014"))) {
            wlog(QStringLiteral("voice"),
                 QStringLiteral("voice server dropped us (4014); staying in the channel to rejoin"));
            m_pendingVoiceToken.clear();
            m_pendingVoiceEndpoint.clear();
            m_voiceSessionId.clear();
            m_rejoinVoiceAfterGateway = true;
            if (m_voiceState)
                m_voiceState->setText(QStringLiteral("Reconnecting..."));
            flashStatus(QStringLiteral("Voice dropped, reconnecting..."), 5000);
            if (m_gateway->state() == GatewayClient::State::Ready)
                m_voiceRetryTimer.start(800);
            return;
        }

        // Marked final by the voice side: trying again would only repeat it.
        if (reason.contains(QStringLiteral("[final]"))) {
            wlog(QStringLiteral("voice"), QStringLiteral("not retrying: %1").arg(reason));
            if (m_voiceState)
                m_voiceState->setText(QStringLiteral("Voice unavailable"));

            if (reason.contains(QStringLiteral("4017"))) {
                // Since March 2026 Discord requires end-to-end encryption on
                // every call, so no other channel will work either. Saying
                // "try another one" would just waste someone's time.
                flashStatus(
                    QStringLiteral("Discord now requires end-to-end encryption on every call. "
                                   "Singularity does not implement it yet, so voice cannot connect "
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
                flashStatus(
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
        if (m_rejoinVoiceAfterGateway) {
            rejoinVoiceIfNeeded();
            return;
        }

        AppConfig &config = AppConfig::instance();
        m_gateway->joinVoice(m_voiceGuildId, m_voiceChannelId,
                             config.value(QStringLiteral("voice/joinMuted"), false).toBool(),
                             config.value(QStringLiteral("voice/joinDeafened"), false).toBool(),
                             true);
    });

    // Reaching the top of a channel fetches what came before it.
    //
    // A margin rather than exactly zero, so the next batch is already on its
    // way by the time the reader gets there and the scroll does not stop dead
    // while they wait.
    connect(m_messageView->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        QScrollBar *bar = m_messageView->verticalScrollBar();

        // The reader taking the wheel, and only the reader.
        //
        // Position alone is not enough to tell "they scrolled up" from "the
        // document grew underneath them". Pictures arriving, a late embed,
        // the layout settling - all of those move the bar without anybody
        // touching it, and any one of them could cancel the follow and leave
        // the channel parked half way up. So it is cancelled only after a
        // wheel, a drag or a key, which are the only ways a person does it.
        if (!m_autoScrolling && m_readerMoved)
            m_stickToBottom = value >= bar->maximum() - 8;

        // Reaching the top to read older messages, and only that.
        //
        // The test used to be the position alone, and setting the text of a
        // channel resets the position to zero - so opening any channel looked
        // exactly like somebody scrolling up for history. It fetched fifty
        // more messages, laid the whole conversation out a second time, and
        // landed the reader at the very top of it. That was both the jump and
        // the lag on switching channels, from one line.
        //
        // Somebody pinned to the newest message is not reading history, so
        // that alone rules it out.
        // Our own scrolling — opening a channel, restoring a place, inserting
        // a page of history — must not be read as the reader arriving
        // somewhere. setTextCursor(End) in particular jumps to the bottom for
        // one moment during a redraw, and treating that as "they scrolled
        // back down" put the newest page back on screen every time they
        // tried to read upwards.
        if (m_autoScrolling)
            return;

        if (!m_windowAtTail && m_readerMoved && value >= bar->maximum() - 240
            && bar->maximum() > 0) {
            revealNewer();
        }

        if (!m_stickToBottom && m_readerMoved && value <= 240 && bar->maximum() > 0)
            reachedTop();
    });

    // What counts as the reader moving: a wheel, a drag of the bar, or the
    // keys that scroll. Nothing the program or the layout does reaches here.
    m_messageView->viewport()->installEventFilter(this);
    m_messageView->verticalScrollBar()->installEventFilter(this);

    // The document finishing its measuring, which happens in pieces and long
    // after the text was set: first the text, then each picture as it arrives.
    // Every one of those makes the document taller, and while we are meant to
    // be at the newest message that means going there again.
    connect(m_messageView->document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged, this, [this](const QSizeF &) {
                if (m_stickToBottom)
                    scrollToBottom();
            });

    // Hands each slice of microphone sound to the plugins on its way out.
    //
    // Wired here rather than inside VoiceConnection so the network code keeps
    // knowing nothing about plugins. Captured by raw pointer on purpose: this
    // runs fifty times a second, and both objects outlive the window.
    m_voice->setMicrophoneProcessor(
        [plugins = m_plugins](qint16 *samples, int frames, int channels, int rate) {
            plugins->runMicrophoneFrame(samples, frames, channels, rate);
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
        flashStatus(
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
        // Whoever is in the call, and what they are doing, changes on the same
        // events the sidebar watches.
        if (m_callView)
            m_callView->refresh();

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
    // On Windows, a translucent child of QOpenGLWidget paints whatever is
    // BEHIND the GL widget (the black window fill), not the FBO. A full-size
    // #Chrome overlay is why the disk vanished with no click: it covered
    // every pixel. The hole is the window. Cards are opaque islands. Layout
    // stretch and margins have no widget, so those pixels are the shader.
    m_aurora = new AuroraWidget(this);
    m_aurora->setAttribute(Qt::WA_OpaquePaintEvent, true);
    m_aurora->setAttribute(Qt::WA_NoSystemBackground, true);
    m_aurora->setAttribute(Qt::WA_StyledBackground, false);
    m_aurora->setAutoFillBackground(false);
    m_aurora->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCentralWidget(m_aurora);

    // Needed for the resize edges below: without tracking, move events only
    // arrive while a button is held, so the cursor would never change until
    // after the press it is meant to invite.
    m_aurora->setMouseTracking(true);
    m_aurora->installEventFilter(this);

    auto *shell = new QVBoxLayout(m_aurora);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    auto *titleLayout = new QHBoxLayout();
    titleLayout->setContentsMargins(8, 4, 0, 0);
    titleLayout->setSpacing(0);

    m_menuBar = new QMenuBar(m_aurora);
    m_menuBar->setObjectName(QStringLiteral("AppMenu"));
    m_menuBar->setNativeMenuBar(false);
    titleLayout->addWidget(m_menuBar);

    // Something to actually take hold of.
    //
    // The window is frameless, so Windows draws no title bar to drag it by,
    // and the strip where one would be was layout stretch rather than a
    // widget: nothing there to receive a press. This is that strip, and it
    // hands the drag to the window manager through startSystemMove, which
    // behaves like a real title bar including snapping to edges.
    auto *dragStrip = new TitleDragArea(this, m_aurora);
    dragStrip->setFixedHeight(32);
    titleLayout->addWidget(dragStrip, 1);

    auto *minBtn = captionButton(m_aurora, QStringLiteral("CaptionMin"), QStringLiteral("–"));
    m_captionMax = captionButton(m_aurora, QStringLiteral("CaptionMax"), QStringLiteral("□"));
    auto *closeBtn = captionButton(m_aurora, QStringLiteral("CaptionClose"), QStringLiteral("✕"));
    connect(minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(m_captionMax, &QPushButton::clicked, this, [this]() {
        if (isMaximized())
            showNormal();
        else
            showMaximized();
    });
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);
    titleLayout->addWidget(minBtn);
    titleLayout->addWidget(m_captionMax);
    titleLayout->addWidget(closeBtn);
    shell->addLayout(titleLayout);

    auto *rootLayout = new QHBoxLayout();
    rootLayout->setContentsMargins(14, 8, 14, 8);
    rootLayout->setSpacing(12);

    rootLayout->addWidget(buildGuildRail(m_aurora));
    rootLayout->addWidget(buildSidebar(m_aurora));

    auto *chatCard = new QFrame(m_aurora);
    chatCard->setObjectName(QStringLiteral("ChatColumn"));
    chatCard->setAttribute(Qt::WA_StyledBackground, true);
    chatCard->setAutoFillBackground(true);
    auto *chatLayout = new QVBoxLayout(chatCard);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(0);

    m_chatSplitter = new QSplitter(Qt::Vertical, chatCard);
    m_chatSplitter->setChildrenCollapsible(true);
    m_chatSplitter->setHandleWidth(8);

    m_callView = new CallView(m_store, m_chatSplitter);
    connect(m_callView, &CallView::profileRequested, this,
            [this](const QString &userId) { showProfile(userId, QCursor::pos()); });
    connect(m_callView, &CallView::watchAttempted, this, &MainWindow::watchStream);
    connect(m_callView, &CallView::focusRequested, this,
            [this](const QString &userId, CallView::Surface surface) {
                m_callView->setFocusedUser(userId, surface);
            });
    connect(m_callView, &CallView::visibilityChanged, this, [this](bool on) {
        if (!on || !m_chatSplitter)
            return;
        const int h = m_chatSplitter->height();
        if (h < 160)
            return;
        m_chatSplitter->setSizes({int(h * 0.62), int(h * 0.38)});
    });

    m_chatStack = new QStackedWidget(m_chatSplitter);
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

    m_chatSplitter->addWidget(m_callView);
    m_chatSplitter->addWidget(m_chatStack);
    m_chatSplitter->setStretchFactor(0, 3);
    m_chatSplitter->setStretchFactor(1, 2);
    chatLayout->addWidget(m_chatSplitter);

    rootLayout->addWidget(chatCard, 1);

    // The people, down the right. Outside the chat card rather than inside it
    // so it keeps its own background and full height, the way the sidebar on
    // the left does.
    m_members = new MemberListPanel(m_store, m_aurora);
    connect(m_members, &MemberListPanel::profileRequested, this,
            [this](const QString &userId) { showProfile(userId, QCursor::pos()); });
    rootLayout->addWidget(m_members);

    shell->addLayout(rootLayout, 1);

    auto *statusChip = new QWidget(m_aurora);
    statusChip->setObjectName(QStringLiteral("StatusChip"));
    statusChip->setFixedHeight(26);
    auto *statusLayout = new QHBoxLayout(statusChip);
    statusLayout->setContentsMargins(12, 0, 10, 0);
    statusLayout->setSpacing(8);
    m_statusMessage = new QLabel(statusChip);
    m_statusMessage->setObjectName(QStringLiteral("StatusMessage"));
    m_statusDot = new QLabel(QStringLiteral("offline"), statusChip);
    m_statusDot->setObjectName(QStringLiteral("StatusDot"));
    m_statusDot->setStyleSheet(
        QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextFaint)));
    statusLayout->addWidget(m_statusMessage);
    statusLayout->addWidget(m_statusDot);

    auto *statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(14, 0, 14, 8);
    statusRow->addStretch(1);
    statusRow->addWidget(statusChip);
    shell->addLayout(statusRow);

    m_aurora->setHoleColors(Theme::holeAccent(), Theme::holeDisk(), Theme::holeGrade());
    m_aurora->setRunning(
        AppConfig::instance().value(QStringLiteral("appearance/animatedBackground"), true).toBool());
    applyBackgroundSettings();
}

QWidget *MainWindow::buildGuildRail(QWidget *parent)
{
    auto *rail = new QWidget(parent);
    rail->setObjectName(QStringLiteral("GuildRail"));
    rail->setAttribute(Qt::WA_StyledBackground, true);
    rail->setAutoFillBackground(true);
    rail->setFixedWidth(RailWidth);

    auto *layout = new QVBoxLayout(rail);
    layout->setContentsMargins(4, 10, 4, 10);
    layout->setSpacing(0);

    m_guildRail = new QListWidget(rail);
    m_guildRail->setIconSize(QSize(GuildIconPixels, GuildIconPixels));
    m_guildRail->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_guildRail->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_guildRail->viewport()->setAutoFillBackground(false);
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
    sidebar->setAttribute(Qt::WA_StyledBackground, true);
    sidebar->setAutoFillBackground(true);
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
    m_channelList->viewport()->setAutoFillBackground(false);

    m_channelDelegate = new ChannelDelegate(m_channelList, m_channelList);
    m_channelList->setItemDelegate(m_channelDelegate);
    layout->addWidget(m_channelList, 1);

    // The pointer crossing a channel's name starts fetching it.
    //
    // Between noticing a channel and clicking it there is somewhere between a
    // fifth and a whole second of human time, and the round trip to Discord
    // is about a third of a second. Spending the one inside the other is the
    // entire trick: by the time the click lands the messages are usually
    // already here, and opening looks instant because no waiting happened
    // while anybody was looking.
    m_channelList->setMouseTracking(true);
    connect(m_channelList, &QListWidget::itemEntered, this, [this](QListWidgetItem *item) {
        if (!item || item->data(KindRole).toString() != QLatin1String("channel"))
            return;
        prefetchChannel(item->data(IdRole).toString());
    });

    // One at a time, spaced apart. Six requests at once is how a client gets
    // itself rate limited, and the allowance belongs to the person's real
    // clicks rather than to our guesses about them.
    m_prefetchTimer.setSingleShot(true);
    m_prefetchTimer.setInterval(220);
    connect(&m_prefetchTimer, &QTimer::timeout, this, &MainWindow::pumpPrefetch);

    // What the program is holding, written down every minute.
    //
    // Memory was the one thing the log never said anything about, so the only
    // evidence was a number in the task manager with nothing to attribute it
    // to. Now the log names what is holding it.
    m_memoryTimer.setInterval(60000);
    connect(&m_memoryTimer, &QTimer::timeout, this, &MainWindow::reportMemory);
    m_memoryTimer.start();

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
        if (!item || item->data(KindRole).toString() != QLatin1String("voicemember"))
            return;

        // Somebody clicking a LIVE tag is trying to watch, not to read a
        // profile. Saying so is better than opening the wrong thing and
        // looking broken.
        if (item->data(SingularityRoles::Streaming).toBool()) {
            watchStream(item->data(IdRole).toString());
            return;
        }

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
    panel->setFixedHeight(132);

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

    // Two rows of two, not one row of four.
    //
    // Four buttons across a sidebar this narrow cannot fit. Each carried a
    // minimum width, and when the minimums add up to more than there is, Qt
    // stops shrinking and they simply overlap - "Share screen" was printed on
    // top of "Leave". A grid gives each one an equal share of whatever width
    // there is, so a narrower sidebar makes them smaller rather than broken.
    auto *buttons = new QGridLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setHorizontalSpacing(6);
    buttons->setVerticalSpacing(6);
    buttons->setColumnStretch(0, 1);
    buttons->setColumnStretch(1, 1);

    m_muteButton = new QPushButton(QStringLiteral("Mute"), panel);
    m_muteButton->setFixedHeight(26);
    m_muteButton->setCheckable(true);
    m_muteButton->setToolTip(QStringLiteral("Stop sending your voice"));
    connect(m_muteButton, &QPushButton::toggled, this, [this](bool on) {
        m_voice->setMuted(on);
        m_muteButton->setText(on ? QStringLiteral("Muted") : QStringLiteral("Mute"));
        // Tell the server too, so other people see the crossed out microphone.
        m_gateway->joinVoice(m_voiceGuildId, m_voiceChannelId, on, m_deafenButton->isChecked());
    });
    buttons->addWidget(m_muteButton, 0, 0);

    m_deafenButton = new QPushButton(QStringLiteral("Deafen"), panel);
    m_deafenButton->setFixedHeight(26);
    m_deafenButton->setCheckable(true);
    m_deafenButton->setToolTip(QStringLiteral("Stop hearing everyone"));
    connect(m_deafenButton, &QPushButton::toggled, this, [this](bool on) {
        m_voice->setDeafened(on);
        m_deafenButton->setText(on ? QStringLiteral("Deaf") : QStringLiteral("Deafen"));
        m_gateway->joinVoice(m_voiceGuildId, m_voiceChannelId, m_muteButton->isChecked(), on);
    });
    buttons->addWidget(m_deafenButton, 0, 1);

    m_shareButton = new QPushButton(QStringLiteral("Share"), panel);
    m_shareButton->setFixedHeight(26);
    m_shareButton->setToolTip(QStringLiteral("Show your screen to everyone in this call"));
    connect(m_shareButton, &QPushButton::clicked, this, &MainWindow::startScreenShare);
    buttons->addWidget(m_shareButton, 1, 0);

    auto *leave = new QPushButton(QStringLiteral("Leave"), panel);
    leave->setFixedHeight(26);
    leave->setToolTip(QStringLiteral("Leave the call"));
    connect(leave, &QPushButton::clicked, this, &MainWindow::leaveVoice);
    buttons->addWidget(leave, 1, 1);

    // No trailing stretch: the columns already share the width between them,
    // which is what stops them overlapping.
    layout->addLayout(buttons);

    AppConfig &config = AppConfig::instance();

    auto *volumeRow = new QHBoxLayout;
    volumeRow->setContentsMargins(0, 2, 0, 0);
    volumeRow->setSpacing(8);
    auto *volumeLabel = new QLabel(QStringLiteral("Vol"), panel);
    volumeLabel->setFixedWidth(36);
    volumeLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                                   .arg(QLatin1String(Theme::TextMuted)));
    m_outputVolumeSlider = new QSlider(Qt::Horizontal, panel);
    m_outputVolumeSlider->setRange(0, 200);
    m_outputVolumeSlider->setValue(config.value(QStringLiteral("voice/outputVolume"), 100).toInt());
    m_outputVolumeSlider->setToolTip(QStringLiteral("How loud everyone in the call is"));
    connect(m_outputVolumeSlider, &QSlider::valueChanged, this, [this](int value) {
        AppConfig::instance().setValue(QStringLiteral("voice/outputVolume"), value);
        if (m_voice)
            m_voice->setOutputVolume(value);
    });
    volumeRow->addWidget(volumeLabel);
    volumeRow->addWidget(m_outputVolumeSlider, 1);
    layout->addLayout(volumeRow);

    m_streamControls = new QWidget(panel);
    m_streamControls->setVisible(false);
    auto *streamLayout = new QVBoxLayout(m_streamControls);
    streamLayout->setContentsMargins(0, 0, 0, 0);
    streamLayout->setSpacing(6);

    auto *shareRow = new QHBoxLayout;
    shareRow->setContentsMargins(0, 0, 0, 0);
    shareRow->setSpacing(8);
    auto *shareLabel = new QLabel(QStringLiteral("Share"), m_streamControls);
    shareLabel->setFixedWidth(36);
    shareLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                                  .arg(QLatin1String(Theme::TextMuted)));
    m_streamVolumeSlider = new QSlider(Qt::Horizontal, m_streamControls);
    m_streamVolumeSlider->setRange(0, 200);
    m_streamVolumeSlider->setValue(config.value(QStringLiteral("voice/streamVolume"), 80).toInt());
    m_streamVolumeSlider->setToolTip(QStringLiteral("How loud the shared screen is"));
    connect(m_streamVolumeSlider, &QSlider::valueChanged, this, [this](int value) {
        AppConfig::instance().setValue(QStringLiteral("voice/streamVolume"), value);
        if (m_streamVoice)
            m_streamVoice->setOutputVolume(value);
    });
    shareRow->addWidget(shareLabel);
    shareRow->addWidget(m_streamVolumeSlider, 1);
    streamLayout->addLayout(shareRow);

    m_stopWatchButton = new QPushButton(QStringLiteral("Stop watching"), m_streamControls);
    m_stopWatchButton->setFixedHeight(26);
    connect(m_stopWatchButton, &QPushButton::clicked, this, &MainWindow::stopWatchingStream);
    streamLayout->addWidget(m_stopWatchButton);

    layout->addWidget(m_streamControls);

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
    header->setFixedHeight(58);
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(22, 10, 22, 10);
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
    composerLayout->setContentsMargins(18, 4, 18, 16);

    auto *composerBox = new QFrame(composerWrap);
    composerBox->setObjectName(QStringLiteral("ComposerBox"));
    auto *boxLayout = new QVBoxLayout(composerBox);
    boxLayout->setContentsMargins(0, 0, 0, 0);

    m_composer = new QTextEdit(composerBox);
    m_composer->setObjectName(QStringLiteral("MessageInput"));
    m_composer->setPlaceholderText(QStringLiteral("Write a message"));
    m_composer->setFixedHeight(52);
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
    if (!m_menuBar)
        return;
    QMenu *fileMenu = m_menuBar->addMenu(QStringLiteral("&Singularity"));

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

    auto *updateAction = fileMenu->addAction(QStringLiteral("Check for updates..."));
    connect(updateAction, &QAction::triggered, this, [this]() { UpdateFlow::run(false, this); });

    fileMenu->addSeparator();

    auto *logOutAction = fileMenu->addAction(QStringLiteral("Log out"));
    connect(logOutAction, &QAction::triggered, this, &MainWindow::logOut);

    auto *quitAction = fileMenu->addAction(QStringLiteral("Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);
}

void MainWindow::startSession(const QString &token)
{
    // In front of everything until the window has something worth showing.
    //
    // Signing in returns in a moment, but the client is not ready then: forty
    // servers, their channels, a couple of thousand voice states and a
    // hundred presences arrive over the following second and a half, and each
    // one redraws part of the window. Watching that is watching the furniture
    // being carried in.
    m_loading = new LoadingOverlay(this);
    m_loading->show();
    m_loading->raise();

    m_gateway->start(token);
    flashStatus(QStringLiteral("Connecting..."));
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

    // A new session makes any voice socket built on the old one invalid, but
    // the user did not leave. Keep the channel and join again once the rest
    // of READY has been ingested.
    if (!m_voiceChannelId.isEmpty()) {
        wlog(QStringLiteral("voice"), QStringLiteral("signed in again, will rejoin the call"));
        stopWatchingStream();
        m_voice->disconnectFromVoice();
        m_pendingVoiceToken.clear();
        m_pendingVoiceEndpoint.clear();
        m_voiceSessionId.clear();
        m_rejoinVoiceAfterGateway = true;
        if (m_voiceState)
            m_voiceState->setText(QStringLiteral("Reconnecting..."));
    }

    // A reconnect rebuilds everything. Without this you get dropped back to
    // direct messages every time the connection hiccups.
    const QString wasInGuild = m_currentGuildId;
    const QString wasInChannel = m_currentChannelId;

    if (m_loading)
        m_loading->setStep(QStringLiteral("Sorting your servers..."));

    m_store->ingestReady(payload);
    populateGuildRail();

    // The status you last chose, carried across restarts. Discord remembers
    // this for its own client; nothing in the sign-in carries it back to us,
    // so it is kept here.
    const QString saved = AppConfig::instance()
                              .value(QStringLiteral("presence/status"), QStringLiteral("online"))
                              .toString();
    if (saved != QLatin1String("online"))
        m_gateway->setPresenceStatus(saved);

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

    // The window is furnished. The overlay waits a moment longer on its own
    // if it has only just appeared, then fades.
    if (m_loading) {
        m_loading->finish();
        m_loading = nullptr;
    }

    wlog(QStringLiteral("ui"), QStringLiteral("rail built: %1 servers, %2 direct chats")
                                   .arg(m_store->guilds().size())
                                   .arg(m_store->directChannels().size()));
    flashStatus(QStringLiteral("Connected."), 4000);
    rejoinVoiceIfNeeded();
}

void MainWindow::onGatewayDispatch(const QString &eventType, const QJsonObject &data)
{
    // Plugins see every event before the client reacts to it.
    m_plugins->dispatchGatewayEvent(eventType, data);

    if (eventType == QLatin1String("RESUMED")) {
        if (!m_voiceChannelId.isEmpty()
            && m_voice->state() != VoiceConnection::State::Connected)
            m_rejoinVoiceAfterGateway = true;
        rejoinVoiceIfNeeded();
        return;
    }

    if (eventType == QLatin1String("MESSAGE_CREATE")) {
        const int dropped = m_store->appendMessage(data);
        const QString channelId = data.value(QStringLiteral("channel_id")).toString();
        if (dropped > 0 && channelId == m_currentChannelId)
            m_renderFirst = qMax(0, m_renderFirst - dropped);
        if (dropped >= 0 && channelId == m_currentChannelId && m_store->hasHistory(channelId)) {
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

        // Says out loud who the main gateway thinks has a camera or a screen
        // running in the call we are in.
        //
        // Without this, "I cannot see anybody's camera" and "nobody had one
        // on" look identical from the log, and they need completely different
        // fixes. This is the line that tells them apart.
        if (!m_voiceChannelId.isEmpty()
            && data.value(QStringLiteral("channel_id")).toString() == m_voiceChannelId) {
            const bool video = data.value(QStringLiteral("self_video")).toBool();
            const bool stream = data.value(QStringLiteral("self_stream")).toBool();
            if (video || stream) {
                const QString who = data.value(QStringLiteral("user_id")).toString();
                wlog(QStringLiteral("video"),
                     QStringLiteral("the gateway says %1 has %2 running in this call")
                         .arg(m_store->userName(who).isEmpty() ? who : m_store->userName(who),
                              stream ? QStringLiteral("a shared screen") : QStringLiteral("a camera")));
            }
        }

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

            if (nowIn.isEmpty() && !m_voiceChannelId.isEmpty()) {
                if (m_voiceWatchdog.isActive()) {
                    wlog(QStringLiteral("voice"),
                         QStringLiteral("own state: (left) while join is in flight — ignoring"));
                    return;
                }
                if (m_rejoinVoiceAfterGateway
                    && m_gateway->state() != GatewayClient::State::Ready) {
                    wlog(QStringLiteral("voice"),
                         QStringLiteral("own state: (left) while gateway is reconnecting — ignoring"));
                    return;
                }
                m_rejoinVoiceAfterGateway = false;
                m_voiceRetryTimer.stop();
            }

            // This is the half of the handshake that carries the session id.
            // Only keep it while we are actually in a channel, so it can never
            // be paired with a later call's server details.
            m_voiceSessionId = nowIn.isEmpty()
                ? QString()
                : data.value(QStringLiteral("session_id")).toString();

            const QString wasIn = m_voiceChannelId;

            if (nowIn != m_voiceChannelId) {
                m_voiceChannelId = nowIn;
                updateVoicePanel();
            }

            if (nowIn.isEmpty()) {
                m_voice->disconnectFromVoice();
                return;
            }

            // Discord has put us somewhere. Make the actual connection agree
            // with that, however we got there.
            //
            // This is what a "Join to Create" channel needs. You join the
            // lobby, a bot makes a channel and moves you, and the move is not
            // something this client asked for: the voice server details we
            // were given belong to the channel we are no longer in, and they
            // were used up connecting to it. The old code called
            // tryStartVoice() here, which found nothing left to use and
            // returned quietly - so the connection stayed pointed at a channel
            // we had left, Discord tore it down, and it looked like being
            // kicked straight back out.
            //
            // Being moved by a moderator is the same shape, and so is the
            // version where Discord sends the move as a leave followed by a
            // join rather than as one event.
            const bool alreadyThere = m_voice->state() == VoiceConnection::State::Connected
                && m_voice->channelId() == nowIn;
            if (alreadyThere)
                return;

            // A join we started is still being answered; its own events are on
            // the way and asking again would only duplicate it.
            if (m_voiceWatchdog.isActive() || !m_pendingVoiceToken.isEmpty()) {
                tryStartVoice();
                return;
            }

            // Nothing in flight and nothing to use, so ask for this channel
            // from the beginning. Discord answers with a fresh voice server.
            wlog(QStringLiteral("voice"),
                 QStringLiteral("we were moved %1-> %2; asking for that channel's voice server")
                     .arg(wasIn.isEmpty() ? QString() : QStringLiteral("from %1 ").arg(wasIn),
                          nowIn));

            m_voice->disconnectFromVoice();
            m_pendingVoiceToken.clear();
            m_pendingVoiceEndpoint.clear();

            AppConfig &config = AppConfig::instance();
            m_gateway->joinVoice(m_voiceGuildId, nowIn,
                                 config.value(QStringLiteral("voice/joinMuted"), false).toBool(),
                                 config.value(QStringLiteral("voice/joinDeafened"), false).toBool(),
                                 true);
            m_voiceWatchdog.start(10000);
        }
        return;
    }

    if (eventType == QLatin1String("STREAM_CREATE")) {
        const QString key = data.value(QStringLiteral("stream_key")).toString();

        // The same event answers both "let me watch that" and "let me go
        // live". Which one it is depends on whose key it carries.
        if (!m_myStreamKey.isEmpty() && key == m_myStreamKey) {
            m_myStreamServerId = data.value(QStringLiteral("rtc_server_id")).toString();
            m_myStreamChannelId = data.value(QStringLiteral("rtc_channel_id")).toString();
            wlog(QStringLiteral("share"),
                 QStringLiteral("our stream was accepted, server %1").arg(m_myStreamServerId));
            tryBeginBroadcast();
            return;
        }

        // Names the stream and the server that will carry it.
        if (key != m_streamKey)
            return;

        m_streamServerId = data.value(QStringLiteral("rtc_server_id")).toString();
        m_streamChannelId = data.value(QStringLiteral("rtc_channel_id")).toString();
        wlog(QStringLiteral("stream"),
             QStringLiteral("stream accepted, server %1 channel %2")
                 .arg(m_streamServerId, m_streamChannelId));
        tryStartStream();
        return;
    }

    if (eventType == QLatin1String("STREAM_SERVER_UPDATE")) {
        const QString key = data.value(QStringLiteral("stream_key")).toString();

        if (!m_myStreamKey.isEmpty() && key == m_myStreamKey) {
            m_myStreamToken = data.value(QStringLiteral("token")).toString();
            m_myStreamEndpoint = data.value(QStringLiteral("endpoint")).toString();
            wlog(QStringLiteral("share"),
                 QStringLiteral("our stream server: %1").arg(m_myStreamEndpoint));
            tryBeginBroadcast();
            return;
        }

        // The other half: where that server is, and the password for it.
        if (key != m_streamKey)
            return;

        m_streamToken = data.value(QStringLiteral("token")).toString();
        m_streamEndpoint = data.value(QStringLiteral("endpoint")).toString();
        wlog(QStringLiteral("stream"), QStringLiteral("stream server: %1").arg(m_streamEndpoint));
        tryStartStream();
        return;
    }

    if (eventType == QLatin1String("STREAM_DELETE")) {
        const QString goneKey = data.value(QStringLiteral("stream_key")).toString();

        // Discord ended ours - the call emptied, or it was stopped elsewhere.
        if (!m_myStreamKey.isEmpty() && goneKey == m_myStreamKey) {
            wlog(QStringLiteral("share"), QStringLiteral("Discord ended our stream"));
            stopScreenShare();
            return;
        }

        if (goneKey == m_streamKey) {
            const QString reason = data.value(QStringLiteral("reason")).toString();
            wlog(QStringLiteral("stream"),
                 QStringLiteral("the stream ended%1")
                     .arg(reason.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(reason)));
            flashStatus(reason.isEmpty()
                                         ? QStringLiteral("That stream ended.")
                                         : QStringLiteral("Stream ended: %1").arg(reason),
                                     4000);
            stopWatchingStream();
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

    if (eventType == QLatin1String("MESSAGE_REACTION_ADD")) {
        m_store->applyReaction(data, true, m_selfUserId);
        return;
    }

    if (eventType == QLatin1String("MESSAGE_REACTION_REMOVE")) {
        m_store->applyReaction(data, false, m_selfUserId);
        return;
    }

    if (eventType == QLatin1String("MESSAGE_REACTION_REMOVE_ALL")
        || eventType == QLatin1String("MESSAGE_REACTION_REMOVE_EMOJI")) {
        m_store->clearReactions(data);
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
    dmItem->setIcon(MediaCache::brandMark(GuildIconPixels));
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

    // A direct message has no members to list, and setGuild hides the panel
    // for that case rather than leaving an empty column standing there.
    if (m_members)
        m_members->setGuild(m_currentGuildId);

    // Start pulling the channels near the top of this server down before
    // anybody picks one.
    prefetchGuild(m_currentGuildId);

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

        item->setData(SingularityRoles::Status, m_store->presenceBubble(otherId));
        item->setData(SingularityRoles::Subtitle, describePresence(presence, m_store->activityHint(otherId)));

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
            item->setData(SingularityRoles::Status, m_store->presenceBubble(otherId));
            item->setData(SingularityRoles::Subtitle,
                          describePresence(presence, m_store->activityHint(otherId)));
        } else {
            item->setData(SingularityRoles::Status, QStringLiteral("offline"));
            item->setData(SingularityRoles::Subtitle,
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
            memberItem->setData(SingularityRoles::Streaming, state.streaming);
            memberItem->setData(SingularityRoles::Video, state.video);
            memberItem->setData(SingularityRoles::VoiceMuted, state.muted);
            memberItem->setData(SingularityRoles::VoiceDeafened, state.deafened);

            QStringList marks;
            if (state.streaming)
                marks << QStringLiteral("sharing a screen, which Singularity cannot show yet");
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
        friends->setData(IdRole, QStringLiteral("singularity:friends"));
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
    if (channelId == QLatin1String("singularity:friends")) {
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

    // Let go of channels nobody has been near for a while. Fetching ahead
    // fills the store with channels that were never opened, and without this
    // every one of them stayed for the rest of the session.
    m_store->trimHistories(channelId);

    // Anything the last channel had in flight belongs to the last channel.
    //
    // A history fetch that has not come back yet carries an anchor saying
    // "keep this many pixels above the bottom", and applying that to a
    // different conversation puts the reader somewhere arbitrary in it.
    m_anchorMessageId.clear();
    m_renderedIds.clear();
    m_loadingOlder = false;
    m_stickToBottom = true;
    m_readerMoved = false;

    // A new channel starts showing only its newest messages again.
    m_renderWindow = RenderWindowStep;
    m_renderedCount = 0;
    m_renderFirst = 0;
    m_windowAtTail = true;
    m_growCooldown.invalidate();
    m_scrolledSinceLoad = false;

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

    m_messageView->setHtml(loadingSkeletonHtml());

    // It is already on its way, because the pointer crossed this channel a
    // moment before the click did. Asking again would send a second request
    // for the same thing and arrive no sooner.
    if (m_prefetchInFlight && m_prefetchCurrent == channelId)
        return;

    // Anything queued for this channel is now being fetched properly, so it
    // does not need fetching twice.
    m_prefetchQueue.removeAll(channelId);
    const int limit =
        AppConfig::instance().value(QStringLiteral("appearance/historyLimit"), HistoryLimit).toInt();

    // How long Discord took to hand the messages over.
    //
    // A channel opened for the first time has to fetch, and a channel opened
    // again does not - which is exactly the difference somebody describes as
    // "it lags the first time". Whether that wait is the network or this
    // client's own drawing cannot be told apart by watching, so it is timed.
    auto started = std::make_shared<QElapsedTimer>();
    started->start();

    m_rest->fetchMessages(
        channelId, limit,
        [this, channelId, started](const QJsonArray &messages) {
            wlog(QStringLiteral("rest"),
                 QStringLiteral("history for %1 arrived in %2 ms (%3 messages)")
                     .arg(channelId)
                     .arg(started->elapsed())
                     .arg(messages.size()));
            m_store->setHistory(channelId, messages);
        },
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

// The shape of a conversation, before there is one.
//
// Opening a channel for the first time waits on Discord, and that wait was
// measured rather than guessed at: about 350 milliseconds of round trip
// whatever is asked for, plus a little for the messages themselves. No change
// to this client makes that faster - the request has to go to Discord and
// come back.
//
// What can change is what the half second looks like. A line of text saying
// "Loading messages..." reads as nothing happening; the shape of messages
// waiting to be filled in reads as something arriving. It is the same wait.
//
// Deliberately plain grey with no animation. Anything that moves here would
// be drawn by the same text engine that is about to lay out a hundred
// messages, and would be competing with the thing it is apologising for.
QString MainWindow::loadingSkeletonHtml()
{
    // Widths vary so it reads as speech rather than as a form. Fixed pixels
    // rather than percentages: the text engine treats a percentage inside a
    // nested table as a suggestion, and a bar that ignores it looks like a
    // fault rather than a placeholder.
    static const int nameWidths[] = {84, 62, 96, 70, 58, 88, 66, 92};
    static const int lineWidths[] = {320, 190, 410, 240, 150, 300, 360, 210};

    const QString block = QLatin1String(Theme::SurfaceInput);
    const QString faint = QLatin1String(Theme::SurfaceChat);

    QString html;
    html.reserve(4000);

    for (int i = 0; i < 8; ++i) {
        html += QStringLiteral(
                    "<table width=\"100%%\" cellspacing=\"0\" cellpadding=\"0\">"
                    "<tr>"
                    "<td width=\"52\" valign=\"top\">"
                    "<table cellspacing=\"0\" cellpadding=\"0\"><tr>"
                    "<td width=\"34\" height=\"34\" bgcolor=\"%1\"></td>"
                    "</tr></table>"
                    "</td>"
                    "<td valign=\"top\">"
                    "<table cellspacing=\"0\" cellpadding=\"0\"><tr>"
                    "<td width=\"%2\" height=\"11\" bgcolor=\"%3\"></td>"
                    "</tr></table>"
                    "<table cellspacing=\"0\" cellpadding=\"0\"><tr>"
                    "<td width=\"%4\" height=\"10\" bgcolor=\"%5\"></td>"
                    "</tr></table>"
                    "</td>"
                    "</tr></table>"
                    "<p style=\"line-height:6px\">&nbsp;</p>")
                    .arg(block)
                    .arg(nameWidths[i])
                    .arg(block)
                    .arg(lineWidths[i])
                    .arg(faint);
    }

    return html;
}

// ---------------------------------------------------------------------------
// Fetching ahead
// ---------------------------------------------------------------------------

void MainWindow::prefetchChannel(const QString &channelId)
{
    if (m_prefetchGaveUp || channelId.isEmpty())
        return;

    // Already here, already asked for, or already on the list.
    if (m_store->hasHistory(channelId) || m_prefetchAsked.contains(channelId))
        return;

    const ChannelInfo channel = m_store->channel(channelId);

    // Only the ones that hold messages. A voice channel or a category has no
    // history to fetch and asking for it spends the allowance for nothing.
    if (!channel.isTextLike())
        return;

    m_prefetchAsked.insert(channelId);
    m_prefetchQueue.append(channelId);
    pumpPrefetch();
}

void MainWindow::prefetchGuild(const QString &guildId)
{
    if (guildId.isEmpty())
        return;

    // The first handful only. Somebody opening a server looks at the top of
    // the list; fetching forty channels to cover the one they might pick is
    // how a client gets itself rate limited.
    int asked = 0;
    const QList<ChannelGroup> groups = m_store->groupedChannels(guildId);
    for (const ChannelGroup &group : groups) {
        for (const ChannelInfo &channel : group.channels) {
            if (asked >= 6)
                return;
            if (!channel.isTextLike())
                continue;
            prefetchChannel(channel.id);
            ++asked;
        }
    }
}

void MainWindow::pumpPrefetch()
{
    if (m_prefetchGaveUp || m_prefetchInFlight || m_prefetchQueue.isEmpty())
        return;

    // Never while a channel somebody actually opened is still waiting. A
    // guess must not stand in front of a request.
    if (m_loadingOlder)
        return;

    const QString channelId = m_prefetchQueue.takeFirst();
    if (m_store->hasHistory(channelId)) {
        pumpPrefetch();
        return;
    }

    m_prefetchInFlight = true;
    m_prefetchCurrent = channelId;

    // Half of what an opened channel asks for.
    //
    // A guess does not need to be as complete as a real request. The window
    // opens showing forty messages, so fifty covers the opening and the first
    // scroll, and the rest can be fetched if somebody actually goes looking.
    // Every message in the reply is parsed on this thread, and doing that for
    // a hundred of them across twenty five channels nobody opened is work
    // taken from the window while somebody is using it.
    const int configured =
        AppConfig::instance().value(QStringLiteral("appearance/historyLimit"), HistoryLimit).toInt();
    const int limit = qMin(configured, 50);

    m_rest->fetchMessages(
        channelId, limit,
        [this, channelId](const QJsonArray &messages) {
            m_prefetchInFlight = false;
            m_prefetchCurrent.clear();

            // Only if nothing else filled it in the meantime.
            if (!m_store->hasHistory(channelId))
                m_store->setHistory(channelId, messages);

            // Logged, because until now the log showed only the channels that
            // were fetched after a click - which made fetching ahead look like
            // it was doing nothing whenever it was in fact working.
            wlog(QStringLiteral("ui"),
                 QStringLiteral("fetched ahead: %1 (%2 messages)")
                     .arg(channelId)
                     .arg(messages.size()));

            m_prefetchTimer.start();
        },
        [this, channelId](const RestClient::Error &error) {
            m_prefetchInFlight = false;
            m_prefetchCurrent.clear();

            // Somebody opened this while it was on its way, and it failed. If
            // it is left as a skeleton for ever that is worse than the wait,
            // so the ordinary path takes over.
            if (channelId == m_currentChannelId && !m_store->hasHistory(channelId))
                openChannel(channelId);

            if (error.isRateLimit()) {
                // Stop entirely. Carrying on would spend the allowance that
                // the next channel somebody actually clicks will need.
                m_prefetchGaveUp = true;
                m_prefetchQueue.clear();
                wlog(QStringLiteral("prefetch"),
                     QStringLiteral("rate limited, fetching ahead is off for this session"));
                return;
            }

            // A channel we cannot read is not worth mentioning. It is the
            // ordinary case in a large server.
            m_prefetchTimer.start();
        });
}

void MainWindow::reportMemory()
{
    // The working set is what the task manager shows, so the log and the task
    // manager can be compared directly.
    qint64 workingSetMb = 0;
    qint64 privateMb = 0;
#ifdef Q_OS_WIN
    // Two numbers, because they answer different questions.
    //
    // The working set is what the task manager shows, and it includes pages
    // the graphics driver has mapped into the process for the window - which
    // the program never allocated and cannot free. Private bytes is what this
    // program actually asked for. If the two are far apart, the difference is
    // the graphics card, and no amount of caching less will move it.
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                             sizeof(counters))) {
        workingSetMb = qint64(counters.WorkingSetSize) / (1024 * 1024);
        privateMb = qint64(counters.PrivateUsage) / (1024 * 1024);
    }
#endif

    wlog(QStringLiteral("mem"),
         QStringLiteral("%1 MB in use (%2 MB ours); media: %3; store: %4")
             .arg(workingSetMb)
             .arg(privateMb)
             .arg(MediaCache::instance().summary(), m_store->memorySummary()));

#ifdef Q_OS_WIN
    // What the address space is actually made of.
    //
    // Media and messages come to sixty megabytes and the process is at five
    // hundred and eighty, so the rest is something nobody has counted. Adding
    // another cache counter would only be another guess; walking the address
    // space says what kind of memory it is, which narrows it to a cause.
    //
    //   heap     - memory this program asked for. Ours to fix.
    //   mapped   - shared with the graphics driver and the like. Usually not.
    //   code     - the program and its libraries. Fixed, and large here
    //              because of FFmpeg and Qt.
    //
    // A single huge block is a buffer somebody forgot. Many small ones are a
    // cache, or a leak of small objects.
    qint64 heapMb = 0;
    qint64 mappedMb = 0;
    qint64 codeMb = 0;
    qint64 biggestMb = 0;
    int bigBlocks = 0;

    MEMORY_BASIC_INFORMATION region{};
    for (const char *address = nullptr;
         VirtualQuery(address, &region, sizeof(region)) == sizeof(region);) {
        if (region.State == MEM_COMMIT) {
            const qint64 mb = qint64(region.RegionSize) / (1024 * 1024);
            switch (region.Type) {
            case MEM_PRIVATE:
                heapMb += mb;
                if (mb >= 8) {
                    ++bigBlocks;
                    biggestMb = qMax(biggestMb, mb);
                }
                break;
            case MEM_MAPPED:
                mappedMb += mb;
                break;
            case MEM_IMAGE:
                codeMb += mb;
                break;
            default:
                break;
            }
        }

        const char *next = static_cast<const char *>(region.BaseAddress) + region.RegionSize;
        if (next <= address)
            break;
        address = next;
    }

    wlog(QStringLiteral("mem"),
         QStringLiteral("  made of: %1 MB heap, %2 MB mapped, %3 MB code; "
                        "%4 blocks of 8 MB or more, biggest %5 MB")
             .arg(heapMb)
             .arg(mappedMb)
             .arg(codeMb)
             .arg(bigBlocks)
             .arg(biggestMb));
#endif
}

void MainWindow::scheduleRender()
{
    if (!m_renderTimer.isActive())
        m_renderTimer.start();
}

void MainWindow::renderChannel()
{
    // A redraw that has just happened cancels one that was waiting.
    m_renderTimer.stop();

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

    // Somebody reading part way up a channel keeps their place, whatever the
    // reason for this redraw - a reaction, an edit, a message deleted above
    // them, a picture that finally arrived. Every one of those changes the
    // height of the document, which is why restoring a pixel position put
    // them somewhere else.
    if (sameChannel && !wasAtBottom && m_anchorMessageId.isEmpty())
        captureScrollAnchor();

    const QList<MessageInfo> all = m_store->messages(m_currentChannelId);
    if (all.isEmpty()) {
        m_messageView->setHtml(QStringLiteral("<p class=\"system\">No messages here yet.</p>"));
        m_hasLastRendered = false;
        m_renderedCount = 0;
        m_renderedChannelId = m_currentChannelId;
        return;
    }

    // A window onto the conversation, which slides rather than grows.
    //
    // 0.5.3 drew only the newest forty, which worked, and then grew that
    // window every time somebody reached the top - with no ceiling. One
    // session went 40, 90, 150, 200, 250, 300, 350, and the redraw that had
    // come down to 22 ms was back up to 301. A window with no far edge is not
    // a window.
    //
    // So there are two numbers: how many to draw, which stops at
    // MaxRenderedMessages, and where to start, which moves back when somebody
    // reads further up. Past the ceiling the window slides: messages appear at
    // the top and leave at the bottom, and the cost of a redraw stops growing.
    const int held = int(all.size());
    int take = 0;
    int first = 0;
    if (!sameChannel) {
        // A channel opens on its newest messages.
        take = qMin(RenderWindowStep, held);
        first = held - take;
        m_windowAtTail = true;
    } else if (m_windowAtTail) {
        // Still following the end, so a redraw keeps the newest page, and
        // keeps it at least as long as what was already on screen.
        take = qMin(held, qMax(m_renderedCount, RenderWindowStep));
        take = qMin(take, MaxRenderedMessages);
        first = held - take;
    } else {
        // Reading backwards. The page stays where the reader left it.
        take = qMin(held, m_renderedCount > 0 ? m_renderedCount : RenderWindowStep);
        first = qBound(0, m_renderFirst, qMax(0, held - take));
    }

    const QList<MessageInfo> messages = all.mid(first, take);

    // Timed, in two halves, because they are fixed in completely different
    // ways and there is no telling them apart by feel. Building the markup is
    // our own code; laying it out is the text engine, and the only lever
    // there is how much is handed to it.
    QElapsedTimer clock;
    clock.start();

    QString html;
    html.reserve(messages.size() * 400);

    // The ids of what is drawn, in the order drawn. This is what lets a frame
    // in the document be named: the nth frame is this message.
    QStringList drawnIds;
    drawnIds.reserve(messages.size());

    MessageInfo previous;
    bool havePrevious = false;
    for (const MessageInfo &message : messages) {
        const bool grouped = havePrevious && shouldGroup(previous, message);
        html += messageHtml(message, grouped);
        drawnIds.append(message.id);
        previous = message;
        havePrevious = true;
    }

    const qint64 builtMs = clock.elapsed();

    m_lastRendered = previous;
    m_hasLastRendered = havePrevious;

    // Painting is held off until the position has been decided, so the reader
    // never sees the conversation at the wrong place for a frame.
    //
    // The scroll changes this causes are ours, not the reader's. Leaving them
    // unmarked made a redraw while reading history look like they had jumped
    // back to the newest message, and the next draw put that page back.
    m_messageView->setUpdatesEnabled(false);
    m_autoScrolling = true;
    m_messageView->setHtml(html);

    // The cursor goes to the end, and this is not housekeeping.
    //
    // Setting the text leaves the cursor at position zero, and the view then
    // scrolls to wherever the cursor is - which is the top, undoing the jump
    // to the bottom a moment after it was made. That is the other half of why
    // opening a channel landed at the very top; the first half was a fetch
    // that should never have started.
    QTextCursor end(m_messageView->document());
    end.movePosition(QTextCursor::End);
    m_messageView->setTextCursor(end);
    m_autoScrolling = false;

    const qint64 laidOutMs = clock.elapsed() - builtMs;

    // Threshold lowered from forty, which was hiding the answer.
    //
    // Forty made sense while a redraw cost sixty to two hundred milliseconds.
    // Now that most are under it, a log full of nothing says only that the
    // worst case improved - not what the ordinary case costs, which is what a
    // person actually feels while using the thing. One frame is sixteen
    // milliseconds, so anything over twelve is worth seeing.
    const int pictures = m_messageView->takePicturesPrepared();
    const qint64 pictureMs = m_messageView->takePictureWorkMs();

    if (builtMs + laidOutMs > 12) {
        wlog(QStringLiteral("ui"),
             QStringLiteral("drew %1 of %2 messages: %3 ms building, %4 ms laying out "
                            "(%5 of that on %6 pictures)")
                 .arg(messages.size())
                 .arg(all.size())
                 .arg(builtMs)
                 .arg(laidOutMs)
                 .arg(pictureMs)
                 .arg(pictures));
    }

    m_renderedCount = messages.size();
    m_renderFirst = first;
    m_windowAtTail = first + messages.size() >= held;
    m_renderedIds = drawnIds;
    m_renderedChannelId = m_currentChannelId;

    // The message that was under the top edge goes back under the top edge.
    //
    // This replaces three separate pieces of pixel arithmetic, each of which
    // was right about one case and wrong about the others: measuring from the
    // bottom was correct when messages arrived above, wrong when the window
    // slid and the bottom moved too, and wrong again when a picture arrived
    // and changed the height of something in between.
    if (restoreScrollAnchor()) {
        m_stickToBottom = false;
        m_messageView->setUpdatesEnabled(true);
        return;
    }

    // A freshly opened channel always starts at the newest message.
    //
    // Asking for the bottom once is not enough, and that is why opening a
    // channel landed half way up it. A rich text document is laid out lazily:
    // the moment after the html is set, the scroll bar's maximum describes
    // only the part that has been measured so far, and it keeps growing as
    // the rest is measured and as pictures arrive and take up room. Scrolling
    // to the bottom of a document that is still growing leaves you in the
    // middle of the finished one.
    //
    // So the intent is remembered instead, and the bottom is followed until
    // the reader takes over.
    if (!sameChannel || wasAtBottom) {
        m_stickToBottom = true;
        scrollToBottom();
    } else {
        m_stickToBottom = false;
        m_autoScrolling = true;
        bar->setValue(qMin(previousPosition, bar->maximum()));
        m_autoScrolling = false;
    }

    m_messageView->setUpdatesEnabled(true);
}

// Remember which message is at the top of the view, and where.
//
// Every jump in this client has come from keeping the reader's place as a
// number of pixels. Pixels are the wrong unit: the document changes height
// when messages are added above, removed below, edited, or when a picture
// finally arrives - and a pixel count means something different after any of
// those. Measuring from the bottom instead of the top only moved the problem
// to whichever end happened to change.
//
// A message does not have that problem. The one under the top edge of the
// view is the one somebody is reading, and putting it back where it was is
// the whole job, whatever happened to the document around it.
void MainWindow::captureScrollAnchor()
{
    m_anchorMessageId.clear();
    m_anchorOffset = 0;

    if (m_renderedIds.isEmpty() || m_renderedChannelId != m_currentChannelId)
        return;

    QTextDocument *document = m_messageView->document();
    const QList<QTextFrame *> rows = document->rootFrame()->childFrames();
    if (rows.size() != m_renderedIds.size())
        return;

    const int value = m_messageView->verticalScrollBar()->value();

    for (int i = 0; i < rows.size(); ++i) {
        const QRectF box = document->documentLayout()->frameBoundingRect(rows.at(i));
        if (box.bottom() <= value)
            continue;

        m_anchorMessageId = m_renderedIds.at(i);
        m_anchorOffset = qRound(box.top()) - value;
        return;
    }
}

bool MainWindow::restoreScrollAnchor()
{
    if (m_anchorMessageId.isEmpty())
        return false;

    const int index = m_renderedIds.indexOf(m_anchorMessageId);
    m_anchorMessageId.clear();
    if (index < 0)
        return false;

    QTextDocument *document = m_messageView->document();
    const QList<QTextFrame *> rows = document->rootFrame()->childFrames();
    if (index >= rows.size())
        return false;

    const QRectF box = document->documentLayout()->frameBoundingRect(rows.at(index));

    QScrollBar *bar = m_messageView->verticalScrollBar();
    m_autoScrolling = true;
    bar->setValue(qBound(0, qRound(box.top()) - m_anchorOffset, bar->maximum()));
    m_autoScrolling = false;
    return true;
}

void MainWindow::scrollToBottom()
{
    QScrollBar *bar = m_messageView->verticalScrollBar();

    // Guarded, because moving the bar raises valueChanged, and the handler
    // that notices the reader scrolling away would otherwise read our own
    // move as theirs and immediately stop following.
    m_autoScrolling = true;
    bar->setValue(bar->maximum());
    m_autoScrolling = false;
}

// Redraw a single message where it sits.
//
// Every message is written as one <table>, and Qt turns each table into a
// frame of the document. So the frames under the root frame line up one for
// one with the messages on screen, in the same order, and a frame knows its
// own first and last position however much text has been added above it.
//
// That makes an exact swap possible: select the frame, delete it, insert the
// new markup in its place. Only the blocks that changed are laid out again.
//
// Returns false whenever anything does not add up, and the caller then falls
// back to the full redraw. Nothing here is allowed to leave a half edited
// document on screen.
bool MainWindow::replaceMessageInView(const QString &messageId)
{
    if (m_currentChannelId.isEmpty() || m_renderedChannelId != m_currentChannelId)
        return false;

    const QList<MessageInfo> messages = m_store->messages(m_currentChannelId);
    int index = -1;
    for (int i = 0; i < messages.size(); ++i) {
        if (messages.at(i).id == messageId) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return false;

    QTextDocument *document = m_messageView->document();
    const QList<QTextFrame *> rows = document->rootFrame()->childFrames();

    // The frames match the messages that were drawn, which is the newest
    // m_renderedCount of them, not all the ones held. If that does not hold -
    // a system line, an empty channel, a redraw that has not happened yet -
    // there is no way to know which frame is which, so do not guess.
    if (rows.size() != m_renderedCount)
        return false;

    // A message outside the drawn window is not on screen, so there is
    // nothing to redraw and nothing to rebuild either.
    if (index < m_renderFirst || index >= m_renderFirst + m_renderedCount)
        return true;

    QTextFrame *frame = rows.at(index - m_renderFirst);
    const int from = frame->firstPosition() - 1;
    const int to = frame->lastPosition() + 1;
    if (from < 0 || to > document->characterCount())
        return false;

    const bool grouped = index > 0 && shouldGroup(messages.at(index - 1), messages.at(index));

    QScrollBar *bar = m_messageView->verticalScrollBar();
    const bool wasAtBottom = m_stickToBottom || bar->value() >= bar->maximum() - 8;
    const int previousPosition = bar->value();

    m_messageView->setUpdatesEnabled(false);

    QTextCursor cursor(document);
    cursor.beginEditBlock();
    cursor.setPosition(from);
    cursor.setPosition(to, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.insertHtml(messageHtml(messages.at(index), grouped));
    cursor.endEditBlock();

    if (wasAtBottom) {
        m_stickToBottom = true;
        scrollToBottom();
    } else {
        m_autoScrolling = true;
        bar->setValue(qMin(previousPosition, bar->maximum()));
        m_autoScrolling = false;
    }

    m_messageView->setUpdatesEnabled(true);

    // The last message may have been the one rewritten, so the record of it
    // that grouping uses has to follow.
    m_lastRendered = messages.last();
    m_hasLastRendered = true;
    return true;
}

void MainWindow::appendMessageToView(const MessageInfo &message, bool grouped)
{
    // Somebody reading back through history is not looking at the newest
    // message, and the window is not showing it. Adding it to the bottom of
    // what they are reading would put it in the wrong place; it appears when
    // they come back down, which is when the window returns to the end.
    if (!m_windowAtTail)
        return;

    QScrollBar *bar = m_messageView->verticalScrollBar();
    const bool wasAtBottom = bar->value() >= bar->maximum() - 40;

    m_autoScrolling = true;
    QTextCursor cursor(m_messageView->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertHtml(messageHtml(message, grouped));
    m_autoScrolling = false;

    // One more message is on screen than a moment ago, and the drawn window
    // has to say so or the next redraw would leave it out.
    ++m_renderedCount;
    m_renderedIds.append(message.id);
    m_renderWindow = qMin(MaxRenderedMessages, m_renderWindow + 1);

    if (m_renderedCount > MaxRenderedMessages)
        trimRenderedStart(m_renderedCount - (MaxRenderedMessages - RenderWindowStep));

    if (wasAtBottom) {
        m_stickToBottom = true;
        scrollToBottom();
    }
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
            body += QStringLiteral("<div class=\"attach\"><a href=\"singularity-image:%1\">"
                                   "<img src=\"%1\"></a></div>")
                        .arg(safeUrl);
        } else {
            body += QStringLiteral("<div class=\"file\"><a href=\"%1\">%2</a>%3</div>")
                        .arg(safeUrl, label, humanSize(attachment.size));
        }
    }

    body += stickersHtml(message);
    body += embedsHtml(message);
    body += reactionsHtml(message);

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
    const QString profileLink = QStringLiteral("singularity-user:%1").arg(message.authorId);

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
        html += QStringLiteral("<div class=\"attach\"><a href=\"singularity-image:%1\">"
                               "<img src=\"%1\" alt=\"%2\" title=\"%2\"></a></div>")
                    .arg(url, sticker.name.toHtmlEscaped());
    }
    return html;
}

// The row of pills under a message.
//
// Qt's rich text has no flexbox, so these are laid out as inline spans with a
// non-breaking space between them. Custom emoji come through as pictures from
// Discord's own host, the same as everywhere else in the client.
QString MainWindow::reactionsHtml(const MessageInfo &message) const
{
    if (message.reactions.isEmpty())
        return {};

    const bool animate =
        AppConfig::instance().value(QStringLiteral("appearance/animate"), true).toBool();

    QString html = QStringLiteral("<div class=\"reactions\">");

    for (const ReactionInfo &reaction : message.reactions) {
        QString face;
        if (reaction.isCustom()) {
            const QString ext = (reaction.animated && animate) ? QStringLiteral("gif")
                                                               : QStringLiteral("png");
            face = QStringLiteral("<img src=\"https://cdn.discordapp.com/emojis/%1.%2?size=32\" "
                                  "width=\"16\" height=\"16\">")
                       .arg(reaction.id, ext);
        } else {
            face = reaction.name.toHtmlEscaped();
        }

        // Yours is marked, the way the real client outlines one you pressed.
        const char *background = reaction.mine ? Theme::SurfaceHover : Theme::SurfaceInput;
        const char *ink = reaction.mine ? Theme::TextPrimary : Theme::TextMuted;

        html += QStringLiteral("<span style=\"background-color: %1; color: %2; "
                               "border-radius: 8px; padding: 2px 7px;\">%3&#160;%4</span>&#160;")
                    .arg(QLatin1String(background), QLatin1String(ink), face)
                    .arg(reaction.count);
    }

    html += QStringLiteral("</div>");
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
            html += QStringLiteral("<div class=\"attach\"><a href=\"singularity-image:%1\">"
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
            inner += QStringLiteral("<div class=\"attach\"><a href=\"singularity-image:%1\">"
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

// The four things Discord lets you claim to be, and your profile under them.
//
// The dots are drawn rather than written, because the colours are the one part
// of this client the theme never touches: green, yellow and red have to keep
// meaning online, away and busy whatever colour everything else becomes.
void MainWindow::showStatusMenu()
{
    if (m_selfUserId.isEmpty())
        return;

    struct Choice {
        const char *id;
        const char *label;
        const char *colour;
        const char *note;
    };

    static const Choice kChoices[] = {
        {"online", "Online", Theme::Green, ""},
        {"idle", "Idle", Theme::Yellow, ""},
        {"dnd", "Do Not Disturb", Theme::Red, "You will not be pinged"},
        {"invisible", "Invisible", Theme::TextFaint,
         "You will look offline to everyone, but Discord still knows you are here"},
    };

    const QString current = m_gateway->presenceStatus();

    QMenu menu(this);
    menu.setAttribute(Qt::WA_TranslucentBackground, false);

    for (const Choice &choice : kChoices) {
        const QString id = QString::fromLatin1(choice.id);

        // A small filled circle in the right colour, painted here rather than
        // shipped as five more image files.
        QPixmap dot(12, 12);
        dot.fill(Qt::transparent);
        {
            QPainter painter(&dot);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QLatin1String(choice.colour)));
            painter.drawEllipse(1, 1, 10, 10);
        }

        QAction *action = menu.addAction(QIcon(dot), QString::fromLatin1(choice.label));
        action->setCheckable(true);
        action->setChecked(current == id);

        const QString note = QString::fromLatin1(choice.note);
        if (!note.isEmpty())
            action->setToolTip(note);

        connect(action, &QAction::triggered, this, [this, id]() { setPresenceStatus(id); });
    }

    menu.addSeparator();
    QAction *profile = menu.addAction(QStringLiteral("View profile"));
    connect(profile, &QAction::triggered, this, [this]() {
        const QPoint corner = m_userPanel->mapToGlobal(QPoint(m_userPanel->width() + 6, -200));
        showProfile(m_selfUserId, corner);
    });

    // Opened upwards from the panel, which sits at the very bottom of the
    // sidebar: a menu dropped downwards from there would be off the screen.
    const QPoint at = m_userPanel->mapToGlobal(QPoint(8, 0));
    menu.exec(QPoint(at.x(), at.y() - menu.sizeHint().height() - 4));
}

void MainWindow::setPresenceStatus(const QString &status)
{
    m_gateway->setPresenceStatus(status);
    AppConfig::instance().setValue(QStringLiteral("presence/status"), status);

    wlog(QStringLiteral("ui"), QStringLiteral("status set to %1").arg(status));
    updateUserPanel();
}

void MainWindow::updateUserPanel()
{
    if (!m_selfName)
        return;

    // What you chose, not what the connection is doing. Those are different
    // questions and were previously answered by the same line: somebody who
    // had set themselves invisible still read "online" here, which is the one
    // place it matters that it says otherwise.
    if (m_selfStatus && m_gateway->state() == GatewayClient::State::Ready) {
        const QString status = m_gateway->presenceStatus();
        QString label = status;
        QString colour = Theme::Green;

        if (status == QLatin1String("idle")) {
            label = QStringLiteral("idle");
            colour = Theme::Yellow;
        } else if (status == QLatin1String("dnd")) {
            label = QStringLiteral("do not disturb");
            colour = Theme::Red;
        } else if (status == QLatin1String("invisible")) {
            label = QStringLiteral("invisible");
            colour = Theme::TextFaint;
        } else {
            label = QStringLiteral("online");
        }

        m_selfStatus->setText(label);
        m_selfStatus->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(colour));
    }

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
    if (url.scheme() == QLatin1String("singularity-user")) {
        const QString userId = url.path().isEmpty() ? whole.mid(10) : url.path();
        showProfile(userId, QCursor::pos());
        return;
    }

    // A picture in chat opens the big view instead of a browser. The address
    // is taken from the raw text, because a full web address inside another
    // address does not survive being parsed into parts.
    if (url.scheme() == QLatin1String("singularity-image")) {
        const QString target = whole.mid(QStringLiteral("singularity-image:").size());
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
        flashStatus(QStringLiteral("A plugin cancelled that message."), 4000);
        m_composer->clear();
        return;
    }

    if (content.length() > 2000) {
        flashStatus(QStringLiteral("Discord caps messages at 2000 characters."), 5000);
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
            flashStatus(
                QStringLiteral("Send failed (%1). Your text: %2").arg(reason, content.left(60)), 8000);
        });
}

// Which window edges a point is close enough to count as grabbing.
//
// Ten pixels rather than the four or so Windows uses for a frame it draws
// itself. That frame has a visible edge to aim at; this one does not, so the
// target has to be wide enough to find by waving at the corner.
Qt::Edges MainWindow::edgesAt(const QPoint &pos) const
{
    constexpr int grab = 10;

    Qt::Edges edges;
    if (pos.x() <= grab)
        edges |= Qt::LeftEdge;
    if (pos.x() >= m_aurora->width() - grab)
        edges |= Qt::RightEdge;
    if (pos.y() <= grab)
        edges |= Qt::TopEdge;
    if (pos.y() >= m_aurora->height() - grab)
        edges |= Qt::BottomEdge;
    return edges;
}

Qt::CursorShape MainWindow::cursorForEdges(Qt::Edges edges)
{
    if ((edges & Qt::TopEdge && edges & Qt::LeftEdge)
        || (edges & Qt::BottomEdge && edges & Qt::RightEdge)) {
        return Qt::SizeFDiagCursor;
    }
    if ((edges & Qt::TopEdge && edges & Qt::RightEdge)
        || (edges & Qt::BottomEdge && edges & Qt::LeftEdge)) {
        return Qt::SizeBDiagCursor;
    }
    if (edges & (Qt::LeftEdge | Qt::RightEdge))
        return Qt::SizeHorCursor;
    if (edges & (Qt::TopEdge | Qt::BottomEdge))
        return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // A person scrolling the conversation, told apart from the document
    // moving on its own. Watched, never swallowed.
    if (m_messageView
        && (watched == m_messageView->viewport()
            || watched == m_messageView->verticalScrollBar())) {
        switch (event->type()) {
        case QEvent::Wheel:
        case QEvent::MouseButtonPress:
        case QEvent::KeyPress:
        case QEvent::TouchBegin:
            m_readerMoved = true;
            m_scrolledSinceLoad = true;
            break;
        default:
            break;
        }
    }

    // Dragging and resizing the window without a frame.
    //
    // The black hole covers every pixel, so it is what the mouse lands on
    // anywhere the interface has not put a card. That makes it the right
    // place to notice a press near an edge and hand the resize to Windows.
    // Both of these go through the window manager rather than moving the
    // window by hand: snapping to edges and to half the screen come with it,
    // and neither can be imitated by setting a position.
    if (watched == m_aurora && !isMaximized() && !isFullScreen()) {
        if (event->type() == QEvent::MouseMove) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            const Qt::Edges edges = edgesAt(mouse->position().toPoint());
            m_aurora->setCursor(cursorForEdges(edges));
        }

        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            const Qt::Edges edges = edgesAt(mouse->position().toPoint());
            if (mouse->button() == Qt::LeftButton && edges) {
                if (QWindow *handle = windowHandle()) {
                    handle->startSystemResize(edges);
                    return true;
                }
            }
        }

        if (event->type() == QEvent::Leave)
            m_aurora->unsetCursor();
    }

    // Clicking your own panel offers your status, and your profile under it.
    if (watched == m_userPanel && event->type() == QEvent::MouseButtonRelease) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton && !m_selfUserId.isEmpty()) {
            showStatusMenu();
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
// Fetches the batch of messages older than the oldest one on screen.
//
// Discord answers "before this id", so the oldest message being held is the
// starting point. Coming back empty, or with nothing that was not already
// there, means the beginning of the channel, and that is remembered so the
// same request is not made again every time somebody scrolls up.
// Asks to watch somebody's shared screen.
//
// You have to be in the call first, and you have to stay in it: the stream
// rides alongside the voice connection rather than replacing it.
void MainWindow::watchStream(const QString &userId)
{
    if (userId.isEmpty() || m_voiceChannelId.isEmpty())
        return;

    if (m_watchingUserId == userId)
        return;

    stopWatchingStream();

    m_watchingUserId = userId;
    if (m_callView)
        m_callView->setFocusedUser(userId, CallView::Surface::Share);
    m_streamKey = GatewayClient::streamKeyFor(m_voiceGuildId, m_voiceChannelId, userId);
    m_streamServerId.clear();
    m_streamChannelId.clear();
    m_streamToken.clear();
    m_streamEndpoint.clear();

    const QString name = m_store->userName(userId);
    flashStatus(
        QStringLiteral("Asking to watch %1...").arg(name.isEmpty() ? userId : name), 5000);

    m_gateway->watchStream(m_streamKey);
    updateVoicePanel();
}

void MainWindow::stopWatchingStream()
{
    if (m_streamKey.isEmpty())
        return;

    m_gateway->stopWatchingStream(m_streamKey);

    if (m_streamVoice)
        m_streamVoice->disconnectFromVoice();
    if (m_callView)
        m_callView->dropFrames(m_watchingUserId, CallView::Surface::Share);

    m_watchingUserId.clear();
    m_streamKey.clear();
    m_streamServerId.clear();
    m_streamChannelId.clear();
    m_streamToken.clear();
    m_streamEndpoint.clear();
    updateVoicePanel();
}

void MainWindow::applyBackgroundSettings()
{
    if (!m_aurora)
        return;

    AppConfig &config = AppConfig::instance();

    const QString path = config.value(QStringLiteral("appearance/backgroundPath")).toString();
    const int dim = config.value(QStringLiteral("appearance/backgroundDim"), 45).toInt();
    const bool wantsPicture =
        config.value(QStringLiteral("appearance/backgroundMode")).toString()
            == QLatin1String("picture");

    // A picture that has been deleted or moved since it was chosen falls back
    // to the hole rather than to black, so a missing file looks like the
    // program's own design instead of a fault.
    const bool usable = wantsPicture && !path.isEmpty() && QFileInfo::exists(path);
    if (wantsPicture && !usable && !path.isEmpty()) {
        wlog(QStringLiteral("theme"),
             QStringLiteral("the chosen background is no longer there: %1").arg(path));
    }

    m_aurora->setBackgroundPicture(usable ? path : QString(), dim);
    m_aurora->setBackgroundMode(usable ? AuroraWidget::Background::Picture
                                       : AuroraWidget::Background::Hole);
}

// ---------------------------------------------------------------------------
// Sharing our own screen
// ---------------------------------------------------------------------------

void MainWindow::startScreenShare()
{
    if (m_voiceChannelId.isEmpty()) {
        flashStatus(QStringLiteral("Join a voice channel first."), 4000);
        return;
    }

    if (!m_myStreamKey.isEmpty()) {
        stopScreenShare();
        return;
    }

    ShareDialog dialog(this);
    if (!dialog.hasScreens()) {
        QMessageBox::warning(this, QStringLiteral("Nothing to share"),
                             QStringLiteral("Windows reported no screen that can be captured."));
        return;
    }
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_shareMonitorId = dialog.monitorId();
    m_shareWidth = dialog.width();
    m_shareHeight = dialog.height();
    m_shareFps = dialog.frameRate();
    m_shareBitrate = dialog.bitrate();

    if (m_shareMonitorId.isEmpty())
        return;

    // The key Discord will use for our stream is the one naming us, and it is
    // predictable, so the events can be matched the moment they arrive.
    m_myStreamKey = GatewayClient::streamKeyFor(m_voiceGuildId, m_voiceChannelId, m_selfUserId);
    m_myStreamServerId.clear();
    m_myStreamChannelId.clear();
    m_myStreamToken.clear();
    m_myStreamEndpoint.clear();

    flashStatus(QStringLiteral("Starting your screen share..."), 5000);
    m_gateway->startStream(m_voiceGuildId, m_voiceChannelId);
    updateVoicePanel();
}

void MainWindow::stopScreenShare()
{
    if (m_myStreamKey.isEmpty())
        return;

    if (m_share)
        m_share->stop();
    if (m_callView && !m_selfUserId.isEmpty())
        m_callView->dropFrames(m_selfUserId, CallView::Surface::Share);
    if (m_shareVoice) {
        m_shareVoice->stopSendingVideo();
        m_shareVoice->disconnectFromVoice();
    }

    m_gateway->stopStream(m_myStreamKey);

    m_myStreamKey.clear();
    m_myStreamServerId.clear();
    m_myStreamChannelId.clear();
    m_myStreamToken.clear();
    m_myStreamEndpoint.clear();

    flashStatus(QStringLiteral("Your screen share ended."), 3000);
    updateVoicePanel();
}

// Both halves arrive separately and in no fixed order, exactly as they do for
// watching. This runs on each and acts once everything is in hand.
void MainWindow::tryBeginBroadcast()
{
    if (m_myStreamServerId.isEmpty() || m_myStreamToken.isEmpty() || m_myStreamEndpoint.isEmpty())
        return;

    const QString sessionId = m_gateway->sessionId();
    if (sessionId.isEmpty())
        return;

    // Same rule as the viewer path: a Go Live stream's key group is the
    // media-session id, which is one less than the stream's rtc server id.
    quint64 daveGroupId = 0;
    const quint64 serverId = m_myStreamServerId.toULongLong();
    if (serverId > 0)
        daveGroupId = serverId - 1;

    const QString channelId =
        m_myStreamChannelId.isEmpty() ? m_voiceChannelId : m_myStreamChannelId;

    wlog(QStringLiteral("share"),
         QStringLiteral("opening our stream server %1 (group %2)")
             .arg(m_myStreamEndpoint)
             .arg(daveGroupId));

    m_shareVoice->connectToVoice(m_myStreamServerId, channelId, m_selfUserId, sessionId,
                                 m_myStreamToken, m_myStreamEndpoint, daveGroupId);

    m_myStreamToken.clear();
    m_myStreamEndpoint.clear();
}

// Both halves of a stream's details arrive as separate events, in no fixed
// order, exactly like a voice connection. This runs on each and only acts once
// everything is in hand.
void MainWindow::tryStartStream()
{
    if (m_streamServerId.isEmpty() || m_streamToken.isEmpty() || m_streamEndpoint.isEmpty())
        return;

    const QString sessionId = m_gateway->sessionId();
    if (sessionId.isEmpty())
        return;

    // A Go Live stream has its own MLS group. Discord uses the media-session
    // id, which is one less than the stream's rtc server id, not the voice
    // channel id the call underneath is using.
    quint64 daveGroupId = 0;
    const quint64 serverId = m_streamServerId.toULongLong();
    if (serverId > 0)
        daveGroupId = serverId - 1;

    const QString channelId = m_streamChannelId.isEmpty() ? m_voiceChannelId : m_streamChannelId;

    wlog(QStringLiteral("stream"),
         QStringLiteral("opening the stream server %1 (group %2)")
             .arg(m_streamEndpoint)
             .arg(daveGroupId));

    m_streamVoice->connectToVoice(m_streamServerId, channelId, m_selfUserId, sessionId,
                                  m_streamToken, m_streamEndpoint, daveGroupId);

    m_streamToken.clear();
    m_streamEndpoint.clear();
}

// Inserts messages into the document and moves the scrollbar by the height
// that appeared, so the messages already on screen do not jump.
//
// `above` means the new rows belong at the top. The scrollbar has to move
// down by their height, or the reader is thrown to the new top and the page
// they were reading disappears. Below, the scrollbar is already right.
bool MainWindow::insertMessagesIntoView(int storeIndex, int count, bool above)
{
    if (count <= 0 || m_currentChannelId.isEmpty())
        return false;

    const QList<MessageInfo> all = m_store->messages(m_currentChannelId);
    if (storeIndex < 0 || storeIndex + count > all.size())
        return false;

    if (m_renderedCount > 0) {
        const bool contiguous = above
            ? (storeIndex + count == m_renderFirst)
            : (storeIndex == m_renderFirst + m_renderedCount);
        if (!contiguous)
            return false;
    }

    QString html;
    html.reserve(count * 400);
    QStringList ids;
    ids.reserve(count);

    MessageInfo previous;
    bool havePrevious = false;
    if (!above && m_renderedCount > 0 && storeIndex > 0) {
        previous = all.at(storeIndex - 1);
        havePrevious = true;
    }

    for (int i = 0; i < count; ++i) {
        const MessageInfo &message = all.at(storeIndex + i);
        const bool grouped = havePrevious && shouldGroup(previous, message);
        html += messageHtml(message, grouped);
        ids.append(message.id);
        previous = message;
        havePrevious = true;
    }

    QTextDocument *document = m_messageView->document();
    QScrollBar *bar = m_messageView->verticalScrollBar();
    const int value = bar->value();
    const int heightBefore = qRound(document->size().height());

    // Remember the line on screen before the document changes, in case the
    // insert cannot be kept and the page has to be drawn again.
    captureScrollAnchor();

    m_messageView->setUpdatesEnabled(false);
    m_autoScrolling = true;

    QTextCursor cursor(document);
    cursor.beginEditBlock();
    if (above)
        cursor.movePosition(QTextCursor::Start);
    else
        cursor.movePosition(QTextCursor::End);
    cursor.insertHtml(html);
    cursor.endEditBlock();

    if (above) {
        m_renderedIds = ids + m_renderedIds;
        m_renderFirst = storeIndex;
    } else {
        for (const QString &id : ids)
            m_renderedIds.append(id);
    }
    m_renderedCount += count;
    m_windowAtTail = m_renderFirst + m_renderedCount >= all.size();

    const QList<QTextFrame *> rows = document->rootFrame()->childFrames();
    const bool framesMatch = rows.size() == m_renderedCount;

    const int delta = qRound(document->size().height()) - heightBefore;
    if (above && delta > 0)
        bar->setValue(qBound(0, value + delta, bar->maximum()));

    m_autoScrolling = false;
    m_stickToBottom = false;
    m_messageView->setUpdatesEnabled(true);

    if (!framesMatch) {
        // The rows and the messages have to stay in the same order or a
        // later edit cannot find its frame. Draw the page recorded above
        // instead, and keep the reader on the line they were already on.
        wlog(QStringLiteral("ui"),
             QStringLiteral("history insert left %1 frames for %2 messages; redrawing the page")
                 .arg(rows.size())
                 .arg(m_renderedCount));
        if (above)
            m_windowAtTail = false;
        renderChannel();
        return true;
    }

    m_anchorMessageId.clear();

    // The message that used to be first may now belong to the one above it,
    // and was drawn with a header it should no longer have.
    if (above && m_renderedCount > count) {
        const MessageInfo &lastNew = all.at(storeIndex + count - 1);
        const MessageInfo &oldFirst = all.at(storeIndex + count);
        if (shouldGroup(lastNew, oldFirst))
            replaceMessageInView(oldFirst.id);
    }

    wlog(QStringLiteral("ui"),
         QStringLiteral("showing messages %1-%2 of %3")
             .arg(m_renderFirst + 1)
             .arg(m_renderFirst + m_renderedCount)
             .arg(all.size()));
    return true;
}

bool MainWindow::prependMessagesToView(int storeIndex, int count)
{
    return insertMessagesIntoView(storeIndex, count, true);
}

bool MainWindow::appendMessagesToView(int storeIndex, int count)
{
    return insertMessagesIntoView(storeIndex, count, false);
}

void MainWindow::trimRenderedEnd(int keep)
{
    if (keep < 1 || m_renderedCount <= keep)
        return;

    QTextDocument *document = m_messageView->document();
    const QList<QTextFrame *> rows = document->rootFrame()->childFrames();
    if (rows.size() != m_renderedCount)
        return;

    const int from = qMax(0, rows.at(keep)->firstPosition() - 1);
    m_autoScrolling = true;
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    cursor.setPosition(from);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.endEditBlock();
    m_autoScrolling = false;

    while (m_renderedIds.size() > keep)
        m_renderedIds.removeLast();
    m_renderedCount = keep;
    m_windowAtTail = false;
}

void MainWindow::trimRenderedStart(int drop)
{
    if (drop <= 0 || drop >= m_renderedCount)
        return;

    QTextDocument *document = m_messageView->document();
    const QList<QTextFrame *> rows = document->rootFrame()->childFrames();
    if (rows.size() != m_renderedCount)
        return;

    const int heightBefore = qRound(document->size().height());
    const int to = rows.at(drop - 1)->lastPosition() + 1;

    m_autoScrolling = true;
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    cursor.setPosition(0);
    cursor.setPosition(qMin(to, document->characterCount()), QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.endEditBlock();

    const int delta = heightBefore - qRound(document->size().height());
    QScrollBar *bar = m_messageView->verticalScrollBar();
    bar->setValue(qMax(0, bar->value() - delta));
    m_autoScrolling = false;

    m_renderFirst += drop;
    m_renderedCount -= drop;
    m_renderedIds = m_renderedIds.mid(drop);
}

// Somebody has scrolled to the top and wants what is above.
//
// The messages already held are put in above the ones on screen, and the
// scrollbar moves by their height so the reader stays on the line they were
// reading. They then scroll up into the new lines. Replacing the whole page
// and pinning the old top line back to the top is what made every scroll
// land on the same messages.
void MainWindow::reachedTop()
{
    if (m_currentChannelId.isEmpty() || m_loadingOlder)
        return;

    if (m_growCooldown.isValid() && m_growCooldown.elapsed() < 200)
        return;

    if (!m_scrolledSinceLoad)
        return;

    const QList<MessageInfo> all = m_store->messages(m_currentChannelId);
    const int held = all.size();
    if (held == 0 || m_renderedCount == 0)
        return;

    m_scrolledSinceLoad = false;
    m_growCooldown.restart();

    if (m_renderFirst > 0) {
        const int count = qMin(RenderWindowStep, m_renderFirst);
        if (!prependMessagesToView(m_renderFirst - count, count)) {
            m_renderFirst = qMax(0, m_renderFirst - count);
            m_windowAtTail = false;
            captureScrollAnchor();
            renderChannel();
        }
        if (m_renderedCount > MaxRenderedMessages)
            trimRenderedEnd(MaxRenderedMessages - RenderWindowStep);
        return;
    }

    loadOlderMessages();
}

void MainWindow::revealNewer()
{
    if (m_currentChannelId.isEmpty() || m_windowAtTail)
        return;

    const int held = m_store->messages(m_currentChannelId).size();
    const int drawnEnd = m_renderFirst + m_renderedCount;
    if (drawnEnd >= held) {
        m_windowAtTail = true;
        return;
    }

    if (m_growCooldown.isValid() && m_growCooldown.elapsed() < 200)
        return;
    m_growCooldown.restart();

    const int count = qMin(RenderWindowStep, held - drawnEnd);
    if (!appendMessagesToView(drawnEnd, count)) {
        m_windowAtTail = m_renderFirst + m_renderedCount >= held;
        captureScrollAnchor();
        renderChannel();
        return;
    }

    if (m_renderedCount > MaxRenderedMessages)
        trimRenderedStart(m_renderedCount - (MaxRenderedMessages - RenderWindowStep));
}

void MainWindow::loadOlderMessages()
{
    if (m_loadingOlder || m_currentChannelId.isEmpty())
        return;
    if (m_fullyLoaded.contains(m_currentChannelId))
        return;

    const QString oldest = m_store->oldestMessageId(m_currentChannelId);
    if (oldest.isEmpty())
        return;

    m_loadingOlder = true;
    const QString channelId = m_currentChannelId;

    m_rest->fetchMessages(
        channelId, 50,
        [this, channelId](const QJsonArray &messages) {
            m_loadingOlder = false;

            // The store signal would otherwise rebuild the channel from the
            // newest message, which is the scroll repeating itself.
            m_applyingHistory = true;
            const int added = m_store->prependHistory(channelId, messages);
            m_applyingHistory = false;

            if (added > 0 && channelId == m_currentChannelId) {
                // The page already on screen shifted down by the new rows.
                m_renderFirst += added;
                const int count = qMin(added, RenderWindowStep);
                const int index = m_renderFirst - count;
                if (!prependMessagesToView(index, count)) {
                    m_renderFirst = index;
                    m_windowAtTail = false;
                    captureScrollAnchor();
                    renderChannel();
                }
                if (m_renderedCount > MaxRenderedMessages)
                    trimRenderedEnd(MaxRenderedMessages - RenderWindowStep);
            }

            if (added == 0) {
                m_fullyLoaded.insert(channelId);
                if (channelId == m_currentChannelId) {
                    flashStatus(QStringLiteral("That is the beginning of this channel."),
                                             4000);
                }
            }
        },
        [this, channelId](const RestClient::Error &error) {
            m_loadingOlder = false;
            m_anchorMessageId.clear();
            wlog(QStringLiteral("rest"), QStringLiteral("older messages failed for %1: HTTP %2")
                                             .arg(channelId)
                                             .arg(error.httpStatus));
        },
        oldest);
}

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
    stopWatchingStream();
    m_voice->disconnectFromVoice();
    m_pendingVoiceToken.clear();
    m_pendingVoiceEndpoint.clear();
    m_voiceSessionId.clear();
    m_speakingUsers.clear();

    m_voiceRetries = 0;
    m_voiceRetryTimer.stop();

    m_voiceChannelId = channelId;
    m_voiceGuildId = channel.guildId;
    m_rejoinVoiceAfterGateway = false;
    m_gateway->joinVoice(channel.guildId, channelId, muted, deafened, true);
    updateVoicePanel();

    m_voiceWatchdog.start(10000);
    flashStatus(QStringLiteral("Joining %1...").arg(channel.name), 4000);
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

    // Before anything else. A share outlives the call it belonged to if it is
    // not ended explicitly: Discord keeps the stream, and the people left
    // behind watch a picture that has stopped arriving.
    stopScreenShare();

    m_voiceRetryTimer.stop();
    m_voiceRetries = 0;
    m_rejoinVoiceAfterGateway = false;

    m_voiceWatchdog.stop();
    stopWatchingStream();
    m_gateway->leaveVoice(guildId);
    m_voice->disconnectFromVoice();
    m_speakingUsers.clear();

    // Clear straight away so the window responds, then let the gateway's own
    // event confirm it.
    m_voiceChannelId.clear();
    m_voiceGuildId.clear();
    updateVoicePanel();
}

void MainWindow::rejoinVoiceIfNeeded()
{
    if (!m_rejoinVoiceAfterGateway || m_voiceChannelId.isEmpty())
        return;
    if (m_gateway->state() != GatewayClient::State::Ready)
        return;

    const QString channelId = m_voiceChannelId;
    wlog(QStringLiteral("voice"),
         QStringLiteral("gateway is back, rejoining %1").arg(channelId));
    joinVoice(channelId);
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
    if (m_streamVoice) {
        m_streamVoice->setOutputDevice(config.value(QStringLiteral("voice/outputDevice")).toByteArray());
        m_streamVoice->setOutputVolume(config.value(QStringLiteral("voice/streamVolume"), 80).toInt());
    }
    if (m_outputVolumeSlider)
        m_outputVolumeSlider->setValue(config.value(QStringLiteral("voice/outputVolume"), 100).toInt());
    if (m_streamVolumeSlider)
        m_streamVolumeSlider->setValue(config.value(QStringLiteral("voice/streamVolume"), 80).toInt());
}

void MainWindow::updateVoicePanel()
{
    const bool connected = !m_voiceChannelId.isEmpty();
    m_voicePanel->setVisible(connected);
    m_channelDelegate->setJoinedVoiceChannel(m_voiceChannelId);

    // The grid of faces follows the call, not the channel being read, so it
    // stays up while you wander off into another conversation.
    if (m_callView)
        m_callView->setChannel(m_voiceChannelId);

    // The one button does both jobs, because starting and stopping a share
    // are the same thought and a second button would sit disabled most of the
    // time. It says which one it will do.
    if (m_shareButton) {
        const bool live = !m_myStreamKey.isEmpty();
        // Short, because this sits in half a sidebar. The tooltip carries the
        // full sentence.
        m_shareButton->setText(live ? QStringLiteral("Stop") : QStringLiteral("Share"));
        m_shareButton->setToolTip(live
                                      ? QStringLiteral("Stop showing your screen")
                                      : QStringLiteral("Show your screen to everyone in this call"));
    }

    const bool watching = connected && !m_watchingUserId.isEmpty();
    if (m_streamControls)
        m_streamControls->setVisible(watching);
    // A second row of buttons needs its height, or the panel clips them.
    if (m_voicePanel && connected)
        m_voicePanel->setFixedHeight(watching ? 218 : 164);

    if (!connected)
        return;

    // People already sending video when we arrived never produce a fresh
    // event, so the ones we would otherwise miss are listed here instead.
    int sending = 0;
    const QStringList already = m_store->voiceMembers(m_voiceChannelId);
    for (const QString &memberId : already) {
        const VoiceStateInfo state = m_store->voiceState(memberId);
        if (!state.video && !state.streaming)
            continue;

        ++sending;
        const QString name = m_store->userName(memberId);
        wlog(QStringLiteral("video"),
             QStringLiteral("already running when we joined: %1 has %2")
                 .arg(name.isEmpty() ? memberId : name,
                      state.streaming ? QStringLiteral("a shared screen")
                                      : QStringLiteral("a camera")));
    }

    if (sending == 0) {
        wlog(QStringLiteral("video"),
             QStringLiteral("nobody in this call has a camera or a screen running"));
    }

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
            flashStatus(QStringLiteral("Invite copied: %1").arg(link), 10000);
        },
        [this](const RestClient::Error &error) {
            wlog(QStringLiteral("invite"), QStringLiteral("failed: HTTP %1 %2")
                                               .arg(error.httpStatus).arg(error.message));
            flashStatus(error.httpStatus == 403
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
    // Keep one dialog. A stack QDialog is created and destroyed at the same
    // address every open; Windows UI Automation then hits Qt's accessibility
    // cache for that pointer and QWidget::accessibleName crashes on a null
    // widget (Qt6Widgets, same fault offset across dumps).
    if (!m_settingsDialog) {
        m_settingsDialog = new SettingsDialog(m_store, m_rest, m_plugins, m_selfUserId, this);
        connect(m_settingsDialog, &SettingsDialog::appearanceChanged, this, &MainWindow::applyAppearance);
        connect(m_settingsDialog, &SettingsDialog::logOutRequested, this, &MainWindow::logOut);
        // A call already running has to be told, or the sliders only take effect
        // the next time you join one.
        connect(m_settingsDialog, &SettingsDialog::voiceSettingsChanged, this, &MainWindow::applyVoiceSettings);
    }
    m_settingsDialog->exec();
    // Chip clicks already wrote the seed. Re-polish after the modal hides:
    // setStyleSheet during QDialog::exec can leave the main window on the
    // unpolished black fill, which is the "close Settings, hole gone" bug.
    applyAppearance();
}

void MainWindow::applyAppearance()
{
    AppConfig &config = AppConfig::instance();

    const QColor seed(config.value(QStringLiteral("appearance/themeSeed"),
                                   QLatin1String(Theme::DefaultSeed)).toString());
    Theme::applySeed(seed.isValid() ? seed : QColor(QLatin1String(Theme::DefaultSeed)));
    qApp->setStyleSheet(Theme::applicationStyleSheet());

    if (m_messageView) {
        m_messageView->document()->setDefaultStyleSheet(
            Theme::messageViewCss(config.value(QStringLiteral("appearance/fontSize"), 14).toInt()));
        m_messageView->setAnimationsEnabled(
            config.value(QStringLiteral("appearance/playAnimations"), true).toBool());
    }
    if (m_aurora) {
        m_aurora->setAttribute(Qt::WA_OpaquePaintEvent, true);
        m_aurora->setAttribute(Qt::WA_NoSystemBackground, true);
        m_aurora->setAttribute(Qt::WA_StyledBackground, false);
        m_aurora->setAutoFillBackground(false);
        m_aurora->setHoleColors(Theme::holeAccent(), Theme::holeDisk(), Theme::holeGrade());
        m_aurora->setRunning(
            config.value(QStringLiteral("appearance/animatedBackground"), true).toBool());
        applyBackgroundSettings();
        m_aurora->update();
    }

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

void MainWindow::flashStatus(const QString &text, int ms)
{
    if (!m_statusMessage)
        return;
    m_statusMessage->setText(text);
    if (ms > 0)
        m_statusClearTimer.start(ms);
    else
        m_statusClearTimer.stop();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    wlog(QStringLiteral("app"), QStringLiteral("main window shown"));
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange)
        return;

    if (m_captionMax)
        m_captionMax->setText(isMaximized() ? QStringLiteral("❐") : QStringLiteral("□"));

#ifdef Q_OS_WIN
    // Minimised means nobody is looking, so give the pages back.
    //
    // Freed memory is not returned to Windows by itself - the heap keeps it
    // for next time, which is the right thing while the window is in use and
    // the wrong thing while it sits in the task bar. This asks Windows to trim
    // the working set to what is actually needed; anything still wanted comes
    // back when the window does. It is why the official client shrinks when
    // you minimise it, and why this one did not.
    if (isMinimized())
        SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
#endif
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (eventType == "windows_generic_MSG") {
        const MSG *msg = static_cast<MSG *>(message);
        if (msg->message == WM_NCHITTEST) {
            RECT winRect;
            GetWindowRect(msg->hwnd, &winRect);
            const int x = GET_X_LPARAM(msg->lParam);
            const int y = GET_Y_LPARAM(msg->lParam);
            // Windows reports about eight pixels for a frame it draws itself,
            // complete with a visible edge to aim at. This window has no such
            // edge, so eight invisible pixels is a target nobody can hit.
            // Widened to something a person can find by waving at the corner.
            const int border =
                qMax(10, GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER));
            const int localX = x - winRect.left;
            const int localY = y - winRect.top;
            const int w = winRect.right - winRect.left;

            if (!isMaximized() && !isFullScreen()) {
                const bool left = localX < border;
                const bool right = w - localX < border;
                const bool top = localY < border;
                const bool bottom = winRect.bottom - y < border;
                if (top && left)
                    *result = HTTOPLEFT;
                else if (top && right)
                    *result = HTTOPRIGHT;
                else if (bottom && left)
                    *result = HTBOTTOMLEFT;
                else if (bottom && right)
                    *result = HTBOTTOMRIGHT;
                else if (left)
                    *result = HTLEFT;
                else if (right)
                    *result = HTRIGHT;
                else if (top)
                    *result = HTTOP;
                else if (bottom)
                    *result = HTBOTTOM;
                else if (localY < 36 && localX > 90 && localX < w - 46 * 3) {
                    *result = HTCAPTION;
                } else {
                    return QMainWindow::nativeEvent(eventType, message, result);
                }
                return true;
            }

            if (localY < 36 && localX > 90 && localX < w - 46 * 3) {
                *result = HTCAPTION;
                return true;
            }
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_gateway->stop();
    QMainWindow::closeEvent(event);
}
