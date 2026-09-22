#include "ui/MainWindow.h"

#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/RestClient.h"
#include "plugin/PluginHost.h"
#include "ui/ChatView.h"
#include "ui/FriendsPage.h"
#include "ui/ImageViewer.h"
#include "ui/AuroraWidget.h"
#include "ui/CaptchaDialog.h"
#include "core/CameraShare.h"
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
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QEvent>
#include <QIcon>
#include <QMessageBox>
#include <QDesktopServices>
#include <QFrame>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QAbstractTextDocumentLayout>
#include <QCamera>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QLineEdit>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>

#include <memory>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QVideoFrame>
#include <QVideoSink>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QToolTip>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
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

class QLabel;
void setPanelText(QLabel *label, const QString &text);
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
    button->setFixedSize(46, 32);
    button->setCursor(Qt::ArrowCursor);
    return button;
}

bool containsGlobal(const QWidget *widget, const QPoint &global)
{
    return widget && widget->isVisible() && widget->rect().contains(widget->mapFromGlobal(global));
}

// The empty part of the top strip.
//
// The background is its own window, so a hit test on the outer frame never
// sees this strip. The drag has to be started from here. Windows then snaps
// it, and pulling a snapped window back out restores the previous size.
class TitleDragArea : public QWidget
{
public:
    explicit TitleDragArea(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground, false);
        setAutoFillBackground(false);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            if (QWindow *handle = window()->windowHandle())
                handle->startSystemMove();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mouseDoubleClickEvent(event);
            return;
        }
        QWidget *top = window();
        if (top->isMaximized())
            top->showNormal();
        else
            top->showMaximized();
    }
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
    // A normal resizable window. The sizing border stays, so the edges can
    // be dragged and a snap has somewhere to land. The caption bar itself is
    // pulled up in WM_NCCALCSIZE so the buttons are not left under an empty strip.
    setWindowFlags(Qt::Window | Qt::CustomizeWindowHint);

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
            setPanelText(m_selfStatus, QStringLiteral("signed out"));
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

    m_mentionRefresh.setSingleShot(true);
    m_mentionRefresh.setInterval(400);
    connect(&m_mentionRefresh, &QTimer::timeout, this, [this]() {
        if (m_currentChannelId.isEmpty() || !m_store->hasHistory(m_currentChannelId))
            return;
        renderChannel();
    });

    connect(m_plugins, &PluginHost::repaintRequested, this, [this]() { scheduleRender(); });
    connect(m_plugins, &PluginHost::pluginLogged, this, [this](const QString &id, const QString &line) {
        flashStatus(QStringLiteral("[%1] %2").arg(id, line), 5000);
    });

    connect(m_store, &MessageStore::readStateChanged, this, &MainWindow::refreshUnreadMarks);
    connect(m_store, &MessageStore::directOrderChanged, this, [this]() {
        if (!m_currentGuildId.isEmpty() || !m_channelList)
            return;

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
        m_channelList->verticalScrollBar()->setValue(scroll);
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

    connect(m_voice, &VoiceConnection::keyframeWanted, this, [this]() {
        if (m_camera && m_camera->isRunning())
            m_camera->requestKeyframe();
    });

    m_camera = new CameraShare(this);
    connect(m_camera, &CameraShare::picture, this, [this](const QList<QByteArray> &units, bool) {
        if (m_voice)
            m_voice->sendPicture(units);
    });
    connect(m_camera, &CameraShare::started, this, [this](int width, int height, const QString &encoder) {
        wlog(QStringLiteral("camera"), QStringLiteral("encoding with %1").arg(encoder));
        if (m_voice)
            m_voice->startSendingVideo(width, height);
        m_gateway->setSelfVideo(true);
        flashStatus(QStringLiteral("Camera on — %1x%2").arg(width).arg(height), 4000);
        updateVoicePanel();
    });
    connect(m_camera, &CameraShare::failed, this, [this](const QString &reason) {
        flashStatus(reason, 6000);
        stopCamera();
    });

    const QCameraDevice device = QMediaDevices::defaultVideoInput();
    if (!device.isNull()) {
        m_webcam = new QCamera(device, this);
        m_cameraSession = new QMediaCaptureSession(this);
        m_cameraSink = new QVideoSink(this);
        m_cameraSession->setCamera(m_webcam);
        m_cameraSession->setVideoSink(m_cameraSink);
        connect(m_cameraSink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &incoming) {
            if (!m_camera || !m_camera->isRunning())
                return;
            QVideoFrame frame = incoming;
            if (!frame.map(QVideoFrame::ReadOnly))
                return;
            QImage image = frame.toImage();
            frame.unmap();
            if (image.isNull())
                return;
            image = image.scaled(960, 540, Qt::KeepAspectRatio, Qt::FastTransformation)
                        .convertToFormat(QImage::Format_ARGB32);
            m_camera->submit(image);
            if (m_callView && !m_selfUserId.isEmpty())
                m_callView->setFrame(m_selfUserId, image, CallView::Surface::Camera);
        });
    }

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

    m_volumePush.setSingleShot(true);
    m_volumePush.setInterval(400);
    connect(&m_volumePush, &QTimer::timeout, this, &MainWindow::flushUserAudio);
    loadUserAudio();

    // In place before the first paint, so the hole is never the first thing
    // on screen. startSession only raises it and opens the connection.
    m_loading = new LoadingOverlay(this);
    m_loading->setGeometry(rect());
    m_loading->show();
    m_loading->raise();
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

    m_aurora->installEventFilter(this);

    auto *shell = new QVBoxLayout(m_aurora);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    m_titleLayout = new QHBoxLayout();
    auto *titleLayout = m_titleLayout;
    titleLayout->setSpacing(0);
    titleLayout->setAlignment(Qt::AlignVCenter);
    layoutTitleRow();

    m_menuBar = new QMenuBar(m_aurora);
    m_menuBar->setObjectName(QStringLiteral("AppMenu"));
    m_menuBar->setNativeMenuBar(false);
    m_menuBar->setFixedHeight(32);
    titleLayout->addWidget(m_menuBar, 0, Qt::AlignVCenter);

    // Empty strip between the menu and the caption buttons. A press here is
    // the caption, so the window can be dragged and snapped. The menu and the
    // buttons are not part of that strip: a press on them has to reach them.
    auto *dragStrip = new TitleDragArea(m_aurora);
    dragStrip->setObjectName(QStringLiteral("TitleDrag"));
    dragStrip->setFixedHeight(32);
    titleLayout->addWidget(dragStrip, 1, Qt::AlignVCenter);

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
    titleLayout->addWidget(minBtn, 0, Qt::AlignVCenter);
    titleLayout->addWidget(m_captionMax, 0, Qt::AlignVCenter);
    titleLayout->addWidget(closeBtn, 0, Qt::AlignVCenter);
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
    connect(m_callView, &CallView::volumeMenuRequested, this, &MainWindow::showPersonMenu);
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
    connect(m_friends, &FriendsPage::personMenuRequested, this, &MainWindow::showPersonMenu);
    connect(m_friends, &FriendsPage::joinVoiceChannel, this, &MainWindow::joinVoiceAt);
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
    connect(m_members, &MemberListPanel::volumeMenuRequested, this, &MainWindow::showPersonMenu);
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

    m_channelList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_channelList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_channelList->itemAt(pos);
        if (!item)
            return;
        const QString kind = item->data(KindRole).toString();
        const QPoint at = m_channelList->viewport()->mapToGlobal(pos);
        if (kind == QLatin1String("voicemember")) {
            showPersonMenu(item->data(IdRole).toString(), at);
            return;
        }
        if (kind == QLatin1String("dm")) {
            const ChannelInfo channel = m_store->channel(item->data(IdRole).toString());
            if (channel.type == 1 && channel.recipientIds.size() == 1)
                showPersonMenu(channel.recipientIds.first(), at);
        }
    });

    return sidebar;
}

// A name that is wider than the gap beside the buttons was clipped mid-letter.
// This keeps the whole string and draws as much of it as fits.
class ElidingLabel : public QLabel
{
public:
    explicit ElidingLabel(const QString &text, QWidget *parent = nullptr)
        : QLabel(parent)
        , m_full(text)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(0);
        QLabel::setText(text);
    }

    void setFullText(const QString &text)
    {
        m_full = text;
        elide();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        elide();
    }

private:
    void elide()
    {
        const int width = contentsRect().width();
        QLabel::setText(width > 0 ? fontMetrics().elidedText(m_full, Qt::ElideRight, width) : m_full);
    }

    QString m_full;
};

void setPanelText(QLabel *label, const QString &text)
{
    if (auto *eliding = dynamic_cast<ElidingLabel *>(label))
        eliding->setFullText(text);
    else if (label)
        label->setText(text);
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
    layout->setContentsMargins(8, 8, 4, 8);
    layout->setSpacing(6);

    m_selfAvatar = new QLabel(panel);
    m_selfAvatar->setFixedSize(32, 32);
    layout->addWidget(m_selfAvatar);

    auto *text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(0);

    m_selfName = new ElidingLabel(QStringLiteral("Signing in..."), panel);
    m_selfName->setObjectName(QStringLiteral("SelfName"));
    text->addWidget(m_selfName);

    m_selfStatus = new ElidingLabel(QStringLiteral("connecting"), panel);
    m_selfStatus->setObjectName(QStringLiteral("SelfStatus"));
    text->addWidget(m_selfStatus);

    layout->addLayout(text, 1);

    QFont icons(QStringLiteral("Segoe MDL2 Assets"));
    icons.setPixelSize(16);
    const auto iconButton = [&](ushort glyph, const QString &tip) {
        auto *button = new QPushButton(QChar(glyph), panel);
        button->setFont(icons);
        button->setToolTip(tip);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedSize(26, 26);
        return button;
    };

    const bool share = AppConfig::instance().value(QStringLiteral("presence/shareActivity"), true).toBool();
    m_gateway->setActivityShared(share);
    m_panelActivity = iconButton(0xE7FC, QStringLiteral("Activity"));
    m_panelActivity->setCheckable(true);
    m_panelActivity->setChecked(!share);
    connect(m_panelActivity, &QPushButton::toggled, this, [this](bool hidden) { setActivityShared(!hidden); });
    layout->addWidget(m_panelActivity);

    m_panelMute = iconButton(0xE720, QStringLiteral("Mute"));
    m_panelMute->setCheckable(true);
    connect(m_panelMute, &QPushButton::toggled, this, [this](bool on) { setSelfMuted(on); });
    layout->addWidget(m_panelMute);

    m_panelDeafen = iconButton(0xE7F6, QStringLiteral("Deafen"));
    m_panelDeafen->setCheckable(true);
    connect(m_panelDeafen, &QPushButton::toggled, this, [this](bool on) { setSelfDeafened(on); });
    layout->addWidget(m_panelDeafen);

    auto *settings = iconButton(0xE713, QStringLiteral("User Settings"));
    connect(settings, &QPushButton::clicked, this, &MainWindow::openSettings);
    layout->addWidget(settings);

    return panel;
}

QWidget *MainWindow::buildVoicePanel(QWidget *parent)
{
    auto *panel = new QFrame(parent);
    panel->setObjectName(QStringLiteral("VoicePanel"));
    panel->setVisible(false);
    panel->setFixedHeight(168);

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
    connect(m_muteButton, &QPushButton::toggled, this, [this](bool on) { setSelfMuted(on); });
    buttons->addWidget(m_muteButton, 0, 0);

    m_deafenButton = new QPushButton(QStringLiteral("Deafen"), panel);
    m_deafenButton->setFixedHeight(26);
    m_deafenButton->setCheckable(true);
    m_deafenButton->setToolTip(QStringLiteral("Stop hearing everyone"));
    connect(m_deafenButton, &QPushButton::toggled, this, [this](bool on) { setSelfDeafened(on); });
    buttons->addWidget(m_deafenButton, 0, 1);

    m_shareButton = new QPushButton(QStringLiteral("Share"), panel);
    m_shareButton->setFixedHeight(26);
    m_shareButton->setToolTip(QStringLiteral("Show your screen to everyone in this call"));
    connect(m_shareButton, &QPushButton::clicked, this, &MainWindow::startScreenShare);
    buttons->addWidget(m_shareButton, 1, 0);

    m_cameraButton = new QPushButton(QStringLiteral("Camera"), panel);
    m_cameraButton->setFixedHeight(26);
    m_cameraButton->setCheckable(true);
    m_cameraButton->setToolTip(QStringLiteral("Show your camera to everyone in this call"));
    connect(m_cameraButton, &QPushButton::clicked, this, &MainWindow::toggleCamera);
    buttons->addWidget(m_cameraButton, 1, 1);

    auto *leave = new QPushButton(QStringLiteral("Leave"), panel);
    leave->setFixedHeight(26);
    leave->setToolTip(QStringLiteral("Leave the call"));
    connect(leave, &QPushButton::clicked, this, &MainWindow::leaveVoice);
    buttons->addWidget(leave, 2, 0, 1, 2);

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
    m_messageView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_messageView, &ChatView::anchorClicked, this, &MainWindow::handleAnchor);
    connect(m_messageView, &QWidget::customContextMenuRequested, this, &MainWindow::showMessageMenu);
    layout->addWidget(m_messageView, 1);

    m_typingLabel = new QLabel(QString(), chat);
    m_typingLabel->setObjectName(QStringLiteral("TypingLabel"));
    m_typingLabel->setFixedHeight(18);
    layout->addWidget(m_typingLabel);

    auto *composerWrap = new QWidget(chat);
    composerWrap->setObjectName(QStringLiteral("Composer"));
    auto *composerLayout = new QVBoxLayout(composerWrap);
    composerLayout->setContentsMargins(18, 4, 18, 16);

    m_composerContext = new QWidget(composerWrap);
    auto *contextLayout = new QHBoxLayout(m_composerContext);
    contextLayout->setContentsMargins(4, 0, 4, 0);
    m_composerContextText = new QLabel(m_composerContext);
    m_composerContextText->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));
    auto *cancelContext = new QPushButton(QStringLiteral("Cancel"), m_composerContext);
    cancelContext->setObjectName(QStringLiteral("ComposerTool"));
    cancelContext->setFixedHeight(28);
    connect(cancelContext, &QPushButton::clicked, this, &MainWindow::clearComposerContext);
    contextLayout->addWidget(m_composerContextText, 1);
    contextLayout->addWidget(cancelContext);
    m_composerContext->hide();
    composerLayout->addWidget(m_composerContext);

    m_attachmentLabel = new QLabel(composerWrap);
    m_attachmentLabel->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));
    m_attachmentLabel->hide();
    composerLayout->addWidget(m_attachmentLabel);

    auto *composerBox = new QFrame(composerWrap);
    composerBox->setObjectName(QStringLiteral("ComposerBox"));
    auto *boxLayout = new QVBoxLayout(composerBox);
    boxLayout->setContentsMargins(0, 0, 0, 0);

    // Attach, the text, and Emoji share one row. A row of their own above
    // the field, squeezed into 24 pixels, clipped the words and left them
    // floating off the baseline of the message.
    auto *row = new QHBoxLayout;
    row->setContentsMargins(8, 6, 8, 6);
    row->setSpacing(4);

    auto *attach = new QPushButton(QStringLiteral("Attach"), composerBox);
    auto *emoji = new QPushButton(QStringLiteral("Emoji"), composerBox);
    for (QPushButton *button : {attach, emoji}) {
        button->setObjectName(QStringLiteral("ComposerTool"));
        button->setFixedHeight(28);
        button->setCursor(Qt::PointingHandCursor);
    }
    attach->setToolTip(QStringLiteral("Add a file or a picture"));
    emoji->setToolTip(QStringLiteral("Insert an emoji"));
    connect(attach, &QPushButton::clicked, this, &MainWindow::chooseAttachment);
    connect(emoji, &QPushButton::clicked, this, &MainWindow::showEmojiMenu);

    m_composer = new QTextEdit(composerBox);
    m_composer->setObjectName(QStringLiteral("MessageInput"));
    m_composer->setPlaceholderText(QStringLiteral("Write a message"));
    m_composer->setFixedHeight(32);
    m_composer->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_composer->installEventFilter(this);
    m_composer->setEnabled(false);

    row->addWidget(attach, 0, Qt::AlignVCenter);
    row->addWidget(m_composer, 1);
    row->addWidget(emoji, 0, Qt::AlignVCenter);
    boxLayout->addLayout(row);

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
    if (!m_loading) {
        m_loading = new LoadingOverlay(this);
        m_loading->setGeometry(rect());
        m_loading->show();
    }
    m_loading->raise();

    // Chosen before the socket opens, so the sign-in and the later presence
    // message agree. Applying it only after ready used to skip the send
    // whenever the saved choice was already "online".
    const QString saved = AppConfig::instance()
                              .value(QStringLiteral("presence/status"), QStringLiteral("online"))
                              .toString();
    m_gateway->setPresenceStatus(saved);

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
        m_selfStatus->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(colour));
        setPanelText(m_selfStatus, text);
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
    const QByteArray settingsProto = QByteArray::fromBase64(
        payload.value(QStringLiteral("user_settings_proto")).toString().toLatin1());
    if (!settingsProto.isEmpty())
        ingestSettingsProto(settingsProto, false);
    populateGuildRail();

    // Say it again now that the session exists. The copy sent before the
    // socket was open was only remembered, and Discord does not treat the
    // status inside the first sign-in as the one other people should see.
    setPresenceStatus(m_gateway->presenceStatus());
    ensureClientActivity();

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

    if (eventType == QLatin1String("AUDIO_SETTINGS_UPDATE")) {
        applyAudioSettingsUpdate(data);
        return;
    }

    if (eventType == QLatin1String("USER_SETTINGS_PROTO_UPDATE")) {
        const QJsonObject settings = data.value(QStringLiteral("settings")).toObject();
        if (settings.contains(QStringLiteral("type"))
            && settings.value(QStringLiteral("type")).toInt() != 1)
            return;
        const QByteArray proto = QByteArray::fromBase64(
            settings.value(QStringLiteral("proto")).toString().toLatin1());
        if (!proto.isEmpty())
            ingestSettingsProto(proto, data.value(QStringLiteral("partial")).toBool());
        return;
    }

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
        if (dropped >= 0) {
            const MessageInfo message = MessageStore::parseMessage(data);
            const bool mine = message.authorId == m_selfUserId;
            const bool seen = mine || (channelId == m_currentChannelId && m_stickToBottom);
            const QString raw = data.value(QStringLiteral("content")).toString();
            bool mention = data.value(QStringLiteral("mention_everyone")).toBool()
                || raw.contains(QStringLiteral("<@") + m_selfUserId)
                || raw.contains(QStringLiteral("<@!") + m_selfUserId);
            const QJsonArray named = data.value(QStringLiteral("mentions")).toArray();
            for (const QJsonValue &value : named) {
                if (value.toObject().value(QStringLiteral("id")).toString() == m_selfUserId)
                    mention = true;
            }
            m_store->noteIncoming(channelId, message.id, mention && !mine, seen);
            if (seen && channelId == m_currentChannelId)
                m_rest->ackMessage(channelId, message.id);

            if (channelId == m_currentChannelId && m_store->hasHistory(channelId)) {
                const bool grouped = m_hasLastRendered && shouldGroup(m_lastRendered, message);
                appendMessageToView(message, grouped);
                m_lastRendered = message;
                m_hasLastRendered = true;
            }
        }
        return;
    }

    if (eventType == QLatin1String("MESSAGE_ACK")) {
        m_store->markChannelRead(data.value(QStringLiteral("channel_id")).toString(),
                                 data.value(QStringLiteral("message_id")).toString());
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

    if (eventType == QLatin1String("GUILD_CREATE")) {
        m_store->applyGuild(data);
        if (data.value(QStringLiteral("id")).toString() == m_currentGuildId)
            populateChannelList(false);
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
        if (userId == m_selfUserId) {
            wlog(QStringLiteral("gateway"),
                 QStringLiteral("Discord echoed our presence with %1 activities")
                     .arg(data.value(QStringLiteral("activities")).toArray().size()));
        }
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
    refreshUnreadMarks();
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
        if (channel.isVoice()) {
            const int sitting = m_store->voiceMembers(channel.id).size();
            item->setData(SingularityRoles::VoiceCount, sitting);
            item->setData(SingularityRoles::VoiceLimit, channel.userLimit);
            if (channel.userLimit > 0 && sitting >= channel.userLimit)
                item->setToolTip(QStringLiteral("%1 is full (%2/%3)")
                                     .arg(channel.name)
                                     .arg(sitting)
                                     .arg(channel.userLimit));
        }
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

    refreshUnreadMarks();

    if (!autoSelectFirst)
        return;

    if (firstSelectable >= 0)
        m_channelList->setCurrentRow(firstSelectable);
    else
        openChannel(QString());
}

void MainWindow::watchGuildChannel(const QString &guildId, const QString &channelId)
{
    if (guildId.isEmpty() || channelId.isEmpty())
        return;
    if (m_listGuild == guildId && m_listChannel == channelId)
        return;

    // Drop the previous channel's rows only when the server itself changes.
    // Clearing on every channel, then asking a channel Discord will not list,
    // is how the panel got stuck on "0 online".
    if (m_listGuild != guildId)
        m_store->clearMemberList(guildId);
    m_listGuild = guildId;
    m_listChannel = channelId;
    m_gateway->subscribeToGuild(guildId, channelId);
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
        if (m_members)
            m_members->setFocusChannel(QString(), false);
        // The call stays connected. The stage is for when that channel is
        // what you are looking at. On Friends it was covering the list.
        if (m_callView)
            m_callView->setStageSuppressed(true);
        m_friends->refresh();
        m_chatStack->setCurrentWidget(m_friends);
        m_composer->setEnabled(false);
        return;
    }

    if (m_callView)
        m_callView->setStageSuppressed(false);
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
    if (m_members)
        m_members->setFocusChannel(channelId, channel.isVoice());
    if (!channel.guildId.isEmpty())
        watchGuildChannel(channel.guildId, channelId);

    m_composer->setEnabled(true);
    m_composer->setPlaceholderText(
        QStringLiteral("Message %1").arg(channel.isDirect() ? channel.name : QStringLiteral("#") + channel.name));
    m_composer->setFocus();

    clearComposerContext();

    if (m_store->hasHistory(channelId)) {
        renderChannel();
        acknowledgeChannel(channelId);
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
            if (channelId == m_currentChannelId)
                acknowledgeChannel(channelId);
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

QString MainWindow::embedsHtml(const MessageInfo &message)
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
            const QString title = renderContent(embed.title);
            inner += embed.url.isEmpty()
                ? QStringLiteral("<div class=\"embed-title\">%1</div>").arg(title)
                : QStringLiteral("<div class=\"embed-title\"><a href=\"%1\">%2</a></div>")
                      .arg(embed.url.toHtmlEscaped(), title);
        }
        if (!embed.description.isEmpty()) {
            inner += QStringLiteral("<div class=\"embed-body\">%1</div>").arg(renderContent(embed.description));
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

QString MainWindow::renderContent(const QString &raw)
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
    {
        const GuildInfo guild = m_store->guild(m_store->channel(m_currentChannelId).guildId);
        QString rebuilt;
        int last = 0;
        auto it = roleRe.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const RoleInfo role = guild.roles.value(match.captured(1));
            const QString label = role.name.isEmpty() ? QStringLiteral("role") : role.name;
            rebuilt += text.mid(last, match.capturedStart() - last);
            rebuilt += QStringLiteral("<span class=\"mention\">@%1</span>").arg(label.toHtmlEscaped());
            last = match.capturedEnd();
        }
        rebuilt += text.mid(last);
        text = rebuilt;
    }

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
            rebuilt += QStringLiteral("<a href=\"singularity-channel:%1\" class=\"mention\">#%2</a>")
                           .arg(match.captured(1), label.toHtmlEscaped());
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
            const QString userId = match.captured(1);
            QString name = m_store->userName(userId);
            if (name.isEmpty()) {
                requestUnknownName(userId);
                name = QStringLiteral("…");
            }
            rebuilt += text.mid(last, match.capturedStart() - last);
            rebuilt += QStringLiteral("<a href=\"singularity-user:%1\" class=\"mention\">@%2</a>")
                           .arg(userId, name.toHtmlEscaped());
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

    // A pasted channel address is the same thing as <#id>: the name, clickable,
    // not the long URL.
    static const QRegularExpression discordChannelRe(
        QStringLiteral("https?://(?:(?:ptb|canary)\\.)?discord(?:app)?\\.com/channels/"
                       "(\\d+|@me)/(\\d+)(?:/\\d+)?"));
    {
        QString rebuilt;
        int last = 0;
        auto it = discordChannelRe.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const QString channelId = match.captured(2);
            const ChannelInfo channel = m_store->channel(channelId);
            const QString label = channel.name.isEmpty() ? QStringLiteral("channel") : channel.name;
            rebuilt += text.mid(last, match.capturedStart() - last);
            rebuilt += QStringLiteral("<a href=\"singularity-channel:%1\" class=\"mention\">#%2</a>")
                           .arg(channelId, label.toHtmlEscaped());
            last = match.capturedEnd();
        }
        rebuilt += text.mid(last);
        text = rebuilt;
    }

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

void MainWindow::setSelfMuted(bool on)
{
    if (m_voice)
        m_voice->setMuted(on);
    if (m_muteButton && m_muteButton->isChecked() != on) {
        QSignalBlocker block(m_muteButton);
        m_muteButton->setChecked(on);
    }
    if (m_muteButton)
        m_muteButton->setText(on ? QStringLiteral("Muted") : QStringLiteral("Mute"));
    if (m_panelMute && m_panelMute->isChecked() != on) {
        QSignalBlocker block(m_panelMute);
        m_panelMute->setChecked(on);
    }
    if (!m_voiceChannelId.isEmpty())
        m_gateway->joinVoice(m_voiceGuildId, m_voiceChannelId, on,
                             m_deafenButton && m_deafenButton->isChecked());
}

void MainWindow::setSelfDeafened(bool on)
{
    if (m_voice)
        m_voice->setDeafened(on);
    if (m_deafenButton && m_deafenButton->isChecked() != on) {
        QSignalBlocker block(m_deafenButton);
        m_deafenButton->setChecked(on);
    }
    if (m_deafenButton)
        m_deafenButton->setText(on ? QStringLiteral("Deaf") : QStringLiteral("Deafen"));
    if (m_panelDeafen && m_panelDeafen->isChecked() != on) {
        QSignalBlocker block(m_panelDeafen);
        m_panelDeafen->setChecked(on);
    }
    if (on)
        setSelfMuted(true);
    else if (!m_voiceChannelId.isEmpty())
        m_gateway->joinVoice(m_voiceGuildId, m_voiceChannelId,
                             m_muteButton && m_muteButton->isChecked(), false);
}

void MainWindow::setActivityShared(bool on)
{
    AppConfig::instance().setValue(QStringLiteral("presence/shareActivity"), on);
    m_gateway->setActivityShared(on);
    if (m_panelActivity && m_panelActivity->isChecked() == on) {
        QSignalBlocker block(m_panelActivity);
        m_panelActivity->setChecked(!on);
    }
    m_gateway->publishPresence();
    if (!m_selfUserId.isEmpty()) {
        m_store->setPresence(m_selfUserId,
                             QJsonObject{
                                 {QStringLiteral("status"), m_gateway->presenceStatus()},
                                 {QStringLiteral("activities"), m_gateway->clientActivities()},
                             });
    }
}

void MainWindow::setPresenceStatus(const QString &status)
{
    m_gateway->setPresenceStatus(status);
    AppConfig::instance().setValue(QStringLiteral("presence/status"), status);

    // The panel reads the gateway. The member list and everyone else read the
    // presence store, which only changes when Discord says so. Put the choice
    // there too, so this client agrees with itself at once.
    if (!m_selfUserId.isEmpty()) {
        m_store->setPresence(m_selfUserId,
                             QJsonObject{
                                 {QStringLiteral("status"), m_gateway->presenceStatus()},
                                 {QStringLiteral("activities"), m_gateway->clientActivities()},
                             });
    }

    // Opcode 3 tells this session. The settings write is what the real client
    // does, and it is what other people and your other sessions actually use.
    if (m_rest) {
        const QString chosen = m_gateway->presenceStatus();
        m_rest->updateStatus(
            chosen,
            [chosen](const QJsonObject &) {
                wlog(QStringLiteral("gateway"),
                     QStringLiteral("Discord stored status \"%1\"").arg(chosen));
            },
            [chosen](const RestClient::Error &error) {
                wlog(QStringLiteral("gateway"),
                     QStringLiteral("Discord refused status \"%1\": HTTP %2 %3")
                         .arg(chosen)
                         .arg(error.httpStatus)
                         .arg(error.message));
            });
    }

    wlog(QStringLiteral("ui"), QStringLiteral("status set to %1").arg(m_gateway->presenceStatus()));
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

        m_selfStatus->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(colour));
        setPanelText(m_selfStatus, label);
    }

    setPanelText(m_selfName, m_selfDisplayName.isEmpty() ? QStringLiteral("Signed in") : m_selfDisplayName);

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
        QString userId = url.path();
        if (userId.startsWith(QLatin1Char('/')))
            userId.remove(0, 1);
        if (userId.isEmpty())
            userId = whole.mid(QStringLiteral("singularity-user:").size());
        showProfile(userId, QCursor::pos());
        return;
    }

    if (url.scheme() == QLatin1String("singularity-channel")) {
        QString channelId = url.path();
        if (channelId.startsWith(QLatin1Char('/')))
            channelId.remove(0, 1);
        if (channelId.isEmpty())
            channelId = whole.mid(QStringLiteral("singularity-channel:").size());
        selectChannelEverywhere(channelId);
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

QString MainWindow::messageIdAt(const QPoint &viewportPos) const
{
    if (!m_messageView)
        return {};

    const QTextCursor cursor = m_messageView->cursorForPosition(viewportPos);
    const int position = cursor.position();
    const QList<QTextFrame *> frames = m_messageView->document()->rootFrame()->childFrames();
    if (frames.size() != m_renderedIds.size())
        return {};

    for (int i = 0; i < frames.size(); ++i) {
        if (position >= frames.at(i)->firstPosition() && position <= frames.at(i)->lastPosition())
            return m_renderedIds.at(i);
    }
    return {};
}

void MainWindow::refreshComposerContext()
{
    if (!m_composerContext)
        return;

    if (!m_editingMessageId.isEmpty()) {
        m_composerContextText->setText(QStringLiteral("Editing a message"));
        m_composerContext->show();
    } else if (!m_replyMessageId.isEmpty()) {
        QString preview;
        const QList<MessageInfo> messages = m_store->messages(m_currentChannelId);
        for (const MessageInfo &message : messages) {
            if (message.id != m_replyMessageId)
                continue;
            preview = message.content.left(80);
            if (preview.isEmpty())
                preview = QStringLiteral("attachment");
            preview = message.authorName + QStringLiteral(": ") + preview;
            break;
        }
        m_composerContextText->setText(QStringLiteral("Replying to %1").arg(preview));
        m_composerContext->show();
    } else {
        m_composerContext->hide();
    }

    if (m_pendingFiles.isEmpty()) {
        m_attachmentLabel->hide();
    } else {
        QStringList names;
        for (const QString &path : m_pendingFiles)
            names.append(QFileInfo(path).fileName());
        m_attachmentLabel->setText(QStringLiteral("Attached: %1").arg(names.join(QStringLiteral(", "))));
        m_attachmentLabel->show();
    }
}

void MainWindow::clearComposerContext()
{
    m_replyMessageId.clear();
    m_editingMessageId.clear();
    m_pendingFiles.clear();
    refreshComposerContext();
}

void MainWindow::beginReply(const QString &messageId)
{
    m_editingMessageId.clear();
    m_replyMessageId = messageId;
    refreshComposerContext();
    m_composer->setFocus();
}

void MainWindow::beginEdit(const QString &messageId)
{
    const QList<MessageInfo> messages = m_store->messages(m_currentChannelId);
    for (const MessageInfo &message : messages) {
        if (message.id != messageId)
            continue;
        m_replyMessageId.clear();
        m_pendingFiles.clear();
        m_editingMessageId = messageId;
        m_composer->setPlainText(message.content);
        refreshComposerContext();
        m_composer->setFocus();
        return;
    }
}

void MainWindow::chooseAttachment()
{
    const QStringList picked = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Attach files"), QString(),
        QStringLiteral("All files (*.*)"));
    if (picked.isEmpty())
        return;

    for (const QString &path : picked) {
        if (QFileInfo(path).size() > 25ll * 1024 * 1024) {
            flashStatus(QStringLiteral("%1 is over 25 MB, which Discord will not take.")
                            .arg(QFileInfo(path).fileName()),
                        5000);
            continue;
        }
        if (!m_pendingFiles.contains(path))
            m_pendingFiles.append(path);
    }
    if (m_pendingFiles.size() > 10)
        m_pendingFiles = m_pendingFiles.mid(0, 10);
    refreshComposerContext();
}

// The emoji page is painted, not a button per face.
//
// One widget per custom emoji, each of them fetching its picture the moment
// the picker opens, is what froze the window on a server with a few thousand
// of them. Only the rows on screen are drawn, and only those pictures are asked for.
class EmojiBoard : public QWidget
{
public:
    struct Cell
    {
        QString insert;
        QString filter;
        QString face;
        QUrl icon;
    };

    QList<Cell> cells;
    std::function<void(const QString &)> onPick;

    explicit EmojiBoard(QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
    }

    void setQuery(const QString &query)
    {
        m_shown.clear();
        for (int index = 0; index < cells.size(); ++index) {
            if (query.isEmpty() || cells.at(index).filter.contains(query, Qt::CaseInsensitive))
                m_shown.append(index);
        }
        m_hover = -1;
        const int rows = m_shown.isEmpty() ? 1 : (m_shown.size() + Columns - 1) / Columns;
        setFixedHeight(rows * CellSize);
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QRect clip = event->rect();
        const int first = qMax(0, clip.top() / CellSize);
        const int last = qMin((m_shown.size() + Columns - 1) / Columns - 1, clip.bottom() / CellSize);
        QFont font(QStringLiteral("Segoe UI Emoji"));
        font.setPixelSize(18);
        painter.setFont(font);
        painter.setPen(Qt::white);

        for (int row = first; row <= last; ++row) {
            for (int column = 0; column < Columns; ++column) {
                const int slot = row * Columns + column;
                if (slot < 0 || slot >= m_shown.size())
                    return;
                const QRect box(column * CellSize, row * CellSize, CellSize, CellSize);
                if (slot == m_hover)
                    painter.fillRect(box.adjusted(2, 2, -2, -2), QColor(255, 255, 255, 28));
                const Cell &cell = cells.at(m_shown.at(slot));
                if (!cell.face.isEmpty()) {
                    painter.drawText(box, Qt::AlignCenter, cell.face);
                    continue;
                }
                const QImage picture = MediaCache::instance().image(cell.icon);
                if (!picture.isNull())
                    painter.drawImage(box.adjusted(5, 5, -5, -5), picture);
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int slot = slotAt(event->position().toPoint());
        if (slot == m_hover)
            return;
        m_hover = slot;
        if (slot >= 0 && slot < m_shown.size()) {
            QToolTip::showText(event->globalPosition().toPoint(), cells.at(m_shown.at(slot)).filter,
                               this);
        } else {
            QToolTip::hideText();
        }
        update();
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = -1;
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !onPick)
            return;
        const int slot = slotAt(event->position().toPoint());
        if (slot < 0 || slot >= m_shown.size())
            return;
        onPick(cells.at(m_shown.at(slot)).insert);
    }

private:
    static constexpr int Columns = 8;
    static constexpr int CellSize = 36;

    QList<int> m_shown;
    int m_hover = -1;

    int slotAt(const QPoint &pos) const
    {
        if (pos.x() < 0 || pos.y() < 0 || pos.x() >= Columns * CellSize)
            return -1;
        return (pos.y() / CellSize) * Columns + (pos.x() / CellSize);
    }
};

void MainWindow::showEmojiMenu()
{
    // A grid, the way Discord's picker is. A menu stretches each face to the
    // height of a text row and prints a server emoji as its raw name, which
    // is a column of huge pictures and :NUM_1: running off the window.
    auto *anchor = qobject_cast<QWidget *>(sender());
    auto *popup = new QFrame(nullptr, Qt::Popup | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setObjectName(QStringLiteral("EmojiPicker"));
    popup->setFixedWidth(328);
    popup->setStyleSheet(QStringLiteral(
        "QFrame#EmojiPicker { background-color: %1; border: 1px solid %2; border-radius: 12px; }"
        "QLineEdit { background: %3; color: %4; border: none; border-radius: 8px; padding: 6px 8px; "
        "font-family: \"Segoe UI\"; font-size: 13px; }"
        "QToolButton { background: transparent; border: none; border-radius: 6px; font-size: 18px; }"
        "QToolButton:hover { background: %5; }"
        "QScrollArea { background: transparent; border: none; }"
        "QLabel { color: %6; background: transparent; font-size: 11px; font-weight: 700; "
        "letter-spacing: 0.6px; }")
                              .arg(QLatin1String(Theme::SurfaceSidebar), QLatin1String(Theme::Border),
                                   QLatin1String(Theme::SurfaceInput), QLatin1String(Theme::TextPrimary),
                                   QLatin1String(Theme::SurfaceHover), QLatin1String(Theme::TextMuted)));

    auto *outer = new QVBoxLayout(popup);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(8);

    auto *search = new QLineEdit(popup);
    search->setPlaceholderText(QStringLiteral("Find an emoji"));
    search->setClearButtonEnabled(true);
    outer->addWidget(search);

    auto *scroll = new QScrollArea(popup);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedHeight(292);
    outer->addWidget(scroll);

    auto *page = new QWidget;
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    scroll->setWidget(page);

    auto *board = new EmojiBoard(page);
    pageLayout->addWidget(board);

    auto *buttonHost = new QWidget(page);
    auto *grid = new QGridLayout(buttonHost);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(2);
    buttonHost->hide();
    pageLayout->addWidget(buttonHost);

    QList<EmojiBoard::Cell> cells;

    const struct {
        const char *face;
        const char *name;
    } stock[] = {
        {"😀", "grin smile"}, {"😂", "joy laugh"}, {"❤️", "heart love"}, {"👍", "thumb yes"},
        {"👎", "thumb no"},  {"🔥", "fire"},       {"🎉", "party"},       {"😭", "cry sob"},
        {"😮", "wow"},       {"😡", "angry"},      {"👀", "eyes"},        {"💯", "hundred"},
        {"✅", "check yes"}, {"🙏", "pray"},       {"💀", "skull"},       {"🤔", "think"},
        {"😎", "cool"},      {"🥳", "party"},      {"😢", "sad"},         {"🤝", "handshake"},
    };
    for (const auto &item : stock) {
        EmojiBoard::Cell cell;
        cell.insert = QString::fromUtf8(item.face);
        cell.face = cell.insert;
        cell.filter = cell.insert + QLatin1Char(' ') + QString::fromUtf8(item.name);
        cells.append(cell);
    }

    // Nitro emoji come from every server you are in, not only the one open.
    // A direct message has no current server, which is why the picker used to
    // stop after the twenty faces.
    const QList<GuildInfo> guilds = m_store->guilds();
    for (const GuildInfo &server : guilds) {
        for (const EmojiInfo &emoji : server.emojis) {
            EmojiBoard::Cell cell;
            cell.insert = emoji.animated ? QStringLiteral("<a:%1:%2>").arg(emoji.name, emoji.id)
                                         : QStringLiteral("<:%1:%2>").arg(emoji.name, emoji.id);
            cell.filter = server.name + QLatin1Char(' ') + emoji.name;
            const QString ext = emoji.animated ? QStringLiteral("gif") : QStringLiteral("png");
            cell.icon = QUrl(QStringLiteral("https://cdn.discordapp.com/emojis/%1.%2?size=64")
                                 .arg(emoji.id, ext));
            cells.append(cell);
        }
    }

    constexpr int columns = 8;
    board->cells = cells;
    board->onPick = [this, popup](const QString &token) {
        m_composer->insertPlainText(token);
        m_composer->setFocus();
        popup->close();
    };
    board->setQuery(QString());
    connect(&MediaCache::instance(), &MediaCache::ready, board, [board](const QUrl &) { board->update(); });

    auto held = std::make_shared<QList<QToolButton *>>();
    popup->setProperty("kind", QStringLiteral("emoji"));

    auto refill = [grid, held, popup](const QString &query) {
        while (QLayoutItem *item = grid->takeAt(0))
            delete item;
        const QString kind = popup->property("kind").toString();
        int placed = 0;
        for (QToolButton *button : *held) {
            if (button->property("kind").toString() != kind) {
                button->hide();
                continue;
            }
            const QString key = button->property("filter").toString();
            const bool show = query.isEmpty() || key.contains(query, Qt::CaseInsensitive);
            button->setVisible(show);
            if (!show)
                continue;
            grid->addWidget(button, placed / columns, placed % columns);
            ++placed;
        }
    };
    auto addPictureButton = [this, popup, buttonHost, held](const QString &kind, const QString &filter,
                                                      const QUrl &icon, const std::function<void()> &onClick) {
        auto *button = new QToolButton(buttonHost);
        button->setFixedSize(36, 36);
        button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(filter);
        button->setProperty("filter", filter);
        button->setProperty("iconUrl", icon);
        button->setProperty("kind", kind);
        const QImage picture = MediaCache::instance().image(icon);
        if (!picture.isNull()) {
            button->setIcon(QPixmap::fromImage(picture));
            button->setIconSize(QSize(26, 26));
        }
        connect(button, &QToolButton::clicked, popup, [onClick]() { onClick(); });
        held->append(button);
    };

    auto *gifTimer = new QTimer(popup);
    gifTimer->setSingleShot(true);
    gifTimer->setInterval(280);

    auto loadGifs = [this, popup, buttonHost, held, search, refill]() {
        const int ticket = popup->property("gifTicket").toInt() + 1;
        popup->setProperty("gifTicket", ticket);
        const QString query = search->text();
        QPointer<QFrame> alive(popup);
        m_rest->searchGifs(query, [alive, buttonHost, held, refill, ticket, this](const QJsonObject &payload) {
            if (!alive || alive->property("gifTicket").toInt() != ticket)
                return;
            QList<QToolButton *> doomed;
            for (QToolButton *button : *held) {
                if (button->property("kind").toString() == QLatin1String("gif"))
                    doomed.append(button);
            }
            for (QToolButton *button : doomed) {
                held->removeAll(button);
                button->deleteLater();
            }
            const QJsonArray gifs = payload.value(QStringLiteral("gifs")).toArray();
            for (const QJsonValue &value : gifs) {
                const QJsonObject gif = value.toObject();
                const QString page = gif.value(QStringLiteral("url")).toString();
                QString preview = gif.value(QStringLiteral("gif_src")).toString();
                if (preview.isEmpty())
                    preview = gif.value(QStringLiteral("src")).toString();
                if (page.isEmpty() || preview.isEmpty())
                    continue;
                auto *button = new QToolButton(buttonHost);
                button->setFixedSize(72, 72);
                button->setAutoRaise(true);
                button->setCursor(Qt::PointingHandCursor);
                button->setToolTip(gif.value(QStringLiteral("title")).toString());
                button->setProperty("filter", gif.value(QStringLiteral("title")).toString());
                button->setProperty("iconUrl", QUrl(preview));
                button->setProperty("kind", QStringLiteral("gif"));
                const QImage picture = MediaCache::instance().image(QUrl(preview));
                if (!picture.isNull()) {
                    button->setIcon(QPixmap::fromImage(picture));
                    button->setIconSize(QSize(64, 64));
                }
                connect(button, &QToolButton::clicked, alive.data(), [this, alive, page]() {
                    if (m_currentChannelId.isEmpty())
                        return;
                    m_rest->sendMessage(m_currentChannelId, page, QString(), {},
                                        [](const QJsonObject &) {},
                                        [this](const RestClient::Error &error) {
                                            flashStatus(QStringLiteral("GIF failed (%1).")
                                                            .arg(error.message.left(120)),
                                                        5000);
                                        });
                    if (alive)
                        alive->close();
                });
                held->append(button);
            }
            if (alive->property("kind").toString() == QLatin1String("gif"))
                refill(QString());
        }, [](const RestClient::Error &) {});
    };

    connect(gifTimer, &QTimer::timeout, popup, loadGifs);

    auto *tabs = new QHBoxLayout;
    tabs->setSpacing(6);
    const struct { const char *label; const char *kind; } tabNames[] = {
        {"Emoji", "emoji"}, {"Stickers", "sticker"}, {"GIFs", "gif"},
    };
    for (const auto &tab : tabNames) {
        auto *button = new QPushButton(QString::fromUtf8(tab.label), popup);
        button->setObjectName(QStringLiteral("ComposerTool"));
        button->setFixedHeight(28);
        button->setCursor(Qt::PointingHandCursor);
        const QString kind = QString::fromUtf8(tab.kind);
        connect(button, &QPushButton::clicked, popup,
                [this, popup, search, refill, loadGifs, kind, buttonHost, board, held, addPictureButton]() {
            popup->setProperty("kind", kind);
            const bool emojiTab = kind == QLatin1String("emoji");
            board->setVisible(emojiTab);
            buttonHost->setVisible(!emojiTab);
            if (emojiTab)
                board->setQuery(search->text());
            if (kind == QLatin1String("sticker") && !popup->property("guildStickers").toBool()) {
                popup->setProperty("guildStickers", true);
                for (const GuildInfo &server : m_store->guilds()) {
                    for (const GuildSticker &sticker : server.stickers) {
                        const QString ext =
                            sticker.formatType == 4 ? QStringLiteral("gif") : QStringLiteral("png");
                        const QUrl icon(QStringLiteral("https://media.discordapp.net/stickers/%1.%2?size=160")
                                            .arg(sticker.id, ext));
                        const QString id = sticker.id;
                        const QString filter = server.name + QLatin1Char(' ') + sticker.name;
                        addPictureButton(QStringLiteral("sticker"), filter, icon, [this, popup, id]() {
                            if (m_currentChannelId.isEmpty())
                                return;
                            m_rest->sendMessage(m_currentChannelId, QString(), QString(), {},
                                                [](const QJsonObject &) {},
                                                [this](const RestClient::Error &error) {
                                                    flashStatus(QStringLiteral("Sticker failed (%1).")
                                                                    .arg(error.message.left(120)),
                                                                5000);
                                                },
                                                id);
                            popup->close();
                        });
                    }
                }
            }
            if (kind == QLatin1String("sticker") && !popup->property("packs").toBool()) {
                popup->setProperty("packs", true);
                QPointer<QFrame> alive(popup);
                m_rest->fetchStickerPacks([alive, buttonHost, held, refill, this](const QJsonObject &payload) {
                    if (!alive)
                        return;
                    const QJsonArray packs = payload.value(QStringLiteral("sticker_packs")).toArray();
                    for (const QJsonValue &packValue : packs) {
                        const QJsonArray stickers = packValue.toObject().value(QStringLiteral("stickers")).toArray();
                        for (const QJsonValue &value : stickers) {
                            const QJsonObject sticker = value.toObject();
                            if (sticker.value(QStringLiteral("format_type")).toInt() == 3)
                                continue;
                            const QString id = sticker.value(QStringLiteral("id")).toString();
                            if (id.isEmpty())
                                continue;
                            const int format = sticker.value(QStringLiteral("format_type")).toInt(1);
                            const QString ext = format == 4 ? QStringLiteral("gif") : QStringLiteral("png");
                            const QUrl icon(QStringLiteral("https://media.discordapp.net/stickers/%1.%2?size=160")
                                                .arg(id, ext));
                            const QString name = sticker.value(QStringLiteral("name")).toString();
                            auto *button = new QToolButton(buttonHost);
                            button->setFixedSize(36, 36);
                            button->setProperty("filter", name);
                            button->setProperty("iconUrl", icon);
                            button->setProperty("kind", QStringLiteral("sticker"));
                            const QImage picture = MediaCache::instance().image(icon);
                            if (!picture.isNull()) {
                                button->setIcon(QPixmap::fromImage(picture));
                                button->setIconSize(QSize(26, 26));
                            }
                            connect(button, &QToolButton::clicked, alive.data(), [this, alive, id]() {
                                if (m_currentChannelId.isEmpty())
                                    return;
                                m_rest->sendMessage(m_currentChannelId, QString(), QString(), {},
                                                    [](const QJsonObject &) {},
                                                    [this](const RestClient::Error &error) {
                                                        flashStatus(QStringLiteral("Sticker failed (%1).")
                                                                        .arg(error.message.left(120)),
                                                                    5000);
                                                    },
                                                    id);
                                if (alive)
                                    alive->close();
                            });
                            held->append(button);
                        }
                    }
                    if (alive->property("kind").toString() == QLatin1String("sticker"))
                        refill(QString());
                }, [](const RestClient::Error &) {});
            }
            if (kind == QLatin1String("gif")) {
                search->setPlaceholderText(QStringLiteral("Search GIFs"));
                loadGifs();
            } else if (kind == QLatin1String("sticker")) {
                search->setPlaceholderText(QStringLiteral("Find a sticker"));
            } else {
                search->setPlaceholderText(QStringLiteral("Find an emoji"));
            }
            if (!emojiTab)
                refill(search->text());
        });
        tabs->addWidget(button);
    }
    outer->addLayout(tabs);

    connect(search, &QLineEdit::textChanged, popup, [popup, board, gifTimer, refill](const QString &text) {
        const QString kind = popup->property("kind").toString();
        if (kind == QLatin1String("gif"))
            gifTimer->start();
        else if (kind == QLatin1String("emoji"))
            board->setQuery(text);
        else
            refill(text);
    });
    connect(&MediaCache::instance(), &MediaCache::ready, popup, [popup](const QUrl &url) {
        const QImage picture = MediaCache::instance().image(url);
        if (picture.isNull())
            return;
        const auto found = popup->findChildren<QToolButton *>();
        for (QToolButton *button : found) {
            if (button->property("iconUrl").toUrl() != url)
                continue;
            button->setIcon(QPixmap::fromImage(picture));
            button->setIconSize(QSize(26, 26));
        }
    });

    popup->adjustSize();
    QPoint topLeft = QCursor::pos();
    if (anchor)
        topLeft = anchor->mapToGlobal(QPoint(anchor->width() - popup->width(), -popup->height() - 6));
    popup->move(topLeft);
    popup->show();
    search->setFocus();
}

void MainWindow::showMessageMenu(const QPoint &pos)
{
    const QString messageId = messageIdAt(pos);
    if (messageId.isEmpty() || m_currentChannelId.isEmpty())
        return;

    MessageInfo message;
    const QList<MessageInfo> messages = m_store->messages(m_currentChannelId);
    for (const MessageInfo &candidate : messages) {
        if (candidate.id == messageId) {
            message = candidate;
            break;
        }
    }
    if (message.id.isEmpty())
        return;

    const bool mine = message.authorId == m_selfUserId;
    const ChannelInfo channel = m_store->channel(m_currentChannelId);

    QMenu menu(this);
    connect(menu.addAction(QStringLiteral("Reply")), &QAction::triggered, this, [this, messageId]() {
        beginReply(messageId);
    });
    if (mine) {
        connect(menu.addAction(QStringLiteral("Edit")), &QAction::triggered, this, [this, messageId]() {
            beginEdit(messageId);
        });
        connect(menu.addAction(QStringLiteral("Delete")), &QAction::triggered, this, [this, messageId]() {
            const QString channelId = m_currentChannelId;
            m_rest->deleteMessage(
                channelId, messageId, [](const QJsonObject &) {},
                [this](const RestClient::Error &error) {
                    flashStatus(QStringLiteral("Could not delete that message (%1).").arg(error.message),
                                5000);
                });
        });
    }

    connect(menu.addAction(QStringLiteral("Copy text")), &QAction::triggered, this, [message]() {
        QApplication::clipboard()->setText(message.content);
    });

    const QString link = channel.guildId.isEmpty()
        ? QStringLiteral("https://discord.com/channels/@me/%1/%2").arg(channel.id, message.id)
        : QStringLiteral("https://discord.com/channels/%1/%2/%3").arg(channel.guildId, channel.id, message.id);
    connect(menu.addAction(QStringLiteral("Copy link")), &QAction::triggered, this, [link]() {
        QApplication::clipboard()->setText(link);
    });

    QMenu *react = menu.addMenu(QStringLiteral("Add reaction"));
    const QStringList faces{QStringLiteral("👍"), QStringLiteral("❤️"), QStringLiteral("😂"),
                            QStringLiteral("😮"), QStringLiteral("😢"), QStringLiteral("🔥")};
    for (const QString &face : faces) {
        connect(react->addAction(face), &QAction::triggered, this, [this, messageId, face]() {
            m_rest->addReaction(m_currentChannelId, messageId, face, [](const QJsonObject &) {},
                                [this](const RestClient::Error &error) {
                                    flashStatus(QStringLiteral("Could not react (%1).").arg(error.message),
                                                4000);
                                });
        });
    }

    menu.exec(m_messageView->viewport()->mapToGlobal(pos));
}

void MainWindow::refreshUnreadMarks()
{
    if (!m_guildRail || !m_channelList || !m_store)
        return;

    for (int row = 0; row < m_guildRail->count(); ++row) {
        QListWidgetItem *item = m_guildRail->item(row);
        const QString id = item->data(IdRole).toString();
        if (item->data(KindRole).toString() != QLatin1String("guild"))
            continue;
        item->setData(SingularityRoles::Mentions, m_store->guildMentionCount(id));
        item->setData(SingularityRoles::Unread, m_store->guildHasUnread(id));
    }

    for (int row = 0; row < m_channelList->count(); ++row) {
        QListWidgetItem *item = m_channelList->item(row);
        const QString kind = item->data(KindRole).toString();
        if (kind != QLatin1String("channel") && kind != QLatin1String("dm")
            && kind != QLatin1String("voice"))
            continue;
        const QString id = item->data(IdRole).toString();
        item->setData(SingularityRoles::Mentions, m_store->mentionCount(id));
        item->setData(SingularityRoles::Unread, id == m_currentChannelId ? false : m_store->isUnread(id));
    }

    m_guildRail->viewport()->update();
    m_channelList->viewport()->update();
}

void MainWindow::acknowledgeChannel(const QString &channelId)
{
    if (channelId.isEmpty() || !m_store || !m_rest)
        return;

    QString messageId = m_store->channel(channelId).lastMessageId;
    const QList<MessageInfo> messages = m_store->messages(channelId);
    if (!messages.isEmpty())
        messageId = messages.last().id;
    if (messageId.isEmpty())
        return;

    m_store->markChannelRead(channelId, messageId);
    m_rest->ackMessage(channelId, messageId);
}

void MainWindow::sendCurrentMessage()
{
    if (m_currentChannelId.isEmpty())
        return;

    QString content = m_composer->toPlainText();
    const bool editing = !m_editingMessageId.isEmpty();
    if (content.trimmed().isEmpty() && (editing || m_pendingFiles.isEmpty()))
        return;

    if (!editing && !m_plugins->runOutgoingMessage(content, m_currentChannelId)) {
        flashStatus(QStringLiteral("A plugin cancelled that message."), 4000);
        m_composer->clear();
        clearComposerContext();
        return;
    }

    if (content.length() > 2000) {
        flashStatus(QStringLiteral("Discord caps messages at 2000 characters."), 5000);
        return;
    }

    const QString channelId = m_currentChannelId;
    const QString replyTo = m_replyMessageId;
    const QString editingId = m_editingMessageId;
    const QStringList files = m_pendingFiles;
    m_composer->clear();
    clearComposerContext();

    if (editing) {
        m_rest->editMessage(
            channelId, editingId, content, [](const QJsonObject &) {},
            [this, content](const RestClient::Error &error) {
                flashStatus(QStringLiteral("Edit failed (%1).").arg(error.message), 6000);
                m_composer->setPlainText(content);
            });
        return;
    }

    m_rest->sendMessage(
        channelId, content, replyTo, files, [](const QJsonObject &) {},
        [this, channelId, content, replyTo, files](const RestClient::Error &error) {
            const QJsonArray keys = error.body.value(QStringLiteral("captcha_key")).toArray();
            bool needsCheck = false;
            for (const QJsonValue &key : keys) {
                if (key.toString() == QLatin1String("captcha-required"))
                    needsCheck = true;
            }
            if (needsCheck) {
                const QString token = CaptchaDialog::solve(
                    this, error.body.value(QStringLiteral("captcha_sitekey")).toString(),
                    error.body.value(QStringLiteral("captcha_rqdata")).toString());
                if (token.isEmpty()) {
                    m_composer->setPlainText(content);
                    m_replyMessageId = replyTo;
                    m_pendingFiles = files;
                    refreshComposerContext();
                    flashStatus(QStringLiteral("Discord asked for a check, and it was not finished."),
                                6000);
                    return;
                }
                RestClient::CaptchaProof proof;
                proof.key = token;
                proof.rqtoken = error.body.value(QStringLiteral("captcha_rqtoken")).toString();
                proof.sessionId = error.body.value(QStringLiteral("captcha_session_id")).toString();
                m_rest->sendMessage(channelId, content, replyTo, files, [](const QJsonObject &) {},
                                    [this, content](const RestClient::Error &again) {
                                        m_composer->setPlainText(content);
                                        flashStatus(QStringLiteral("Send failed (%1).")
                                                        .arg(again.message.left(180)),
                                                    8000);
                                    },
                                    QString(), proof);
                return;
            }

            QString reason = error.message;
            if (error.isRateLimit())
                reason = QStringLiteral("rate limited");
            wlog(QStringLiteral("rest"),
                 QStringLiteral("send failed: HTTP %1 %2").arg(error.httpStatus).arg(reason));
            m_composer->setPlainText(content);
            m_replyMessageId = replyTo;
            m_pendingFiles = files;
            refreshComposerContext();
            flashStatus(QStringLiteral("Send failed (%1).").arg(reason.left(180)), 8000);
        });
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
            // Restart, so a channel full of unknown names draws once after
            // the last one arrives instead of once per person.
            m_mentionRefresh.start();
        },
        [userId](const RestClient::Error &error) {
            // Deleted accounts and the like. The number stays, which is honest.
            wlog(QStringLiteral("ui"), QStringLiteral("could not look up user %1: HTTP %2")
                                           .arg(userId)
                                           .arg(error.httpStatus));
        });
}

bool MainWindow::voiceChannelFull(const QString &channelId) const
{
    const ChannelInfo channel = m_store->channel(channelId);
    if (channel.userLimit <= 0 || channelId == m_voiceChannelId)
        return false;
    return m_store->voiceMembers(channelId).size() >= channel.userLimit;
}

void MainWindow::joinVoice(const QString &channelId)
{
    const ChannelInfo channel = m_store->channel(channelId);
    if (!channel.isVoice())
        return;
    joinVoiceAt(channel.guildId, channelId);
}

void MainWindow::joinVoiceAt(const QString &guildId, const QString &channelId)
{
    if (guildId.isEmpty() || channelId.isEmpty())
        return;

    if (voiceChannelFull(channelId)) {
        const ChannelInfo full = m_store->channel(channelId);
        const QString name = full.name.isEmpty() ? QStringLiteral("That voice channel") : full.name;
        flashStatus(QStringLiteral("%1 is full.").arg(name), 4000);
        return;
    }

    const ChannelInfo known = m_store->channel(channelId);
    if (!known.id.isEmpty() && !known.isVoice())
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
    m_voiceGuildId = guildId;
    m_rejoinVoiceAfterGateway = false;
    m_gateway->joinVoice(guildId, channelId, muted, deafened, true);
    updateVoicePanel();

    m_voiceWatchdog.start(10000);
    const QString name = known.name.isEmpty() ? QStringLiteral("voice") : known.name;
    flashStatus(QStringLiteral("Joining %1...").arg(name), 4000);

    if (known.isVoice())
        selectChannelEverywhere(channelId);
    else if (m_callView)
        m_callView->setStageSuppressed(false);
}

void MainWindow::toggleCamera()
{
    if (m_camera && m_camera->isRunning()) {
        stopCamera();
        return;
    }

    if (m_voiceChannelId.isEmpty() || m_voice->state() != VoiceConnection::State::Connected) {
        flashStatus(QStringLiteral("Join a voice channel before turning your camera on."), 4000);
        if (m_cameraButton)
            m_cameraButton->setChecked(false);
        return;
    }
    if (!m_webcam || !m_camera) {
        flashStatus(QStringLiteral("No camera was found on this machine."), 5000);
        if (m_cameraButton)
            m_cameraButton->setChecked(false);
        return;
    }

    m_webcam->start();
    m_camera->start(960, 540, 20);
    if (m_cameraButton) {
        m_cameraButton->setChecked(true);
        m_cameraButton->setText(QStringLiteral("Camera on"));
    }
}

void MainWindow::stopCamera()
{
    if (m_camera)
        m_camera->stop();
    if (m_webcam)
        m_webcam->stop();
    if (m_voice && m_voice->isSendingVideo())
        m_voice->stopSendingVideo();
    if (m_gateway)
        m_gateway->setSelfVideo(false);
    if (m_callView && !m_selfUserId.isEmpty())
        m_callView->dropFrames(m_selfUserId, CallView::Surface::Camera);
    if (m_cameraButton) {
        m_cameraButton->setChecked(false);
        m_cameraButton->setText(QStringLiteral("Camera"));
    }
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
    stopCamera();

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

void MainWindow::loadUserAudio()
{
    const QString raw = AppConfig::instance().value(QStringLiteral("voice/userAudio")).toString();
    const QJsonObject root = QJsonDocument::fromJson(raw.toUtf8()).object();
    m_userAudio.clear();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QJsonObject one = it.value().toObject();
        UserAudioLevel level;
        level.volume = qBound(0, one.value(QStringLiteral("v")).toInt(100), 200);
        level.muted = one.value(QStringLiteral("m")).toBool();
        if (level.volume == 100 && !level.muted)
            continue;
        m_userAudio.insert(it.key(), level);
    }
    syncUserVolumes();
}

void MainWindow::saveUserAudio()
{
    QJsonObject root;
    for (auto it = m_userAudio.cbegin(); it != m_userAudio.cend(); ++it) {
        root.insert(it.key(), QJsonObject{
                                  {QStringLiteral("v"), it->volume},
                                  {QStringLiteral("m"), it->muted},
                              });
    }
    AppConfig::instance().setValue(
        QStringLiteral("voice/userAudio"),
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void MainWindow::syncUserVolumes()
{
    QHash<QString, int> volumes;
    QSet<QString> muted;
    for (auto it = m_userAudio.cbegin(); it != m_userAudio.cend(); ++it) {
        volumes.insert(it.key(), it->volume);
        if (it->muted)
            muted.insert(it.key());
    }
    if (m_voice)
        m_voice->setUserVolumes(volumes, muted);
    if (m_streamVoice)
        m_streamVoice->setUserVolumes(volumes, muted);
}

void MainWindow::rememberUserAudio(const QString &userId, int volume, bool muted, bool publish)
{
    if (userId.isEmpty())
        return;

    volume = qBound(0, volume, 200);
    if (volume == 100 && !muted)
        m_userAudio.remove(userId);
    else
        m_userAudio.insert(userId, UserAudioLevel{volume, muted});

    syncUserVolumes();

    if (!publish) {
        saveUserAudio();
        return;
    }

    // The slider moves many times a second. Discord gets one write after it
    // settles, which is the same number the official client ends on.
    m_volumeDirty.insert(userId);
    m_volumePush.start(400);
}

void MainWindow::flushUserAudio()
{
    if (!m_rest || m_volumeDirty.isEmpty())
        return;

    const QSet<QString> dirty = m_volumeDirty;
    m_volumeDirty.clear();
    saveUserAudio();

    for (const QString &userId : dirty) {
        const UserAudioLevel level = m_userAudio.value(userId);
        m_rest->updateUserVolume(
            userId, level.volume, level.muted, nullptr,
            [userId](const RestClient::Error &error) {
                wlog(QStringLiteral("voice"),
                     QStringLiteral("could not save %1's volume: %2").arg(userId, error.message));
            });
    }
}

void MainWindow::applyAudioSettingsUpdate(const QJsonObject &data)
{
    const QJsonObject users = data.value(QStringLiteral("user")).toObject();
    if (users.isEmpty())
        return;

    bool changed = false;
    for (auto it = users.begin(); it != users.end(); ++it) {
        const QString userId = it.key();
        if (m_volumeDirty.contains(userId))
            continue;

        const QJsonObject one = it.value().toObject();
        UserAudioLevel level = m_userAudio.value(userId);
        if (one.contains(QStringLiteral("volume"))) {
            level.volume = qBound(0, qRound(one.value(QStringLiteral("volume")).toDouble(level.volume)), 200);
        }
        if (one.contains(QStringLiteral("muted")))
            level.muted = one.value(QStringLiteral("muted")).toBool();

        if (level.volume == 100 && !level.muted)
            m_userAudio.remove(userId);
        else
            m_userAudio.insert(userId, level);
        changed = true;
    }

    if (!changed)
        return;

    syncUserVolumes();
    saveUserAudio();
    refreshVolumePopup();
}

void MainWindow::ingestSettingsProto(const QByteArray &proto, bool partial)
{
    QHash<QString, UserAudioLevel> parsed;
    bool present = false;
    if (!audioContextFromProto(proto, &parsed, &present)) {
        wlog(QStringLiteral("voice"), QStringLiteral("could not read saved user volumes"));
        return;
    }
    if (partial && !present)
        return;

    QHash<QString, UserAudioLevel> next = partial ? m_userAudio : QHash<QString, UserAudioLevel>{};
    if (present) {
        for (auto it = parsed.cbegin(); it != parsed.cend(); ++it) {
            if (it->volume == 100 && !it->muted)
                next.remove(it.key());
            else
                next.insert(it.key(), *it);
        }
    }

    // A slider that has not been written yet wins over the copy Discord just
    // echoed, or the drag would jump back.
    for (const QString &userId : m_volumeDirty) {
        if (m_userAudio.contains(userId))
            next.insert(userId, m_userAudio.value(userId));
        else
            next.remove(userId);
    }

    m_userAudio = next;
    syncUserVolumes();
    saveUserAudio();
    refreshVolumePopup();
    if (!partial) {
        wlog(QStringLiteral("voice"),
             QStringLiteral("user volumes loaded: %1").arg(m_userAudio.size()));
    }
}

void MainWindow::refreshVolumePopup()
{
    if (!m_volumePopup)
        return;

    const QString userId = m_volumePopup->property("userId").toString();
    if (userId.isEmpty() || m_volumeDirty.contains(userId))
        return;

    const UserAudioLevel level = m_userAudio.value(userId);
    if (auto *slider = m_volumePopup->findChild<QSlider *>(QStringLiteral("VolumeSlider"))) {
        if (slider->value() != level.volume) {
            QSignalBlocker blocker(slider);
            slider->setValue(level.volume);
        }
    }
    if (auto *readout = m_volumePopup->findChild<QLabel *>(QStringLiteral("VolumeReadout")))
        readout->setText(QStringLiteral("%1%").arg(level.volume));
    if (auto *mute = m_volumePopup->findChild<QCheckBox *>(QStringLiteral("VolumeMute"))) {
        if (mute->isChecked() != level.muted) {
            QSignalBlocker blocker(mute);
            mute->setChecked(level.muted);
        }
    }
}

void MainWindow::showPersonMenu(const QString &userId, const QPoint &globalPos)
{
    if (userId.isEmpty())
        return;

    QMenu menu(this);
    const bool self = userId == m_selfUserId;

    menu.addAction(QStringLiteral("Profile"), this, [this, userId, globalPos]() {
        showProfile(userId, globalPos);
    });

    if (!self) {
        menu.addAction(QStringLiteral("Message"), this, [this, userId]() { openDirectWith(userId); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("User volume"), this, [this, userId, globalPos]() {
            showUserVolumeMenu(userId, globalPos);
        });
    }

    menu.addSeparator();
    menu.addAction(QStringLiteral("Copy User ID"), this, [userId]() {
        QApplication::clipboard()->setText(userId);
    });

    menu.exec(globalPos);
}

void MainWindow::openDirectWith(const QString &userId)
{
    if (!m_rest || userId.isEmpty() || userId == m_selfUserId)
        return;

    const QString existing = m_store->directChannelWith(userId);
    if (!existing.isEmpty()) {
        selectChannelEverywhere(existing);
        return;
    }

    m_rest->openDirectMessage(
        userId,
        [this](const QJsonObject &channel) {
            const QString channelId = channel.value(QStringLiteral("id")).toString();
            if (!channelId.isEmpty())
                selectChannelEverywhere(channelId);
        },
        [](const RestClient::Error &error) {
            wlog(QStringLiteral("ui"),
                 QStringLiteral("could not open the direct message: %1").arg(error.message));
        });
}

bool MainWindow::takeCaptcha(const RestClient::Error &error, RestClient::CaptchaProof *proof)
{
    const QJsonArray keys = error.body.value(QStringLiteral("captcha_key")).toArray();
    bool needed = false;
    for (const QJsonValue &key : keys) {
        if (key.toString() == QLatin1String("captcha-required"))
            needed = true;
    }
    if (!needed)
        return false;

    const QString token = CaptchaDialog::solve(
        this, error.body.value(QStringLiteral("captcha_sitekey")).toString(),
        error.body.value(QStringLiteral("captcha_rqdata")).toString());
    if (token.isEmpty()) {
        wlog(QStringLiteral("gateway"),
             QStringLiteral("Discord asked for a check before the Playing card, and it was not finished"));
        return false;
    }
    proof->key = token;
    proof->rqtoken = error.body.value(QStringLiteral("captcha_rqtoken")).toString();
    proof->sessionId = error.body.value(QStringLiteral("captcha_session_id")).toString();
    return true;
}

void MainWindow::ensureClientActivity()
{
    if (!m_rest)
        return;

    const QString saved = AppConfig::instance().value(QStringLiteral("presence/applicationId")).toString();
    if (!saved.isEmpty()) {
        proxyClientLogo(saved);
        return;
    }
    findSingularityApplication();
}

void MainWindow::findSingularityApplication(const RestClient::CaptchaProof &captcha)
{
    m_rest->listApplications(
        [this](const QJsonArray &apps) {
            for (const QJsonValue &value : apps) {
                const QJsonObject app = value.toObject();
                if (app.value(QStringLiteral("name")).toString() != QLatin1String("Singularity"))
                    continue;
                const QString id = app.value(QStringLiteral("id")).toString();
                if (id.isEmpty())
                    continue;
                AppConfig::instance().setValue(QStringLiteral("presence/applicationId"), id);
                proxyClientLogo(id);
                return;
            }
            createSingularityApplication();
        },
        [this](const RestClient::Error &error) {
            RestClient::CaptchaProof proof;
            if (takeCaptcha(error, &proof)) {
                findSingularityApplication(proof);
                return;
            }
            wlog(QStringLiteral("gateway"),
                 QStringLiteral("could not register the Playing card: HTTP %1 %2")
                     .arg(error.httpStatus)
                     .arg(error.message));
        },
        captcha);
}

void MainWindow::createSingularityApplication(const RestClient::CaptchaProof &captcha)
{
    m_rest->createApplication(
        QStringLiteral("Singularity"),
        [this](const QJsonObject &app) {
            const QString id = app.value(QStringLiteral("id")).toString();
            if (id.isEmpty())
                return;
            AppConfig::instance().setValue(QStringLiteral("presence/applicationId"), id);
            wlog(QStringLiteral("gateway"),
                 QStringLiteral("created the Singularity application %1").arg(id));
            proxyClientLogo(id);
        },
        [this](const RestClient::Error &error) {
            RestClient::CaptchaProof proof;
            if (takeCaptcha(error, &proof)) {
                createSingularityApplication(proof);
                return;
            }
            wlog(QStringLiteral("gateway"),
                 QStringLiteral("could not register the Playing card: HTTP %1 %2")
                     .arg(error.httpStatus)
                     .arg(error.message));
        },
        captcha);
}

void MainWindow::proxyClientLogo(const QString &applicationId)
{
    if (!m_rest || applicationId.isEmpty())
        return;

    m_rest->proxyApplicationAsset(
        applicationId, QStringLiteral("https://singularitycord.pages.dev/mark.png"),
        [this, applicationId](const QJsonArray &assets) {
            QString key;
            if (!assets.isEmpty()) {
                const QString path = assets.first().toObject().value(QStringLiteral("external_asset_path")).toString();
                if (!path.isEmpty())
                    key = QStringLiteral("mp:") + path;
            }
            wlog(QStringLiteral("gateway"),
                 QStringLiteral("Playing card image %1").arg(key.isEmpty() ? QStringLiteral("(none)") : key));
            useClientActivity(applicationId, key);
        },
        [this, applicationId](const RestClient::Error &error) {
            wlog(QStringLiteral("gateway"),
                 QStringLiteral("could not proxy the logo: HTTP %1 %2")
                     .arg(error.httpStatus)
                     .arg(error.message));
            // The name still shows without the picture.
            useClientActivity(applicationId, QString());
        });
}

void MainWindow::useClientActivity(const QString &applicationId, const QString &imageKey)
{
    m_gateway->setClientActivityArt(applicationId, imageKey);
    m_gateway->publishPresence();
    if (m_selfUserId.isEmpty())
        return;
    m_store->setPresence(m_selfUserId,
                         QJsonObject{
                             {QStringLiteral("status"), m_gateway->presenceStatus()},
                             {QStringLiteral("activities"), m_gateway->clientActivities()},
                         });
}

void MainWindow::showUserVolumeMenu(const QString &userId, const QPoint &globalPos)
{
    if (userId.isEmpty() || userId == m_selfUserId)
        return;

    if (m_volumePopup)
        m_volumePopup->close();

    const UserAudioLevel current = m_userAudio.value(userId);

    auto *popup = new QFrame(nullptr, Qt::Popup | Qt::FramelessWindowHint);
    popup->setObjectName(QStringLiteral("UserVolume"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setAttribute(Qt::WA_StyledBackground, true);
    popup->setStyleSheet(QStringLiteral(
        "QFrame#UserVolume { background-color: %1; border: 1px solid %2; border-radius: 12px; }"
        "QLabel#VolumeName { color: %3; font-weight: 600; background: transparent; }"
        "QLabel#VolumeCaption, QLabel#VolumeReadout { color: %4; background: transparent; }"
        "QCheckBox { color: %3; background: transparent; }")
                              .arg(QLatin1String(Theme::SurfaceSidebar), QLatin1String(Theme::Border),
                                   QLatin1String(Theme::TextPrimary), QLatin1String(Theme::TextMuted)));

    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(6);

    QString name = m_store->userName(userId);
    if (name.isEmpty())
        name = QStringLiteral("User volume");
    auto *title = new QLabel(name, popup);
    title->setObjectName(QStringLiteral("VolumeName"));
    layout->addWidget(title);

    auto *caption = new QLabel(QStringLiteral("User volume"), popup);
    caption->setObjectName(QStringLiteral("VolumeCaption"));
    layout->addWidget(caption);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    auto *slider = new QSlider(Qt::Horizontal, popup);
    slider->setObjectName(QStringLiteral("VolumeSlider"));
    slider->setRange(0, 200);
    slider->setValue(current.volume);
    slider->setFixedWidth(188);
    auto *readout = new QLabel(QStringLiteral("%1%").arg(current.volume), popup);
    readout->setObjectName(QStringLiteral("VolumeReadout"));
    readout->setMinimumWidth(44);
    readout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(slider, 1);
    row->addWidget(readout);
    layout->addLayout(row);

    auto *mute = new QCheckBox(QStringLiteral("Mute"), popup);
    mute->setObjectName(QStringLiteral("VolumeMute"));
    mute->setChecked(current.muted);
    layout->addWidget(mute);

    popup->setProperty("userId", userId);

    connect(slider, &QSlider::valueChanged, this, [this, userId, readout, mute](int value) {
        readout->setText(QStringLiteral("%1%").arg(value));
        rememberUserAudio(userId, value, mute->isChecked(), true);
    });
    connect(mute, &QCheckBox::toggled, this, [this, userId, slider](bool on) {
        rememberUserAudio(userId, slider->value(), on, true);
    });
    connect(popup, &QObject::destroyed, this, [this, popup]() {
        if (m_volumePopup == popup)
            m_volumePopup = nullptr;
    });

    popup->adjustSize();
    QPoint pos = globalPos;
    QScreen *screen = QApplication::screenAt(globalPos);
    if (!screen)
        screen = this->screen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        if (pos.x() + popup->width() > area.right())
            pos.setX(qMax(area.left(), area.right() - popup->width() - 8));
        if (pos.y() + popup->height() > area.bottom())
            pos.setY(qMax(area.top(), globalPos.y() - popup->height() - 4));
    }
    popup->move(pos);
    m_volumePopup = popup;
    popup->show();
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

void MainWindow::layoutTitleRow()
{
    if (!m_titleLayout)
        return;

    // Flush with the top of the window. Any inset here is the gap the buttons
    // were sitting under.
    m_titleLayout->setContentsMargins(8, 0, 4, 0);
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
#ifdef Q_OS_WIN
    // The HWND already exists by the first show, which is the first moment
    // the caption bits can be forced on. WM_STYLECHANGING keeps them on
    // afterwards, including when a snap changes the window state.
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style |= WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    style &= ~WS_SYSMENU;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // A frameless window otherwise appears at the top left. The first show
    // puts it in the middle of the work area.
    if (!m_openedCentered && !IsZoomed(hwnd)) {
        m_openedCentered = true;
        QScreen *screen = this->screen();
        if (!screen)
            screen = QApplication::primaryScreen();
        if (screen) {
            const QRect area = screen->availableGeometry();
            QSize fitted = size();
            if (fitted.width() > area.width() || fitted.height() > area.height())
                fitted = fitted.boundedTo(area.size());
            const int x = area.x() + (area.width() - fitted.width()) / 2;
            const int y = area.y() + (area.height() - fitted.height()) / 2;
            SetWindowPos(hwnd, nullptr, x, y, fitted.width(), fitted.height(),
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (m_loading) {
        m_loading->setGeometry(rect());
        m_loading->raise();
        const HWND cover = reinterpret_cast<HWND>(m_loading->winId());
        SetWindowPos(cover, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
#endif
    wlog(QStringLiteral("app"), QStringLiteral("main window shown"));
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange)
        return;

    if (m_captionMax)
        m_captionMax->setText(isMaximized() ? QStringLiteral("❐") : QStringLiteral("□"));
    layoutTitleRow();

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

        // Qt writes the style from the window flags on every show and every
        // state change, and that style has no sizing border. Snap, and the
        // restore size that comes back when a snapped window is pulled out,
        // both require a thick frame. The bits are put back before Windows
        // applies the change.
        //
        // The default calculation keeps a caption and a sizing border. The
        // caption is what left an empty band above the buttons, so the client
        // is pulled up to the top of the window. The left, right and bottom
        // insets stay, and those are the edges you drag to resize.
        if (msg->message == WM_NCCALCSIZE && msg->wParam) {
            auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
            // Maximized, the default client starts below a real caption. That
            // is the gray band. The work area is the screen above the taskbar,
            // so the background fills the top and the taskbar stays visible.
            if (IsZoomed(msg->hwnd)) {
                MONITORINFO info;
                info.cbSize = sizeof(info);
                if (GetMonitorInfo(MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST), &info))
                    params->rgrc[0] = info.rcWork;
                *result = 0;
                return true;
            }
            const int windowTop = params->rgrc[0].top;
            *result = DefWindowProc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
            params->rgrc[0].top = windowTop;
            return true;
        }

        if (msg->message == WM_STYLECHANGING && msg->wParam == GWL_STYLE) {
            auto *change = reinterpret_cast<STYLESTRUCT *>(msg->lParam);
            change->styleNew |= WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
            // The system menu is what paints Windows' own caption buttons.
            // Leaving it on put a second set above the ones in the title row,
            // and reporting the middle one as the real maximize button made
            // a press-and-drag run Windows' snap tracking until the window
            // stopped answering.
            change->styleNew &= ~WS_SYSMENU;
        }

        // The top band is the caption, which is what makes a drag snap. The
        // menu and the three buttons live in that band too, and a caption hit
        // there swallows the click: the menu never opens, and a button press
        // becomes a drag. Those widgets stay client area.
        if (msg->message == WM_NCHITTEST) {
            const QPoint global(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
            const QPoint local = mapFromGlobal(global);
            constexpr int edge = 6;
            const bool onEdge = local.x() < edge || local.y() < edge || local.x() >= width() - edge
                || local.y() >= height() - edge;
            if (!onEdge) {
                for (QWidget *widget = childAt(local); widget && widget != this;
                     widget = widget->parentWidget()) {
                    const QString name = widget->objectName();
                    if (widget == m_menuBar || name.startsWith(QLatin1String("Caption"))) {
                        *result = HTCLIENT;
                        return true;
                    }
                }
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
