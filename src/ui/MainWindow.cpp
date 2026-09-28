#include "ui/MainWindow.h"

#include "core/AppConfig.h"
#include "core/GameActivity.h"
#include "core/Logger.h"
#include "core/RestClient.h"
#include "core/TokenStore.h"
#include "plugin/PluginHost.h"
#include "ui/ChatView.h"
#include "ui/FriendsPage.h"
#include "ui/ImageViewer.h"
#include "ui/Backdrop.h"
#include "ui/CaptchaDialog.h"
#include "core/CameraShare.h"
#include "core/ScreenShare.h"
#include "core/ShareAudio.h"
#include "ui/GuildHeader.h"

#include <QSystemTrayIcon>
#include "ui/LoadingOverlay.h"
#include "ui/MemberListPanel.h"
#include "ui/ShareDialog.h"
#include "ui/UpdateFlow.h"

#include <QProcess>

#include "ui/AnimatedImage.h"
#include "ui/CallView.h"
#include "ui/ChangelogDialog.h"
#include "ui/ListDelegates.h"
#include "ui/LogDialog.h"
#include "ui/MediaCache.h"
#include "ui/MediaCache.h"
#include "ui/PluginsDialog.h"
#include "ui/ProfileDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/Theme.h"
#include "ui/UiTime.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QCloseEvent>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QIcon>
#include <QMessageBox>
#include <QDesktopServices>
#include <QFrame>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>

#include <algorithm>
#include <cmath>
#include <QCamera>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QLineEdit>
#include <QLocale>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>

#include <memory>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QTransform>
#include <QVideoFrame>
#include <QVideoSink>
#include <private/qvideoframeconverter_p.h>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QEnterEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QUrlQuery>
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
#include <QTextCharFormat>
#include <QTextBlock>
#include <QTextFragment>

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
    QStringList games;
    int extras = 0;

    for (const ActivityInfo &activity : presence.activities) {
        if (activity.isCustomStatus()) {
            custom = activity.emoji.isEmpty()
                ? activity.state
                : QStringLiteral("%1 %2").arg(activity.emoji, activity.state).trimmed();
            continue;
        }

        const QString line = (activity.type == 2 && !activity.details.isEmpty())
            ? activity.details
            : activity.name;
        if (line.isEmpty())
            continue;
        if (games.size() < 2)
            games.append(line);
        else
            ++extras;
    }

    if (!games.isEmpty()) {
        QString text = games.join(QStringLiteral(", "));
        if (extras > 0)
            text += QStringLiteral("  +%1").arg(extras);
        return text;
    }

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

// The caption marks used to be font glyphs. A dash, a square and a cross each
// sit on the baseline with their own side bearings, so none of them landed in
// the middle of the button. These are drawn from the button's centre.
class CaptionButton : public QPushButton
{
public:
    enum Kind { Minimize, Maximize, Close };

    CaptionButton(Kind kind, QWidget *parent)
        : QPushButton(parent)
        , m_kind(kind)
    {
        setObjectName(kind == Minimize ? QStringLiteral("CaptionMin")
                      : kind == Maximize ? QStringLiteral("CaptionMax")
                                         : QStringLiteral("CaptionClose"));
        setFlat(true);
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(34, 28);
        setCursor(Qt::ArrowCursor);
    }

    void setRestore(bool restore)
    {
        if (m_restore == restore)
            return;
        m_restore = restore;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPushButton::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool hot = underMouse() || isDown();
        QColor ink(Theme::TextMuted);
        if (m_kind == Close && hot)
            ink = QColor(255, 255, 255);
        else if (hot)
            ink = QColor(Theme::TextPrimary);

        QPen pen(ink);
        pen.setWidthF(1.6);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        const QPointF c = QRectF(rect()).center();
        switch (m_kind) {
        case Minimize:
            painter.drawLine(QPointF(c.x() - 5.0, c.y()), QPointF(c.x() + 5.0, c.y()));
            break;
        case Maximize:
            if (m_restore)
                paintRestore(painter, c);
            else
                painter.drawRect(QRectF(c.x() - 4.5, c.y() - 4.5, 9.0, 9.0));
            break;
        case Close: {
            const qreal arm = 4.25;
            painter.drawLine(QPointF(c.x() - arm, c.y() - arm), QPointF(c.x() + arm, c.y() + arm));
            painter.drawLine(QPointF(c.x() + arm, c.y() - arm), QPointF(c.x() - arm, c.y() + arm));
            break;
        }
        }
    }

private:
    static void paintRestore(QPainter &painter, const QPointF &c)
    {
        // Two squares, the rear one peeking over the top right. The pair is
        // centred as one shape, not as the front square alone.
        const qreal side = 7.0;
        const qreal overlap = 3.0;
        const qreal span = side + overlap;
        const QPointF o(c.x() - span / 2.0, c.y() - span / 2.0);
        const QRectF back(o.x() + overlap, o.y(), side, side);
        const QRectF front(o.x(), o.y() + overlap, side, side);
        painter.drawLine(back.topLeft(), back.topRight());
        painter.drawLine(back.topRight(), back.bottomRight());
        painter.drawRect(front);
    }

    Kind m_kind;
    bool m_restore = false;
};

// The join-a-server mark. A font plus sits on the baseline, so it never lands
// in the middle of the circle. This one is drawn from the centre.
class JoinServerButton : public QToolButton
{
public:
    explicit JoinServerButton(int size, QWidget *parent)
        : QToolButton(parent)
    {
        setObjectName(QStringLiteral("RailJoinButton"));
        setToolTip(QStringLiteral("Join a server"));
        setCursor(Qt::PointingHandCursor);
        setFixedSize(size, size);
        setFocusPolicy(Qt::NoFocus);
        setAutoRaise(true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF box = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
        const bool hot = underMouse() || isDown();
        painter.setPen(Qt::NoPen);
        painter.setBrush(hot ? QColor(Theme::Green) : QColor(Theme::SurfaceInput));
        painter.drawEllipse(box);

        QPen pen(hot ? QColor(255, 255, 255) : QColor(Theme::Green));
        pen.setWidthF(2.6);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        const QPointF c = box.center();
        const qreal arm = box.width() * 0.16;
        painter.drawLine(QPointF(c.x() - arm, c.y()), QPointF(c.x() + arm, c.y()));
        painter.drawLine(QPointF(c.x(), c.y() - arm), QPointF(c.x(), c.y() + arm));
    }
};

// Discord's update control is a green arrow. No circle, no box.
class UpdateButton : public QPushButton
{
public:
    explicit UpdateButton(QWidget *parent)
        : QPushButton(parent)
    {
        setObjectName(QStringLiteral("RailUpdate"));
        setFixedSize(36, 36);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setFlat(true);
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        QPushButton::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        QPushButton::leaveEvent(event);
        update();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool hot = isEnabled() && (underMouse() || isDown());
        QColor ink = isEnabled() ? QColor(Theme::Green) : QColor(Theme::TextFaint);
        if (hot)
            ink = ink.lighter(120);

        QPen pen(ink);
        pen.setWidthF(2.6);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);

        const QPointF c = QRectF(rect()).center();
        const qreal s = 7.0;
        painter.drawLine(QPointF(c.x(), c.y() - s), QPointF(c.x(), c.y() + s * 0.35));
        painter.drawLine(QPointF(c.x() - s * 0.72, c.y() - s * 0.15), QPointF(c.x(), c.y() + s * 0.55));
        painter.drawLine(QPointF(c.x() + s * 0.72, c.y() - s * 0.15), QPointF(c.x(), c.y() + s * 0.55));
    }
};

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
    // No parent: an object with one cannot be moved to another thread, and
    // this one lives on the media thread. The destructor deletes it there.
    , m_voice(new VoiceConnection)
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
    m_gateway->setGames(new GameActivity(this));
    m_gateway->setGameRest(m_rest);
    connect(m_gateway, &GatewayClient::dispatch, this, &MainWindow::onGatewayDispatch);
    connect(m_gateway, &GatewayClient::stateChanged, this, &MainWindow::onGatewayState);
    connect(m_gateway, &GatewayClient::logLine, this, [this](const QString &line) {
        flashStatus(line, 6000);
    });
    connect(m_gateway, &GatewayClient::gameActivityChanged, this, [this]() {
        if (m_selfUserId.isEmpty())
            return;
        m_store->setPresence(m_selfUserId,
                             QJsonObject{
                                 {QStringLiteral("status"), m_gateway->presenceStatus()},
                                 {QStringLiteral("activities"), m_gateway->clientActivities()},
                             });
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

    // Unknown people found while drawing are gathered for a quarter second,
    // then asked about in one gateway message per 100.
    m_memberLookupTimer.setSingleShot(true);
    m_memberLookupTimer.setInterval(250);
    connect(&m_memberLookupTimer, &QTimer::timeout, this, &MainWindow::flushMemberLookups);

    m_mentionRefresh.setSingleShot(true);
    m_mentionRefresh.setInterval(400);
    connect(&m_mentionRefresh, &QTimer::timeout, this, [this]() {
        if (m_currentChannelId.isEmpty() || !m_store->hasHistory(m_currentChannelId))
            return;
        renderChannel();
    });

    m_mentionSearchTimer.setSingleShot(true);
    m_mentionSearchTimer.setInterval(220);
    connect(&m_mentionSearchTimer, &QTimer::timeout, this, [this]() {
        QString query;
        int atPos = -1;
        if (!mentionQuery(&atPos, &query) || query.isEmpty() || m_currentGuildId.isEmpty() || !m_rest)
            return;
        const int serial = ++m_mentionSearchSerial;
        m_mentionSearchedFor = query;
        m_rest->searchGuildMembers(
            m_currentGuildId, query,
            [this, serial, query](const QJsonArray &members) {
                if (serial != m_mentionSearchSerial)
                    return;
                QString still;
                int at = -1;
                if (!mentionQuery(&at, &still) || still != query)
                    return;
                m_mentionSearchHits.clear();
                for (const QJsonValue &value : members) {
                    const QJsonObject member = value.toObject();
                    const QJsonObject user = member.value(QStringLiteral("user")).toObject();
                    m_store->rememberUser(user);
                    const QString id = user.value(QStringLiteral("id")).toString();
                    QString label = member.value(QStringLiteral("nick")).toString();
                    if (label.isEmpty())
                        label = user.value(QStringLiteral("global_name")).toString();
                    if (label.isEmpty())
                        label = user.value(QStringLiteral("username")).toString();
                    if (!id.isEmpty() && !label.isEmpty())
                        m_mentionSearchHits.append({id, label});
                }
                wlog(QStringLiteral("rest"),
                     QStringLiteral("mention search \"%1\" found %2").arg(query).arg(m_mentionSearchHits.size()));
                updateMentionPopup();
            },
            [this, query](const RestClient::Error &error) {
                wlog(QStringLiteral("rest"),
                     QStringLiteral("mention search \"%1\" failed: %2").arg(query, error.message.left(120)));
            });
    });

    connect(m_plugins, &PluginHost::repaintRequested, this, [this]() { scheduleRender(); });
    connect(m_plugins, &PluginHost::pluginLogged, this, [this](const QString &id, const QString &line) {
        flashStatus(QStringLiteral("[%1] %2").arg(id, line), 5000);
    });

    // Windows notifications for plugins. A desktop program raises one through
    // a tray icon, so the icon is made the first time one is needed and then
    // stays, like Discord's; clicking either brings the window forward.
    connect(m_plugins, &PluginHost::notificationRequested, this,
            [this](const QString &id, const QString &title, const QString &text, const QString &channelId) {
                wlog(QStringLiteral("notify"), QStringLiteral("[%1] %2: %3").arg(id, title, text));
                showDesktopNotification(title, text, channelId);
            });

    // A background update finished: said once, quietly, at the bottom.
    UpdateFlow::setDownloadingHandler([this](const QString &version) {
        if (!m_updateButton)
            return;
        if (version.isEmpty()) {   // the download or install failed; it tries again later
            m_updateButton->hide();
            return;
        }
        m_updateButton->setEnabled(false);
        m_updateButton->setToolTip(QStringLiteral("Downloading Singularity %1...").arg(version));
        m_updateButton->show();
    });
    UpdateFlow::setStagedHandler([this](const QString &version) {
        if (m_updateButton) {
            m_updateButton->setEnabled(true);
            m_updateButton->setToolTip(
                QStringLiteral("Singularity %1 is ready. Click to restart and update.").arg(version));
            m_updateButton->show();
        }
        flashStatus(QStringLiteral("Singularity %1 is ready. Click the green arrow to restart, or it opens next time.")
                        .arg(version),
                    10000);
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
    m_streamVoice = new VoiceConnection;
    m_streamVoice->setViewerOnly(true);
    // Cameras and streams turned off earlier stay off.
    {
        const QStringList cameras = AppConfig::instance().value(QStringLiteral("voice/videoHidden")).toStringList();
        m_voice->setVideoOff(QSet<QString>(cameras.begin(), cameras.end()));
        const QStringList streams = AppConfig::instance().value(QStringLiteral("voice/streamVideoHidden")).toStringList();
        m_streamVoice->setVideoOff(QSet<QString>(streams.begin(), streams.end()));
    }
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
    m_shareVoice = new VoiceConnection;
    m_shareVoice->setViewerOnly(true);

    // All three onto the media thread, now that the settings that must be in
    // place before anything runs have been given.
    startMediaThread();

    m_share = new ScreenShare(this);

    // Capture and encoding run on their own thread and hand finished pictures
    // straight to the media thread, where the keys and the socket are.
    //
    // The connection is made against the voice connection itself rather than
    // the window, so pictures never wait in the window's queue. A share used
    // to stall for as long as the window was busy drawing.
    connect(m_share, &ScreenShare::picture, m_shareVoice,
            [voice = m_shareVoice](const QList<QByteArray> &units, bool) {
                voice->sendPicture(units);
            });

    // Our own tile, filled from our own capture. Nothing comes back off the
    // network for a stream we are the one sending.
    connect(m_share, &ScreenShare::preview, this, [this](const QImage &frame) {
        m_lastSharePicture = frame;
        if (m_callView && !m_selfUserId.isEmpty())
            m_callView->setFrame(m_selfUserId, frame, CallView::Surface::Share);
    });

    m_streamPreviewTimer.setSingleShot(true);
    connect(&m_streamPreviewTimer, &QTimer::timeout, this, &MainWindow::uploadStreamPreview);

    connect(m_share, &ScreenShare::started, this,
            [this](int width, int height, const QString &encoder, bool hardware) {
                wlog(QStringLiteral("share"),
                     QStringLiteral("encoding with %1 (%2)")
                         .arg(encoder, hardware ? QStringLiteral("hardware")
                                                : QStringLiteral("software")));
                if (m_shareVoice)
                    m_shareVoice->startSendingVideo(width, height, m_shareFps, m_shareBitrate);

                // Sound starts with the picture, once the stream connection
                // holds its keys, so nothing piles up waiting for them.
                const auto source = ShareAudio::Source(m_shareSoundSource);
                if (m_shareAudio && source != ShareAudio::Source::Nothing)
                    m_shareAudio->start(source, m_shareSoundPid);

                flashStatus(QStringLiteral("You are live — %1x%2, %3%4")
                                .arg(width)
                                .arg(height)
                                .arg(hardware ? QStringLiteral("hardware encoded")
                                              : QStringLiteral("software encoded"))
                                .arg(m_shareSoundName.isEmpty()
                                         ? QStringLiteral(", no sound")
                                         : QStringLiteral(", with %1").arg(m_shareSoundName)),
                            5000);

                // The tile's still picture, once a few pictures exist.
                m_streamPreviewTries = 0;
                m_streamPreviewTimer.start(1500);
                updateVoicePanel();
            });

    connect(m_share, &ScreenShare::failed, this, [this](const QString &reason) {
        wlog(QStringLiteral("share"), QStringLiteral("share failed: %1").arg(reason));
        flashStatus(reason, 7000);
        stopScreenShare();
    });

    // The share's sound goes from the capture thread straight to the stream
    // connection, which takes it under a lock; the window never touches it.
    m_shareAudio = new ShareAudio(this);
    m_shareAudio->setSink([voice = m_shareVoice](const QByteArray &pcm) {
        voice->offerSharedSound(pcm);
    });
    connect(m_shareAudio, &ShareAudio::failed, this,
            [this](const QString &reason) { flashStatus(reason, 6000); });

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
    // Same as the share: straight to the media thread, past the window.
    connect(m_camera, &CameraShare::picture, m_voice,
            [voice = m_voice](const QList<QByteArray> &units, bool) { voice->sendPicture(units); });
    connect(m_camera, &CameraShare::started, this, [this](int width, int height, const QString &encoder) {
        wlog(QStringLiteral("camera"), QStringLiteral("encoding with %1").arg(encoder));
        // The camera runs at 20 pictures a second and 1.5 Mbit (CameraShare).
        if (m_voice)
            m_voice->startSendingVideo(width, height, 20, 1500000);
        m_gateway->setSelfVideo(true);
        flashStatus(QStringLiteral("Camera on — %1x%2").arg(width).arg(height), 4000);
        updateVoicePanel();
    });
    connect(m_camera, &CameraShare::failed, this, [this](const QString &reason) {
        flashStatus(reason, 6000);
        stopCamera();
    });

    // The capture session and its sink last for the whole run. The webcam is
    // chosen from settings by applyCameraDevice() below, and can be swapped
    // later without disturbing this frame path.
    {
        m_cameraSession = new QMediaCaptureSession(this);
        m_cameraSink = new QVideoSink(this);
        m_cameraSession->setVideoSink(m_cameraSink);
        connect(m_cameraSink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &incoming) {
            if (!m_camera || !m_camera->isRunning())
                return;
            QVideoFrame frame = incoming;
            if (!frame.map(QVideoFrame::ReadOnly))
                return;
            // On the CPU: the default sends it to the card and waits for it
            // back, on the window thread.
            QImage image = qImageFromVideoFrame(frame, /*forceCpu=*/true);
            const QtVideo::Rotation rotation = frame.rotation();
            const bool mirrored = frame.mirrored();
            frame.unmap();
            if (image.isNull())
                return;
            // The camera reports which way is up. Drawing the raw frame leaves
            // a sideways picture, which is what other people were seeing.
            QTransform turn;
            switch (rotation) {
            case QtVideo::Rotation::Clockwise90:  turn.rotate(90); break;
            case QtVideo::Rotation::Clockwise180: turn.rotate(180); break;
            case QtVideo::Rotation::Clockwise270: turn.rotate(270); break;
            default: break;
            }
            if (mirrored)
                turn.scale(-1, 1);
            if (!turn.isIdentity())
                image = image.transformed(turn, Qt::SmoothTransformation);
            image = image.scaled(960, 540, Qt::KeepAspectRatio, Qt::FastTransformation)
                        .convertToFormat(QImage::Format_ARGB32);
            m_camera->submit(image);
            if (m_callView && !m_selfUserId.isEmpty())
                m_callView->setFrame(m_selfUserId, image, CallView::Surface::Camera);
        });
    }

    applyCameraDevice();

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
        //
        // 4022 "call terminated" is the same thing when it lands while the
        // gateway is reconnecting: Discord's docs list "the main gateway
        // session was dropped" among its causes. A reconnect (op 7) that could
        // not resume produced exactly that, and treating it as final left the
        // call on its own.
        const bool droppedWithGateway = reason.contains(QStringLiteral("4022"))
                                        && m_gateway->state() != GatewayClient::State::Ready;
        if (reason.contains(QStringLiteral("4014")) || droppedWithGateway) {
            wlog(QStringLiteral("voice"),
                 QStringLiteral("voice server dropped us (%1); staying in the channel to rejoin")
                     .arg(droppedWithGateway ? QStringLiteral("4022 during a gateway reconnect")
                                             : QStringLiteral("4014")));
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

        ++m_voiceRetries;
        wlog(QStringLiteral("voice"), QStringLiteral("retry %1 after: %2").arg(m_voiceRetries).arg(reason));

        if (m_voiceState)
            m_voiceState->setText(QStringLiteral("Retrying (%1)...").arg(m_voiceRetries));

        m_voice->disconnectFromVoice();
        m_pendingVoiceToken.clear();
        m_pendingVoiceEndpoint.clear();

        // The voice connection already tried to resume the call without
        // leaving, and could not. What is left is to leave and join again,
        // which makes Discord throw away the old voice session and hand out a
        // fresh server and ticket.
        //
        // Asking for the same channel without leaving was tried first in
        // 0.6.94 and the log settled it: Discord confirms you are in the
        // channel and sends no new voice server, so it only added ten seconds
        // of silence before this.
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

        // Marks the join as ours and in flight, so Discord's answer is used to
        // connect rather than read as somebody moving us, and so a missing
        // answer is named in the log instead of leaving the panel stuck.
        m_voiceWatchdog.start(10000);
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

        // A rejoin that got no answer is given one more go.
        if (m_voiceRetries == 1) {
            ++m_voiceRetries;
            wlog(QStringLiteral("voice"),
                 QStringLiteral("rejoin got no answer (missing %1); leaving and rejoining once more")
                     .arg(missing.join(QStringLiteral(" and "))));
            if (m_voiceState)
                m_voiceState->setText(QStringLiteral("Retrying (%1)...").arg(m_voiceRetries));
            m_voice->disconnectFromVoice();
            m_pendingVoiceToken.clear();
            m_pendingVoiceEndpoint.clear();
            m_voiceSessionId.clear();
            m_gateway->leaveVoice(m_voiceGuildId);
            m_voiceRetryTimer.start(1200);
            return;
        }

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
    // The window stands on the Backdrop: your picture drawn with QPainter, or
    // the black hole's OpenGL widget as its bottom-most child. See Backdrop.h
    // for why a picture no longer goes through the graphics card. Cards are
    // opaque islands; layout stretch and margins have no widget, so those
    // pixels are the background.
    m_backdrop = new Backdrop(this);
    m_backdrop->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCentralWidget(m_backdrop);

    m_backdrop->installEventFilter(this);

    auto *shell = new QVBoxLayout(m_backdrop);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    m_titleLayout = new QHBoxLayout();
    auto *titleLayout = m_titleLayout;
    titleLayout->setSpacing(0);
    titleLayout->setAlignment(Qt::AlignVCenter);
    layoutTitleRow();

    // A menu bar paints its title high in the strip. A tool button is the
    // same 32 px as the caption marks, with the word in the middle.
    m_appMenu = new QToolButton(m_backdrop);
    m_appMenu->setObjectName(QStringLiteral("AppMenu"));
    m_appMenu->setText(QStringLiteral("Singularity"));
    m_appMenu->setPopupMode(QToolButton::InstantPopup);
    m_appMenu->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_appMenu->setFixedHeight(28);
    m_appMenu->setFocusPolicy(Qt::NoFocus);
    m_appMenu->setCursor(Qt::ArrowCursor);
    m_appMenu->setAutoRaise(false);
    titleLayout->addWidget(m_appMenu, 0, Qt::AlignVCenter);

    // Empty strip between the menu and the caption buttons. A press here is
    // the caption, so the window can be dragged and snapped. The menu and the
    // buttons are not part of that strip: a press on them has to reach them.
    auto *dragStrip = new TitleDragArea(m_backdrop);
    dragStrip->setObjectName(QStringLiteral("TitleDrag"));
    dragStrip->setFixedHeight(32);
    titleLayout->addWidget(dragStrip, 1, Qt::AlignVCenter);

    auto *minBtn = new CaptionButton(CaptionButton::Minimize, m_backdrop);
    m_captionMax = new CaptionButton(CaptionButton::Maximize, m_backdrop);
    auto *closeBtn = new CaptionButton(CaptionButton::Close, m_backdrop);
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

    rootLayout->addWidget(buildGuildRail(m_backdrop));
    rootLayout->addWidget(buildSidebar(m_backdrop));

    auto *chatCard = new QFrame(m_backdrop);
    chatCard->setObjectName(QStringLiteral("ChatColumn"));
    chatCard->setAttribute(Qt::WA_StyledBackground, true);
    chatCard->setAutoFillBackground(false);
    auto *chatLayout = new QVBoxLayout(chatCard);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(0);

    m_chatSplitter = new QSplitter(Qt::Vertical, chatCard);
    m_chatSplitter->setChildrenCollapsible(true);
    m_chatSplitter->setHandleWidth(8);
    m_chatSplitter->setAutoFillBackground(false);

    m_callView = new CallView(m_store, m_chatSplitter);
    {
        const QStringList hidden = AppConfig::instance().value(QStringLiteral("voice/videoHidden")).toStringList();
        m_callView->setVideoHidden(QSet<QString>(hidden.begin(), hidden.end()));
        const QStringList streams = AppConfig::instance().value(QStringLiteral("voice/streamVideoHidden")).toStringList();
        m_callView->setStreamHidden(QSet<QString>(streams.begin(), streams.end()));
    }
    connect(m_callView, &CallView::profileRequested, this,
            [this](const QString &userId) { showProfile(userId, QCursor::pos()); });
    connect(m_callView, &CallView::volumeMenuRequested, this, &MainWindow::showPersonMenu);
    connect(m_callView, &CallView::watchAttempted, this, &MainWindow::watchStream);
    // Your own share gets a full sized picture only while it is the big tile.
    connect(m_callView, &CallView::stageChanged, this,
            [this](const QString &userId, CallView::Surface surface) {
                if (m_share)
                    m_share->setPreviewLarge(!m_selfUserId.isEmpty() && userId == m_selfUserId
                                             && surface == CallView::Surface::Share);
                m_paceStream = !userId.isEmpty() && surface == CallView::Surface::Share;
                updateFramePace();
            });
    // Only the cameras on screen are downloaded, at the size they are drawn.
    connect(m_callView, &CallView::videoViewsChanged, this, [this](const QHash<QString, int> &views) {
        if (m_voice)
            m_voice->setVideoViews(views);
    });
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
    m_chatStack->setAutoFillBackground(false);
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
    connect(m_friends, &FriendsPage::statusMessage, this,
            [this](const QString &text) { flashStatus(text, 6000); });
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
    m_members = new MemberListPanel(m_store, m_backdrop);
    connect(m_members, &MemberListPanel::profileRequested, this,
            [this](const QString &userId) { showProfile(userId, QCursor::pos()); });
    connect(m_members, &MemberListPanel::volumeMenuRequested, this, &MainWindow::showPersonMenu);
    rootLayout->addWidget(m_members);

    shell->addLayout(rootLayout, 1);

    auto *statusChip = new QWidget(m_backdrop);
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

    m_backdrop->setHoleColors(Theme::holeAccent(), Theme::holeDisk(), Theme::holeGrade());
    m_backdrop->setRunning(
        AppConfig::instance().value(QStringLiteral("appearance/animatedBackground"), true).toBool());
    applyBackgroundSettings();
}

QWidget *MainWindow::buildGuildRail(QWidget *parent)
{
    auto *rail = new QWidget(parent);
    rail->setObjectName(QStringLiteral("GuildRail"));
    rail->setAttribute(Qt::WA_StyledBackground, true);
    rail->setAutoFillBackground(false);
    rail->setFixedWidth(RailWidth);

    auto *layout = new QVBoxLayout(rail);
    layout->setContentsMargins(4, 10, 4, 10);
    layout->setSpacing(0);

    // Marks every unread chat read, like Vencord's Read All button above
    // Discord's rail.
    auto *readAllButton = new QPushButton(QStringLiteral("Read All"), rail);
    readAllButton->setObjectName(QStringLiteral("RailReadAll"));
    readAllButton->setCursor(Qt::PointingHandCursor);
    readAllButton->setToolTip(QStringLiteral("Mark every server and chat as read"));
    readAllButton->setStyleSheet(QStringLiteral(
        "QPushButton#RailReadAll { background: transparent; border: none; color: %1; font-size: 11px; "
        "padding: 2px 0 8px 0; }"
        "QPushButton#RailReadAll:hover { color: %2; }")
                                     .arg(QLatin1String(Theme::TextMuted), QLatin1String(Theme::TextPrimary)));
    connect(readAllButton, &QPushButton::clicked, this, &MainWindow::readAll);

    // Discord's update control: a green arrow, nothing around it.
    m_updateButton = new UpdateButton(rail);
    m_updateButton->setToolTip(QStringLiteral("Update ready. Click to restart into it"));
    m_updateButton->hide();
    connect(m_updateButton, &QPushButton::clicked, this, [this]() {
        if (!m_voiceChannelId.isEmpty()) {
            const auto answer = QMessageBox::question(
                this, QStringLiteral("Restart to update"),
                QStringLiteral("You're in a voice call. Restarting to update will disconnect you. Restart now?"),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes)
                return;
        }
        wlog(QStringLiteral("update"), QStringLiteral("Restart to update pressed"));
        // This copy restarts; the restart finds the new version and opens it.
        restartInto({});
    });
    layout->addWidget(m_updateButton, 0, Qt::AlignHCenter);
    layout->addSpacing(6);

    layout->addWidget(readAllButton, 0, Qt::AlignHCenter);

    m_guildRail = new QListWidget(rail);
    m_guildRail->setIconSize(QSize(GuildIconPixels, GuildIconPixels));
    m_guildRail->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_guildRail->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    Theme::showThrough(m_guildRail);
    m_guildRail->setUniformItemSizes(true);
    // The delegate draws the rows itself, so the stylesheet must not.
    m_guildRail->setItemDelegate(new GuildRailDelegate(m_guildRail, m_guildRail));

    // Drag a tile to reorder it. The arrangement is remembered on this machine.
    m_guildRail->setDragDropMode(QAbstractItemView::InternalMove);
    m_guildRail->setDefaultDropAction(Qt::MoveAction);
    m_guildRail->setContextMenuPolicy(Qt::CustomContextMenu);

    layout->addWidget(m_guildRail, 1);

    // Under the servers, where Discord keeps its own: join one by invite.
    auto *joinServer = new JoinServerButton(GuildIconPixels, rail);
    layout->addSpacing(6);
    layout->addWidget(joinServer, 0, Qt::AlignHCenter);
    connect(joinServer, &QToolButton::clicked, this, [this]() { showJoinServerDialog(); });

    connect(m_guildRail, &QListWidget::currentRowChanged, this, &MainWindow::onGuildSelected);
    connect(m_guildRail, &QListWidget::customContextMenuRequested, this, &MainWindow::showRailMenu);

    // After a drag, read the tiles back and save the new order.
    connect(m_guildRail->model(), &QAbstractItemModel::rowsMoved, this, [this]() {
        if (m_rebuildingRail)
            return;
        saveRailOrderFromView();
        railChangedByUser();
    });

    // Dropping onto the middle of a tile, rather than between two, is how a
    // folder is made. Qt's list only knows "between", so the drop is looked
    // at first.
    m_guildRail->viewport()->installEventFilter(this);

    // Changes go to Discord shortly after the last one, so a few quick drags
    // are one save rather than several racing each other.
    m_railPushTimer.setSingleShot(true);
    m_railPushTimer.setInterval(700);
    connect(&m_railPushTimer, &QTimer::timeout, this, &MainWindow::pushRailToDiscord);

    m_rail.load();
    return rail;
}

QWidget *MainWindow::buildSidebar(QWidget *parent)
{
    auto *sidebar = new QWidget(parent);
    sidebar->setObjectName(QStringLiteral("Sidebar"));
    sidebar->setAttribute(Qt::WA_StyledBackground, true);
    sidebar->setAutoFillBackground(false);
    sidebar->setFixedWidth(SidebarWidth);

    auto *layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_sidebarHeader = new GuildHeader(sidebar);
    m_sidebarHeader->setDirectMessages();
    connect(m_sidebarHeader, &GuildHeader::menuRequested, this, &MainWindow::showGuildMenu);
    layout->addWidget(m_sidebarHeader);

    m_channelList = new QListWidget(sidebar);
    m_channelList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    Theme::showThrough(m_channelList);

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
    m_channelList->setAcceptDrops(true);
    m_channelList->viewport()->setAcceptDrops(true);
    m_channelList->installEventFilter(this);
    m_channelList->viewport()->installEventFilter(this);
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

    // A pulse on the window's own thread, to measure how long it is ever too
    // busy to answer. Everything the window draws - the black hole included -
    // waits behind whatever the thread is doing, so this is the number that
    // says whether a dropped frame was ours.
    m_uiPulse.setTimerType(Qt::PreciseTimer);
    m_uiPulse.setInterval(50);
    connect(&m_uiPulse, &QTimer::timeout, this, [this]() {
        if (!m_uiPulseClock.isValid()) {
            m_uiPulseClock.start();
            return;
        }
        const qint64 late = m_uiPulseClock.restart() - 50;
        if (late > m_uiWorstStallMs)
            m_uiWorstStallMs = late;
        if (late > 33)
            ++m_uiStalls;
        // A big one is written down the moment it ends, so it lands in the
        // log right beside whatever caused it rather than in a summary a
        // minute later.
        if (late > 250) {
            wlog(QStringLiteral("perf"),
                 QStringLiteral("window thread was blocked for %1 ms just now; the lines above say by what")
                     .arg(late));
        }
    });
    m_uiPulse.start();

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
        if (m_voiceDragMoved) {
            m_voiceDragMoved = false;
            return;
        }
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
            if (!channel.isDirect())
                return;
            if (channel.type == 1 && channel.recipientIds.size() == 1)
                showPersonMenuAt(channel.recipientIds.first(), at, channel.id);
            else
                showPersonMenuAt(QString(), at, channel.id);
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
    m_panelActivity = iconButton(0xE7FC, QStringLiteral("Activity — games are in Settings"));
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
    panel->setFixedHeight(196);

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
    m_voiceChannelLabel->setCursor(Qt::PointingHandCursor);
    m_voiceChannelLabel->setToolTip(QStringLiteral("Open this channel"));
    m_voiceChannelLabel->installEventFilter(this);
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
    buttons->setContentsMargins(0, 4, 0, 0);
    buttons->setHorizontalSpacing(12);
    buttons->setVerticalSpacing(10);
    buttons->setColumnStretch(0, 1);
    buttons->setColumnStretch(1, 1);

    m_muteButton = new QPushButton(QStringLiteral("Mute"), panel);
    m_muteButton->setFixedHeight(32);
    m_muteButton->setCheckable(true);
    m_muteButton->setToolTip(QStringLiteral("Stop sending your voice"));
    connect(m_muteButton, &QPushButton::toggled, this, [this](bool on) { setSelfMuted(on); });
    buttons->addWidget(m_muteButton, 0, 0);

    m_deafenButton = new QPushButton(QStringLiteral("Deafen"), panel);
    m_deafenButton->setFixedHeight(32);
    m_deafenButton->setCheckable(true);
    m_deafenButton->setToolTip(QStringLiteral("Stop hearing everyone"));
    connect(m_deafenButton, &QPushButton::toggled, this, [this](bool on) { setSelfDeafened(on); });
    buttons->addWidget(m_deafenButton, 0, 1);

    m_shareButton = new QPushButton(QStringLiteral("Share"), panel);
    m_shareButton->setFixedHeight(32);
    m_shareButton->setToolTip(QStringLiteral("Show your screen to everyone in this call"));
    connect(m_shareButton, &QPushButton::clicked, this, &MainWindow::startScreenShare);
    buttons->addWidget(m_shareButton, 1, 0);

    m_cameraButton = new QPushButton(QStringLiteral("Camera"), panel);
    m_cameraButton->setFixedHeight(32);
    m_cameraButton->setCheckable(true);
    m_cameraButton->setToolTip(QStringLiteral("Show your camera to everyone in this call"));
    connect(m_cameraButton, &QPushButton::clicked, this, &MainWindow::toggleCamera);
    buttons->addWidget(m_cameraButton, 1, 1);

    auto *leave = new QPushButton(QStringLiteral("Leave"), panel);
    leave->setFixedHeight(32);
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
        AppConfig::instance().value(QStringLiteral("appearance/fontSize"), 15).toInt()));
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
    connect(cancelContext, &QPushButton::clicked, this, &MainWindow::stopEditing);
    contextLayout->addWidget(m_composerContextText, 1);
    contextLayout->addWidget(cancelContext);
    m_composerContext->hide();
    composerLayout->addWidget(m_composerContext);

    auto *attachmentRow = new QWidget(composerWrap);
    auto *attachmentLayout = new QHBoxLayout(attachmentRow);
    attachmentLayout->setContentsMargins(4, 0, 4, 0);
    m_attachmentLabel = new QLabel(attachmentRow);
    m_attachmentLabel->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));
    m_removeAttachment = new QPushButton(QStringLiteral("Remove"), attachmentRow);
    m_removeAttachment->setObjectName(QStringLiteral("ComposerTool"));
    m_removeAttachment->setFixedHeight(28);
    m_removeAttachment->setCursor(Qt::PointingHandCursor);
    m_removeAttachment->setToolTip(QStringLiteral("Take the file off this message"));
    connect(m_removeAttachment, &QPushButton::clicked, this, [this]() {
        m_pendingFiles.clear();
        refreshComposerContext();
    });
    attachmentLayout->addWidget(m_attachmentLabel, 1);
    attachmentLayout->addWidget(m_removeAttachment);
    attachmentRow->hide();
    composerLayout->addWidget(attachmentRow);

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

    // Files dropped anywhere on the window attach, like Discord. The box takes
    // drops itself (it would type the file's path in), so it is watched too.
    m_composer->viewport()->installEventFilter(this);
    setAcceptDrops(true);

    // The box grows with what is typed, a line at a time, up to eight lines,
    // and scrolls after that - Discord's behaviour. It used to be fixed at one
    // line with the scroll bar off, so a second line pushed the first up out
    // of sight and the text looked cut off.
    const auto fitComposer = [this]() {
        const QFontMetricsF metrics(m_composer->font());
        const qreal line = metrics.lineSpacing();
        const qreal margin = m_composer->document()->documentMargin();
        const qreal oneLine = line + 2 * margin;
        const qreal extra = qMax<qreal>(0, m_composer->document()->size().height() - oneLine);
        const int most = int(32 + 7 * line);
        const int wanted = int(32 + std::ceil(extra));
        m_composer->setFixedHeight(qMin(wanted, most));
        m_composer->setVerticalScrollBarPolicy(wanted > most ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    };
    connect(m_composer->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [fitComposer](const QSizeF &) { fitComposer(); });

    // The buttons stay beside the first line as the box grows, as in Discord.
    row->addWidget(attach, 0, Qt::AlignTop);
    row->addWidget(m_composer, 1);
    m_stopEdit = new QPushButton(QStringLiteral("Stop"), composerBox);
    m_stopEdit->setObjectName(QStringLiteral("ComposerTool"));
    m_stopEdit->setFixedHeight(28);
    m_stopEdit->setCursor(Qt::PointingHandCursor);
    m_stopEdit->setToolTip(QStringLiteral("Stop editing this message"));
    m_stopEdit->hide();
    connect(m_stopEdit, &QPushButton::clicked, this, &MainWindow::stopEditing);
    row->addWidget(m_stopEdit, 0, Qt::AlignTop);
    row->addWidget(emoji, 0, Qt::AlignTop);
    boxLayout->addLayout(row);

    composerLayout->addWidget(composerBox);
    layout->addWidget(composerWrap);

    connect(m_composer, &QTextEdit::textChanged, this, &MainWindow::onComposerChanged);
    return chat;
}

void MainWindow::buildMenu()
{
    if (!m_appMenu)
        return;
    auto *fileMenu = new QMenu(m_appMenu);

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

    // Every release and what it changed, newest first.
    auto *changelogAction = fileMenu->addAction(QStringLiteral("What's new..."));
    connect(changelogAction, &QAction::triggered, this, [this]() {
        auto *dialog = new ChangelogDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });

    fileMenu->addSeparator();

    auto *logOutAction = fileMenu->addAction(QStringLiteral("Log out"));
    connect(logOutAction, &QAction::triggered, this, &MainWindow::logOut);

    auto *quitAction = fileMenu->addAction(QStringLiteral("Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    // Shortcuts live on the window. A tool button's menu is not a menu bar,
    // so they would otherwise only fire while the menu is open.
    addAction(settingsAction);
    addAction(pluginsAction);
    addAction(logAction);
    addAction(quitAction);

    m_appMenu->setMenu(fileMenu);
}

void MainWindow::startSession(const QString &token)
{
    // Kept for the account switcher. Never logged.
    m_sessionToken = token;

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
    if (m_gateway->games())
        m_gateway->games()->start(m_rest);
    m_selfPremiumType = user.value(QStringLiteral("premium_type")).toInt();
    m_selfAvatarHash = user.value(QStringLiteral("avatar")).toString();
    m_selfDisplayName = user.value(QStringLiteral("global_name")).toString();
    if (m_selfDisplayName.isEmpty())
        m_selfDisplayName = user.value(QStringLiteral("username")).toString();

    // Into the account switcher - but only when this session is the one saved
    // to disk, which is to say "stay signed in" was ticked. An account signed
    // in without it must not be quietly kept somewhere else instead.
    if (!m_sessionToken.isEmpty() && AppConfig::instance().token() == m_sessionToken) {
        TokenStore::rememberAccount(TokenStore::Account{
            m_selfUserId, user.value(QStringLiteral("username")).toString(), m_selfAvatarHash, m_sessionToken});
    }

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
    m_notifyRules = NotificationRules();
    m_notifyRules.ingestReady(payload, m_selfUserId);
    const QByteArray settingsProto = QByteArray::fromBase64(
        payload.value(QStringLiteral("user_settings_proto")).toString().toLatin1());
    if (!settingsProto.isEmpty())
        ingestSettingsProto(settingsProto, false);

    // Discord's arrangement of the rail replaces whatever this machine had.
    applyDiscordFolders(settingsProto, false);
    populateGuildRail();

    // Say it again now that the session exists, using the status Discord has
    // stored for the account rather than the one this machine last saw.
    //
    // The copy sent before the socket was open was only remembered, and
    // Discord does not treat the status inside the first sign-in as the one
    // other people should see - so it does need saying. But what it says has
    // to be the account's, not ours: another device may have changed it
    // since, and saying ours back overwrote that.
    {
        const QString stored = statusFromProto(settingsProto);
        setPresenceStatus(stored.isEmpty() ? m_gateway->presenceStatus() : stored,
                          /*storeOnDiscord*/ false);
    }
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

    if (eventType == QLatin1String("INTERACTION_MODAL_CREATE")) {
        showInteractionModal(data);
        return;
    }

    if (eventType == QLatin1String("INTERACTION_IFRAME_MODAL_CREATE")) {
        flashStatus(QStringLiteral("That button opened a page this client cannot show."), 5000);
        return;
    }

    if (eventType == QLatin1String("INTERACTION_FAILURE")) {
        const int reason = data.value(QStringLiteral("reason_code")).toInt();
        flashStatus(reason == 2 ? QStringLiteral("The bot did not answer in time.")
                                 : QStringLiteral("That button did not go through."),
                    5000);
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
            // In a direct message every message counts, as in Discord: the
            // red number on a DM is how many you have not read.
            bool mention = data.value(QStringLiteral("guild_id")).toString().isEmpty()
                || data.value(QStringLiteral("mention_everyone")).toBool()
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
            if (!mine)
                maybeNotify(data, message);

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

    if (eventType == QLatin1String("CHANNEL_DELETE")) {
        // Nobody can still be sitting in a channel that no longer exists.
        m_store->dropVoiceStatesInChannel(data.value(QStringLiteral("id")).toString());
        applyChannelClosed(data.value(QStringLiteral("id")).toString());
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

    // Notification settings changed in any client, or your roles changed.
    if (eventType == QLatin1String("USER_GUILD_SETTINGS_UPDATE")) {
        m_notifyRules.applySettingsEntry(data);
        return;
    }
    if (eventType == QLatin1String("GUILD_MEMBER_UPDATE")
        && data.value(QStringLiteral("user")).toObject().value(QStringLiteral("id")).toString() == m_selfUserId) {
        QStringList roles;
        for (const QJsonValue &role : data.value(QStringLiteral("roles")).toArray())
            roles.append(role.toString());
        m_notifyRules.setSelfRoles(data.value(QStringLiteral("guild_id")).toString(), roles);
        m_store->setSelfRoles(data.value(QStringLiteral("guild_id")).toString(), roles);
        // Fall through: anything else that wants member updates still gets it.
    }

    // Left, kicked or banned - from here or any other device. "unavailable"
    // means an outage instead, and the server comes back by itself.
    if (eventType == QLatin1String("GUILD_DELETE")) {
        if (!data.value(QStringLiteral("unavailable")).toBool())
            guildGone(data.value(QStringLiteral("id")).toString());
        return;
    }

    if (eventType == QLatin1String("GUILD_CREATE")) {
        const QString guildId = data.value(QStringLiteral("id")).toString();
        m_notifyRules.applyGuild(data);
        const bool joinedJustNow = !guildId.isEmpty() && m_store->guild(guildId).id.isEmpty();
        m_store->applyGuild(data);
        if (guildId == m_currentGuildId)
            populateChannelList(false);

        // A server we were not in before: it needs a tile on the rail, and
        // the invite cards for it now say Joined.
        if (joinedJustNow) {
            m_rebuildingRail = true;
            populateGuildRail();
            m_rebuildingRail = false;
            for (auto it = m_invites.constBegin(); it != m_invites.constEnd(); ++it) {
                if (it.value().guildId == guildId)
                    redrawInviteWaiters(it.key());
            }
        }

        // The server a join from here was waiting for: open it.
        if (!guildId.isEmpty() && guildId == m_pendingJoinGuildId) {
            const QString channelId = m_pendingJoinChannelId;
            m_pendingJoinGuildId.clear();
            m_pendingJoinChannelId.clear();
            if (!channelId.isEmpty())
                selectChannelEverywhere(channelId);
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

    if (eventType == QLatin1String("VOICE_STATE_UPDATE_BATCH")) {
        // Big servers group their joins and leaves into this. Discord's own
        // client sends every entry down the same path as a single update, and
        // so does this: everyone else in one go, and our own entry, if there
        // is one, through the full handling below so a move is still noticed.
        const QJsonArray states = data.value(QStringLiteral("voice_states")).toArray();
        QJsonArray others;
        QList<QJsonObject> ours;
        for (const QJsonValue &value : states) {
            const QJsonObject state = value.toObject();
            if (!m_selfUserId.isEmpty()
                && state.value(QStringLiteral("user_id")).toString() == m_selfUserId)
                ours.append(state);
            else
                others.append(state);
        }
        m_store->setVoiceStates(others);
        for (const QJsonObject &state : ours)
            onGatewayDispatch(QStringLiteral("VOICE_STATE_UPDATE"), state);
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

            // Our account, but not this program's session: you are in voice on
            // your phone or in another client. Discord's VoiceStateStore only
            // treats a state as its own when the session matches, and this
            // did not check - so joining on a phone made the desktop join the
            // same call and take the connection away from the phone.
            const QString stateSession = data.value(QStringLiteral("session_id")).toString();
            const QString ourSession = m_gateway->sessionId();
            if (!stateSession.isEmpty() && !ourSession.isEmpty() && stateSession != ourSession) {
                wlog(QStringLiteral("voice"),
                     QStringLiteral("that is our account on another device; leaving this program as it is"));
                return;
            }

            // A late copy of where we were, sent just before our own "leave"
            // reached Discord. Taken at face value it looked like being moved
            // back into the channel we had just left, so the program joined it
            // again - and the real "you left" that followed was then ignored
            // because a join was in flight. Anything naming the channel we
            // left in the last few seconds is that echo.
            if (!nowIn.isEmpty() && nowIn == m_leftVoiceChannelId && m_leftVoiceAt.isValid()
                && m_leftVoiceAt.elapsed() < 8000 && !m_voiceWatchdog.isActive()) {
                wlog(QStringLiteral("voice"),
                     QStringLiteral("late echo of the channel we just left; ignoring it"));
                return;
            }

            if (nowIn.isEmpty() && !m_voiceChannelId.isEmpty()) {
                // Our own retry left on purpose and is about to rejoin. This
                // used to be read as "you left", which cleared the channel and
                // cancelled the rejoin - the retry itself kicked you out.
                if (m_voiceRetryTimer.isActive()) {
                    wlog(QStringLiteral("voice"),
                         QStringLiteral("own state: (left) from our own retry — rejoining shortly"));
                    return;
                }
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

    // The answer to requestGuildMembers: a member object per person asked
    // about. Their user part is all the sidebar needs - name and picture.
    if (eventType == QLatin1String("GUILD_MEMBERS_CHUNK")) {
        const QJsonArray members = data.value(QStringLiteral("members")).toArray();
        const bool forMention = data.value(QStringLiteral("nonce")).toString() == QLatin1String("s-mention");
        if (forMention)
            m_mentionSearchHits.clear();
        for (const QJsonValue &value : members) {
            const QJsonObject member = value.toObject();
            m_store->rememberUser(member.value(QStringLiteral("user")).toObject());
            if (forMention) {
                const QJsonObject user = member.value(QStringLiteral("user")).toObject();
                const QString id = user.value(QStringLiteral("id")).toString();
                QString label = member.value(QStringLiteral("nick")).toString();
                if (label.isEmpty())
                    label = m_store->userName(id);
                if (!id.isEmpty() && !label.isEmpty())
                    m_mentionSearchHits.append({id, label});
            }
        }
        const int missing = data.value(QStringLiteral("not_found")).toArray().size();
        wlog(QStringLiteral("gateway"), QStringLiteral("learned %1 people in guild %2%3")
                                            .arg(members.size())
                                            .arg(data.value(QStringLiteral("guild_id")).toString())
                                            .arg(missing ? QStringLiteral(", %1 not found").arg(missing)
                                                         : QString()));
        if (!m_voiceRefreshTimer.isActive())
            m_voiceRefreshTimer.start(400);
        m_mentionRefresh.start();
        if (forMention && m_mentionPopup && m_mentionPopup->isVisible())
            updateMentionPopup();
        return;
    }

    if (eventType == QLatin1String("RELATIONSHIP_REMOVE")) {
        m_store->setRelationship(data.value(QStringLiteral("id")).toString(), 0);
        return;
    }

    // A change made on another device that keeps the person, such as a
    // request turning into a friendship. Only the type matters here.
    if (eventType == QLatin1String("RELATIONSHIP_UPDATE")) {
        const QJsonValue type = data.value(QStringLiteral("type"));
        if (type.isDouble())
            m_store->setRelationship(data.value(QStringLiteral("id")).toString(), type.toInt());
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
        if (entry.folder.hasColor)
            item->setData(SingularityRoles::FolderColor, QColor(QRgb(entry.folder.color & 0xFFFFFF)));

        // An unnamed folder is listed by what is in it, as Discord does.
        QString label = entry.folder.name;
        if (label.isEmpty()) {
            QStringList names;
            for (const QString &guildId : entry.folder.guildIds.mid(0, 3))
                names.append(m_store->guild(guildId).name);
            label = names.join(QStringLiteral(", "));
            if (entry.folder.guildIds.size() > 3)
                label += QStringLiteral("...");
        }
        item->setToolTip(QStringLiteral("%1 (%2 servers)").arg(label).arg(entry.folder.guildIds.size()));
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

        if (id.isEmpty() || kind == QLatin1String("unreadDm"))
            continue;   // direct messages, and the unread ones under them

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

        railChangedByUser();
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

        // Most folders made in Discord have no name, and Discord shows those
        // by the servers in them. An empty name here was a blank line, which
        // is why the list looked empty.
        QString label = entry.folder.name.trimmed();
        if (label.isEmpty()) {
            QStringList names;
            for (const QString &guildId : entry.folder.guildIds) {
                const QString name = m_store->guild(guildId).name;
                if (!name.isEmpty())
                    names << name;
                if (names.size() == 3)
                    break;
            }
            label = names.isEmpty() ? QStringLiteral("Folder") : names.join(QStringLiteral(", "));
            if (entry.folder.guildIds.size() > names.size() && !names.isEmpty())
                label += QStringLiteral(" and %1 more").arg(entry.folder.guildIds.size() - names.size());
        }
        QAction *target = moveTo->addAction(QFontMetrics(moveTo->font()).elidedText(label, Qt::ElideRight, 320));
        target->setToolTip(label);
        moveTargets.insert(target, entry.folder.id);
    }

    QAction *takeOut = nullptr;
    if (!inFolder.isEmpty())
        takeOut = menu.addAction(QStringLiteral("Take out of folder"));

    menu.addSeparator();
    QAction *markRead = menu.addAction(QStringLiteral("Mark As Read"));
    markRead->setEnabled(m_store->guildHasUnread(id));
    QAction *copyId = menu.addAction(QStringLiteral("Copy Server ID"));
    menu.addSeparator();
    QAction *leave = menu.addAction(QStringLiteral("Leave Server"));

    QAction *chosen = menu.exec(m_guildRail->mapToGlobal(where));
    if (!chosen)
        return;

    if (chosen == markRead) {
        markGuildRead(id);
        return;
    }
    if (chosen == copyId) {
        QApplication::clipboard()->setText(id);
        return;
    }
    if (chosen == leave) {
        leaveGuild(id);
        return;
    }

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

    railChangedByUser();
    populateGuildRail();
}

bool MainWindow::handleRailDrag(QEvent *event)
{
    const auto setTarget = [this](int row) {
        if (row == m_railMergeRow)
            return;
        m_railMergeRow = row;
        if (auto *delegate = qobject_cast<GuildRailDelegate *>(m_guildRail->itemDelegate()))
            delegate->setMergeRow(row);
        m_guildRail->viewport()->update();
    };

    if (event->type() == QEvent::DragLeave) {
        setTarget(-1);
        return false;
    }

    // DragMove is a kind of drop event, so both read the same way.
    auto *drop = static_cast<QDropEvent *>(event);
    const QPoint pos = drop->position().toPoint();

    // The tile being dragged is the one the drag started from.
    const QList<QListWidgetItem *> picked = m_guildRail->selectedItems();
    QListWidgetItem *source = picked.isEmpty() ? nullptr : picked.first();
    const bool sourceIsServer = source && source->data(KindRole).toString() == QLatin1String("guild")
        && !source->data(IdRole).toString().isEmpty();

    // Over the middle half of a tile is "onto"; the top and bottom quarters
    // stay "between", so reordering still works exactly as before.
    QListWidgetItem *target = m_guildRail->itemAt(pos);
    int mergeRow = -1;
    if (sourceIsServer && target && target != source && !target->data(IdRole).toString().isEmpty()) {
        const QRect box = m_guildRail->visualItemRect(target);
        const bool middle = pos.y() > box.top() + box.height() / 4 && pos.y() < box.bottom() - box.height() / 4;
        const QString kind = target->data(KindRole).toString();
        if (middle && (kind == QLatin1String("guild") || kind == QLatin1String("folder")))
            mergeRow = m_guildRail->row(target);
    }

    if (event->type() == QEvent::DragMove) {
        setTarget(mergeRow);
        if (mergeRow < 0)
            return false;
        drop->acceptProposedAction();
        return true;
    }

    // The drop itself.
    setTarget(-1);
    if (mergeRow < 0)
        return false;

    const QString draggedId = source->data(IdRole).toString();
    const QString targetId = target->data(IdRole).toString();
    const bool ontoFolder = target->data(KindRole).toString() == QLatin1String("folder");

    // Refused as far as Qt's own drag is concerned. Accepting it as a move
    // would have the list delete the dragged row once the drag ended - after
    // the rail had already been rebuilt below, taking a tile with it.
    drop->setDropAction(Qt::IgnoreAction);
    drop->accept();

    // Changed once the drag is fully over, not in the middle of it.
    QTimer::singleShot(0, this, [this, draggedId, targetId, ontoFolder]() {
        if (ontoFolder)
            m_rail.insertIntoFolder(draggedId, targetId, QString());
        else
            m_rail.mergeIntoNewFolder(targetId, draggedId);
        railChangedByUser();
        m_rebuildingRail = true;
        populateGuildRail();
        m_rebuildingRail = false;
    });
    return true;
}

void MainWindow::railChangedByUser()
{
    m_railPushTimer.start();
}

void MainWindow::pushRailToDiscord()
{
    // Never before Discord's own arrangement has been read. Sending first
    // would replace the folders on every device with whatever this machine
    // happened to have saved.
    if (!m_railSynced || !m_rest) {
        wlog(QStringLiteral("rail"), QStringLiteral("folders not sent: Discord's copy has not been read yet"));
        return;
    }
    if (m_railPushInFlight) {
        m_railPushTimer.start(); // send again once this one is answered
        return;
    }

    const QJsonArray folders = m_rail.toDiscordFolders();
    int realFolders = 0;
    for (const QJsonValue &value : folders) {
        if (!value.toObject().value(QStringLiteral("id")).isNull())
            ++realFolders;
    }

    m_railPushInFlight = true;
    m_rest->updateGuildFolders(
        folders,
        [this, realFolders, count = folders.size()](const QJsonObject &) {
            m_railPushInFlight = false;
            wlog(QStringLiteral("rail"),
                 QStringLiteral("Discord stored the server rail: %1 tiles, %2 folders")
                     .arg(count)
                     .arg(realFolders));
        },
        [this](const RestClient::Error &error) {
            m_railPushInFlight = false;
            wlog(QStringLiteral("rail"),
                 QStringLiteral("Discord refused the server rail: HTTP %1 %2")
                     .arg(error.httpStatus)
                     .arg(error.message));
            flashStatus(QStringLiteral("Your folders changed here but Discord did not save them (%1).")
                            .arg(error.httpStatus),
                        6000);
        });
}

void MainWindow::applyDiscordFolders(const QByteArray &settingsProto, bool partial)
{
    QList<DiscordFolder> folders;
    bool present = false;
    if (!guildFoldersFromProto(settingsProto, &folders, &present)) {
        wlog(QStringLiteral("rail"), QStringLiteral("could not read Discord's server folders"));
        return;
    }
    if (!present) {
        // A full sign-in with no folders at all means none have ever been
        // made on this account; the rail is still in step with Discord.
        if (!partial)
            m_railSynced = true;
        return;
    }

    // A change of ours is waiting to be sent or is on its way. What just
    // arrived is older than it, so it must not undo it.
    if (partial && (m_railPushTimer.isActive() || m_railPushInFlight))
        return;

    QStringList knownIds;
    for (const GuildInfo &guild : m_store->guilds())
        knownIds.append(guild.id);

    m_rail.applyFromDiscord(folders, knownIds);
    m_railSynced = true;

    int realFolders = 0;
    for (const DiscordFolder &folder : folders) {
        if (folder.id != 0)
            ++realFolders;
    }
    wlog(QStringLiteral("rail"),
         QStringLiteral("server rail from Discord: %1 folders").arg(realFolders));

    if (partial) {
        m_rebuildingRail = true;
        populateGuildRail();
        m_rebuildingRail = false;
    }
}

void MainWindow::refreshGuildIcons()
{
    // Faces on the unread direct message tiles arrive the same way.
    refreshDmTiles();

    for (int row = 0; row < m_guildRail->count(); ++row) {
        QListWidgetItem *item = m_guildRail->item(row);
        const QString guildId = item->data(IdRole).toString();
        if (guildId.isEmpty() || item->data(KindRole).toString() == QLatin1String("unreadDm"))
            continue;

        // A folder shows small pictures of the first four servers inside.
        if (item->data(KindRole).toString() == QLatin1String("folder")) {
            const RailLayout::Folder *folder = m_rail.folder(guildId);
            if (!folder)
                continue;
            QVariantList previews;
            for (const QString &memberId : folder->guildIds.mid(0, 4)) {
                const GuildInfo member = m_store->guild(memberId);
                const QUrl url = MediaCache::guildIconUrl(memberId, member.iconHash, 64);
                const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
                previews.append(picture.isNull()
                                    ? MediaCache::initialsAvatar(member.name.isEmpty() ? QStringLiteral("?")
                                                                                       : member.name,
                                                                 32)
                                    : MediaCache::circular(picture, 32));
            }
            item->setData(SingularityRoles::FolderIcons, previews);
            continue;
        }

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

    // An unread direct message: open that chat.
    if (item->data(KindRole).toString() == QLatin1String("unreadDm")) {
        // The home tile takes the highlight, since this one goes away as soon
        // as the chat is read; then the chat opens.
        const QString channelId = item->data(IdRole).toString();
        QTimer::singleShot(0, this, [this, channelId]() {
            m_guildRail->setCurrentRow(0);
            selectChannelEverywhere(channelId);
        });
        return;
    }

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
    if (m_currentGuildId.isEmpty())
        m_sidebarHeader->setDirectMessages();
    else
        m_sidebarHeader->setGuild(m_store->guild(m_currentGuildId));

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
                requestUnknownName(memberId, m_currentGuildId);

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
            if (state.serverDeafened)
                marks << QStringLiteral("server deafened");
            else if (state.deafened)
                marks << QStringLiteral("cannot hear anyone");
            else if (state.serverMuted)
                marks << QStringLiteral("server muted");
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
    m_store->noteMemberListRequest(guildId);
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
    hideMentionPopup();
    m_mentionSearchHits.clear();

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
    // A reply always stands on its own. Grouping it hides the line that says
    // who it answered.
    if (current.type == 19 || !current.replyToId.isEmpty())
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

void MainWindow::startMediaThread()
{
    // Runs on the new thread itself, before anything else does, so the
    // priority is in place by the time the first beat is scheduled.
    connect(&m_mediaThread, &QThread::started, []() { VoiceConnection::prepareMediaThread(); });

    m_mediaThread.setObjectName(QStringLiteral("singularity_media"));
    for (VoiceConnection *voice : {m_voice, m_streamVoice, m_shareVoice}) {
        if (voice)
            voice->moveToThread(&m_mediaThread);
    }
    m_mediaThread.start(QThread::TimeCriticalPriority);
}

MainWindow::~MainWindow()
{
    // Each connection is deleted on the thread it lives on - its sockets and
    // timers belong to that thread and cannot be torn down from this one -
    // and only then is the thread stopped. The share connection goes first
    // because the watching one hands its sound to the main one.
    //
    // The screen's sound thread writes into the share connection, so it is
    // stopped before that connection is gone.
    if (m_shareAudio)
        m_shareAudio->stop();
    const bool running = m_mediaThread.isRunning();
    for (VoiceConnection *voice : {m_shareVoice, m_streamVoice, m_voice}) {
        if (!voice)
            continue;
        if (running)
            QMetaObject::invokeMethod(voice, [voice]() { delete voice; }, Qt::BlockingQueuedConnection);
        else
            delete voice;
    }
    m_shareVoice = nullptr;
    m_streamVoice = nullptr;
    m_voice = nullptr;

    m_mediaThread.quit();
    m_mediaThread.wait(3000);
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

    // Printed beside memory because it is read the same way: once a minute,
    // as a number rather than a feeling. Over 33 ms is two frames at sixty.
    wlog(QStringLiteral("ui"),
         QStringLiteral("window thread: worst stall %1 ms, %2 stalls over 33 ms in the last minute")
             .arg(m_uiWorstStallMs)
             .arg(m_uiStalls));
    // And where its time went, in the same minute.
    wlog(QStringLiteral("perf"), SingularityApplication::takeReport());
    m_uiWorstStallMs = 0;
    m_uiStalls = 0;

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
        m_messageView->setHtml(emptyChannelHtml());
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

    html += wavePromptHtml(all);

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

    // The empty-DM Wave line is not a message frame. A first message, or a
    // wave landing, has to rebuild so that line comes and goes with the chat.
    if (m_renderedCount == 0 || messageHasWave(message)
        || (isOneToOneDm() && m_store->messages(m_currentChannelId).size() <= 11)) {
        renderChannel();
        return;
    }

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

    // A message that is only one link, whose preview is one picture or GIF,
    // shows the picture and not the link - Discord's rule (its message
    // content code checks for exactly one embed, of type image or gifv, and
    // content that is a single link and nothing else).
    const auto wordsOf = [this](const MessageInfo &m) {
        static const QRegularExpression oneLink(QStringLiteral(R"(^https?://\S+$)"));
        const QString trimmed = m.content.trimmed();
        // Not a fake-Nitro emoji link: that one is drawn by the words as an
        // emoji, and its big picture is the part that is dropped.
        if (m.embeds.size() == 1 && m.embeds.first().replacesItsLink() && oneLink.match(trimmed).hasMatch()
            && !trimmed.contains(QLatin1String("/emojis/")))
            return QString();
        return renderContent(m.content, true);
    };

    QString body = wordsOf(message);

    // Discord writes "(edited)" small and faint straight after the words, not
    // beside the name or the time.
    if (message.edited && !body.isEmpty())
        body += QStringLiteral("&#160;<span class=\"tag-edited\">(edited)</span>");

    const auto attachmentsHtml = [](const QList<Attachment> &attachments) {
        QString html;
        for (const Attachment &attachment : attachments) {
            const QString safeUrl = attachment.url.toHtmlEscaped();
            const QString label = attachment.filename.isEmpty() ? QStringLiteral("file")
                                                                : attachment.filename.toHtmlEscaped();

            if (attachment.isImage() && ChatView::isAllowedImageHost(QUrl(attachment.url))) {
                // Wrapped so a click opens the big view rather than a browser.
                html += QStringLiteral("<div class=\"attach\"><a href=\"singularity-image:%1\">"
                                       "<img src=\"%1\"></a></div>")
                            .arg(safeUrl);
            } else {
                html += QStringLiteral("<div class=\"file\"><a href=\"%1\">%2</a>%3</div>")
                            .arg(safeUrl, label, humanSize(attachment.size));
            }
        }
        return html;
    };

    body += attachmentsHtml(message.attachments);

    // A forward, drawn the way Discord draws it: a grey bar down the left,
    // "Forwarded" in italics, the original's words and pictures, and where it
    // came from underneath. The outer message is empty, so this is all of it.
    for (const MessageInfo &snapshot : message.snapshots) {
        QString inner = QStringLiteral("<div class=\"fwd-tag\">&#8618; <i>Forwarded</i></div>");
        const QString words = wordsOf(snapshot);
        if (!words.isEmpty())
            inner += QStringLiteral("<div>%1</div>").arg(words);
        inner += attachmentsHtml(snapshot.attachments);
        inner += stickersHtml(snapshot);
        inner += embedsHtml(snapshot);
        inner += componentsHtml(snapshot.components, message.id, message.applicationId, message.flags);

        // Where it came from: the channel, when we know it, and when it was
        // first sent. The channel name opens that channel.
        QStringList origin;
        const ChannelInfo source = m_store->channel(message.forwardedFromChannelId);
        if (!source.id.isEmpty() && !source.isDirect() && !source.name.isEmpty()) {
            origin << QStringLiteral("<a href=\"singularity-channel:%1\" class=\"fwd-link\">#%2</a>")
                          .arg(source.id, source.name.toHtmlEscaped());
        }
        if (snapshot.timestamp.isValid()) {
            const bool today = snapshot.timestamp.date() == QDate::currentDate();
            origin << QLocale().toString(snapshot.timestamp, today ? QStringLiteral("h:mm AP")
                                                                   : QStringLiteral("M/d/yy, h:mm AP"));
        }
        if (!origin.isEmpty())
            inner += QStringLiteral("<div class=\"fwd-src\">%1</div>").arg(origin.join(QStringLiteral(" &#8226; ")));

        body += QStringLiteral("<table class=\"fwd\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                               "<td width=\"3\" bgcolor=\"%1\"></td>"
                               "<td class=\"fwd-inner\">%2</td></tr></table>")
                    .arg(QLatin1String(Theme::Border), inner);
    }

    body += stickersHtml(message);
    body += embedsHtml(message);
    body += componentsHtml(message.components, message.id, message.applicationId, message.flags);
    body += inviteCardsHtml(message);
    body += activityInviteHtml(message);
    body += reactionsHtml(message);

    if (body.isEmpty())
        body = QStringLiteral("<span class=\"system\">(no text)</span>");

    const QString bodyClass = message.deleted ? QStringLiteral("body deleted") : QStringLiteral("body");

    // A reply sits on its own line above the message: a small hook, their
    // picture, their name, and a short bit of what they said. That is how
    // Discord draws it. The hook is a picture, because rich text cannot draw
    // that curve.
    QString replyHtml;
    if (message.type == 19 || !message.replyToId.isEmpty() || message.replyMissing) {
        QString authorId = message.replyAuthorId;
        QString authorName = message.replyAuthorName;
        QString authorAvatar = message.replyAuthorAvatar;
        QString snippet = message.replyContent;
        bool edited = message.replyEdited;
        bool attachment = message.replyHasAttachment;
        if (authorName.isEmpty() && !message.replyToId.isEmpty() && !message.replyMissing) {
            const QList<MessageInfo> known = m_store->messages(message.channelId);
            for (const MessageInfo &other : known) {
                if (other.id != message.replyToId)
                    continue;
                authorId = other.authorId;
                authorName = other.authorName;
                authorAvatar = other.authorAvatar;
                snippet = other.content;
                edited = other.edited;
                attachment = !other.attachments.isEmpty() || !other.embeds.isEmpty() || !other.stickers.isEmpty();
                break;
            }
        }
        snippet = snippet.simplified();
        if (snippet.size() > 80)
            snippet = snippet.left(77) + QStringLiteral("...");
        QString preview;
        if (message.replyMissing)
            preview = QStringLiteral("<i class=\"reply-text\">Original message was deleted</i>");
        else if (snippet.isEmpty() && attachment)
            preview = QStringLiteral("<span class=\"reply-text\">Click to see attachment</span>");
        else
            preview = QStringLiteral("<span class=\"reply-text\">%1</span>").arg(snippet.toHtmlEscaped());
        if (edited && !message.replyMissing)
            preview += QStringLiteral(" <span class=\"tag-edited\">(edited)</span>");

        QString face;
        if (!authorId.isEmpty()) {
            QUrl avatar = MediaCache::avatarUrl(authorId, authorAvatar, 32);
            if (!avatar.isEmpty()) {
                avatar.setFragment(QStringLiteral("a16"));
                face = QStringLiteral("<a href=\"singularity-user:%1\"><img src=\"%2\" width=\"16\" height=\"16\"></a> ")
                           .arg(authorId, avatar.toString().toHtmlEscaped());
            }
        }
        const QString name = authorName.isEmpty()
                                 ? QString()
                                 : QStringLiteral("<a class=\"reply-name\" href=\"singularity-user:%1\">%2</a> ")
                                       .arg(authorId, authorName.toHtmlEscaped());
        replyHtml = QStringLiteral(
                        "<table class=\"reply\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                        "<td width=\"56\" align=\"right\" valign=\"middle\">"
                        "<img src=\"singularity-spine:hook\" width=\"22\" height=\"12\"></td>"
                        "<td valign=\"middle\">%1%2%3</td></tr></table>")
                        .arg(face, name, preview);
    }

    // A grouped message has no avatar and no name, only the text, lined up
    // under the block it belongs to. The narrow strip on the left carries the
    // clock time, the way the real client shows it on hover.
    if (grouped) {
        return replyHtml + QStringLiteral(
                   "<table class=\"row\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">"
                   "<tr><td width=\"56\" valign=\"top\" nowrap class=\"gut\">%1</td>"
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

    return replyHtml + QStringLiteral(
               "<table class=\"row\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">"
               "<tr>"
               "<td width=\"56\" valign=\"top\" class=\"ava\">%1</td>"
               "<td valign=\"top\"><div class=\"hdr\">"
               "<a class=\"namelink\" href=\"%2\"><span class=\"%3\">%4</span></a>%5</div>"
               "<div class=\"%6\">%7</div></td>"
               "</tr></table>")
        .arg(avatarCell, profileLink, authorClass, displayName, decorations, bodyClass, body);
}

// A Components V2 text block is markdown with a little more than a message
// has: # / ## / ### headings and -# small print, a line at a time.
QString MainWindow::textDisplayHtml(const QString &markdown)
{
    QStringList lines;
    for (const QString &line : markdown.split(QLatin1Char('\n'))) {
        struct Prefix { const char *mark; int px; bool bold; bool faint; };
        static const Prefix prefixes[] = {
            {"### ", 15, true, false}, {"## ", 17, true, false}, {"# ", 20, true, false}, {"-# ", 11, false, true},
        };
        bool done = false;
        for (const Prefix &prefix : prefixes) {
            if (!line.startsWith(QLatin1String(prefix.mark)))
                continue;
            const QString inner = renderContent(line.mid(int(qstrlen(prefix.mark))));
            lines << QStringLiteral("<span style=\"font-size:%1px;%2%3\">%4</span>")
                         .arg(prefix.px)
                         .arg(prefix.bold ? QStringLiteral(" font-weight:700;") : QString())
                         .arg(prefix.faint ? QStringLiteral(" color:%1;").arg(QLatin1String(Theme::TextFaint)) : QString())
                         .arg(inner);
            done = true;
            break;
        }
        if (!done)
            lines << renderContent(line);
    }
    return lines.join(QStringLiteral("<br>"));
}

// Discord's message components, drawn with what Qt's rich text can do.
//
//   1  a row of buttons or a menu        10 a block of text (markdown)
//   9  a section: text with a picture    11 a thumbnail picture
//      or button beside it               12 a gallery of pictures
//   13 a file                            14 a divider or a gap
//   17 a container: a box with a coloured edge, holding the rest
//
// Buttons show what they say. A link button opens its address. Any other
// button sends the press to the bot, the way Discord does when you click it.
// The style sits on the link itself. A span inside the link is not a link
// to Qt, so the pill used to draw and then ignore the click.
QString MainWindow::componentsHtml(const QJsonArray &components, const QString &messageId,
                                   const QString &applicationId, int messageFlags, int depth)
{
    if (components.isEmpty() || depth > 6)
        return {};

    const auto mediaUrl = [](const QJsonObject &media) {
        QString url = media.value(QStringLiteral("proxy_url")).toString();
        if (url.isEmpty())
            url = media.value(QStringLiteral("url")).toString();
        return url;
    };
    const auto picture = [](const QString &url, int width) {
        if (url.isEmpty() || !ChatView::isAllowedImageHost(QUrl(url)))
            return QString();
        const QString safe = url.toHtmlEscaped();
        return width > 0 ? QStringLiteral("<a href=\"singularity-image:%1\"><img src=\"%1\" width=\"%2\"></a>").arg(safe).arg(width)
                         : QStringLiteral("<a href=\"singularity-image:%1\"><img src=\"%1\"></a>").arg(safe);
    };
    const auto button = [messageId, applicationId, messageFlags](const QJsonObject &b) {
        QString label = b.value(QStringLiteral("label")).toString().toHtmlEscaped();
        const QJsonObject emoji = b.value(QStringLiteral("emoji")).toObject();
        if (!emoji.isEmpty()) {
            const QString id = emoji.value(QStringLiteral("id")).toString();
            const QString face = id.isEmpty()
                ? emoji.value(QStringLiteral("name")).toString().toHtmlEscaped()
                : QStringLiteral("<img src=\"https://cdn.discordapp.com/emojis/%1.%2?size=48\" width=\"18\" height=\"18\">")
                      .arg(id, emoji.value(QStringLiteral("animated")).toBool() ? QStringLiteral("gif") : QStringLiteral("png"));
            label = label.isEmpty() ? face : face + QStringLiteral("&#160;") + label;
        }
        const int style = b.value(QStringLiteral("style")).toInt(2);
        const bool disabled = b.value(QStringLiteral("disabled")).toBool();
        const char *fill = Theme::SurfaceHover;
        if (style == 1) fill = Theme::Accent;
        else if (style == 3) fill = Theme::Green;
        else if (style == 4) fill = Theme::Red;
        const char *ink = disabled ? Theme::TextFaint
                                   : (style == 1) ? Theme::Dark : Theme::TextPrimary;
        const QString url = b.value(QStringLiteral("url")).toString();
        const bool link = style == 5 && !url.isEmpty();
        QString href;
        if (!disabled && link) {
            href = url;
        } else if (!disabled && !messageId.isEmpty()) {
            const QString customId = b.value(QStringLiteral("custom_id")).toString();
            if (!customId.isEmpty()) {
                QUrl press;
                press.setScheme(QStringLiteral("singularity-press"));
                press.setHost(QStringLiteral("button"));
                QUrlQuery query;
                query.addQueryItem(QStringLiteral("m"), messageId);
                query.addQueryItem(QStringLiteral("a"), applicationId);
                query.addQueryItem(QStringLiteral("f"), QString::number(messageFlags));
                query.addQueryItem(QStringLiteral("c"), customId);
                press.setQuery(query);
                href = press.toString(QUrl::FullyEncoded);
            }
        }
        const QString face = QStringLiteral("&#160;%1%2&#160;")
                                 .arg(label.isEmpty() ? QStringLiteral("&#8943;") : label,
                                      link ? QStringLiteral(" &#8599;") : QString());
        const QString look = QStringLiteral("text-decoration:none; background-color:%1; color:%2; padding:4px 12px;")
                                 .arg(QLatin1String(fill), QLatin1String(ink));
        if (href.isEmpty())
            return QStringLiteral("<span style=\"%1\">%2</span>").arg(look, face);
        return QStringLiteral("<a class=\"cmpbtn\" href=\"%1\" style=\"%2\">%3</a>")
            .arg(href.toHtmlEscaped(), look, face);
    };

    QString html;
    for (const QJsonValue &value : components) {
        const QJsonObject c = value.toObject();
        switch (c.value(QStringLiteral("type")).toInt()) {
        case 1: {   // a row of buttons, or one menu
            QStringList parts;
            for (const QJsonValue &inner : c.value(QStringLiteral("components")).toArray()) {
                const QJsonObject item = inner.toObject();
                const int type = item.value(QStringLiteral("type")).toInt();
                if (type == 2) {
                    parts << button(item);
                } else if (type >= 3 && type <= 8) {
                    QString hint = item.value(QStringLiteral("placeholder")).toString();
                    if (hint.isEmpty())
                        hint = QStringLiteral("Make a selection");
                    const QString look = QStringLiteral("text-decoration:none; background-color:%1; color:%2; padding:4px 12px;")
                                             .arg(QLatin1String(Theme::SurfaceInput), QLatin1String(Theme::TextMuted));
                    const QString face = QStringLiteral("&#160;%1 &#9662;&#160;").arg(hint.toHtmlEscaped());
                    const QString customId = item.value(QStringLiteral("custom_id")).toString();
                    if (type == 3 && !item.value(QStringLiteral("disabled")).toBool()
                        && !messageId.isEmpty() && !customId.isEmpty()) {
                        QUrl press;
                        press.setScheme(QStringLiteral("singularity-press"));
                        press.setHost(QStringLiteral("select"));
                        QUrlQuery query;
                        query.addQueryItem(QStringLiteral("m"), messageId);
                        query.addQueryItem(QStringLiteral("a"), applicationId);
                        query.addQueryItem(QStringLiteral("f"), QString::number(messageFlags));
                        query.addQueryItem(QStringLiteral("c"), customId);
                        press.setQuery(query);
                        parts << QStringLiteral("<a class=\"cmpbtn\" href=\"%1\" style=\"%2\">%3</a>")
                                    .arg(press.toString(QUrl::FullyEncoded).toHtmlEscaped(), look, face);
                    } else {
                        parts << QStringLiteral("<span style=\"%1\">%2</span>").arg(look, face);
                    }
                }
            }
            if (!parts.isEmpty())
                html += QStringLiteral("<div style=\"margin-top:6px; margin-bottom:2px;\">%1</div>")
                            .arg(parts.join(QStringLiteral("&#160;&#160;")));
            break;
        }
        case 10:    // text
            html += QStringLiteral("<div style=\"margin-top:2px; margin-bottom:2px;\">%1</div>")
                        .arg(textDisplayHtml(c.value(QStringLiteral("content")).toString()));
            break;
        case 9: {   // text with a picture or a button beside it
            const QString text = componentsHtml(c.value(QStringLiteral("components")).toArray(),
                                                 messageId, applicationId, messageFlags, depth + 1);
            const QJsonObject accessory = c.value(QStringLiteral("accessory")).toObject();
            QString side;
            if (accessory.value(QStringLiteral("type")).toInt() == 11)
                side = picture(mediaUrl(accessory.value(QStringLiteral("media")).toObject()), 80);
            else if (accessory.value(QStringLiteral("type")).toInt() == 2)
                side = button(accessory);
            html += side.isEmpty()
                ? text
                : QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr><td valign=\"top\">%1</td>"
                                 "<td valign=\"top\" align=\"right\" style=\"padding-left:12px;\">%2</td></tr></table>")
                      .arg(text, side);
            break;
        }
        case 11:    // a thumbnail on its own
            html += picture(mediaUrl(c.value(QStringLiteral("media")).toObject()), 80);
            break;
        case 12: {  // a gallery
            for (const QJsonValue &item : c.value(QStringLiteral("items")).toArray()) {
                const QString img = picture(mediaUrl(item.toObject().value(QStringLiteral("media")).toObject()), 0);
                if (!img.isEmpty())
                    html += QStringLiteral("<div class=\"attach\">%1</div>").arg(img);
            }
            break;
        }
        case 13: {  // a file
            const QJsonObject file = c.value(QStringLiteral("file")).toObject();
            QString name = c.value(QStringLiteral("name")).toString();
            if (name.isEmpty())
                name = file.value(QStringLiteral("url")).toString().section(QLatin1Char('/'), -1);
            html += QStringLiteral("<div class=\"file\">&#128206; %1</div>").arg(name.toHtmlEscaped());
            break;
        }
        case 14:    // a divider, or just space
            html += c.value(QStringLiteral("divider")).toBool(true)
                ? QStringLiteral("<hr style=\"margin-top:6px; margin-bottom:6px;\">")
                : QStringLiteral("<div style=\"margin-top:%1px;\"></div>")
                      .arg(c.value(QStringLiteral("spacing")).toInt(1) == 2 ? 16 : 8);
            break;
        case 17: {  // a box with a coloured edge, like an embed
            const int accent = c.value(QStringLiteral("accent_color")).toInt(-1);
            const QString edge = accent >= 0 ? QColor(QRgb(accent)).name() : QString::fromLatin1(Theme::Border);
            html += QStringLiteral("<table class=\"embed\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                                   "<td width=\"4\" bgcolor=\"%1\"></td><td class=\"embed-inner\">%2</td></tr></table>")
                        .arg(edge, componentsHtml(c.value(QStringLiteral("components")).toArray(),
                                                   messageId, applicationId, messageFlags, depth + 1));
            break;
        }
        default:
            break;
        }
    }
    return html;
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
            const int textSize =
                qBound(12, AppConfig::instance().value(QStringLiteral("appearance/fontSize"), 15).toInt(), 24);
            const int pixels = qRound(textSize * 16 / 14.0);
            face = QStringLiteral("<img src=\"https://cdn.discordapp.com/emojis/%1.%2?size=48#e%3\" "
                                  "width=\"%3\" height=\"%3\">")
                       .arg(reaction.id, ext)
                       .arg(pixels);
        } else {
            face = reaction.name.toHtmlEscaped();
        }

        // Yours is marked, the way the real client outlines one you pressed.
        const char *background = reaction.mine ? Theme::SurfaceHover : Theme::SurfaceInput;
        const char *ink = reaction.mine ? Theme::TextPrimary : Theme::TextMuted;

        // A pill that is not a link cannot be pressed. Carl-bot colour roles
        // and the rest of reaction-roles are these pills, so they have to be.
        const QString emoji = reaction.isCustom()
            ? (reaction.name + QLatin1Char(':') + reaction.id)
            : reaction.name;
        QUrl press;
        press.setScheme(QStringLiteral("singularity-react"));
        press.setHost(QStringLiteral("toggle"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("m"), message.id);
        query.addQueryItem(QStringLiteral("e"), emoji);
        press.setQuery(query);

        html += QStringLiteral("<a href=\"%1\" style=\"text-decoration:none; background-color: %2; color: %3; "
                               "border-radius: 8px; padding: 2px 7px;\">%4&#160;%5</a>&#160;")
                    .arg(press.toString(QUrl::FullyEncoded).toHtmlEscaped(),
                         QLatin1String(background), QLatin1String(ink), face)
                    .arg(reaction.count);
    }

    html += QStringLiteral("</div>");
    return html;
}

namespace {

// Wumpus Wave from the default "Wumpus Beyond" pack. Discord's empty-DM
// button sends this sticker. format_type 3 (Lottie) — we cannot draw it,
// but Discord still accepts it as a wave.
constexpr auto kWaveStickerId = "749054660769218631";

bool isUserChat(const MessageInfo &message)
{
    // 0 default, 19 reply, 20 slash, 23 context-menu command.
    switch (message.type) {
    case 0:
    case 19:
    case 20:
    case 23:
        return true;
    default:
        return false;
    }
}

} // namespace

bool MainWindow::messageHasWave(const MessageInfo &message)
{
    for (const StickerInfo &sticker : message.stickers) {
        if (sticker.id == QLatin1String(kWaveStickerId))
            return true;
    }
    return false;
}

bool MainWindow::isOneToOneDm() const
{
    if (m_currentChannelId.isEmpty() || !m_store)
        return false;
    const ChannelInfo channel = m_store->channel(m_currentChannelId);
    return channel.type == 1 && channel.recipientIds.size() == 1;
}

QString MainWindow::emptyChannelHtml() const
{
    const QString wave = wavePromptHtml({});
    if (!wave.isEmpty()) {
        const ChannelInfo channel = m_store->channel(m_currentChannelId);
        const UserInfo other = m_store->user(channel.recipientIds.first());
        QString name = other.displayName();
        if (name.isEmpty())
            name = channel.name;
        if (name.isEmpty())
            name = QStringLiteral("them");
        return QStringLiteral("<p class=\"system\">This is the beginning of your direct message "
                              "history with %1.</p>%2")
            .arg(name.toHtmlEscaped(), wave);
    }
    return QStringLiteral("<p class=\"system\">No messages here yet.</p>");
}

QString MainWindow::wavePromptHtml(const QList<MessageInfo> &messages) const
{
    if (!isOneToOneDm())
        return {};
    if (m_waveSentChannelId == m_currentChannelId)
        return {};

    bool weWaved = false;
    bool theyWaved = false;
    for (const MessageInfo &message : messages) {
        const bool wave = messageHasWave(message);
        if (message.authorId == m_selfUserId) {
            if (wave || isUserChat(message))
                weWaved = true;
        } else if (wave) {
            theyWaved = true;
        }
    }
    if (weWaved)
        return {};
    // A new DM (a handful of lines, including "accepted your friend request")
    // still gets Wave to. An old chat does not grow a button at the bottom.
    if (!theyWaved && messages.size() > 10)
        return {};

    const ChannelInfo channel = m_store->channel(m_currentChannelId);
    const UserInfo other = m_store->user(channel.recipientIds.first());
    QString name = other.displayName();
    if (name.isEmpty())
        name = channel.name;
    if (name.isEmpty())
        name = QStringLiteral("them");

    const QString label = theyWaved ? QStringLiteral("👋 Wave back")
                                    : QStringLiteral("👋 Wave to %1").arg(name.toHtmlEscaped());
    return QStringLiteral("<p><a class=\"wave\" href=\"singularity-wave:\">%1</a></p>").arg(label);
}

void MainWindow::sendWave()
{
    if (m_currentChannelId.isEmpty() || !m_rest)
        return;

    const QString channelId = m_currentChannelId;
    m_waveSentChannelId = channelId;
    wlog(QStringLiteral("ui"), QStringLiteral("waving in %1").arg(channelId));

    const auto fail = [this, channelId](const QString &why) {
        if (m_waveSentChannelId == channelId)
            m_waveSentChannelId.clear();
        flashStatus(why, 5000);
        if (channelId == m_currentChannelId)
            renderChannel();
    };

    const auto ok = [this, channelId](const QJsonObject &) {
        wlog(QStringLiteral("ui"), QStringLiteral("wave sent"));
        if (channelId == m_currentChannelId)
            renderChannel();
    };

    const QString sticker = QString::fromLatin1(kWaveStickerId);

    m_rest->sendMessage(
        channelId, QString(), QString(), {}, ok,
        [this, channelId, sticker, ok, fail](const RestClient::Error &error) {
            RestClient::CaptchaProof proof;
            if (CaptchaDialog::isDemand(error.body)) {
                if (!takeCaptcha(error, &proof)) {
                    fail(QStringLiteral("Discord asked for a check, and it was not finished."));
                    return;
                }
                m_rest->sendMessage(channelId, QString(), QString(), {}, ok,
                                    [fail](const RestClient::Error &) { fail(QStringLiteral("Wave failed.")); },
                                    sticker, proof);
                return;
            }

            // Standard stickers can be refused without Nitro. A wave emoji
            // still says it, and Discord accepts that from anyone.
            wlog(QStringLiteral("ui"),
                 QStringLiteral("wave sticker refused (HTTP %1), sending 👋 instead")
                     .arg(error.httpStatus));
            m_rest->sendMessage(
                channelId, QStringLiteral("👋"), QString(), {}, ok,
                [this, channelId, ok, fail](const RestClient::Error &again) {
                    RestClient::CaptchaProof proof;
                    if (CaptchaDialog::isDemand(again.body)) {
                        if (!takeCaptcha(again, &proof)) {
                            fail(QStringLiteral("Discord asked for a check, and it was not finished."));
                            return;
                        }
                        m_rest->sendMessage(channelId, QStringLiteral("👋"), QString(), {}, ok,
                                            [fail](const RestClient::Error &) {
                                                fail(QStringLiteral("Wave failed."));
                                            },
                                            QString(), proof);
                        return;
                    }
                    fail(QStringLiteral("Wave failed."));
                });
        },
        sticker);
}

QString MainWindow::embedsHtml(const MessageInfo &message)
{
    QString html;

    // Discord turns a fake-Nitro emoji link into a big picture under the
    // message. The text already shows it as an emoji, so the big copy goes.
    const auto isEmojiPicture = [](const QString &address) {
        const QUrl url(address);
        const QString host = url.host();
        return (host == QLatin1String("cdn.discordapp.com") || host == QLatin1String("media.discordapp.net"))
               && url.path().startsWith(QLatin1String("/emojis/"));
    };

    for (const EmbedInfo &embed : message.embeds) {
        if (isEmojiPicture(embed.url) || (embed.url.isEmpty() && isEmojiPicture(embed.imageUrl)))
            continue;

        // gifv's image is a poster. The mp4 next to it is what actually plays.
        //
        // The poster goes along in the address ("#poster=..."), so the chat
        // shows it at once and keeps it if the video never plays. A Klipy GIF
        // pasted into chat was a blank icon because nothing stood in for it.
        QString shown = embed.imageUrl;
        QString opened = embed.imageUrl;   // what a click opens, without the poster
        const bool hasVideo = (embed.type == QLatin1String("gifv") || embed.type == QLatin1String("video"))
                              && !embed.videoUrl.isEmpty();
        if (hasVideo) {
            const QUrl video(embed.videoUrl);
            const QString path = video.path().toLower();
            if ((path.endsWith(QLatin1String(".mp4")) || path.endsWith(QLatin1String(".webm")))
                && ChatView::isAllowedImageHost(video)) {
                opened = embed.videoUrl;
                shown = embed.videoUrl;
                if (!embed.imageUrl.isEmpty() && ChatView::isAllowedImageHost(QUrl(embed.imageUrl))) {
                    shown += QStringLiteral("#poster=")
                             + QString::fromLatin1(QUrl::toPercentEncoding(embed.imageUrl));
                }
            }
        }
        const bool pictureReachable = !shown.isEmpty() && ChatView::isAllowedImageHost(QUrl(shown));

        // A Tenor, Klipy or Giphy link is only a picture, so skip the card
        // frame. A gifv with a video and no poster is one too.
        if (embed.isPictureOnly()) {
            if (!pictureReachable) {
                wlog(QStringLiteral("media"), QStringLiteral("a %1 preview has no picture we can load: image %2, video %3")
                                                  .arg(embed.type, embed.imageUrl.left(160), embed.videoUrl.left(160)));
                continue;
            }
            html += QStringLiteral("<div class=\"attach\"><a href=\"singularity-image:%1\">"
                                   "<img src=\"%2\"></a></div>")
                        .arg(opened.toHtmlEscaped(), shown.toHtmlEscaped());
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
                                    "<img src=\"%2\"></a></div>")
                         .arg(opened.toHtmlEscaped(), shown.toHtmlEscaped());
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

namespace {

// A code point that starts an emoji: pictographs, symbols, arrows and the
// few older characters Discord draws as emoji.
bool startsEmoji(char32_t c)
{
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF)
           || (c >= 0x2300 && c <= 0x23FF) || (c >= 0x2B00 && c <= 0x2BFF)
           || (c >= 0x2190 && c <= 0x21FF) || (c >= 0x25AA && c <= 0x25FE)
           || c == 0x2934 || c == 0x2935 || c == 0x3030 || c == 0x303D || c == 0x3297
           || c == 0x3299 || c == 0x00A9 || c == 0x00AE || c == 0x203C || c == 0x2049
           || c == 0x2122 || c == 0x2139 || c == 0x24C2;
}

// A code point that only changes the emoji before it: the joiner that makes
// families, colour and text selectors, the keycap, skin tones, flag tags.
bool continuesEmoji(char32_t c)
{
    return c == 0x200D || c == 0xFE0F || c == 0xFE0E || c == 0x20E3 || (c >= 0x1F3FB && c <= 0x1F3FF)
           || (c >= 0xE0020 && c <= 0xE007F);
}

// Arrows, (c), (r), (tm) and the like are ordinary text unless the emoji
// selector after them asks for the picture.
bool usuallyText(char32_t c)
{
    return c < 0x2300 || (c >= 0x25AA && c <= 0x25FE) || (c >= 0x3000 && c <= 0x3299);
}

// How many emoji a message is, when it is nothing but emoji and spaces; 0
// when there is anything else in it. A family or a flag counts as one.
int emojiOnlyCount(const QString &raw)
{
    static const QRegularExpression custom(QStringLiteral(
        "<a?:[A-Za-z0-9_]+:\\d+>|(\\[[^\\]\\n]*\\]\\()?https://(?:cdn|media)\\.discordapp\\.(?:com|net)"
        "/emojis/\\d+\\.(?:png|gif|webp)[^)\\s]*\\)?"));

    int count = 0;
    QString rest;
    int last = 0;
    for (auto it = custom.globalMatch(raw); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        rest += raw.mid(last, match.capturedStart() - last) + QLatin1Char(' ');
        last = int(match.capturedEnd());
        ++count;
    }
    rest += raw.mid(last);

    bool afterJoiner = false;
    bool halfFlag = false;
    const QList<uint> points = rest.toUcs4();
    for (qsizetype i = 0; i < points.size(); ++i) {
        const char32_t c = points.at(i);
        if (QChar::isSpace(c)) {
            afterJoiner = false;
            continue;
        }
        if (continuesEmoji(c)) {
            afterJoiner = c == 0x200D;
            continue;
        }
        if (c >= 0x1F1E6 && c <= 0x1F1FF) {
            // Flags are two letters that make one picture.
            if (!halfFlag)
                ++count;
            halfFlag = !halfFlag;
            continue;
        }
        if (!startsEmoji(c))
            return 0;
        if (usuallyText(c) && !(i + 1 < points.size() && points.at(i + 1) == 0xFE0F))
            return 0;
        if (!afterJoiner)
            ++count;
        afterJoiner = false;
        halfFlag = false;
    }
    return count;
}

// Every run of emoji characters wrapped so it can be drawn larger than the
// words around it, the way Discord draws them.
QString enlargeEmoji(const QString &text, int pixels)
{
    const QString open = QStringLiteral("<span style=\"font-family: 'Segoe UI Emoji'; font-size: %1px;\">")
                             .arg(pixels);
    QString out;
    out.reserve(text.size());
    bool inRun = false;
    const QList<uint> points = text.toUcs4();
    for (qsizetype i = 0; i < points.size(); ++i) {
        const char32_t c = points.at(i);
        bool part = (c >= 0x1F1E6 && c <= 0x1F1FF) || (inRun && continuesEmoji(c));
        if (!part && startsEmoji(c))
            part = !usuallyText(c) || (i + 1 < points.size() && points.at(i + 1) == 0xFE0F);
        if (part && !inRun)
            out += open;
        else if (!part && inRun)
            out += QStringLiteral("</span>");
        inRun = part;
        out += QString::fromUcs4(&c, 1);
    }
    if (inRun)
        out += QStringLiteral("</span>");
    return out;
}

} // namespace

QString MainWindow::renderContent(const QString &raw, bool jumbo)
{
    if (raw.isEmpty())
        return {};

    // Emoji follow the chat text size, in Discord's proportions: inline ones a
    // little over the line height, and a message of up to 27 emoji and
    // nothing else shown at three times the text.
    const int textSize = qBound(12, AppConfig::instance().value(QStringLiteral("appearance/fontSize"), 15).toInt(), 24);
    const int emojiOnly = jumbo ? emojiOnlyCount(raw) : 0;
    const int emojiPx = (emojiOnly > 0 && emojiOnly <= 27) ? qRound(textSize * 48 / 14.0)
                                                          : qRound(textSize * 22 / 14.0);
    const QString emojiSource = emojiPx > 32 ? QStringLiteral("96") : QStringLiteral("48");

    QString text = enlargeEmoji(raw.toHtmlEscaped(), qRound(emojiPx * 0.85));

    // A fake-Nitro emoji: a link to an emoji's picture, sent by our own Fake
    // Nitro plugin or by Vencord's. Shown as the emoji it stands for, whether it
    // came as [name](address) or as the bare address. Done before real emoji,
    // whose pictures carry the same kind of address and would match here.
    static const QRegularExpression fakeEmojiRe(QStringLiteral(
        "(\\[[^\\]\\n]*\\]\\()?https://(?:cdn|media)\\.discordapp\\.(?:com|net)/emojis/(\\d+)\\."
        "(png|gif|webp)([^)\\s]*)"));
    {
        QString rebuilt;
        int last = 0;
        auto it = fakeEmojiRe.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const QString id = match.captured(2);
            const bool animated = match.captured(3) == QLatin1String("gif")
                                  || match.captured(4).contains(QLatin1String("animated=true"));
            int end = int(match.capturedEnd());
            // [name](address) closes with a bracket that belongs to it.
            if (!match.captured(1).isEmpty() && end < text.size() && text.at(end) == QLatin1Char(')'))
                ++end;

            rebuilt += text.mid(last, match.capturedStart() - last);
            rebuilt += QStringLiteral("<img src=\"https://cdn.discordapp.com/emojis/%1.%2?size=%3#e%4\" "
                                      "width=\"%4\" height=\"%4\">")
                           .arg(id, animated ? QStringLiteral("gif") : QStringLiteral("png"), emojiSource)
                           .arg(emojiPx);
            last = end;
        }
        rebuilt += text.mid(last);
        text = rebuilt;
    }

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
            const QString id = match.captured(3);

            rebuilt += text.mid(last, match.capturedStart() - last);
            // width and height only. Qt's text engine does not understand
            // title, and the rest of the tag was spilling onto the page as
            // text next to a blank square.
            rebuilt += QStringLiteral("<img src=\"https://cdn.discordapp.com/emojis/%1.%2?size=%3#e%4\" "
                                      "width=\"%4\" height=\"%4\">")
                           .arg(id, animated ? QStringLiteral("gif") : QStringLiteral("png"), emojiSource)
                           .arg(emojiPx);
            last = match.capturedEnd();
        }
        rebuilt += text.mid(last);
        text = rebuilt;
    }

    // Discord timestamps: <t:seconds> and <t:seconds:style>, shown in the
    // reader's own time, with Discord's styles (t T d D f F R).
    static const QRegularExpression timeRe(QStringLiteral("&lt;t:(-?\\d{1,13})(?::([tTdDfFR]))?&gt;"));
    {
        QString rebuilt;
        int last = 0;
        auto it = timeRe.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const QDateTime when = QDateTime::fromSecsSinceEpoch(match.captured(1).toLongLong()).toLocalTime();
            const QString style = match.captured(2).isEmpty() ? QStringLiteral("f") : match.captured(2);
            QString shown;
            const QLocale locale;
            if (style == QLatin1String("t"))
                shown = locale.toString(when, QStringLiteral("h:mm AP"));
            else if (style == QLatin1String("T"))
                shown = locale.toString(when, QStringLiteral("h:mm:ss AP"));
            else if (style == QLatin1String("d"))
                shown = locale.toString(when, QStringLiteral("M/d/yyyy"));
            else if (style == QLatin1String("D"))
                shown = locale.toString(when, QStringLiteral("MMMM d, yyyy"));
            else if (style == QLatin1String("F"))
                shown = locale.toString(when, QStringLiteral("dddd, MMMM d, yyyy h:mm AP"));
            else if (style == QLatin1String("R")) {
                const qint64 secs = QDateTime::currentDateTime().secsTo(when);
                const qint64 a = qAbs(secs);
                const auto unit = [](qint64 n, const char *one) {
                    return QStringLiteral("%1 %2%3").arg(n).arg(QLatin1String(one)).arg(n == 1 ? QString() : QStringLiteral("s"));
                };
                const QString amount = a < 60 ? unit(a, "second") : a < 3600 ? unit(a / 60, "minute")
                    : a < 86400 ? unit(a / 3600, "hour") : a < 2592000 ? unit(a / 86400, "day")
                    : a < 31536000 ? unit(a / 2592000, "month") : unit(a / 31536000, "year");
                shown = secs >= 0 ? QStringLiteral("in ") + amount : amount + QStringLiteral(" ago");
            } else
                shown = locale.toString(when, QStringLiteral("MMMM d, yyyy h:mm AP"));
            rebuilt += text.mid(last, match.capturedStart() - last);
            rebuilt += QStringLiteral("<span style=\"background-color:%1;\">%2</span>")
                           .arg(QLatin1String(Theme::SurfaceInput), shown.toHtmlEscaped());
            last = int(match.capturedEnd());
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
    //
    // Only in the words, never inside markup this function wrote. A custom
    // emoji has already become <img src="https://cdn.discordapp.com/...">
    // by now, and this used to find that address inside the tag and wrap it in
    // a link - which broke the tag open, so the message showed a broken
    // picture followed by the rest of the tag as text.
    //
    // Telling the two apart is safe because the message was escaped at the
    // very top: every '<' still in the text is one written here, so anything
    // between a '<' and the next '>' is ours, and so is anything inside an
    // <a> this function opened.
    static const QRegularExpression linkRe(QStringLiteral("(https?://[^\\s<\"]+)"));
    const auto insideMarkup = [&text](int pos) {
        const int tagOpen = text.lastIndexOf(QLatin1Char('<'), pos);
        if (tagOpen >= 0 && text.lastIndexOf(QLatin1Char('>'), pos) < tagOpen)
            return true; // inside a tag's attributes
        const int anchorOpen = text.lastIndexOf(QLatin1String("<a "), pos);
        return anchorOpen >= 0 && text.lastIndexOf(QLatin1String("</a>"), pos) < anchorOpen;
    };
    {
        QString rebuilt;
        int last = 0;
        auto it = linkRe.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            // Left exactly as it is; the next piece of text copied carries it.
            if (insideMarkup(int(match.capturedStart())))
                continue;
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

    // Switch Accounts, as in the official client's profile menu.
    menu.addSeparator();
    QMenu *switcher = menu.addMenu(QStringLiteral("Switch Accounts"));
    const QList<TokenStore::Account> saved = TokenStore::accounts();
    for (const TokenStore::Account &account : saved) {
        const QUrl url = MediaCache::avatarUrl(account.userId, account.avatarHash, 64);
        const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
        const QIcon icon(picture.isNull() ? MediaCache::initialsAvatar(account.username, 20)
                                          : MediaCache::circular(picture, 20));
        QAction *pick = switcher->addAction(icon, account.username.isEmpty() ? account.userId
                                                                            : account.username);
        const bool current = account.userId == m_selfUserId;
        pick->setCheckable(true);
        pick->setChecked(current);
        pick->setEnabled(!current);
        const QString userId = account.userId;
        connect(pick, &QAction::triggered, this, [this, userId]() { switchToAccount(userId); });
    }
    if (!saved.isEmpty())
        switcher->addSeparator();
    connect(switcher->addAction(QStringLiteral("Add an account...")), &QAction::triggered, this,
            [this]() { restartInto({QStringLiteral("--add-account")}); });
    if (!saved.isEmpty()) {
        connect(switcher->addAction(QStringLiteral("Manage accounts...")), &QAction::triggered, this,
                &MainWindow::manageAccounts);
    }

    // Opened upwards from the panel, which sits at the very bottom of the
    // sidebar: a menu dropped downwards from there would be off the screen.
    const QPoint at = m_userPanel->mapToGlobal(QPoint(8, 0));
    menu.exec(QPoint(at.x(), at.y() - menu.sizeHint().height() - 4));
}

// Switching is a restart into the other account, which is what the official
// client does too - it reloads. Every piece of state belongs to one account:
// the gateway session, the voice connections, the message store, the caches.
// Starting clean is the only way to be sure none of it carries across.
void MainWindow::switchToAccount(const QString &userId)
{
    for (const TokenStore::Account &account : TokenStore::accounts()) {
        if (account.userId != userId)
            continue;
        if (!AppConfig::instance().setToken(account.token)) {
            flashStatus(QStringLiteral("Could not switch: the session file could not be written."), 6000);
            return;
        }
        wlog(QStringLiteral("app"), QStringLiteral("switching accounts"));
        restartInto({});
        return;
    }
}

void MainWindow::restartInto(const QStringList &arguments)
{
    // --replace tells the new copy that this one is on its way out, so it
    // waits for it rather than handing itself back to it as a duplicate.
    QStringList withReplace = arguments;
    withReplace << QStringLiteral("--replace");
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), withReplace)) {
        flashStatus(QStringLiteral("Could not restart Singularity."), 6000);
        return;
    }
    m_gateway->stop();
    qApp->quit();
}

void MainWindow::manageAccounts()
{
    const QList<TokenStore::Account> saved = TokenStore::accounts();

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Manage accounts"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto *note = new QLabel(QStringLiteral("Accounts that can be switched to from this machine. "
                                           "Removing one signs it out here and forgets its session."),
                            &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);

    QString removeId;
    for (const TokenStore::Account &account : saved) {
        auto *row = new QHBoxLayout;
        const QUrl url = MediaCache::avatarUrl(account.userId, account.avatarHash, 64);
        const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
        auto *face = new QLabel(&dialog);
        face->setPixmap(picture.isNull() ? MediaCache::initialsAvatar(account.username, 28)
                                         : MediaCache::circular(picture, 28));
        row->addWidget(face);

        const bool current = account.userId == m_selfUserId;
        auto *name = new QLabel(account.username + (current ? QStringLiteral("   (signed in now)") : QString()),
                                &dialog);
        row->addWidget(name, 1);

        auto *remove = new QPushButton(current ? QStringLiteral("Log out") : QStringLiteral("Remove"), &dialog);
        const QString userId = account.userId;
        connect(remove, &QPushButton::clicked, &dialog, [&dialog, &removeId, userId]() {
            removeId = userId;
            dialog.accept();
        });
        row->addWidget(remove);
        layout->addLayout(row);
    }

    auto *close = new QPushButton(QStringLiteral("Close"), &dialog);
    close->setObjectName(QStringLiteral("PrimaryButton"));
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    layout->addWidget(close, 0, Qt::AlignRight);

    if (dialog.exec() != QDialog::Accepted || removeId.isEmpty())
        return;

    if (removeId == m_selfUserId) {
        logOut(); // forgets it too
        return;
    }
    TokenStore::forgetAccount(removeId);
    wlog(QStringLiteral("app"), QStringLiteral("removed a saved account"));
    flashStatus(QStringLiteral("Account removed from this machine."), 4000);
}

void MainWindow::setSelfMuted(bool on)
{
    // Unmuting while deafened undeafens as well, as in Discord: you cannot
    // talk to people you have chosen not to hear.
    if (!on && m_deafenButton && m_deafenButton->isChecked()) {
        m_mutedBeforeDeafen = false;
        setSelfDeafened(false);
        return;
    }

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
    // Deafen mutes too. Undeafen gives the microphone back unless it was
    // already muted before deafening. It used to stay muted either way.
    if (on) {
        if (m_muteButton)
            m_mutedBeforeDeafen = m_muteButton->isChecked();
        setSelfMuted(true);
    } else {
        setSelfMuted(m_mutedBeforeDeafen);
    }
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

void MainWindow::setPresenceStatus(const QString &status, bool storeOnDiscord)
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
    //
    // Only for a choice somebody made. This used to run on every sign-in with
    // whatever status was saved on this machine, so an old copy of the
    // program, or one started from a stale settings file, quietly set the
    // whole account back to what it had last seen - Do Not Disturb turned
    // into Online by nothing more than opening it.
    if (m_rest && storeOnDiscord) {
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

namespace {

// discord.gg/CODE, discord.com/invite/CODE and discordapp.com/invite/CODE, the
// forms Discord's own client turns into an invite card. Not preceded by a
// letter or a dot, so notdiscord.gg is left alone.
const QRegularExpression &inviteLinkPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral(R"((?<![\w.])(?:https?://)?(?:www\.)?(?:discord\.gg|discord(?:app)?\.com/invite)/([A-Za-z0-9-]{2,32})(?![\w-]))"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

// The codes in a message, first three, each once.
QStringList inviteCodesIn(const QString &text)
{
    QStringList codes;
    auto it = inviteLinkPattern().globalMatch(text);
    while (it.hasNext() && codes.size() < 3) {
        const QString code = it.next().captured(1);
        if (!codes.contains(code))
            codes.append(code);
    }
    return codes;
}

// What someone typed into "Join a server": a whole link or just the code.
QString inviteCodeFrom(const QString &typed)
{
    const QString text = typed.trimmed();
    const QRegularExpressionMatch link = inviteLinkPattern().match(text);
    if (link.hasMatch())
        return link.captured(1);
    static const QRegularExpression bare(QStringLiteral(R"(^[A-Za-z0-9-]{2,32}$)"));
    return bare.match(text).hasMatch() ? text : QString();
}

// X-Context-Properties, as the real client writes it (checked against
// discord.py-self's tracking.py): compact JSON in this key order, sent as
// base64. Where the join was pressed decides the "location".
QByteArray joinGuildContext()
{
    return QByteArrayLiteral(R"({"location":"Join Guild"})");
}

QByteArray inviteButtonContext(const QString &guildId, const QString &channelId, int channelType,
                               const QString &messageId)
{
    const auto quoted = [](const QString &value) {
        return value.isEmpty() ? QStringLiteral("null") : QStringLiteral("\"%1\"").arg(value);
    };
    return QStringLiteral(R"({"location":"Invite Button Embed","location_guild_id":%1,)"
                          R"("location_channel_id":%2,"location_channel_type":%3,"location_message_id":%4})")
        .arg(quoted(guildId), quoted(channelId), QString::number(channelType), quoted(messageId))
        .toUtf8();
}

} // namespace

void MainWindow::handleAnchor(const QUrl &url)
{
    const QString whole = url.toString();

    // A button that is not a link. The query carries which message and which
    // custom id, so the press can be sent to the bot that owns it.
    if (url.scheme() == QLatin1String("singularity-press")) {
        const QUrlQuery query(url);
        if (url.host() == QLatin1String("select")) {
            pressMessageSelect(query.queryItemValue(QStringLiteral("m")),
                               query.queryItemValue(QStringLiteral("a")),
                               query.queryItemValue(QStringLiteral("f")).toInt(),
                               query.queryItemValue(QStringLiteral("c"), QUrl::FullyDecoded));
        } else {
            pressMessageButton(query.queryItemValue(QStringLiteral("m")),
                               query.queryItemValue(QStringLiteral("a")),
                               query.queryItemValue(QStringLiteral("f")).toInt(),
                               query.queryItemValue(QStringLiteral("c"), QUrl::FullyDecoded));
        }
        return;
    }

    if (url.scheme() == QLatin1String("singularity-react")) {
        const QUrlQuery query(url);
        toggleReaction(query.queryItemValue(QStringLiteral("m")),
                       query.queryItemValue(QStringLiteral("e"), QUrl::FullyDecoded));
        return;
    }

    // A game invite's Launch Game. Discord would start the installed copy;
    // we open the application's page, which is what we can do without its
    // launcher.
    if (url.scheme() == QLatin1String("singularity-game")) {
        QString id = url.path();
        if (id.startsWith(QLatin1Char('/')))
            id.remove(0, 1);
        if (id.isEmpty())
            id = whole.mid(QStringLiteral("singularity-game:").size());
        if (!id.isEmpty())
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://discord.com/application-directory/%1").arg(id)));
        return;
    }

    if (url.scheme() == QLatin1String("singularity-wave")) {
        sendWave();
        return;
    }

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

    // The Join button on an invite card: "singularity-invite:CODE/MESSAGEID".
    if (url.scheme() == QLatin1String("singularity-invite")) {
        const QStringList parts = whole.mid(QStringLiteral("singularity-invite:").size()).split(QLatin1Char('/'));
        const QString code = parts.value(0);
        const QString messageId = parts.value(1);
        const ChannelInfo channel = m_store->channel(m_currentChannelId);
        joinInvite(code, inviteButtonContext(channel.guildId, m_currentChannelId, channel.type, messageId));
        return;
    }

    // An invite link opens the join box with it filled in, instead of a
    // browser tab asking to open Discord.
    if ((url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
        && inviteLinkPattern().match(whole).hasMatch()) {
        showJoinServerDialog(whole);
        return;
    }

    // A link to a channel opens it here. Discord does the same for a button
    // that points at a channel, instead of handing it to a browser.
    static const QRegularExpression channelLink(QStringLiteral(
        R"(^https?://(?:(?:ptb|canary)\.)?discord(?:app)?\.com/channels/[^/]+/(\d+))"));
    const QRegularExpressionMatch channelHit = channelLink.match(whole);
    if (channelHit.hasMatch()) {
        selectChannelEverywhere(channelHit.captured(1));
        return;
    }

    // Anything else is a real web link, opened in the normal browser.
    if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
        QDesktopServices::openUrl(url);
}

void MainWindow::pressMessageButton(const QString &messageId, const QString &applicationId,
                                    int messageFlags, const QString &customId)
{
    if (messageId.isEmpty() || customId.isEmpty())
        return;
    if (applicationId.isEmpty()) {
        flashStatus(QStringLiteral("That button has no bot to send the press to."), 4000);
        return;
    }
    if (!m_gateway || m_gateway->sessionId().isEmpty()) {
        flashStatus(QStringLiteral("Not connected yet."), 3000);
        return;
    }

    m_buttonChannelId = m_currentChannelId;
    m_buttonGuildId = m_currentGuildId;
    m_buttonMessageId = messageId;
    m_buttonApplicationId = applicationId;
    m_buttonMessageFlags = messageFlags;

    QJsonObject body{
        {QStringLiteral("type"), 3},
        {QStringLiteral("application_id"), applicationId},
        {QStringLiteral("channel_id"), m_currentChannelId},
        {QStringLiteral("message_id"), messageId},
        {QStringLiteral("message_flags"), messageFlags},
        {QStringLiteral("session_id"), m_gateway->sessionId()},
        {QStringLiteral("data"), QJsonObject{
            {QStringLiteral("component_type"), 2},
            {QStringLiteral("custom_id"), customId},
        }},
    };
    // A direct message has no server. Sending a made-up guild id is a refusal.
    bool guildIsNumber = !m_currentGuildId.isEmpty();
    for (const QChar c : m_currentGuildId) {
        if (!c.isDigit())
            guildIsNumber = false;
    }
    if (guildIsNumber)
        body.insert(QStringLiteral("guild_id"), m_currentGuildId);

    flashStatus(QStringLiteral("Sending..."), 3000);
    m_rest->createInteraction(
        body,
        [this](const QJsonObject &) { flashStatus(QStringLiteral("Sent."), 2000); },
        [this](const RestClient::Error &error) {
            const QString why = error.message.isEmpty() ? QStringLiteral("Discord refused that button.")
                                                        : error.message;
            flashStatus(why, 5000);
        });
}

static QJsonObject findMessageComponent(const QJsonArray &components, const QString &customId)
{
    for (const QJsonValue &value : components) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("custom_id")).toString() == customId)
            return item;
        const QJsonObject nested = findMessageComponent(item.value(QStringLiteral("components")).toArray(), customId);
        if (!nested.isEmpty())
            return nested;
        const QJsonObject accessory = item.value(QStringLiteral("accessory")).toObject();
        if (accessory.value(QStringLiteral("custom_id")).toString() == customId)
            return accessory;
    }
    return {};
}

void MainWindow::pressMessageSelect(const QString &messageId, const QString &applicationId,
                                    int messageFlags, const QString &customId)
{
    if (messageId.isEmpty() || customId.isEmpty())
        return;
    if (applicationId.isEmpty()) {
        flashStatus(QStringLiteral("That menu has no bot to send the choice to."), 4000);
        return;
    }

    QJsonArray components;
    for (const MessageInfo &message : m_store->messages(m_currentChannelId)) {
        if (message.id == messageId) {
            components = message.components;
            break;
        }
    }
    const QJsonObject select = findMessageComponent(components, customId);
    const QJsonArray options = select.value(QStringLiteral("options")).toArray();
    if (options.isEmpty()) {
        flashStatus(QStringLiteral("That menu has no choices."), 3000);
        return;
    }

    QMenu menu(this);
    for (const QJsonValue &value : options) {
        const QJsonObject option = value.toObject();
        const QString label = option.value(QStringLiteral("label")).toString();
        const QString optionValue = option.value(QStringLiteral("value")).toString();
        if (label.isEmpty() || optionValue.isEmpty())
            continue;
        QAction *action = menu.addAction(label);
        action->setData(optionValue);
        if (option.value(QStringLiteral("default")).toBool()) {
            action->setCheckable(true);
            action->setChecked(true);
        }
    }
    QAction *chosen = menu.exec(QCursor::pos());
    if (!chosen)
        return;
    if (!m_gateway || m_gateway->sessionId().isEmpty()) {
        flashStatus(QStringLiteral("Not connected yet."), 3000);
        return;
    }

    m_buttonChannelId = m_currentChannelId;
    m_buttonGuildId = m_currentGuildId;
    m_buttonMessageId = messageId;
    m_buttonApplicationId = applicationId;
    m_buttonMessageFlags = messageFlags;

    QJsonObject body{
        {QStringLiteral("type"), 3},
        {QStringLiteral("application_id"), applicationId},
        {QStringLiteral("channel_id"), m_currentChannelId},
        {QStringLiteral("message_id"), messageId},
        {QStringLiteral("message_flags"), messageFlags},
        {QStringLiteral("session_id"), m_gateway->sessionId()},
        {QStringLiteral("data"), QJsonObject{
            {QStringLiteral("component_type"), 3},
            {QStringLiteral("custom_id"), customId},
            {QStringLiteral("values"), QJsonArray{chosen->data().toString()}},
        }},
    };
    bool guildIsNumber = !m_currentGuildId.isEmpty();
    for (const QChar c : m_currentGuildId) {
        if (!c.isDigit())
            guildIsNumber = false;
    }
    if (guildIsNumber)
        body.insert(QStringLiteral("guild_id"), m_currentGuildId);

    flashStatus(QStringLiteral("Sending..."), 3000);
    m_rest->createInteraction(
        body,
        [this](const QJsonObject &) { flashStatus(QStringLiteral("Sent."), 2000); },
        [this](const RestClient::Error &error) {
            const QString why = error.message.isEmpty() ? QStringLiteral("Discord refused that menu.")
                                                        : error.message;
            flashStatus(why, 5000);
        });
}

void MainWindow::toggleReaction(const QString &messageId, const QString &emoji)
{
    if (messageId.isEmpty() || emoji.isEmpty() || m_currentChannelId.isEmpty())
        return;

    bool mine = false;
    bool found = false;
    for (const MessageInfo &message : m_store->messages(m_currentChannelId)) {
        if (message.id != messageId)
            continue;
        for (const ReactionInfo &reaction : message.reactions) {
            const QString key = reaction.isCustom()
                ? (reaction.name + QLatin1Char(':') + reaction.id)
                : reaction.name;
            if (key != emoji)
                continue;
            mine = reaction.mine;
            found = true;
            break;
        }
        break;
    }
    if (!found)
        return;

    auto onOk = [](const QJsonObject &) {};
    auto onError = [this](const RestClient::Error &error) {
        const QString why = error.message.isEmpty() ? QStringLiteral("Discord refused that reaction.")
                                                    : error.message;
        flashStatus(why, 4000);
    };
    if (mine)
        m_rest->removeReaction(m_currentChannelId, messageId, emoji, onOk, onError);
    else
        m_rest->addReaction(m_currentChannelId, messageId, emoji, onOk, onError);
}

void MainWindow::showInteractionModal(const QJsonObject &data)
{
    const QString title = data.value(QStringLiteral("title")).toString();
    const QString modalCustomId = data.value(QStringLiteral("custom_id")).toString();
    const QString interactionId = data.value(QStringLiteral("id")).toString();

    struct Field {
        QString customId;
        QString label;
        QString placeholder;
        QString value;
        bool paragraph = false;
        bool required = true;
        int maxLength = 0;
    };
    QList<Field> fields;
    const auto takeInput = [&fields](const QJsonObject &input, const QString &labelOverride) {
        if (input.value(QStringLiteral("type")).toInt() != 4)
            return;
        Field field;
        field.customId = input.value(QStringLiteral("custom_id")).toString();
        field.label = labelOverride.isEmpty() ? input.value(QStringLiteral("label")).toString() : labelOverride;
        if (field.label.isEmpty())
            field.label = field.customId;
        field.placeholder = input.value(QStringLiteral("placeholder")).toString();
        field.value = input.value(QStringLiteral("value")).toString();
        field.paragraph = input.value(QStringLiteral("style")).toInt() == 2;
        field.required = input.value(QStringLiteral("required")).toBool(true);
        field.maxLength = input.value(QStringLiteral("max_length")).toInt();
        if (!field.customId.isEmpty())
            fields.append(field);
    };
    for (const QJsonValue &row : data.value(QStringLiteral("components")).toArray()) {
        const QJsonObject component = row.toObject();
        const int type = component.value(QStringLiteral("type")).toInt();
        if (type == 4)
            takeInput(component, {});
        else if (type == 18)
            takeInput(component.value(QStringLiteral("component")).toObject(),
                      component.value(QStringLiteral("label")).toString());
        else if (type == 1) {
            for (const QJsonValue &child : component.value(QStringLiteral("components")).toArray())
                takeInput(child.toObject(), {});
        }
    }
    if (fields.isEmpty()) {
        flashStatus(title.isEmpty() ? QStringLiteral("The bot answered in a way this client cannot show.")
                                     : title,
                    5000);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(title.isEmpty() ? QStringLiteral("The bot") : title);
    dialog.setMinimumWidth(440);
    auto *layout = new QVBoxLayout(&dialog);
    QList<QWidget *> editors;
    for (const Field &field : fields) {
        layout->addWidget(new QLabel(field.required ? field.label + QStringLiteral(" *") : field.label, &dialog));
        if (field.paragraph) {
            auto *edit = new QPlainTextEdit(field.value, &dialog);
            edit->setPlaceholderText(field.placeholder);
            edit->setFixedHeight(96);
            layout->addWidget(edit);
            editors.append(edit);
        } else {
            auto *edit = new QLineEdit(field.value, &dialog);
            edit->setPlaceholderText(field.placeholder);
            if (field.maxLength > 0)
                edit->setMaxLength(field.maxLength);
            layout->addWidget(edit);
            editors.append(edit);
        }
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        for (int i = 0; i < fields.size(); ++i) {
            const QString value = editors.at(i)->inherits("QPlainTextEdit")
                ? static_cast<QPlainTextEdit *>(editors.at(i))->toPlainText()
                : static_cast<QLineEdit *>(editors.at(i))->text();
            if (fields.at(i).required && value.trimmed().isEmpty()) {
                flashStatus(QStringLiteral("Fill in %1.").arg(fields.at(i).label), 4000);
                return;
            }
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted)
        return;

    QJsonArray rows;
    for (int i = 0; i < fields.size(); ++i) {
        const QString value = editors.at(i)->inherits("QPlainTextEdit")
            ? static_cast<QPlainTextEdit *>(editors.at(i))->toPlainText()
            : static_cast<QLineEdit *>(editors.at(i))->text();
        rows.append(QJsonObject{
            {QStringLiteral("type"), 1},
            {QStringLiteral("components"), QJsonArray{QJsonObject{
                {QStringLiteral("type"), 4},
                {QStringLiteral("custom_id"), fields.at(i).customId},
                {QStringLiteral("value"), value},
            }}},
        });
    }

    QString applicationId = data.value(QStringLiteral("application")).toObject()
                                .value(QStringLiteral("id")).toString();
    if (applicationId.isEmpty())
        applicationId = m_buttonApplicationId;
    QString channelId = data.value(QStringLiteral("channel_id")).toString();
    if (channelId.isEmpty())
        channelId = m_buttonChannelId;

    QJsonObject body{
        {QStringLiteral("type"), 5},
        {QStringLiteral("application_id"), applicationId},
        {QStringLiteral("channel_id"), channelId},
        {QStringLiteral("session_id"), m_gateway ? m_gateway->sessionId() : QString()},
        {QStringLiteral("data"), QJsonObject{
            {QStringLiteral("id"), interactionId},
            {QStringLiteral("custom_id"), modalCustomId},
            {QStringLiteral("components"), rows},
        }},
    };
    if (!m_buttonMessageId.isEmpty()) {
        body.insert(QStringLiteral("message_id"), m_buttonMessageId);
        body.insert(QStringLiteral("message_flags"), m_buttonMessageFlags);
    }
    bool guildIsNumber = !m_buttonGuildId.isEmpty();
    for (const QChar c : m_buttonGuildId) {
        if (!c.isDigit())
            guildIsNumber = false;
    }
    if (guildIsNumber)
        body.insert(QStringLiteral("guild_id"), m_buttonGuildId);

    flashStatus(QStringLiteral("Sending..."), 3000);
    m_rest->createInteraction(
        body,
        [this](const QJsonObject &) { flashStatus(QStringLiteral("Sent."), 2500); },
        [this](const RestClient::Error &error) {
            flashStatus(error.message.isEmpty() ? QStringLiteral("Discord refused that form.")
                                                : error.message,
                        5000);
        });
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

    // A link can point at a channel we have never loaded. It shows as "unknown"
    // because the store has nothing for it. Ask Discord what it is once, keep
    // the answer, then come back here with a channel we can actually jump to.
    // The store now has a real id, so this branch does not fire a second time.
    if (channel.id.isEmpty()) {
        flashStatus(QStringLiteral("Opening that channel..."), 3000);
        m_rest->fetchChannel(
            channelId,
            [this, channelId](const QJsonObject &object) {
                m_store->ingestChannelObject(object);
                selectChannelEverywhere(channelId);
            },
            [this](const RestClient::Error &error) {
                flashStatus(QStringLiteral("That channel could not be opened: %1").arg(error.message),
                            5000);
            });
        return;
    }

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
    updateMentionPopup();

    if (m_currentChannelId.isEmpty() || m_composer->toPlainText().trimmed().isEmpty())
        return;

    if (m_typingSentTimer.isValid() && m_typingSentTimer.elapsed() < TypingIntervalMs)
        return;
    m_typingSentTimer.restart();

    if (m_plugins->runBeforeTyping(m_currentChannelId))
        m_rest->sendTyping(m_currentChannelId);
}

namespace {
constexpr int kMentionId = QTextFormat::UserProperty + 50;
constexpr int kMentionKind = QTextFormat::UserProperty + 51;
}

bool MainWindow::mentionQuery(int *atPos, QString *query) const
{
    if (!m_composer || !atPos || !query)
        return false;

    const QTextCursor cursor = m_composer->textCursor();
    const int pos = cursor.position();
    const QString text = m_composer->toPlainText();
    const int at = text.lastIndexOf(QLatin1Char('@'), qMax(0, pos - 1));
    if (at < 0 || at >= pos)
        return false;
    if (at > 0) {
        const QChar before = text.at(at - 1);
        if (!before.isSpace() && before != QLatin1Char('\n'))
            return false;
    }

    QTextCursor probe(m_composer->document());
    probe.setPosition(at + 1);
    if (!probe.charFormat().property(kMentionId).toString().isEmpty()
        || probe.charFormat().property(kMentionKind).toString() == QLatin1String("everyone")
        || probe.charFormat().property(kMentionKind).toString() == QLatin1String("here")) {
        return false;
    }

    const QString rest = text.mid(at + 1, pos - at - 1);
    if (rest.contains(QLatin1Char(' ')) || rest.contains(QLatin1Char('\n')))
        return false;

    *atPos = at;
    *query = rest;
    return true;
}

QString MainWindow::composerPayload() const
{
    if (!m_composer)
        return {};

    QString out;
    const QTextDocument *doc = m_composer->document();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        if (block != doc->begin())
            out += QLatin1Char('\n');
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            const QString id = frag.charFormat().property(kMentionId).toString();
            const QString kind = frag.charFormat().property(kMentionKind).toString();
            if (kind == QLatin1String("user") && !id.isEmpty())
                out += QStringLiteral("<@%1>").arg(id);
            else if (kind == QLatin1String("role") && !id.isEmpty())
                out += QStringLiteral("<@&%1>").arg(id);
            else
                out += frag.text();
        }
    }
    return out;
}

void MainWindow::hideMentionPopup()
{
    if (m_mentionPopup)
        m_mentionPopup->hide();
    m_mentionAtPos = -1;
}

void MainWindow::insertPickedMention()
{
    if (!m_mentionPopup || !m_mentionPopup->currentItem() || m_mentionAtPos < 0)
        return;

    const QListWidgetItem *item = m_mentionPopup->currentItem();
    const QString id = item->data(Qt::UserRole).toString();
    const QString kind = item->data(Qt::UserRole + 1).toString();
    const QString label = item->data(Qt::UserRole + 2).toString();

    QTextCursor cursor = m_composer->textCursor();
    cursor.setPosition(m_mentionAtPos);
    cursor.setPosition(m_composer->textCursor().position(), QTextCursor::KeepAnchor);
    cursor.removeSelectedText();

    QTextCharFormat fmt;
    fmt.setForeground(QColor(QLatin1String(Theme::Accent)));
    fmt.setFontWeight(QFont::DemiBold);
    fmt.setProperty(kMentionKind, kind);
    if (!id.isEmpty())
        fmt.setProperty(kMentionId, id);

    cursor.insertText(QLatin1Char('@') + label, fmt);
    cursor.setCharFormat(QTextCharFormat());
    cursor.insertText(QStringLiteral(" "));
    m_composer->setTextCursor(cursor);
    hideMentionPopup();
}

void MainWindow::updateMentionPopup()
{
    QString query;
    int atPos = -1;
    if (!mentionQuery(&atPos, &query)) {
        hideMentionPopup();
        return;
    }
    m_mentionAtPos = atPos;

    struct Hit {
        QString id;
        QString kind;
        QString label;
        QString filter;
        bool prefix = false;
    };
    QList<Hit> hits;
    QSet<QString> seen;

    const auto consider = [&](const QString &id, const QString &kind, const QString &label,
                              const QString &filter) {
        if (label.isEmpty() || seen.contains(kind + id + label))
            return;
        if (!query.isEmpty() && !filter.contains(query, Qt::CaseInsensitive))
            return;
        seen.insert(kind + id + label);
        Hit hit{id, kind, label, filter, filter.startsWith(query, Qt::CaseInsensitive)};
        hits.append(hit);
    };

    const ChannelInfo channel = m_store->channel(m_currentChannelId);
    const bool inGuild = !channel.guildId.isEmpty();

    if (inGuild) {
        if (QStringLiteral("everyone").startsWith(query, Qt::CaseInsensitive))
            consider({}, QStringLiteral("everyone"), QStringLiteral("everyone"), QStringLiteral("everyone"));
        if (QStringLiteral("here").startsWith(query, Qt::CaseInsensitive))
            consider({}, QStringLiteral("here"), QStringLiteral("here"), QStringLiteral("here"));

        const MemberList list = m_store->memberList(channel.guildId);
        for (const MemberRow &row : list.rows) {
            if (row.heading || row.userId.isEmpty())
                continue;
            QString label = row.nickname;
            if (label.isEmpty())
                label = m_store->userName(row.userId);
            const UserInfo info = m_store->user(row.userId);
            consider(row.userId, QStringLiteral("user"), label,
                     label + QLatin1Char(' ') + info.username);
        }

        for (const auto &hit : m_mentionSearchHits) {
            const UserInfo info = m_store->user(hit.first);
            consider(hit.first, QStringLiteral("user"), hit.second,
                     hit.second + QLatin1Char(' ') + info.username + QLatin1Char(' ') + info.globalName);
        }

        const GuildInfo server = m_store->guild(channel.guildId);
        for (auto it = server.roles.constBegin(); it != server.roles.constEnd(); ++it) {
            if (it.key() == server.id || it->name.isEmpty())
                continue;
            consider(it.key(), QStringLiteral("role"), it->name, it->name);
        }
    } else {
        for (const QString &userId : channel.recipientIds) {
            const UserInfo info = m_store->user(userId);
            const QString label = info.displayName().isEmpty() ? info.username : info.displayName();
            consider(userId, QStringLiteral("user"), label, label + QLatin1Char(' ') + info.username);
        }
    }

    const QList<MessageInfo> recent = m_store->messages(m_currentChannelId);
    const int from = qMax(0, int(recent.size()) - 80);
    for (int i = recent.size() - 1; i >= from; --i) {
        const MessageInfo &message = recent.at(i);
        if (message.authorId.isEmpty())
            continue;
        QString label = message.authorName;
        if (label.isEmpty())
            label = m_store->userName(message.authorId);
        consider(message.authorId, QStringLiteral("user"), label, label);
    }

    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        if (a.prefix != b.prefix)
            return a.prefix;
        return a.label.localeAwareCompare(b.label) < 0;
    });
    if (hits.size() > 12)
        hits = hits.mid(0, 12);

    if (hits.isEmpty()) {
        hideMentionPopup();
        if (!query.isEmpty() && inGuild && query != m_mentionSearchedFor)
            m_mentionSearchTimer.start();
        return;
    }

    if (!m_mentionPopup) {
        m_mentionPopup = new QListWidget(this);
        m_mentionPopup->setObjectName(QStringLiteral("MentionPopup"));
        m_mentionPopup->setFocusPolicy(Qt::NoFocus);
        m_mentionPopup->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_mentionPopup->setStyleSheet(QStringLiteral(
            "QListWidget#MentionPopup { background-color: %1; color: %2; border: 1px solid %3; "
            "border-radius: 10px; padding: 4px; font-family: \"Segoe UI\"; font-size: 13px; }"
            "QListWidget#MentionPopup::item { padding: 6px 10px; border-radius: 6px; }"
            "QListWidget#MentionPopup::item:selected { background: %4; }")
                                          .arg(QLatin1String(Theme::SurfaceSidebar),
                                               QLatin1String(Theme::TextPrimary),
                                               QLatin1String(Theme::Border),
                                               QLatin1String(Theme::SurfaceHover)));
        connect(m_mentionPopup, &QListWidget::itemClicked, this, [this](QListWidgetItem *) {
            insertPickedMention();
        });
    }

    const QString previous = m_mentionPopup->currentItem()
                                 ? m_mentionPopup->currentItem()->data(Qt::UserRole).toString()
                                 : QString();
    m_mentionPopup->clear();
    int select = 0;
    for (int i = 0; i < hits.size(); ++i) {
        const Hit &hit = hits.at(i);
        auto *item = new QListWidgetItem(QLatin1Char('@') + hit.label, m_mentionPopup);
        if (hit.kind == QLatin1String("user")) {
            const UserInfo info = m_store->user(hit.id);
            if (!info.username.isEmpty() && info.username.compare(hit.label, Qt::CaseInsensitive) != 0)
                item->setText(QStringLiteral("@%1   %2").arg(hit.label, info.username));
        }
        item->setData(Qt::UserRole, hit.id);
        item->setData(Qt::UserRole + 1, hit.kind);
        item->setData(Qt::UserRole + 2, hit.label);
        if (hit.id == previous)
            select = i;
    }
    m_mentionPopup->setCurrentRow(select);

    const int rowH = 28;
    const int height = qMin(8, hits.size()) * rowH + 10;
    const int width = qBound(220, m_composer->width(), 320);
    m_mentionPopup->setFixedSize(width, height);

    const QPoint topLeft = m_composer->mapTo(this, QPoint(0, 0));
    m_mentionPopup->move(topLeft.x(), topLeft.y() - height - 6);
    m_mentionPopup->raise();
    m_mentionPopup->show();

    if (!query.isEmpty() && inGuild && query != m_mentionSearchedFor)
        m_mentionSearchTimer.start();
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

    if (m_stopEdit)
        m_stopEdit->setVisible(!m_editingMessageId.isEmpty());

    QStringList parts;
    if (!m_pendingFiles.isEmpty()) {
        QStringList names;
        for (const QString &path : m_pendingFiles)
            names.append(QFileInfo(path).fileName());
        parts.append(QStringLiteral("Attached: %1").arg(names.join(QStringLiteral(", "))));
    }
    for (auto it = m_uploadsInFlight.cbegin(); it != m_uploadsInFlight.cend(); ++it) {
        parts.append(QStringLiteral("Uploading %1 to GoFile… %2%")
                         .arg(QFileInfo(it.key()).fileName())
                         .arg(it.value()));
    }

    if (parts.isEmpty()) {
        m_attachmentLabel->parentWidget()->hide();
    } else {
        m_attachmentLabel->setText(parts.join(QStringLiteral("   ·   ")));
        m_attachmentLabel->parentWidget()->show();
        // Only a file waiting to go with the message can be taken off.
        // An upload already on its way is not that.
        m_removeAttachment->setVisible(!m_pendingFiles.isEmpty());
    }
}

void MainWindow::stopEditing()
{
    const bool editing = !m_editingMessageId.isEmpty();
    if (m_editingMessageId.isEmpty() && m_replyMessageId.isEmpty())
        return;

    m_editingMessageId.clear();
    m_replyMessageId.clear();
    // The words in the box are the message being edited. Leaving them would
    // send that message again, as a new one, the next time Enter is pressed.
    if (editing) {
        m_pendingFiles.clear();
        m_composer->clear();
    }
    refreshComposerContext();
}

void MainWindow::clearComposerContext()
{
    m_replyMessageId.clear();
    m_editingMessageId.clear();
    m_pendingFiles.clear();
    refreshComposerContext();
}

// Discord's Forward: a list of places with a search box, up to five ticked,
// and an optional line of your own that goes after the forward as an
// ordinary message.
void MainWindow::forwardMessage(const QString &messageId)
{
    const QString fromChannelId = m_currentChannelId;
    const ChannelInfo from = m_store->channel(fromChannelId);
    if (fromChannelId.isEmpty() || messageId.isEmpty())
        return;

    struct Place
    {
        QString channelId;
        QString label;
        QString search;
    };
    QList<Place> places;

    // Direct messages, most recent first.
    QList<ChannelInfo> direct = m_store->directChannels();
    std::sort(direct.begin(), direct.end(), [](const ChannelInfo &a, const ChannelInfo &b) {
        return a.lastMessageId.size() != b.lastMessageId.size() ? a.lastMessageId.size() > b.lastMessageId.size()
                                                                : a.lastMessageId > b.lastMessageId;
    });
    for (const ChannelInfo &channel : direct) {
        QString name = channel.name;
        if (name.isEmpty()) {
            QStringList people;
            for (const QString &userId : channel.recipientIds)
                people << m_store->userName(userId);
            people.removeAll(QString());
            name = people.join(QStringLiteral(", "));
        }
        if (name.isEmpty())
            continue;
        places.append({channel.id, QStringLiteral("@ ") + name, name});
    }

    // Text channels, the server you are in first.
    QList<GuildInfo> guilds = m_store->guilds();
    std::stable_sort(guilds.begin(), guilds.end(), [&from](const GuildInfo &a, const GuildInfo &b) {
        return (a.id == from.guildId) > (b.id == from.guildId);
    });
    for (const GuildInfo &guild : guilds) {
        for (const ChannelInfo &channel : m_store->channelsOfGuild(guild.id)) {
            if (!channel.isTextLike() || channel.isDirect() || channel.name.isEmpty())
                continue;
            places.append({channel.id, QStringLiteral("# %1   —   %2").arg(channel.name, guild.name),
                           channel.name + QLatin1Char(' ') + guild.name});
        }
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Forward to"));
    dialog.resize(460, 520);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("Forward to"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600;"));
    layout->addWidget(title);

    auto *search = new QLineEdit(&dialog);
    search->setPlaceholderText(QStringLiteral("Search"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);

    // A click on a row picks it and a second click lets it go, like the
    // round tick boxes in Discord's list.
    auto *list = new QListWidget(&dialog);
    list->setUniformItemSizes(true);
    list->setSelectionMode(QAbstractItemView::MultiSelection);
    for (const Place &place : std::as_const(places)) {
        auto *item = new QListWidgetItem(place.label, list);
        item->setData(Qt::UserRole, place.channelId);
        item->setData(Qt::UserRole + 1, place.search);
    }
    layout->addWidget(list, 1);

    auto *note = new QLineEdit(&dialog);
    note->setPlaceholderText(QStringLiteral("Add an optional message..."));
    layout->addWidget(note);

    auto *buttons = new QHBoxLayout;
    auto *count = new QLabel(&dialog);
    auto *cancel = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto *send = new QPushButton(QStringLiteral("Send"), &dialog);
    send->setObjectName(QStringLiteral("PrimaryButton"));
    send->setEnabled(false);
    buttons->addWidget(count, 1);
    buttons->addWidget(cancel);
    buttons->addWidget(send);
    layout->addLayout(buttons);

    // Discord lets one forward go to five places at most.
    static constexpr int MostPlaces = 5;
    const auto chosen = [list]() {
        QStringList ids;
        for (QListWidgetItem *item : list->selectedItems())
            ids << item->data(Qt::UserRole).toString();
        return ids;
    };
    connect(list, &QListWidget::itemSelectionChanged, &dialog, [list, count, send]() {
        QList<QListWidgetItem *> picked = list->selectedItems();
        if (picked.size() > MostPlaces && list->currentItem()) {
            QSignalBlocker block(list);
            list->currentItem()->setSelected(false);
            picked = list->selectedItems();
        }
        count->setText(picked.isEmpty() ? QString()
                                        : QStringLiteral("%1 of %2 picked").arg(picked.size()).arg(MostPlaces));
        send->setEnabled(!picked.isEmpty());
    });
    connect(search, &QLineEdit::textChanged, &dialog, [list](const QString &text) {
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem *item = list->item(row);
            item->setHidden(!text.isEmpty()
                            && !item->data(Qt::UserRole + 1).toString().contains(text, Qt::CaseInsensitive)
                            && !item->isSelected());
        }
    });
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(send, &QPushButton::clicked, &dialog, &QDialog::accept);
    search->setFocus();

    if (dialog.exec() != QDialog::Accepted)
        return;

    const QStringList targets = chosen();
    const QString line = note->text().trimmed();
    for (const QString &target : targets) {
        m_rest->forwardMessage(
            target, fromChannelId, from.guildId, messageId,
            [this, target, line](const QJsonObject &) {
                // The line goes after the forward, so it reads underneath it.
                if (!line.isEmpty())
                    m_rest->sendMessage(target, line, QString(), {}, [](const QJsonObject &) {},
                                        [](const RestClient::Error &) {});
            },
            [this](const RestClient::Error &error) {
                wlog(QStringLiteral("rest"), QStringLiteral("forward failed: HTTP %1 %2")
                                                 .arg(error.httpStatus)
                                                 .arg(error.message.left(160)));
                flashStatus(QStringLiteral("Could not forward (%1).").arg(error.message.left(120)), 6000);
            });
    }
    flashStatus(targets.size() == 1 ? QStringLiteral("Forwarded.")
                                    : QStringLiteral("Forwarded to %1 places.").arg(targets.size()),
                3000);
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
    addAttachments(picked);
}

// What Discord lets you send in the channel you are in. The check that
// matters is Discord's own, on its servers; this only saves a doomed upload.
qint64 MainWindow::uploadLimitBytes() const
{
    constexpr qint64 MiB = 1024 * 1024;

    // Your account: no Nitro, Nitro Classic, Nitro, Nitro Basic.
    qint64 limit = 10 * MiB;
    switch (m_selfPremiumType) {
    case 1:
    case 3: limit = 50 * MiB; break;
    case 2: limit = 500 * MiB; break;
    default: break;
    }

    // A boosted server raises it for everyone in it.
    const QString guildId = m_store->channel(m_currentChannelId).guildId;
    if (!guildId.isEmpty()) {
        const int tier = m_store->guild(guildId).premiumTier;
        if (tier >= 3)
            limit = qMax(limit, 100 * MiB);
        else if (tier == 2)
            limit = qMax(limit, 50 * MiB);
    }
    return limit;
}

void MainWindow::addAttachments(const QStringList &paths)
{
    if (m_currentChannelId.isEmpty() || !m_composer->isEnabled()) {
        flashStatus(QStringLiteral("Open a channel first, then drop the file there."), 4000);
        return;
    }

    constexpr qint64 MiB = 1024 * 1024;
    const qint64 limit = uploadLimitBytes();

    QStringList tooBig;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            flashStatus(QStringLiteral("%1 is a folder. Zip it first: right-click it, then "
                                       "Send to > Compressed (zipped) folder.")
                            .arg(info.fileName()),
                        8000);
            continue;
        }
        if (!info.isFile())
            continue;
        if (info.size() > limit) {
            tooBig.append(path);
            continue;
        }
        if (!m_pendingFiles.contains(path))
            m_pendingFiles.append(path);
    }

    if (m_pendingFiles.size() > 10) {
        m_pendingFiles = m_pendingFiles.mid(0, 10);
        flashStatus(QStringLiteral("Discord takes up to 10 files in one message."), 5000);
    }
    refreshComposerContext();

    if (tooBig.isEmpty())
        return;

    // Sending a file to another company is your call, so the first time it
    // asks. Saying yes turns the plugin on, and after that it just works.
    if (!m_plugins->isEnabled(QStringLiteral("big-files"))) {
        QStringList names;
        for (const QString &path : tooBig)
            names.append(QFileInfo(path).fileName());

        const auto answer = QMessageBox::question(
            this, QStringLiteral("Too big for Discord"),
            QStringLiteral("%1 is bigger than your Discord limit of %2 MB.\n\n"
                           "The Big files plugin can upload it to GoFile, a free file host, and put the "
                           "download link in your message instead. Anyone with the link can download it."
                           "\n\nTurn on Big files and upload it?")
                .arg(names.join(QStringLiteral(", ")))
                .arg(limit / MiB));
        if (answer != QMessageBox::Yes)
            return;
        m_plugins->setEnabled(QStringLiteral("big-files"), true);
    }

    for (const QString &path : tooBig)
        startBigUpload(path);
}

void MainWindow::startBigUpload(const QString &path)
{
    if (m_uploadsInFlight.contains(path))
        return;

    const QString name = QFileInfo(path).fileName();
    const QString channelId = m_currentChannelId;

    m_uploadsInFlight.insert(path, 0);
    refreshComposerContext();

    // Quitting mid-upload closes this window before the plugin stops the
    // upload, and the "stopped" report then arrives for a window that is gone.
    const QPointer<MainWindow> guard(this);

    const bool taken = m_plugins->runOversizedFile(
        path,
        [this, guard, path](qint64 sent, qint64 total) {
            if (!guard)
                return;
            if (total <= 0 || !m_uploadsInFlight.contains(path))
                return;
            const int percent = int(sent * 100 / total);
            if (percent == m_uploadsInFlight.value(path))
                return;
            m_uploadsInFlight.insert(path, percent);
            refreshComposerContext();
        },
        [this, guard, path, name, channelId](const QString &link, const QString &error) {
            if (!guard)
                return;
            m_uploadsInFlight.remove(path);
            refreshComposerContext();

            if (link.isEmpty()) {
                flashStatus(QStringLiteral("Could not upload %1: %2").arg(name, error), 8000);
                return;
            }

            // Still in the same channel: the link goes into your message, and
            // Enter sends it. Moved on: it goes on the clipboard instead, so it
            // never lands in a conversation it was not meant for.
            if (channelId == m_currentChannelId && m_composer->isEnabled()) {
                const QString existing = m_composer->toPlainText();
                QTextCursor cursor = m_composer->textCursor();
                cursor.movePosition(QTextCursor::End);
                const bool newLine = !existing.isEmpty() && !existing.endsWith(QLatin1Char('\n'));
                cursor.insertText((newLine ? QStringLiteral("\n") : QString()) + name + QStringLiteral(": ")
                                  + link);
                m_composer->setTextCursor(cursor);
                m_composer->setFocus();
                flashStatus(QStringLiteral("%1 is uploaded. The link is in your message. Press Enter to send it.")
                                .arg(name),
                            8000);
            } else {
                QApplication::clipboard()->setText(link);
                flashStatus(QStringLiteral("%1 is uploaded. You changed channel, so the link is copied. "
                                           "Paste it where you want it.")
                                .arg(name),
                            10000);
            }
        });

    if (!taken) {
        m_uploadsInFlight.remove(path);
        refreshComposerContext();
        flashStatus(QStringLiteral("Nothing could upload %1.").arg(name), 6000);
    }
}

bool MainWindow::handleFileDrop(QEvent *event)
{
    auto *drop = static_cast<QDropEvent *>(event);
    const QMimeData *mime = drop->mimeData();
    if (!mime || !mime->hasUrls())
        return false;

    QStringList files;
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile())
            files.append(url.toLocalFile());
    }
    if (files.isEmpty())
        return false;

    drop->acceptProposedAction();
    if (event->type() == QEvent::Drop)
        addAttachments(files);
    return true;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!handleFileDrop(event))
        QMainWindow::dragEnterEvent(event);
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (!handleFileDrop(event))
        QMainWindow::dragMoveEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (!handleFileDrop(event))
        QMainWindow::dropEvent(event);
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
        bool animated = false;
    };

    QList<Cell> cells;
    std::function<void(const QString &)> onPick;

    explicit EmojiBoard(QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
    }

    ~EmojiBoard() override { stopMoving(); }

    void setQuery(const QString &query)
    {
        stopMoving();
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
        font.setPixelSize(26);
        painter.setFont(font);
        painter.setPen(Qt::white);

        syncMoving();

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
                QImage picture;
                if (AnimatedImage *moving = m_moving.value(cell.icon.toString()))
                    picture = moving->currentFrame();
                if (picture.isNull())
                    picture = MediaCache::instance().image(cell.icon);
                if (!picture.isNull())
                    painter.drawImage(box.adjusted(6, 6, -6, -6), picture);
            }
        }
    }

    void hideEvent(QHideEvent *event) override
    {
        stopMoving();
        QWidget::hideEvent(event);
    }

    // Animated server emoji move while they are on screen, as in Discord's
    // picker. Only the rows in view play: a server with hundreds of animated
    // emoji would otherwise decode all of them the whole time the picker is
    // open. Ones scrolled away stop and let go of their frames.
    void syncMoving()
    {
        const QRect seen = visibleRegion().boundingRect();
        QSet<QString> wanted;
        if (!seen.isEmpty() && !m_shown.isEmpty()) {
            const int firstRow = qMax(0, seen.top() / CellSize);
            const int lastRow = qMin((int(m_shown.size()) + Columns - 1) / Columns - 1, seen.bottom() / CellSize);
            for (int row = firstRow; row <= lastRow; ++row) {
                for (int column = 0; column < Columns; ++column) {
                    const int slot = row * Columns + column;
                    if (slot >= m_shown.size())
                        break;
                    const Cell &cell = cells.at(m_shown.at(slot));
                    if (!cell.animated || cell.icon.isEmpty())
                        continue;
                    const QString key = cell.icon.toString();
                    wanted.insert(key);
                    if (m_moving.contains(key))
                        continue;
                    const QByteArray bytes = MediaCache::instance().animationData(cell.icon);
                    if (bytes.isEmpty())
                        continue;   // not here yet; the arrival repaints and this tries again
                    auto *moving = new AnimatedImage(this);
                    if (!moving->setData(bytes) || !moving->isAnimated()) {
                        delete moving;
                        continue;
                    }
                    const QRect box(column * CellSize, row * CellSize, CellSize, CellSize);
                    connect(moving, &AnimatedImage::frameChanged, this, [this, box]() { update(box); });
                    moving->setPlaying(true);
                    m_moving.insert(key, moving);
                }
            }
        }
        for (auto it = m_moving.begin(); it != m_moving.end();) {
            if (wanted.contains(it.key())) {
                ++it;
            } else {
                delete it.value();
                it = m_moving.erase(it);
            }
        }
    }

    void stopMoving()
    {
        qDeleteAll(m_moving);
        m_moving.clear();
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
    // Discord's picker draws faces at 40 in 48 cells; this one is a little
    // narrower so nine fit across without a sideways scroll.
    static constexpr int Columns = 9;
    static constexpr int CellSize = 44;

    QList<int> m_shown;
    int m_hover = -1;
    QHash<QString, AnimatedImage *> m_moving;   // playing now, by picture address

    int slotAt(const QPoint &pos) const
    {
        if (pos.x() < 0 || pos.y() < 0 || pos.x() >= Columns * CellSize)
            return -1;
        return (pos.y() / CellSize) * Columns + (pos.x() / CellSize);
    }
};

// Every face the reaction picker and the composer picker share: a short stock
// row, then every custom emoji from every server you are in.
QList<EmojiBoard::Cell> allEmojiCells(const MessageStore *store)
{
    QList<EmojiBoard::Cell> cells;
    const struct {
        const char *face;
        const char *name;
    } stock[] = {
        {"👍", "thumb yes"}, {"❤️", "heart love"}, {"😂", "joy laugh"}, {"😮", "wow"},
        {"😢", "sad cry"},   {"🔥", "fire"},       {"😀", "grin smile"}, {"👎", "thumb no"},
        {"🎉", "party"},     {"😭", "cry sob"},    {"😡", "angry"},      {"👀", "eyes"},
        {"💯", "hundred"},   {"✅", "check yes"},  {"🙏", "pray"},       {"💀", "skull"},
        {"🤔", "think"},     {"😎", "cool"},       {"🥳", "party"},      {"🤝", "handshake"},
    };
    for (const auto &item : stock) {
        EmojiBoard::Cell cell;
        cell.insert = QString::fromUtf8(item.face);
        cell.face = cell.insert;
        cell.filter = cell.insert + QLatin1Char(' ') + QString::fromUtf8(item.name);
        cells.append(cell);
    }
    if (!store)
        return cells;
    for (const GuildInfo &server : store->guilds()) {
        for (const EmojiInfo &emoji : server.emojis) {
            if (emoji.id.isEmpty() || emoji.name.isEmpty())
                continue;
            EmojiBoard::Cell cell;
            cell.insert = emoji.animated ? QStringLiteral("<a:%1:%2>").arg(emoji.name, emoji.id)
                                         : QStringLiteral("<:%1:%2>").arg(emoji.name, emoji.id);
            cell.filter = server.name + QLatin1Char(' ') + emoji.name;
            cell.animated = emoji.animated;
            const QString ext = emoji.animated ? QStringLiteral("gif") : QStringLiteral("png");
            cell.icon = QUrl(QStringLiteral("https://cdn.discordapp.com/emojis/%1.%2?size=64")
                                 .arg(emoji.id, ext));
            cells.append(cell);
        }
    }
    return cells;
}

// A reaction is name:id, not the <:name:id> the composer inserts.
QString reactionToken(const QString &insert)
{
    static const QRegularExpression custom(QStringLiteral("^<a?:([^:]+):([0-9]+)>$"));
    const QRegularExpressionMatch match = custom.match(insert);
    if (match.hasMatch())
        return match.captured(1) + QLatin1Char(':') + match.captured(2);
    return insert;
}

// The GIF page: two columns of tiles, each as tall as its picture wants, the
// way Discord lays them out. Painted, like the emoji page, so only the tiles
// on screen fetch anything. Tiles are still until the pointer is on one;
// fifty moving pictures at once is a lot of work for a menu.
class GifBoard : public QWidget
{
public:
    struct Tile
    {
        QString id;
        QString title;     // a category's name, drawn over it
        QUrl picture;      // a small .gif from Tenor
        QString page;      // what is sent: Discord turns it into the moving card
        int width = 0;
        int height = 0;
        bool category = false;
    };

    std::function<void(const Tile &)> onPick;

    explicit GifBoard(QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
    }

    void setTiles(const QList<Tile> &tiles)
    {
        m_tiles = tiles;
        m_message.clear();
        stopHover();
        relayout();
    }

    // Shown instead of tiles: "Loading", nothing found, or what went wrong.
    void setMessage(const QString &text)
    {
        m_tiles.clear();
        m_rects.clear();
        m_message = text;
        stopHover();
        setFixedHeight(120);
        update();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (event->oldSize().width() != width())
            relayout();
    }

    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        if (!m_message.isEmpty()) {
            painter.setPen(QColor(QLatin1String(Theme::TextMuted)));
            painter.drawText(rect().adjusted(12, 0, -12, 0), Qt::AlignCenter | Qt::TextWordWrap, m_message);
            return;
        }

        for (int index = 0; index < m_rects.size(); ++index) {
            const QRect box = m_rects.at(index);
            if (!box.intersects(event->rect()))
                continue;
            const Tile &tile = m_tiles.at(index);

            QPainterPath shape;
            shape.addRoundedRect(QRectF(box), 6, 6);
            painter.save();
            painter.setClipPath(shape);
            painter.fillRect(box, QColor(255, 255, 255, 12));

            QImage frame;
            if (index == m_hover && m_playing)
                frame = m_playing->currentFrame();
            if (frame.isNull())
                frame = MediaCache::instance().image(tile.picture);
            if (!frame.isNull()) {
                // Cover the tile, trimming whichever side is too long.
                const QSize fitted = frame.size().scaled(box.size(), Qt::KeepAspectRatioByExpanding);
                const QRect target(box.center().x() - fitted.width() / 2 + 1,
                                   box.center().y() - fitted.height() / 2 + 1, fitted.width(), fitted.height());
                painter.drawImage(target, frame);
            }

            if (tile.category) {
                painter.fillRect(box, QColor(0, 0, 0, index == m_hover ? 90 : 140));
                QFont font = painter.font();
                font.setPixelSize(15);
                font.setBold(true);
                painter.setFont(font);
                painter.setPen(Qt::white);
                painter.drawText(box.adjusted(8, 0, -8, 0), Qt::AlignCenter | Qt::TextWordWrap, tile.title);
            } else if (index == m_hover) {
                painter.setPen(QPen(QColor(QLatin1String(Theme::Accent)), 2));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(QRectF(box).adjusted(1, 1, -1, -1), 6, 6);
            }
            painter.restore();
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int index = tileAt(event->position().toPoint());
        if (index == m_hover)
            return;
        stopHover();
        m_hover = index;
        startHover();
        update();
    }

    void leaveEvent(QEvent *) override
    {
        stopHover();
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !onPick)
            return;
        const int index = tileAt(event->position().toPoint());
        if (index >= 0)
            onPick(m_tiles.at(index));
    }

private:
    static constexpr int Gap = 6;
    static constexpr int CategoryHeight = 96;

    QList<Tile> m_tiles;
    QList<QRect> m_rects;
    QString m_message;
    int m_hover = -1;
    AnimatedImage *m_playing = nullptr;

    int tileAt(const QPoint &pos) const
    {
        for (int index = 0; index < m_rects.size(); ++index) {
            if (m_rects.at(index).contains(pos))
                return index;
        }
        return -1;
    }

    // Each tile goes under whichever column is shorter so far.
    void relayout()
    {
        m_rects.clear();
        const int columnWidth = qMax(40, (width() - Gap) / 2);
        int bottom[2] = {0, 0};
        for (const Tile &tile : std::as_const(m_tiles)) {
            int height = CategoryHeight;
            if (!tile.category && tile.width > 0 && tile.height > 0)
                height = qBound(60, columnWidth * tile.height / tile.width, 280);
            const int column = bottom[1] < bottom[0] ? 1 : 0;
            m_rects.append(QRect(column * (columnWidth + Gap), bottom[column], columnWidth, height));
            bottom[column] += height + Gap;
        }
        setFixedHeight(qMax(120, qMax(bottom[0], bottom[1])));
        update();
    }

    void startHover()
    {
        if (m_hover < 0)
            return;
        const QByteArray bytes = MediaCache::instance().animationData(m_tiles.at(m_hover).picture);
        if (bytes.isEmpty())
            return;
        m_playing = new AnimatedImage(this);
        if (!m_playing->setData(bytes) || !m_playing->isAnimated()) {
            delete m_playing;
            m_playing = nullptr;
            return;
        }
        const QRect box = m_rects.at(m_hover);
        connect(m_playing, &AnimatedImage::frameChanged, this, [this, box]() { update(box); });
        m_playing->setPlaying(true);
    }

    void stopHover()
    {
        m_hover = -1;
        delete m_playing;
        m_playing = nullptr;
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
    popup->setFixedWidth(432);
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
    scroll->setFixedHeight(380);
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

    auto *gifBoard = new GifBoard(page);
    gifBoard->hide();
    pageLayout->addWidget(gifBoard);
    pageLayout->addStretch(1);

    QList<EmojiBoard::Cell> cells = allEmojiCells(m_store);

    // Stickers are pictures, not letters: four across at 96, near the size
    // Discord's own sticker page shows them.
    constexpr int columns = 4;
    constexpr int StickerCell = 96;
    constexpr int StickerPicture = 88;
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
        button->setFixedSize(StickerCell, StickerCell);
        button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(filter);
        button->setProperty("filter", filter);
        button->setProperty("iconUrl", icon);
        button->setProperty("kind", kind);
        button->setIconSize(QSize(StickerPicture, StickerPicture));
        const QImage picture = MediaCache::instance().image(icon);
        if (!picture.isNull())
            button->setIcon(QPixmap::fromImage(picture));
        connect(button, &QToolButton::clicked, popup, [onClick]() { onClick(); });
        held->append(button);
    };

    auto *gifTimer = new QTimer(popup);
    gifTimer->setSingleShot(true);
    // Long enough that a word being typed is one search, not one per letter;
    // Discord rate limits GIF searches tightly.
    gifTimer->setInterval(500);

    // A gif from any of Discord's GIF answers, as a tile. Discord's list is
    // {id, url, src, gif_src, preview, width, height}; src is in the size we
    // asked for (tinygif), url is the Tenor page that gets sent.
    const auto gifTiles = [](const QJsonArray &gifs) {
        QList<GifBoard::Tile> tiles;
        for (const QJsonValue &value : gifs) {
            const QJsonObject gif = value.toObject();
            GifBoard::Tile tile;
            tile.id = gif.value(QStringLiteral("id")).toVariant().toString();
            tile.page = gif.value(QStringLiteral("url")).toString();
            // src is in the format asked for (a small WebP); gif_src when src
            // turns out to be a video, which the tiles cannot draw.
            tile.picture = QUrl(gif.value(QStringLiteral("src")).toString());
            const QString srcPath = tile.picture.path().toLower();
            if (!srcPath.endsWith(QLatin1String(".webp")) && !srcPath.endsWith(QLatin1String(".gif")))
                tile.picture = QUrl(gif.value(QStringLiteral("gif_src")).toString());
            tile.width = gif.value(QStringLiteral("width")).toInt();
            tile.height = gif.value(QStringLiteral("height")).toInt();
            if (!tile.page.isEmpty() && tile.picture.isValid() && !tile.picture.isEmpty())
                tiles.append(tile);
        }
        return tiles;
    };

    // One answer at a time: a slow reply to an older search must not replace
    // the newer one on screen.
    const auto nextTicket = [popup]() {
        const int ticket = popup->property("gifTicket").toInt() + 1;
        popup->setProperty("gifTicket", ticket);
        return ticket;
    };
    const auto showGifError = [gifBoard](const RestClient::Error &error) {
        wlog(QStringLiteral("gifs"), QStringLiteral("GIF request failed: HTTP %1 %2")
                                         .arg(error.httpStatus)
                                         .arg(error.message.left(160)));
        gifBoard->setMessage(QStringLiteral("Could not load GIFs (%1).").arg(error.message.left(80)));
    };

    // The first page, as in Discord: a Trending tile, then Tenor's categories.
    // A category is a search for its name.
    auto loadGifs = [this, popup, search, gifBoard, gifTiles, nextTicket, showGifError]() {
        const int ticket = nextTicket();
        const QString query = search->text().trimmed();
        QPointer<QFrame> alive(popup);
        gifBoard->setMessage(QStringLiteral("Loading GIFs…"));

        const auto current = [alive, ticket]() {
            return alive && alive->property("gifTicket").toInt() == ticket;
        };
        const auto failed = [current, showGifError](const RestClient::Error &error) {
            if (current())
                showGifError(error);
        };
        const auto showList = [current, gifBoard, gifTiles](const QJsonArray &gifs) {
            if (!current())
                return;
            const QList<GifBoard::Tile> tiles = gifTiles(gifs);
            if (tiles.isEmpty())
                gifBoard->setMessage(QStringLiteral("No GIFs found."));
            else
                gifBoard->setTiles(tiles);
        };

        if (popup->property("gifTrending").toBool()) {
            m_rest->trendingGifs(showList, failed);
        } else if (!query.isEmpty()) {
            m_rest->searchGifs(query, showList, failed);
        } else {
            m_rest->gifCategories([current, gifBoard, gifTiles](const QJsonObject &payload) {
                if (!current())
                    return;
                QList<GifBoard::Tile> tiles;
                const QList<GifBoard::Tile> trending = gifTiles(payload.value(QStringLiteral("gifs")).toArray());
                if (!trending.isEmpty()) {
                    GifBoard::Tile tile = trending.first();
                    tile.category = true;
                    tile.title = QStringLiteral("Trending GIFs");
                    tile.page.clear();
                    tiles.append(tile);
                }
                for (const QJsonValue &value : payload.value(QStringLiteral("categories")).toArray()) {
                    const QJsonObject category = value.toObject();
                    GifBoard::Tile tile;
                    tile.category = true;
                    tile.title = category.value(QStringLiteral("name")).toString();
                    tile.picture = QUrl(category.value(QStringLiteral("src")).toString());
                    if (!tile.title.isEmpty())
                        tiles.append(tile);
                }
                if (tiles.isEmpty())
                    gifBoard->setMessage(QStringLiteral("No GIFs right now."));
                else
                    gifBoard->setTiles(tiles);
            }, failed);
        }
    };

    gifBoard->onPick = [this, popup, search, loadGifs](const GifBoard::Tile &tile) {
        if (tile.category) {
            // Trending has no search word; the rest are searched by name,
            // which also puts the word in the box, as Discord does.
            if (tile.title == QLatin1String("Trending GIFs") && tile.page.isEmpty()) {
                popup->setProperty("gifTrending", true);
                loadGifs();
            } else {
                search->setText(tile.title);
            }
            return;
        }
        if (m_currentChannelId.isEmpty())
            return;
        // Sent as the Tenor address, exactly what the official client sends;
        // Discord turns it into the moving card for everyone.
        m_rest->sendMessage(m_currentChannelId, tile.page, QString(), {}, [](const QJsonObject &) {},
                            [this](const RestClient::Error &error) {
                                flashStatus(QStringLiteral("GIF failed (%1).").arg(error.message.left(120)), 5000);
                            });
        m_rest->selectGif(tile.id, search->text().trimmed());
        popup->close();
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
                [this, popup, search, refill, loadGifs, kind, buttonHost, board, gifBoard, addPictureButton]() {
            popup->setProperty("kind", kind);
            const bool emojiTab = kind == QLatin1String("emoji");
            const bool gifTab = kind == QLatin1String("gif");
            board->setVisible(emojiTab);
            gifBoard->setVisible(gifTab);
            buttonHost->setVisible(!emojiTab && !gifTab);
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
                m_rest->fetchStickerPacks([alive, refill, addPictureButton, this](const QJsonObject &payload) {
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
                            addPictureButton(QStringLiteral("sticker"), name, icon, [this, alive, id]() {
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
                        }
                    }
                    if (alive->property("kind").toString() == QLatin1String("sticker"))
                        refill(QString());
                }, [](const RestClient::Error &) {});
            }
            if (gifTab) {
                search->setPlaceholderText(QStringLiteral("Search GIFs"));
                popup->setProperty("gifTrending", false);
                loadGifs();
            } else if (kind == QLatin1String("sticker")) {
                search->setPlaceholderText(QStringLiteral("Find a sticker"));
            } else {
                search->setPlaceholderText(QStringLiteral("Find an emoji"));
            }
            if (!emojiTab && !gifTab)
                refill(search->text());
        });
        tabs->addWidget(button);
    }
    outer->addLayout(tabs);

    connect(search, &QLineEdit::textChanged, popup, [popup, board, gifTimer, refill](const QString &text) {
        const QString kind = popup->property("kind").toString();
        if (kind == QLatin1String("gif")) {
            popup->setProperty("gifTrending", false);
            gifTimer->start();
        } else if (kind == QLatin1String("emoji"))
            board->setQuery(text);
        else
            refill(text);
    });
    connect(&MediaCache::instance(), &MediaCache::ready, popup, [popup, gifBoard](const QUrl &url) {
        if (gifBoard->isVisible())
            gifBoard->update();
        const QImage picture = MediaCache::instance().image(url);
        if (picture.isNull())
            return;
        const auto found = popup->findChildren<QToolButton *>();
        for (QToolButton *button : found) {
            if (button->property("iconUrl").toUrl() == url)
                button->setIcon(QPixmap::fromImage(picture));
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

void MainWindow::showReactionPicker(const QString &messageId, const QPoint &globalPos)
{
    if (messageId.isEmpty() || m_currentChannelId.isEmpty() || !m_rest)
        return;

    auto *popup = new QFrame(nullptr, Qt::Popup | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setObjectName(QStringLiteral("EmojiPicker"));
    popup->setFixedWidth(432);
    popup->setStyleSheet(QStringLiteral(
        "QFrame#EmojiPicker { background-color: %1; border: 1px solid %2; border-radius: 12px; }"
        "QLineEdit { background: %3; color: %4; border: none; border-radius: 8px; padding: 6px 8px; "
        "font-family: \"Segoe UI\"; font-size: 13px; }"
        "QScrollArea { background: transparent; border: none; }"
        "QLabel { color: %5; background: transparent; font-size: 11px; }")
                              .arg(QLatin1String(Theme::SurfaceSidebar), QLatin1String(Theme::Border),
                                   QLatin1String(Theme::SurfaceInput), QLatin1String(Theme::TextPrimary),
                                   QLatin1String(Theme::TextMuted)));

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
    scroll->setFixedHeight(380);
    outer->addWidget(scroll);

    auto *page = new QWidget;
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    scroll->setWidget(page);

    auto *board = new EmojiBoard(page);
    board->cells = allEmojiCells(m_store);
    board->onPick = [this, popup, messageId](const QString &token) {
        const QString emoji = reactionToken(token);
        popup->close();
        if (emoji.isEmpty() || m_currentChannelId.isEmpty())
            return;
        m_rest->addReaction(m_currentChannelId, messageId, emoji, [](const QJsonObject &) {},
                            [this](const RestClient::Error &error) {
                                flashStatus(QStringLiteral("Could not react (%1).").arg(error.message.left(120)),
                                            4000);
                            });
    };
    board->setQuery(QString());
    pageLayout->addWidget(board);
    connect(search, &QLineEdit::textChanged, board, [board](const QString &text) { board->setQuery(text); });
    connect(&MediaCache::instance(), &MediaCache::ready, board, [board](const QUrl &) { board->update(); });

    popup->adjustSize();
    QPoint topLeft = globalPos;
    if (QScreen *screen = QGuiApplication::screenAt(globalPos)) {
        const QRect area = screen->availableGeometry();
        if (topLeft.x() + popup->width() > area.right())
            topLeft.setX(qMax(area.left(), area.right() - popup->width() - 8));
        if (topLeft.y() + popup->height() > area.bottom())
            topLeft.setY(qMax(area.top(), globalPos.y() - popup->height() - 8));
    }
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

    // Discord puts the link under the pointer first: Copy Link and Open Link
    // for a web link, Copy Image Link for a picture. The message's own link
    // is a different thing and keeps its own name further down.
    const QString anchor = m_messageView->anchorAt(pos);
    QString pointedUrl;
    bool pointedImage = false;
    if (anchor.startsWith(QLatin1String("http://")) || anchor.startsWith(QLatin1String("https://"))) {
        pointedUrl = anchor;
    } else if (anchor.startsWith(QLatin1String("singularity-image:"))) {
        pointedUrl = anchor.mid(int(qstrlen("singularity-image:")));
        pointedImage = true;
    }
    if (!pointedUrl.isEmpty()) {
        connect(menu.addAction(pointedImage ? QStringLiteral("Copy Image Link") : QStringLiteral("Copy Link")),
                &QAction::triggered, this, [pointedUrl]() { QApplication::clipboard()->setText(pointedUrl); });
        connect(menu.addAction(pointedImage ? QStringLiteral("Open Image in Browser")
                                            : QStringLiteral("Open Link")),
                &QAction::triggered, this, [pointedUrl]() { QDesktopServices::openUrl(QUrl(pointedUrl)); });
        menu.addSeparator();
    }

    connect(menu.addAction(QStringLiteral("Reply")), &QAction::triggered, this, [this, messageId]() {
        beginReply(messageId);
    });
    connect(menu.addAction(QStringLiteral("Forward")), &QAction::triggered, this, [this, messageId]() {
        forwardMessage(messageId);
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
    connect(menu.addAction(QStringLiteral("Copy Message Link")), &QAction::triggered, this, [link]() {
        QApplication::clipboard()->setText(link);
    });

    // Right-clicking beside a link rather than on it still offers it. One
    // link gets a plain entry; several get a list to pick from.
    if (pointedUrl.isEmpty()) {
        static const QRegularExpression webLink(QStringLiteral(R"(https?://[^\s<>]+)"));
        QStringList urls;
        auto it = webLink.globalMatch(message.content);
        while (it.hasNext()) {
            QString url = it.next().captured(0);
            // Punctuation after a link belongs to the sentence, not the link.
            while (!url.isEmpty() && QStringLiteral(".,;:!?)]'\"*_~|").contains(url.back())
                   && !(url.back() == QLatin1Char(')') && url.count(QLatin1Char('(')) >= url.count(QLatin1Char(')'))))
                url.chop(1);
            if (!url.isEmpty() && !urls.contains(url))
                urls.append(url);
        }
        const auto shortName = [](const QString &url) {
            return url.size() > 60 ? url.left(57) + QStringLiteral("...") : url;
        };
        if (urls.size() == 1) {
            const QString url = urls.first();
            connect(menu.addAction(QStringLiteral("Copy Link")), &QAction::triggered, this,
                    [url]() { QApplication::clipboard()->setText(url); });
        } else if (urls.size() > 1) {
            QMenu *copyLinks = menu.addMenu(QStringLiteral("Copy Link"));
            for (const QString &url : urls) {
                connect(copyLinks->addAction(shortName(url)), &QAction::triggered, this,
                        [url]() { QApplication::clipboard()->setText(url); });
            }
        }
    }

    connect(menu.addAction(QStringLiteral("Copy Message ID")), &QAction::triggered, this, [messageId]() {
        QApplication::clipboard()->setText(messageId);
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
    connect(react->addAction(QStringLiteral("All emoji…")), &QAction::triggered, this,
            [this, messageId, pos]() {
                const QPoint at = m_messageView->viewport()->mapToGlobal(pos);
                QTimer::singleShot(0, this, [this, messageId, at]() { showReactionPicker(messageId, at); });
            });

    menu.exec(m_messageView->viewport()->mapToGlobal(pos));
}

void MainWindow::refreshDmTiles()
{
    if (!m_guildRail || m_guildRail->count() == 0 || m_refreshingDmTiles)
        return;

    // Which chats should have a tile: the unread direct messages, newest
    // first, as many as Discord would show before it starts scrolling.
    QStringList wanted;
    for (const ChannelInfo &channel : m_store->directChannels()) {
        if (m_store->mentionCount(channel.id) > 0)
            wanted.append(channel.id);
        if (wanted.size() >= 8)
            break;
    }

    // What is there now: the tiles straight after the home tile.
    QStringList have;
    for (int row = 1; row < m_guildRail->count(); ++row) {
        QListWidgetItem *item = m_guildRail->item(row);
        if (item->data(KindRole).toString() != QLatin1String("unreadDm"))
            break;
        have.append(item->data(IdRole).toString());
    }

    const auto describe = [this](QListWidgetItem *item, const QString &channelId) {
        const ChannelInfo channel = m_store->channel(channelId);
        QString name = channel.name;
        QIcon icon;
        if (channel.recipientIds.size() == 1) {
            const UserInfo person = m_store->user(channel.recipientIds.first());
            name = person.displayName().isEmpty() ? person.username : person.displayName();
            const QUrl url = MediaCache::avatarUrl(person.id, person.avatarHash, 96);
            const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
            if (!picture.isNull())
                icon = MediaCache::circular(picture, GuildIconPixels);
        }
        if (name.isEmpty())
            name = QStringLiteral("Direct message");
        if (icon.isNull())
            icon = MediaCache::initialsAvatar(name, GuildIconPixels);
        item->setIcon(icon);
        item->setToolTip(name);
        item->setData(SingularityRoles::Mentions, m_store->mentionCount(channelId));
        item->setData(SingularityRoles::Unread, true);
    };

    if (have == wanted) {
        for (int i = 0; i < have.size(); ++i)
            describe(m_guildRail->item(1 + i), have.at(i));
        m_guildRail->viewport()->update();
        return;
    }

    // Rebuild just those rows. The selection is held by what it is, not by
    // row number, because the rows under it move.
    m_refreshingDmTiles = true;
    QListWidgetItem *current = m_guildRail->currentItem();
    const QString currentKind = current ? current->data(KindRole).toString() : QString();
    const QString currentId = current ? current->data(IdRole).toString() : QString();

    m_guildRail->blockSignals(true);
    for (int i = 0; i < have.size(); ++i)
        delete m_guildRail->takeItem(1);
    for (int i = 0; i < wanted.size(); ++i) {
        auto *item = new QListWidgetItem;
        item->setData(IdRole, wanted.at(i));
        item->setData(KindRole, QStringLiteral("unreadDm"));
        item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled & ~Qt::ItemIsDropEnabled);
        describe(item, wanted.at(i));
        m_guildRail->insertItem(1 + i, item);
    }
    for (int row = 0; row < m_guildRail->count(); ++row) {
        QListWidgetItem *item = m_guildRail->item(row);
        if (item->data(KindRole).toString() == currentKind && item->data(IdRole).toString() == currentId) {
            m_guildRail->setCurrentRow(row);
            break;
        }
    }
    m_guildRail->blockSignals(false);
    m_refreshingDmTiles = false;
}

void MainWindow::readAll()
{
    const QList<QPair<QString, QString>> unread = m_store->unreadChannels();
    if (unread.isEmpty()) {
        flashStatus(QStringLiteral("Nothing is unread."), 4000);
        return;
    }

    // Marked here at once, with one redraw; Discord is told through the queue.
    m_store->markChannelsRead(unread);
    wlog(QStringLiteral("ui"), QStringLiteral("Read All: %1 chats marked read here").arg(unread.size()));
    queueAcks(unread);
}

void MainWindow::queueAcks(const QList<QPair<QString, QString>> &channelsAndMessages)
{
    // One entry per chat: a chat already waiting just gets the newer message.
    QHash<QString, int> waitingAt;
    for (int i = 0; i < m_ackQueue.size(); ++i)
        waitingAt.insert(m_ackQueue.at(i).first, i);
    for (const auto &pair : channelsAndMessages) {
        const auto at = waitingAt.constFind(pair.first);
        if (at != waitingAt.constEnd()) {
            m_ackQueue[at.value()].second = pair.second;
            continue;
        }
        waitingAt.insert(pair.first, int(m_ackQueue.size()));
        m_ackQueue.append(pair);
    }
    if (!m_ackSending) {
        m_ackSent = 0;
        m_ackGivenUp = 0;
        sendNextAckBatch();
    }
}

void MainWindow::sendNextAckBatch()
{
    if (m_ackQueue.isEmpty()) {
        m_ackSending = false;
        wlog(QStringLiteral("ui"), QStringLiteral("read marks: Discord has %1 chats%2")
                                       .arg(m_ackSent)
                                       .arg(m_ackGivenUp ? QStringLiteral(", %1 refused").arg(m_ackGivenUp)
                                                         : QString()));
        flashStatus(m_ackGivenUp == 0
                        ? QStringLiteral("Marked %1 chats as read.").arg(m_ackSent)
                        : QStringLiteral("Marked %1 chats as read; Discord refused %2.").arg(m_ackSent).arg(m_ackGivenUp),
                    5000);
        return;
    }

    m_ackSending = true;
    const QList<QPair<QString, QString>> batch = m_ackQueue.mid(0, 100);
    if (m_ackSent + m_ackQueue.size() > 100) {
        flashStatus(QStringLiteral("Marking as read on Discord... %1 left").arg(m_ackQueue.size()), 3000);
    }

    m_rest->ackBulk(
        batch,
        [this, count = batch.size()](const QJsonObject &) {
            m_ackQueue.remove(0, qMin(count, int(m_ackQueue.size())));
            m_ackSent += count;
            // Discord's client waits a second between batches.
            QTimer::singleShot(1000, this, &MainWindow::sendNextAckBatch);
        },
        [this, count = batch.size()](const RestClient::Error &error) {
            if (error.httpStatus == 429) {
                // Too fast: the same batch again when Discord says.
                const double wait = error.body.value(QStringLiteral("retry_after")).toDouble(1.0);
                const int ms = qBound(500, int(wait * 1000) + 250, 60000);
                wlog(QStringLiteral("ui"), QStringLiteral("read marks: Discord asked us to wait %1 ms").arg(ms));
                QTimer::singleShot(ms, this, &MainWindow::sendNextAckBatch);
                return;
            }
            wlog(QStringLiteral("ui"), QStringLiteral("read marks: Discord refused %1 chats: HTTP %2 %3")
                                           .arg(count)
                                           .arg(error.httpStatus)
                                           .arg(error.message.left(160)));
            m_ackQueue.remove(0, qMin(count, int(m_ackQueue.size())));
            m_ackGivenUp += count;
            QTimer::singleShot(1000, this, &MainWindow::sendNextAckBatch);
        });
}

void MainWindow::markGuildRead(const QString &guildId)
{
    QList<QPair<QString, QString>> unread;
    for (const auto &pair : m_store->unreadChannels()) {
        if (m_store->channel(pair.first).guildId == guildId)
            unread.append(pair);
    }
    if (unread.isEmpty()) {
        flashStatus(QStringLiteral("Nothing here is unread."), 3000);
        return;
    }
    m_store->markChannelsRead(unread);
    queueAcks(unread);
    refreshUnreadMarks();
}

void MainWindow::showGuildMenu(const QPoint &globalPos)
{
    const QString guildId = m_currentGuildId;
    const GuildInfo guild = m_store->guild(guildId);
    if (guild.id.isEmpty())
        return;

    QMenu menu(this);
    QAction *markRead = menu.addAction(QStringLiteral("Mark As Read"));
    markRead->setEnabled(m_store->guildHasUnread(guildId));
    QAction *copyId = menu.addAction(QStringLiteral("Copy Server ID"));
    menu.addSeparator();
    QAction *leave = menu.addAction(QStringLiteral("Leave Server"));

    QAction *chosen = menu.exec(globalPos);
    if (!chosen)
        return;
    if (chosen == markRead)
        markGuildRead(guildId);
    else if (chosen == copyId)
        QApplication::clipboard()->setText(guildId);
    else if (chosen == leave)
        leaveGuild(guildId);
}

void MainWindow::leaveGuild(const QString &guildId)
{
    const GuildInfo guild = m_store->guild(guildId);
    if (guild.id.isEmpty())
        return;

    // Discord will not let the owner leave; ownership has to go first.
    if (guild.ownerId == m_selfUserId) {
        QMessageBox::information(this, QStringLiteral("Leave Server"),
                                 QStringLiteral("You own %1. Transfer ownership or delete the server in "
                                                "Discord before leaving it.")
                                     .arg(guild.name));
        return;
    }

    // Discord's own wording.
    QMessageBox ask(QMessageBox::Warning, QStringLiteral("Leave '%1'").arg(guild.name),
                    QStringLiteral("Are you sure you want to leave %1? You won't be able to rejoin this "
                                   "server unless you are re-invited.")
                        .arg(guild.name),
                    QMessageBox::NoButton, this);
    QPushButton *confirm = ask.addButton(QStringLiteral("Leave Server"), QMessageBox::DestructiveRole);
    ask.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
    ask.exec();
    if (ask.clickedButton() != confirm)
        return;

    if (guildId == m_voiceGuildId)
        leaveVoice();

    const QString name = guild.name;
    m_rest->leaveGuild(
        guildId,
        [this, guildId, name](const QJsonObject &) {
            wlog(QStringLiteral("ui"), QStringLiteral("left server %1").arg(guildId));
            guildGone(guildId);
            flashStatus(QStringLiteral("You left %1.").arg(name), 4000);
        },
        [this](const RestClient::Error &error) {
            wlog(QStringLiteral("ui"), QStringLiteral("leaving a server failed: HTTP %1 %2")
                                           .arg(error.httpStatus)
                                           .arg(error.message.left(160)));
            flashStatus(QStringLiteral("Could not leave the server (%1).").arg(error.message.left(120)), 6000);
        });
}

void MainWindow::guildGone(const QString &guildId)
{
    if (!m_store->forgetGuild(guildId))
        return;
    const bool wasOpen = guildId == m_currentGuildId;
    m_rebuildingRail = true;
    populateGuildRail();
    m_rebuildingRail = false;
    if (wasOpen && m_guildRail->count() > 0)
        m_guildRail->setCurrentRow(0);   // back to direct messages
}

void MainWindow::refreshUnreadMarks()
{
    if (!m_guildRail || !m_channelList || !m_store)
        return;

    refreshDmTiles();

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

    QString content = composerPayload();
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
            if (CaptchaDialog::isDemand(error.body)) {
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
    // Files dragged onto the message box or the conversation attach instead of
    // being typed in as a path.
    if ((m_composer && watched == m_composer->viewport())
        || (m_messageView && watched == m_messageView->viewport())) {
        const QEvent::Type type = event->type();
        if ((type == QEvent::DragEnter || type == QEvent::DragMove || type == QEvent::Drop)
            && handleFileDrop(event)) {
            return true;
        }
    }

    // Ctrl+V on a file copied in Explorer attaches it, the same as a drop.
    if (m_composer && watched == m_composer && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent *>(event)->matches(QKeySequence::Paste)) {
        const QMimeData *mime = QApplication::clipboard()->mimeData();
        QStringList files;
        if (mime && mime->hasUrls()) {
            for (const QUrl &url : mime->urls()) {
                if (url.isLocalFile())
                    files.append(url.toLocalFile());
            }
        }
        if (!files.isEmpty()) {
            addAttachments(files);
            return true;
        }
    }

    // A person under a voice channel can be dragged onto another call.
    if (m_channelList && (watched == m_channelList->viewport() || watched == m_channelList)) {
        const bool onViewport = watched == m_channelList->viewport();
        if (onViewport && event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                QListWidgetItem *item = m_channelList->itemAt(mouse->position().toPoint());
                if (item && item->data(KindRole).toString() == QLatin1String("voicemember")) {
                    m_voiceDragUser = item->data(IdRole).toString();
                    m_voiceDragOrigin = mouse->position().toPoint();
                } else {
                    m_voiceDragUser.clear();
                }
            }
        } else if (onViewport && event->type() == QEvent::MouseMove && !m_voiceDragUser.isEmpty()) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if ((mouse->buttons() & Qt::LeftButton)
                && (mouse->position().toPoint() - m_voiceDragOrigin).manhattanLength()
                       >= QApplication::startDragDistance()) {
                const QString userId = m_voiceDragUser;
                m_voiceDragUser.clear();
                startVoiceMemberDrag(userId);
                return true;
            }
        } else if (onViewport && event->type() == QEvent::MouseButtonRelease) {
            m_voiceDragUser.clear();
        } else if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove
                   || event->type() == QEvent::Drop || event->type() == QEvent::DragLeave) {
            if (handleVoiceMemberDrag(event, watched))
                return true;
        }
    }

    // A server dragged onto the middle of another tile makes or fills a
    // folder. Anything else is an ordinary reorder, left to the list.
    if (m_guildRail && watched == m_guildRail->viewport()
        && (event->type() == QEvent::DragMove || event->type() == QEvent::Drop
            || event->type() == QEvent::DragLeave)) {
        if (handleRailDrag(event))
            return true;
    }

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
    if (watched == m_voiceChannelLabel && event->type() == QEvent::MouseButtonRelease) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            goToConnectedVoice();
            return true;
        }
    }

    if (watched == m_userPanel && event->type() == QEvent::MouseButtonRelease) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton && !m_selfUserId.isEmpty()) {
            showStatusMenu();
            return true;
        }
    }

    if (watched == m_composer && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (m_mentionPopup && m_mentionPopup->isVisible()) {
            const int key = keyEvent->key();
            if (key == Qt::Key_Down) {
                const int row = qMin(m_mentionPopup->currentRow() + 1, m_mentionPopup->count() - 1);
                m_mentionPopup->setCurrentRow(row);
                return true;
            }
            if (key == Qt::Key_Up) {
                m_mentionPopup->setCurrentRow(qMax(0, m_mentionPopup->currentRow() - 1));
                return true;
            }
            if (key == Qt::Key_Escape) {
                hideMentionPopup();
                return true;
            }
            if (key == Qt::Key_Tab
                || ((key == Qt::Key_Return || key == Qt::Key_Enter)
                    && !(keyEvent->modifiers() & Qt::ShiftModifier))) {
                insertPickedMention();
                return true;
            }
        }
        if (keyEvent->key() == Qt::Key_Escape
            && (!m_editingMessageId.isEmpty() || !m_replyMessageId.isEmpty())) {
            stopEditing();
            return true;
        }
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

    // Our own share is shown from our own capture, never watched through
    // Discord. Watching it sent our video out and straight back to us, and
    // leaving it afterwards sent STREAM_DELETE for our own key - which Discord
    // reads as "stop broadcasting". A share ended the moment someone clicked
    // another person's tile (19:35:09 in the 0.6.84 log: watch AZARYA, and 80
    // ms later "Discord ended our stream").
    //
    // Checked before anything is stopped. Looking at your own share is only
    // a change of which tile is big; it used to end the stream you were
    // watching as well (11:00:29 and 11:02:50 in the 2026-09-25 log).
    if (userId == m_selfUserId) {
        if (m_callView)
            m_callView->setFocusedUser(userId, CallView::Surface::Share);
        return;
    }

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

    // STREAM_DELETE on our own key ends our broadcast, not our viewing.
    if (m_streamKey != m_myStreamKey)
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
    if (!m_backdrop)
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

    m_backdrop->setBackground(usable ? path : QString(), dim);
    m_paceHole = !usable;
    updateFramePace();

    // Clear over a picture and over the hole. The hole used to turn this off,
    // which made the panels solid — a see-through widget above that OpenGL
    // surface painted black, because Qt had made every sibling its own window.
    // That promotion is off (see main.cpp), so the hole shows through the same
    // panels a picture does. Flipping the flag also restyles every widget, which
    // is the stall on the way out of Settings; it is set once and left on.
    if (!property("glass").toBool()) {
        setProperty("glass", true);
        qApp->setStyleSheet(Theme::applicationStyleSheet());
    }
}

void MainWindow::updateFramePace()
{
    // Off behind the black hole. That background is an OpenGL widget, and Qt
    // repaints every widget in the window when the OpenGL widget and anything
    // else change in the same redraw - gathering redraws together there made
    // things worse, not better (scratchpad pacetest: 12 % of a core became 24).
    // Over a picture: thirty a second, sixty while a stream is the big tile.
    const int ms = m_paceHole ? 0 : m_paceStream ? 16 : 33;
    if (SingularityApplication::framePace() == ms)
        return;
    SingularityApplication::setFramePace(ms);
    wlog(QStringLiteral("perf"), QStringLiteral("redraw pace %1 ms (%2)")
                                     .arg(ms)
                                     .arg(m_paceHole     ? QStringLiteral("off: black hole background")
                                          : m_paceStream ? QStringLiteral("watching a stream")
                                                         : QStringLiteral("normal")));
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
    m_shareSoundSource = int(dialog.soundSource());
    m_shareSoundPid = dialog.soundProcessId();
    m_shareSoundName = dialog.soundName();

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

// The official client's stream preview, copied: fitted inside 512 x 288, a
// JPEG, and never a black picture - it waits for one that has something in
// it (a game still loading, a window not drawn yet).
void MainWindow::uploadStreamPreview()
{
    const QString key = m_myStreamKey;
    if (key.isEmpty())
        return;

    const auto hasSomething = [](const QImage &image) {
        if (image.isNull())
            return false;
        // Not called "small": windows.h defines that as a macro.
        const QImage tiny = image.scaled(32, 18, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                                .convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < tiny.height(); ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(tiny.constScanLine(y));
            for (int x = 0; x < tiny.width(); ++x) {
                if (qRed(row[x]) + qGreen(row[x]) + qBlue(row[x]) > 24)
                    return true;
            }
        }
        return false;
    };

    if (!hasSomething(m_lastSharePicture)) {
        // Discord gives up after sixty frames; this gives it a minute.
        if (++m_streamPreviewTries < 60)
            m_streamPreviewTimer.start(1000);
        else
            m_streamPreviewTimer.start(5 * 60 * 1000);
        return;
    }

    const QImage fitted = (m_lastSharePicture.width() <= 512 && m_lastSharePicture.height() <= 288)
                              ? m_lastSharePicture
                              : m_lastSharePicture.scaled(512, 288, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    fitted.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPEG", 92);

    m_rest->uploadStreamPreview(
        key, jpeg,
        [this, key, size = jpeg.size(), w = fitted.width(), h = fitted.height()](const QJsonObject &) {
            wlog(QStringLiteral("share"),
                 QStringLiteral("stream preview sent: %1x%2, %3 KB").arg(w).arg(h).arg(size / 1024));
            if (m_myStreamKey == key)
                m_streamPreviewTimer.start(5 * 60 * 1000);
        },
        [this, key](const RestClient::Error &error) {
            wlog(QStringLiteral("share"), QStringLiteral("stream preview refused: HTTP %1 %2")
                                              .arg(error.httpStatus)
                                              .arg(error.message.left(160)));
            if (m_myStreamKey == key)
                m_streamPreviewTimer.start(60 * 1000);
        });
}

void MainWindow::stopScreenShare()
{
    if (m_myStreamKey.isEmpty())
        return;

    if (m_share)
        m_share->stop();
    if (m_shareAudio)
        m_shareAudio->stop();
    if (m_callView && !m_selfUserId.isEmpty())
        m_callView->dropFrames(m_selfUserId, CallView::Surface::Share);
    if (m_shareVoice) {
        m_shareVoice->stopSendingVideo();
        m_shareVoice->disconnectFromVoice();
    }

    m_gateway->stopStream(m_myStreamKey);
    m_streamPreviewTimer.stop();
    m_lastSharePicture = QImage();

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

void MainWindow::requestUnknownName(const QString &userId, const QString &guildId)
{
    if (userId.isEmpty() || m_namesRequested.contains(userId) || !m_rest)
        return;
    m_namesRequested.insert(userId);

    // Inside a server: gather everyone unknown for a moment, then ask the
    // gateway about all of them at once, the way Discord's client does.
    if (!guildId.isEmpty()) {
        m_pendingMemberLookups[guildId].insert(userId);
        if (!m_memberLookupTimer.isActive())
            m_memberLookupTimer.start();
        return;
    }

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
        [this, userId](const RestClient::Error &error) {
            wlog(QStringLiteral("ui"), QStringLiteral("could not look up user %1: HTTP %2")
                                           .arg(userId)
                                           .arg(error.httpStatus));
            // 429 means "not now", not "no such person". Being marked as
            // already asked kept a rate-limited person a number for good, so
            // they become askable again once Discord's wait is over.
            if (error.httpStatus == 429) {
                const double wait = error.body.value(QStringLiteral("retry_after")).toDouble();
                const int ms = qBound(5000, int(wait * 1000) + 500, 120000);
                QTimer::singleShot(ms, this, [this, userId]() { m_namesRequested.remove(userId); });
            }
            // Anything else - a deleted account and the like - keeps the
            // number, which is honest.
        });
}

void MainWindow::flushMemberLookups()
{
    for (auto it = m_pendingMemberLookups.cbegin(); it != m_pendingMemberLookups.cend(); ++it)
        m_gateway->requestGuildMembers(it.key(), it.value().values());
    m_pendingMemberLookups.clear();
}

bool MainWindow::voiceChannelFull(const QString &channelId) const
{
    const ChannelInfo channel = m_store->channel(channelId);
    if (channel.userLimit <= 0 || channelId == m_voiceChannelId)
        return false;
    return m_store->voiceMembers(channelId).size() >= channel.userLimit;
}

void MainWindow::goToConnectedVoice()
{
    if (m_voiceChannelId.isEmpty())
        return;
    // The call stays connected. This only opens the channel it belongs to,
    // which is what clicking the name under "Voice connected" does in Discord.
    selectChannelEverywhere(m_voiceChannelId);
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

void MainWindow::applyCameraDevice()
{
    if (!m_cameraSession)
        return;

    // Which camera did the settings pick? An empty id means "the default one".
    const QByteArray wanted =
        AppConfig::instance().value(QStringLiteral("voice/cameraDevice")).toByteArray();

    QCameraDevice device = QMediaDevices::defaultVideoInput();
    if (!wanted.isEmpty()) {
        const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
        for (const QCameraDevice &camera : cameras) {
            if (camera.id() == wanted) {
                device = camera;
                break;
            }
        }
    }

    // No camera on the machine. toggleCamera() already tells the user.
    if (device.isNull())
        return;

    // Already using this camera. Nothing to swap.
    if (m_webcam && m_webcam->cameraDevice().id() == device.id())
        return;

    const bool wasOn = m_webcam && m_webcam->isActive();
    if (m_webcam) {
        m_webcam->stop();
        m_webcam->deleteLater();
        m_webcam = nullptr;
    }

    m_webcam = new QCamera(device, this);
    m_cameraSession->setCamera(m_webcam);
    wlog(QStringLiteral("camera"), QStringLiteral("using camera %1").arg(device.description()));

    // If the camera was live when the pick changed, keep it live on the new one.
    if (wasOn) {
        m_webcam->start();
        if (m_camera && m_camera->isRunning())
            m_camera->requestKeyframe();
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

    // Remembered briefly, to recognise Discord's late echo of it.
    m_leftVoiceChannelId = m_voiceChannelId;
    m_leftVoiceAt.start();

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
    // Status changed on another device. Follow it, the way the official
    // client does, without writing it back. The first sign-in handles its
    // own copy in onGatewayReady.
    if (partial) {
        const QString status = statusFromProto(proto);
        if (!status.isEmpty() && status != m_gateway->presenceStatus())
            setPresenceStatus(status, /*storeOnDiscord*/ false);

        // Folders rearranged on another device, or the echo of our own save.
        applyDiscordFolders(proto, true);
    }

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

// Remembered across calls and restarts, as a list in the settings file.
void MainWindow::setVideoHidden(const QString &userId, bool hidden)
{
    QStringList list = AppConfig::instance().value(QStringLiteral("voice/videoHidden")).toStringList();
    list.removeAll(userId);
    if (hidden)
        list.append(userId);
    AppConfig::instance().setValue(QStringLiteral("voice/videoHidden"), list);
    const QSet<QString> set(list.begin(), list.end());
    if (m_callView)
        m_callView->setVideoHidden(set);
    if (m_voice)
        m_voice->setVideoOff(set);
    wlog(QStringLiteral("video"), QStringLiteral("%1 camera for %2").arg(hidden ? QStringLiteral("turned off")
                                                                               : QStringLiteral("turned on"), userId));
}

// The stream's picture only: the stream connection asks for none of it, and
// its sound keeps playing. Remembered like the camera one.
void MainWindow::setStreamVideoHidden(const QString &userId, bool hidden)
{
    QStringList list = AppConfig::instance().value(QStringLiteral("voice/streamVideoHidden")).toStringList();
    list.removeAll(userId);
    if (hidden)
        list.append(userId);
    AppConfig::instance().setValue(QStringLiteral("voice/streamVideoHidden"), list);
    const QSet<QString> set(list.begin(), list.end());
    if (m_callView)
        m_callView->setStreamHidden(set);
    if (m_streamVoice)
        m_streamVoice->setVideoOff(set);
    wlog(QStringLiteral("video"), QStringLiteral("%1 stream video for %2").arg(hidden ? QStringLiteral("turned off")
                                                                                     : QStringLiteral("turned on"), userId));
}

QString MainWindow::voiceGuildOf(const QString &userId) const
{
    if (!m_store || userId.isEmpty())
        return {};
    const VoiceStateInfo state = m_store->voiceState(userId);
    if (!state.guildId.isEmpty())
        return state.guildId;
    const QString fromChannel = m_store->channel(state.channelId).guildId;
    if (!fromChannel.isEmpty())
        return fromChannel;
    // A voice state sometimes arrives without a guild. The server on screen
    // is the one the move is for.
    return m_currentGuildId;
}

bool MainWindow::canMoveVoiceMember(const QString &userId) const
{
    if (userId.isEmpty())
        return false;
    if (userId == m_selfUserId)
        return true;
    constexpr quint64 MoveMembers = 1ull << 24;
    const QString guildId = voiceGuildOf(userId);
    if (guildId.isEmpty() || !m_store)
        return false;
    return (m_store->selfPermissions(guildId) & MoveMembers) != 0;
}

void MainWindow::patchVoiceMember(const QString &userId, const QJsonObject &fields, const QString &failed)
{
    if (!m_rest || userId.isEmpty())
        return;
    const QString guildId = voiceGuildOf(userId);
    if (guildId.isEmpty()) {
        wlog(QStringLiteral("voice"), QStringLiteral("voice moderation had no server for %1").arg(userId));
        flashStatus(failed, 5000);
        return;
    }

    wlog(QStringLiteral("voice"),
         QStringLiteral("asking Discord to change %1 in %2: %3")
             .arg(userId, guildId, QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact))));

    m_rest->modifyGuildMember(
        guildId, userId, fields,
        [this, userId, guildId, fields](const QJsonObject &) {
            if (!fields.contains(QStringLiteral("channel_id")) || !m_store)
                return;
            const VoiceStateInfo previous = m_store->voiceState(userId);
            QJsonObject state{{QStringLiteral("user_id"), userId},
                              {QStringLiteral("guild_id"), guildId},
                              {QStringLiteral("channel_id"), fields.value(QStringLiteral("channel_id"))},
                              {QStringLiteral("self_mute"), previous.muted && !previous.serverMuted},
                              {QStringLiteral("self_deaf"), previous.deafened && !previous.serverDeafened},
                              {QStringLiteral("mute"), previous.serverMuted},
                              {QStringLiteral("deaf"), previous.serverDeafened},
                              {QStringLiteral("self_stream"), previous.streaming},
                              {QStringLiteral("self_video"), previous.video}};
            m_store->setVoiceState(state);
            wlog(QStringLiteral("voice"), QStringLiteral("Discord accepted the move for %1").arg(userId));
        },
        [this, failed](const RestClient::Error &error) {
            wlog(QStringLiteral("ui"),
                 QStringLiteral("voice moderation failed: HTTP %1 %2")
                     .arg(error.httpStatus)
                     .arg(error.message));
            flashStatus(error.httpStatus == 403
                            ? QStringLiteral("Discord refused that. Their role may be above yours.")
                            : failed,
                        5000);
        });
}

void MainWindow::moveVoiceMember(const QString &userId, const QString &channelId)
{
    if (userId.isEmpty() || !m_store)
        return;
    if (m_store->voiceState(userId).channelId == channelId)
        return;

    if (userId == m_selfUserId) {
        if (channelId.isEmpty())
            leaveVoice();
        else
            joinVoice(channelId);
        return;
    }

    QJsonObject fields;
    if (channelId.isEmpty())
        fields.insert(QStringLiteral("channel_id"), QJsonValue());
    else
        fields.insert(QStringLiteral("channel_id"), channelId);
    patchVoiceMember(userId, fields,
                     channelId.isEmpty() ? QStringLiteral("Could not disconnect them.")
                                         : QStringLiteral("Could not move them."));
}

QString MainWindow::voiceChannelAt(const QPoint &viewportPos) const
{
    if (!m_channelList)
        return {};

    const auto fromItem = [this](QListWidgetItem *item) -> QString {
        if (!item)
            return {};
        for (int i = m_channelList->row(item); i >= 0; --i) {
            QListWidgetItem *it = m_channelList->item(i);
            const QString kind = it->data(KindRole).toString();
            if (kind == QLatin1String("voice"))
                return it->data(IdRole).toString();
            if (kind != QLatin1String("voicemember"))
                return {};
        }
        return {};
    };

    if (QListWidgetItem *exact = m_channelList->itemAt(viewportPos))
        return fromItem(exact);

    // A drop a few pixels off the row used to miss, and the move never left
    // the machine. Only a voice row counts; a text channel stays a miss.
    const QPoint shifts[] = {QPoint(0, -8), QPoint(0, 8), QPoint(0, -16), QPoint(0, 16)};
    for (const QPoint &shift : shifts) {
        QListWidgetItem *item = m_channelList->itemAt(viewportPos + shift);
        if (!item)
            continue;
        const QString kind = item->data(KindRole).toString();
        if (kind == QLatin1String("voice") || kind == QLatin1String("voicemember"))
            return fromItem(item);
    }
    return {};
}

void MainWindow::startVoiceMemberDrag(const QString &userId)
{
    if (!m_channelList || !canMoveVoiceMember(userId))
        return;

    m_voiceDragMoved = true;
    auto *mime = new QMimeData;
    mime->setData("application/x-singularity-voicemember", userId.toUtf8());

    QDrag drag(m_channelList);
    drag.setMimeData(mime);
    for (int i = 0; i < m_channelList->count(); ++i) {
        QListWidgetItem *item = m_channelList->item(i);
        if (item->data(KindRole).toString() == QLatin1String("voicemember")
            && item->data(IdRole).toString() == userId) {
            const QPixmap face = item->icon().pixmap(18, 18);
            if (!face.isNull()) {
                drag.setPixmap(face);
                drag.setHotSpot(QPoint(9, 9));
            }
            break;
        }
    }
    drag.exec(Qt::MoveAction);
    if (m_channelDelegate)
        m_channelDelegate->setDropTarget({});
}

bool MainWindow::handleVoiceMemberDrag(QEvent *event, QObject *watched)
{
    if (!m_channelList || !m_channelDelegate)
        return false;

    if (event->type() == QEvent::DragLeave) {
        m_channelDelegate->setDropTarget({});
        return false;
    }

    auto *drop = static_cast<QDropEvent *>(event);
    if (!drop->mimeData() || !drop->mimeData()->hasFormat("application/x-singularity-voicemember"))
        return false;

    const QString userId = QString::fromUtf8(drop->mimeData()->data("application/x-singularity-voicemember"));
    QPoint viewportPos = drop->position().toPoint();
    if (auto *widget = qobject_cast<QWidget *>(watched)) {
        if (widget != m_channelList->viewport())
            viewportPos = m_channelList->viewport()->mapFrom(widget, viewportPos);
    }
    const QString channelId = voiceChannelAt(viewportPos);
    const bool same = m_store && m_store->voiceState(userId).channelId == channelId;
    const bool ok = !channelId.isEmpty() && !same;

    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        m_channelDelegate->setDropTarget(ok ? channelId : QString());
        if (!ok) {
            drop->ignore();
            return true;
        }
        drop->setDropAction(Qt::MoveAction);
        drop->accept();
        return true;
    }

    if (event->type() != QEvent::Drop)
        return false;

    m_channelDelegate->setDropTarget({});
    drop->setDropAction(Qt::IgnoreAction);
    drop->accept();
    if (ok)
        QTimer::singleShot(0, this, [this, userId, channelId]() { moveVoiceMember(userId, channelId); });
    else
        wlog(QStringLiteral("voice"),
             QStringLiteral("a drop of %1 did not land on another call").arg(userId));
    return true;
}

void MainWindow::showPersonMenu(const QString &userId, const QPoint &globalPos)
{
    showPersonMenuAt(userId, globalPos, QString());
}

void MainWindow::showPersonMenuAt(const QString &userId, const QPoint &globalPos,
                                  const QString &closeChannelId)
{
    if (userId.isEmpty() && closeChannelId.isEmpty())
        return;

    QMenu menu(this);
    const bool self = !userId.isEmpty() && userId == m_selfUserId;

    if (!userId.isEmpty()) {
        menu.addAction(QStringLiteral("Profile"), this, [this, userId, globalPos]() {
            showProfile(userId, globalPos);
        });

        if (!self) {
            menu.addAction(QStringLiteral("Message"), this, [this, userId]() { openDirectWith(userId); });
            menu.addSeparator();
            menu.addAction(QStringLiteral("User volume"), this, [this, userId, globalPos]() {
                showUserVolumeMenu(userId, globalPos);
            });

            // Discord's call options for this person: their camera and their
            // stream, each turned off here only. They are not told.
            //
            // As in Discord, video is turned off per connection: the call's
            // (their camera) and the stream's (their shared screen, whose
            // sound keeps playing). Stop Watching is separate: it leaves the
            // stream altogether.
            const VoiceStateInfo state = m_store->voiceState(userId);
            if ((state.video || state.streaming) && m_callView)
                menu.addSeparator();
            if (state.video && m_callView) {
                const bool hidden = m_callView->isVideoHidden(userId);
                menu.addAction(hidden ? QStringLiteral("Turn On Camera") : QStringLiteral("Turn Off Camera"), this,
                               [this, userId, hidden]() { setVideoHidden(userId, !hidden); });
            }
            if (state.streaming && m_callView) {
                const bool hidden = m_callView->isStreamHidden(userId);
                menu.addAction(hidden ? QStringLiteral("Turn On Stream Video") : QStringLiteral("Turn Off Stream Video"),
                               this, [this, userId, hidden]() { setStreamVideoHidden(userId, !hidden); });
                if (m_watchingUserId == userId)
                    menu.addAction(QStringLiteral("Stop Watching Stream"), this, [this]() { stopWatchingStream(); });
                else
                    menu.addAction(QStringLiteral("Watch Stream"), this, [this, userId]() { watchStream(userId); });
            }
        }

        menu.addSeparator();
    }

    if (!closeChannelId.isEmpty()) {
        menu.addAction(QStringLiteral("Close DM"), this, [this, closeChannelId]() {
            closeDirectMessage(closeChannelId);
        });
        if (!userId.isEmpty())
            menu.addSeparator();
    }

    if (!userId.isEmpty()) {
        const QString guildId = voiceGuildOf(userId);
        const VoiceStateInfo voice = m_store->voiceState(userId);
        if (!guildId.isEmpty() && !voice.channelId.isEmpty()) {
            constexpr quint64 MuteMembers = 1ull << 22;
            constexpr quint64 DeafenMembers = 1ull << 23;
            constexpr quint64 MoveMembers = 1ull << 24;
            const quint64 bits = m_store->selfPermissions(guildId);
            const bool canMute = (bits & MuteMembers) != 0;
            const bool canDeaf = (bits & DeafenMembers) != 0;
            const bool canMove = self || (bits & MoveMembers) != 0;
            if (canMute || canDeaf || canMove)
                menu.addSeparator();
            if (canMute) {
                const bool on = voice.serverMuted || voice.serverDeafened;
                menu.addAction(on ? QStringLiteral("Server Unmute") : QStringLiteral("Server Mute"), this,
                               [this, userId, on, canDeaf]() {
                                   QJsonObject fields{{QStringLiteral("mute"), !on}};
                                   if (on && canDeaf)
                                       fields.insert(QStringLiteral("deaf"), false);
                                   patchVoiceMember(userId, fields, QStringLiteral("Could not change their mute."));
                               });
            }
            if (canDeaf) {
                const bool on = voice.serverDeafened;
                menu.addAction(on ? QStringLiteral("Server Undeafen") : QStringLiteral("Server Deafen"), this,
                               [this, userId, on, canMute]() {
                                   QJsonObject fields{{QStringLiteral("deaf"), !on}};
                                   if (canMute)
                                       fields.insert(QStringLiteral("mute"), !on ? false : true);
                                   patchVoiceMember(userId, fields,
                                                    QStringLiteral("Could not change their deafen."));
                               });
            }
            if (canMove) {
                auto *moveTo = menu.addMenu(QStringLiteral("Move to"));
                int added = 0;
                for (const ChannelGroup &group : m_store->groupedChannels(guildId)) {
                    QList<ChannelInfo> voices;
                    for (const ChannelInfo &channel : group.channels) {
                        if (channel.isVoice() && channel.id != voice.channelId)
                            voices.append(channel);
                    }
                    if (voices.isEmpty())
                        continue;
                    if (!group.name.isEmpty()) {
                        QAction *header = moveTo->addAction(group.name);
                        header->setEnabled(false);
                    }
                    for (const ChannelInfo &channel : voices) {
                        moveTo->addAction(channel.name, this, [this, userId, id = channel.id]() {
                            moveVoiceMember(userId, id);
                        });
                        ++added;
                    }
                }
                if (added == 0)
                    moveTo->setEnabled(false);
                menu.addAction(QStringLiteral("Disconnect"), this,
                               [this, userId]() { moveVoiceMember(userId, QString()); });
            }
        }

        menu.addAction(QStringLiteral("Copy User ID"), this, [userId]() {
            QApplication::clipboard()->setText(userId);
        });
    }

    menu.exec(globalPos);
}

void MainWindow::closeDirectMessage(const QString &channelId)
{
    if (!m_rest || channelId.isEmpty())
        return;

    m_rest->closeDirectChannel(
        channelId,
        [this](const QJsonObject &channel) {
            applyChannelClosed(channel.value(QStringLiteral("id")).toString());
        },
        [](const RestClient::Error &error) {
            wlog(QStringLiteral("ui"),
                 QStringLiteral("could not close the direct message: HTTP %1 %2")
                     .arg(error.httpStatus)
                     .arg(error.message));
        });
}

void MainWindow::applyChannelClosed(const QString &channelId)
{
    if (channelId.isEmpty())
        return;

    const ChannelInfo info = m_store->channel(channelId);
    if (info.id.isEmpty())
        return;

    const bool direct = info.isDirect();
    const QString guildId = info.guildId;
    const bool viewing = m_currentChannelId == channelId;
    if (!m_store->forgetChannel(channelId))
        return;

    const int scroll = m_channelList->verticalScrollBar()->value();
    const QString keep = viewing ? QString() : m_currentChannelId;

    if (m_currentGuildId.isEmpty() || (!guildId.isEmpty() && m_currentGuildId == guildId))
        populateChannelList(false);

    if (viewing && direct) {
        for (int row = 0; row < m_channelList->count(); ++row) {
            if (m_channelList->item(row)->data(IdRole).toString() == QLatin1String("singularity:friends")) {
                m_channelList->setCurrentRow(row);
                return;
            }
        }
        return;
    }

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
    // Any captcha_key with a site key is a check to show, whatever the words
    // in it. Discord's own client does exactly that (its HTTP interceptor
    // opens hCaptcha for every 400 carrying captcha_key). The words vary: a
    // join came back with "You need to update your app to join this server.",
    // which is the text meant for clients that cannot show a captcha, and an
    // earlier version here waited for "captcha-required" and gave up.
    if (!CaptchaDialog::isDemand(error.body))
        return false;
    const QJsonArray keys = error.body.value(QStringLiteral("captcha_key")).toArray();
    wlog(QStringLiteral("ui"), QStringLiteral("Discord asks for a captcha (%1 via %2)")
                                   .arg(keys.first().toString(),
                                        error.body.value(QStringLiteral("captcha_service")).toString()));

    const QString token = CaptchaDialog::solve(
        this, error.body.value(QStringLiteral("captcha_sitekey")).toString(),
        error.body.value(QStringLiteral("captcha_rqdata")).toString());
    if (token.isEmpty()) {
        wlog(QStringLiteral("ui"), QStringLiteral("Discord asked for a check, and it was not finished"));
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

    // Follow a new camera pick, even mid-call.
    applyCameraDevice();
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
        m_voicePanel->setFixedHeight(watching ? 250 : 196);

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

// ---------------------------------------------------------------------------
// Desktop notifications
// ---------------------------------------------------------------------------

void MainWindow::showDesktopNotification(const QString &title, const QString &text, const QString &channelId)
{
    // A desktop program raises one through a tray icon, so the icon is made
    // the first time one is needed and then stays, like Discord's. Clicking
    // the notification opens the chat it was about.
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;
    if (!m_tray) {
        m_tray = new QSystemTrayIcon(windowIcon(), this);
        m_tray->setToolTip(QStringLiteral("Singularity"));
        const auto bringForward = [this]() {
            if (isMinimized())
                showNormal();
            show();
            raise();
            activateWindow();
        };
        connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this, bringForward]() {
            bringForward();
            if (!m_notifyChannelId.isEmpty())
                selectChannelEverywhere(m_notifyChannelId);
        });
        connect(m_tray, &QSystemTrayIcon::activated, this,
                [bringForward](QSystemTrayIcon::ActivationReason) { bringForward(); });
        m_tray->show();
    }
    m_notifyChannelId = channelId;
    m_tray->showMessage(title, text, windowIcon(), 8000);
}

void MainWindow::maybeNotify(const QJsonObject &data, const MessageInfo &message)
{
    AppConfig &config = AppConfig::instance();
    if (!config.value(QStringLiteral("notifications/desktop"), true).toBool())
        return;

    // Discord shows these only while it is not the window in front. Reading
    // the chat already tells you, and a pop-up on top of it would be noise.
    if (QApplication::activeWindow() != nullptr)
        return;

    // Do Not Disturb silences Discord's own notifications. This account sits
    // on it most of the time, so here it is a choice, on by default.
    if (m_gateway && m_gateway->presenceStatus() == QLatin1String("dnd")
        && !config.value(QStringLiteral("notifications/duringDnd"), true).toBool())
        return;

    if (m_store->user(message.authorId).isBlocked())
        return;

    const QString channelId = data.value(QStringLiteral("channel_id")).toString();
    const ChannelInfo channel = m_store->channel(channelId);
    const NotificationRules::Verdict verdict =
        m_notifyRules.judge(data, m_selfUserId, channel.parentId, QDateTime::currentMSecsSinceEpoch());
    if (!verdict.notify)
        return;

    // Who and where, as Discord words it.
    const QString author = message.authorName.isEmpty() ? QStringLiteral("Someone") : message.authorName;
    QString title = author;
    if (!channel.guildId.isEmpty()) {
        const GuildInfo guild = m_store->guild(channel.guildId);
        title = QStringLiteral("%1 (#%2, %3)").arg(author, channel.name, guild.name);
    } else if (channel.type == 3 && !channel.name.isEmpty()) {
        title = QStringLiteral("%1 (%2)").arg(author, channel.name);
    }

    // The words, with <@id> and friends turned back into names.
    QString text;
    if (config.value(QStringLiteral("notifications/showText"), true).toBool()) {
        text = message.content;
        static const QRegularExpression user(QStringLiteral(R"(<@!?(\d+)>)"));
        static const QRegularExpression role(QStringLiteral(R"(<@&(\d+)>)"));
        static const QRegularExpression room(QStringLiteral(R"(<#(\d+)>)"));
        static const QRegularExpression emoji(QStringLiteral(R"(<a?:(\w+):\d+>)"));
        for (auto it = user.globalMatch(message.content); it.hasNext();) {
            const auto match = it.next();
            const QString name = m_store->user(match.captured(1)).displayName();
            text.replace(match.captured(0), QStringLiteral("@") + (name.isEmpty() ? QStringLiteral("someone") : name));
        }
        const GuildInfo guild = m_store->guild(channel.guildId);
        for (auto it = role.globalMatch(message.content); it.hasNext();) {
            const auto match = it.next();
            const QString name = guild.roles.value(match.captured(1)).name;
            text.replace(match.captured(0), QStringLiteral("@") + (name.isEmpty() ? QStringLiteral("role") : name));
        }
        for (auto it = room.globalMatch(message.content); it.hasNext();) {
            const auto match = it.next();
            text.replace(match.captured(0), QStringLiteral("#") + m_store->channel(match.captured(1)).name);
        }
        text.replace(emoji, QStringLiteral(":\\1:"));
        text = text.simplified();
        if (text.size() > 220)
            text = text.left(217) + QStringLiteral("…");
    }
    if (text.isEmpty()) {
        if (!message.attachments.isEmpty())
            text = QStringLiteral("Sent an attachment.");
        else if (!message.stickers.isEmpty())
            text = QStringLiteral("Sent a sticker.");
        else if (!message.embeds.isEmpty())
            text = QStringLiteral("Sent a link.");
        else
            text = QStringLiteral("New message.");
    }

    wlog(QStringLiteral("notify"), QStringLiteral("message from %1 in %2: %3")
                                       .arg(author, channelId, verdict.reason));
    showDesktopNotification(title, text, channelId);

    // The taskbar button flashes until the window is opened, as Discord's does.
    QApplication::alert(this);
}

// ---------------------------------------------------------------------------
// Invites (the link helpers are with handleAnchor)
// ---------------------------------------------------------------------------

QString MainWindow::inviteCardsHtml(const MessageInfo &message)
{
    const QStringList codes = inviteCodesIn(message.content);
    if (codes.isEmpty())
        return {};

    const QLocale locale;
    QString html;
    for (const QString &code : codes) {
        const InviteCard card = m_invites.value(code);
        if (!card.loaded) {
            m_inviteWaiters[code].insert(message.id);
            requestInvite(code);
        }

        QString inner;
        const QString label = QStringLiteral("<div style=\"color:%1; font-size:11px; font-weight:bold;\">%2</div>")
                                  .arg(QLatin1String(Theme::TextMuted));

        if (!card.loaded) {
            inner = label.arg(QStringLiteral("LOOKING UP THE INVITE…"));
        } else if (!card.valid) {
            inner = label.arg(QStringLiteral("YOU RECEIVED AN INVITE, BUT…"))
                    + QStringLiteral("<div style=\"font-weight:bold; margin-top:6px;\">Invalid invite</div>"
                                     "<div style=\"color:%1;\">It has expired, or it was taken down.</div>")
                          .arg(QLatin1String(Theme::TextMuted));
        } else {
            const bool isServer = !card.guildId.isEmpty();
            const bool joined = isServer ? !m_store->guild(card.guildId).id.isEmpty()
                                         : !m_store->channel(card.channelId).id.isEmpty();

            const QUrl iconUrl = isServer ? MediaCache::guildIconUrl(card.guildId, card.iconHash, 96) : QUrl();
            const QString icon = iconUrl.isEmpty()
                ? QStringLiteral("<table cellspacing=\"0\" cellpadding=\"0\"><tr><td width=\"48\" height=\"48\" "
                                 "align=\"center\" bgcolor=\"%1\" style=\"font-weight:bold;\">%2</td></tr></table>")
                      .arg(QLatin1String(Theme::SurfaceHover), card.guildName.left(1).toHtmlEscaped())
                : QStringLiteral("<img src=\"%1\" width=\"48\" height=\"48\">").arg(iconUrl.toString().toHtmlEscaped());

            QString counts;
            if (card.online >= 0) {
                counts += QStringLiteral("<span style=\"color:%1;\">&#9679;</span> %2 Online&#160;&#160; ")
                              .arg(QLatin1String(Theme::Green), locale.toString(card.online));
            }
            if (card.members >= 0) {
                counts += QStringLiteral("<span style=\"color:%1;\">&#9679;</span> %2 Members")
                              .arg(QLatin1String(Theme::TextMuted), locale.toString(card.members));
            }

            // Joined goes to the server. Join joins it; the message id tells
            // Discord which card was pressed, as the real client does.
            //
            // The coloured box is a table of its own inside a middle-aligned
            // cell. Coloured directly, the cell took the whole row's height
            // (the 48 px icon's) with its word stuck at the top.
            const QString buttonBox = QStringLiteral(
                "<td valign=\"middle\"><table cellspacing=\"0\" cellpadding=\"0\"><tr>"
                "<td bgcolor=\"%1\" valign=\"middle\" style=\"padding:8px 16px;\">%2</td>"
                "</tr></table></td>");
            const QString button = joined
                ? buttonBox.arg(QLatin1String(Theme::SurfaceHover),
                                QStringLiteral("<a href=\"singularity-channel:%1\" style=\"color:%2; "
                                               "text-decoration:none;\">Joined</a>")
                                    .arg(card.channelId, QLatin1String(Theme::TextPrimary)))
                : buttonBox.arg(QLatin1String(Theme::Green),
                                QStringLiteral("<a href=\"singularity-invite:%1/%2\" style=\"color:#ffffff; "
                                               "text-decoration:none; font-weight:bold;\">%3</a>")
                                    .arg(code, message.id,
                                         m_invitesJoining.contains(code) ? QStringLiteral("Joining…")
                                                                         : QStringLiteral("Join")));

            inner = label.arg(isServer ? QStringLiteral("YOU'VE BEEN INVITED TO JOIN A SERVER")
                                       : QStringLiteral("YOU'VE BEEN INVITED TO JOIN A GROUP DM"))
                    + QStringLiteral("<table cellspacing=\"0\" cellpadding=\"0\" style=\"margin-top:8px;\"><tr>"
                                     "<td valign=\"middle\">%1</td>"
                                     "<td valign=\"middle\" style=\"padding-left:12px; padding-right:24px;\">"
                                     "<div style=\"font-weight:bold; font-size:15px;\">%2</div>"
                                     "<div style=\"color:%3; font-size:12px;\">%4</div></td>"
                                     "%5</tr></table>")
                          .arg(icon, card.guildName.toHtmlEscaped(), QLatin1String(Theme::TextMuted), counts, button);
        }

        html += QStringLiteral("<table class=\"embed\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                               "<td class=\"embed-inner\" bgcolor=\"%1\" style=\"padding:12px 16px;\">%2</td>"
                               "</tr></table>")
                    .arg(QLatin1String(Theme::SurfaceInput), inner);
    }
    return html;
}

QString MainWindow::activityInviteHtml(const MessageInfo &message) const
{
    if (message.activityApplicationName.isEmpty() && message.activityApplicationId.isEmpty())
        return {};

    const QString name = message.activityApplicationName.isEmpty()
        ? QStringLiteral("Game")
        : message.activityApplicationName;

    bool stillGoing = false;
    const PresenceInfo presence = m_store->presence(message.authorId);
    for (const ActivityInfo &activity : presence.activities) {
        if (activity.isCustomStatus())
            continue;
        if (!message.activityApplicationId.isEmpty() && activity.applicationId == message.activityApplicationId) {
            stillGoing = true;
            break;
        }
        if (!message.activityApplicationName.isEmpty()
            && activity.name.compare(message.activityApplicationName, Qt::CaseInsensitive) == 0) {
            stillGoing = true;
            break;
        }
    }

    QString subtitle = QStringLiteral("Game has ended. Start a new one?");
    if (stillGoing) {
        if (message.activityType == 2)
            subtitle = QStringLiteral("Spectate");
        else if (message.activityType == 3)
            subtitle = QStringLiteral("Listen along");
        else
            subtitle = QStringLiteral("Playing");
    }

    QString icon;
    if (!message.activityApplicationId.isEmpty() && !message.activityApplicationIcon.isEmpty()) {
        const QString url = QStringLiteral("https://cdn.discordapp.com/app-icons/%1/%2.png?size=64")
                                .arg(message.activityApplicationId, message.activityApplicationIcon);
        icon = QStringLiteral("<img src=\"%1\" width=\"48\" height=\"48\">").arg(url.toHtmlEscaped());
    } else {
        icon = QStringLiteral("<table cellspacing=\"0\" cellpadding=\"0\"><tr>"
                              "<td width=\"48\" height=\"48\" align=\"center\" bgcolor=\"%1\" "
                              "style=\"font-weight:bold; color:%2;\">%3</td></tr></table>")
                   .arg(QLatin1String(Theme::SurfaceHover), QLatin1String(Theme::TextPrimary),
                        name.left(3).toHtmlEscaped());
    }

    const QString href = message.activityApplicationId.isEmpty()
        ? QString()
        : QStringLiteral("singularity-game:%1").arg(message.activityApplicationId);
    const QString launch = href.isEmpty()
        ? QStringLiteral("<span style=\"color:%1;\">Launch Game</span>").arg(QLatin1String(Theme::Dark))
        : QStringLiteral("<a href=\"%1\" style=\"color:%2; text-decoration:none; font-weight:bold;\">Launch Game</a>")
              .arg(href.toHtmlEscaped(), QLatin1String(Theme::Dark));
    const QString pad = href.isEmpty()
        ? QStringLiteral("<span style=\"color:%1;\">&#127918;</span>").arg(QLatin1String(Theme::TextPrimary))
        : QStringLiteral("<a href=\"%1\" style=\"color:%2; text-decoration:none;\">&#127918;</a>")
              .arg(href.toHtmlEscaped(), QLatin1String(Theme::TextPrimary));

    const QString inner = QStringLiteral(
        "<table cellspacing=\"0\" cellpadding=\"0\"><tr>"
        "<td valign=\"middle\">%1</td>"
        "<td valign=\"middle\" style=\"padding-left:12px;\">"
        "<div style=\"font-weight:bold; font-size:15px;\">%2</div>"
        "<div style=\"color:%3; font-size:12px;\">%4</div></td></tr></table>"
        "<table cellspacing=\"0\" cellpadding=\"0\" style=\"margin-top:10px;\" width=\"100%\"><tr>"
        "<td bgcolor=\"#ffffff\" valign=\"middle\" align=\"center\" style=\"padding:8px 16px;\">%5</td>"
        "<td width=\"8\"></td>"
        "<td width=\"40\" bgcolor=\"%6\" valign=\"middle\" align=\"center\" style=\"padding:8px;\">%7</td>"
        "</tr></table>")
                              .arg(icon, name.toHtmlEscaped(), QLatin1String(Theme::TextMuted),
                                   subtitle.toHtmlEscaped(), launch, QLatin1String(Theme::SurfaceHover), pad);

    return QStringLiteral("<table class=\"embed\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                          "<td class=\"embed-inner\" bgcolor=\"%1\" style=\"padding:12px 16px;\">%2</td>"
                          "</tr></table>")
        .arg(QLatin1String(Theme::SurfaceInput), inner);
}

void MainWindow::requestInvite(const QString &code)
{
    if (code.isEmpty() || m_invites.contains(code) || !m_rest)
        return;
    m_invites.insert(code, InviteCard{});

    m_rest->fetchInvite(
        code,
        [this, code](const QJsonObject &invite) {
            InviteCard card;
            card.loaded = true;
            const QJsonObject guild = invite.value(QStringLiteral("guild")).toObject();
            const QJsonObject channel = invite.value(QStringLiteral("channel")).toObject();
            card.guildId = guild.value(QStringLiteral("id")).toString();
            card.guildName = guild.value(QStringLiteral("name")).toString();
            card.iconHash = guild.value(QStringLiteral("icon")).toString();
            card.channelId = channel.value(QStringLiteral("id")).toString();
            card.channelType = channel.value(QStringLiteral("type")).toInt();
            if (card.guildName.isEmpty())
                card.guildName = channel.value(QStringLiteral("name")).toString();
            card.online = invite.value(QStringLiteral("approximate_presence_count")).toInt(-1);
            card.members = invite.value(QStringLiteral("approximate_member_count")).toInt(-1);
            card.valid = !card.guildId.isEmpty() || !card.channelId.isEmpty();
            m_invites.insert(code, card);
            redrawInviteWaiters(code);
        },
        [this, code](const RestClient::Error &error) {
            // A rate limit is not a dead invite: forget it so the next
            // redraw asks again.
            if (error.isRateLimit()) {
                m_invites.remove(code);
                return;
            }
            InviteCard card;
            card.loaded = true;
            m_invites.insert(code, card);
            wlog(QStringLiteral("invite"), QStringLiteral("%1 did not resolve: HTTP %2 %3")
                                               .arg(code).arg(error.httpStatus).arg(error.message));
            redrawInviteWaiters(code);
        });
}

void MainWindow::redrawInviteWaiters(const QString &code)
{
    const QSet<QString> waiting = m_inviteWaiters.value(code);
    for (const QString &messageId : waiting) {
        if (!replaceMessageInView(messageId)) {
            scheduleRender();
            return;
        }
    }
}

void MainWindow::joinInvite(const QString &code, const QByteArray &context,
                            std::function<void(bool, const QString &)> onDone,
                            const RestClient::CaptchaProof &captcha)
{
    if (!m_rest || code.isEmpty())
        return;
    if (m_invitesJoining.contains(code) && captcha.key.isEmpty())
        return;
    m_invitesJoining.insert(code);
    redrawInviteWaiters(code);

    const auto finish = [this, code, onDone](bool joined, const QString &problem) {
        m_invitesJoining.remove(code);
        redrawInviteWaiters(code);
        if (onDone)
            onDone(joined, problem);
        else if (!problem.isEmpty())
            flashStatus(problem, 8000);
    };

    m_rest->acceptInvite(
        code, m_gateway ? m_gateway->sessionId() : QString(), context,
        [this, code, finish](const QJsonObject &invite) {
            const QJsonObject guild = invite.value(QStringLiteral("guild")).toObject();
            QString guildId = guild.value(QStringLiteral("id")).toString();
            if (guildId.isEmpty())
                guildId = invite.value(QStringLiteral("guild_id")).toString();
            const QString channelId = invite.value(QStringLiteral("channel")).toObject()
                                          .value(QStringLiteral("id")).toString();
            const QString name = guild.value(QStringLiteral("name")).toString();
            wlog(QStringLiteral("invite"), QStringLiteral("joined %1 (%2) with %3, new member: %4")
                                               .arg(name, guildId, code)
                                               .arg(invite.value(QStringLiteral("new_member")).toBool()));

            // Already in it: go straight there. Otherwise the server arrives
            // over the gateway a moment later, and GUILD_CREATE opens it.
            if (guildId.isEmpty() || !m_store->guild(guildId).id.isEmpty()) {
                if (!channelId.isEmpty())
                    selectChannelEverywhere(channelId);
            } else {
                m_pendingJoinGuildId = guildId;
                m_pendingJoinChannelId = channelId;
            }
            flashStatus(name.isEmpty() ? QStringLiteral("Joined.") : QStringLiteral("Joined %1.").arg(name), 6000);
            finish(true, QString());
        },
        [this, code, context, onDone, finish](const RestClient::Error &error) {
            // Discord often wants a check before a join. The person solves
            // it, and the same join goes again with the proof.
            RestClient::CaptchaProof proof;
            if (takeCaptcha(error, &proof)) {
                m_invitesJoining.remove(code);
                joinInvite(code, context, onDone, proof);
                return;
            }

            const int discordCode = error.body.value(QStringLiteral("code")).toInt();
            QString problem;
            if (error.body.contains(QStringLiteral("captcha_key")))
                problem = QStringLiteral("Discord asked for a check, and it was not finished, so you did not join.");
            else if (discordCode == 10006 || error.httpStatus == 404)
                problem = QStringLiteral("That invite is invalid or has expired.");
            else if (discordCode == 30001)
                problem = QStringLiteral("You are in as many servers as Discord allows (100, or 200 with Nitro).");
            else if (discordCode == 40007)
                problem = QStringLiteral("You are banned from that server.");
            else if (error.isRateLimit())
                problem = QStringLiteral("Discord says to slow down. Try again in a minute.");
            else
                problem = QStringLiteral("Could not join (HTTP %1): %2").arg(error.httpStatus).arg(error.message);

            wlog(QStringLiteral("invite"), QStringLiteral("join with %1 failed: HTTP %2, code %3, %4")
                                               .arg(code).arg(error.httpStatus).arg(discordCode).arg(error.message));
            finish(false, problem);
        },
        captcha);
}

void MainWindow::showJoinServerDialog(const QString &prefill)
{
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Join a Server"));
    dialog->setMinimumWidth(440);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("Join a Server"), dialog);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: bold;"));
    auto *intro = new QLabel(QStringLiteral("Enter an invite below to join an existing server."), dialog);
    intro->setAlignment(Qt::AlignCenter);
    intro->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));

    auto *fieldLabel = new QLabel(QStringLiteral("INVITE LINK"), dialog);
    fieldLabel->setStyleSheet(QStringLiteral("font-size: 11px; font-weight: bold;"));
    auto *field = new QLineEdit(prefill, dialog);
    field->setPlaceholderText(QStringLiteral("https://discord.gg/hTKzmak"));
    field->setClearButtonEnabled(true);

    auto *problem = new QLabel(dialog);
    problem->setWordWrap(true);
    problem->setStyleSheet(QStringLiteral("color: #f23f43;"));
    problem->hide();

    auto *examples = new QLabel(QStringLiteral("<b style=\"font-size:11px;\">INVITES SHOULD LOOK LIKE</b><br>"
                                               "hTKzmak<br>https://discord.gg/hTKzmak<br>https://discord.gg/cool-people"),
                                dialog);
    examples->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));

    auto *buttons = new QHBoxLayout();
    auto *back = new QPushButton(QStringLiteral("Back"), dialog);
    back->setFlat(true);
    auto *join = new QPushButton(QStringLiteral("Join Server"), dialog);
    join->setDefault(true);
    join->setStyleSheet(QStringLiteral("QPushButton { background-color: %1; color: white; font-weight: bold; "
                                       "border: none; border-radius: 4px; padding: 8px 18px; }"
                                       "QPushButton:disabled { background-color: %2; }")
                            .arg(QLatin1String(Theme::Green), QLatin1String(Theme::SurfaceHover)));
    buttons->addWidget(back);
    buttons->addStretch(1);
    buttons->addWidget(join);

    layout->addWidget(title);
    layout->addWidget(intro);
    layout->addSpacing(6);
    layout->addWidget(fieldLabel);
    layout->addWidget(field);
    layout->addWidget(problem);
    layout->addWidget(examples);
    layout->addSpacing(6);
    layout->addLayout(buttons);

    connect(back, &QPushButton::clicked, dialog, &QDialog::reject);

    const QPointer<QDialog> guard(dialog);
    const auto attempt = [this, guard, field, problem, join]() {
        const QString code = inviteCodeFrom(field->text());
        if (code.isEmpty()) {
            problem->setText(QStringLiteral("Please enter a valid invite link or invite code."));
            problem->show();
            return;
        }
        problem->hide();
        join->setEnabled(false);
        join->setText(QStringLiteral("Joining…"));
        joinInvite(code, joinGuildContext(), [guard, problem, join](bool joined, const QString &why) {
            if (!guard)
                return;
            if (joined) {
                guard->accept();
                return;
            }
            join->setEnabled(true);
            join->setText(QStringLiteral("Join Server"));
            problem->setText(why);
            problem->show();
        });
    };
    connect(join, &QPushButton::clicked, dialog, attempt);
    connect(field, &QLineEdit::returnPressed, dialog, attempt);

    dialog->show();
    field->setFocus();
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
    QElapsedTimer clock;
    clock.start();
    wlog(QStringLiteral("ui"), QStringLiteral("settings opening"));

    // A GIF wallpaper is decoded on this thread. Leave it paused while the
    // modal is up so a frame inflate does not land in the same stall as the
    // dialog constructing itself.
    const bool playBackground =
        AppConfig::instance().value(QStringLiteral("appearance/animatedBackground"), true).toBool();
    if (m_backdrop)
        m_backdrop->setRunning(false);

    // Keep one dialog. A stack QDialog is created and destroyed at the same
    // address every open; Windows UI Automation then hits Qt's accessibility
    // cache for that pointer and QWidget::accessibleName crashes on a null
    // widget (Qt6Widgets, same fault offset across dumps).
    if (!m_settingsDialog) {
        m_settingsDialog = new SettingsDialog(m_store, m_rest, m_plugins, m_selfUserId, this);
        m_settingsDialog->setGames(m_gateway->games());
        connect(m_settingsDialog, &SettingsDialog::activityShareChanged, this, &MainWindow::setActivityShared);
        connect(m_settingsDialog, &SettingsDialog::logOutRequested, this, &MainWindow::logOut);
        connect(m_settingsDialog, &SettingsDialog::restartRequested, this, [this]() { restartInto({}); });
        connect(m_settingsDialog, &SettingsDialog::testNotificationRequested, this, [this]() {
            showDesktopNotification(QStringLiteral("Singularity"),
                                    QStringLiteral("This is what a new message looks like."), QString());
        });

        // An account change answered with a new token: the old one is dead
        // from now on, so every place that holds it gets the new one.
        connect(m_settingsDialog, &SettingsDialog::tokenReplaced, this, [this](const QString &token) {
            const bool saved = !m_sessionToken.isEmpty() && AppConfig::instance().token() == m_sessionToken;
            m_sessionToken = token;
            m_rest->setToken(token);
            m_gateway->setToken(token);
            if (saved) {
                AppConfig::instance().setToken(token);
                const UserInfo self = m_store->user(m_selfUserId);
                TokenStore::rememberAccount(TokenStore::Account{m_selfUserId, self.username, m_selfAvatarHash, token});
            }
            wlog(QStringLiteral("account"), QStringLiteral("Discord issued a new token after an account change"));
        });
        // A call already running has to be told, or the sliders only take effect
        // the next time you join one.
        connect(m_settingsDialog, &SettingsDialog::voiceSettingsChanged, this, &MainWindow::applyVoiceSettings);
    }

    wlog(QStringLiteral("ui"), QStringLiteral("settings ready in %1 ms").arg(clock.elapsed()));

    // Applying the theme while the modal is up restyles every widget and
    // rebuilds the conversation (QTextEngine). Opening Settings without
    // touching Appearance used to pay that cost on close for no reason, and
    // dragging Dim paid it on every tick. Config is written as they click;
    // the window is restyled once, after the dialog hides.
    bool appearanceTouched = false;
    const QMetaObject::Connection mark =
        connect(m_settingsDialog, &SettingsDialog::appearanceChanged, this, [&]() { appearanceTouched = true; });

    m_settingsDialog->exec();
    disconnect(mark);

    if (m_backdrop)
        m_backdrop->setRunning(playBackground);

    if (appearanceTouched) {
        // Chip clicks already wrote the seed. Re-polish after the modal hides:
        // setStyleSheet during QDialog::exec can leave the main window on the
        // unpolished black fill, which is the "close Settings, hole gone" bug.
        applyAppearance();
    }

    wlog(QStringLiteral("ui"),
         QStringLiteral("settings closed after %1 ms%2")
             .arg(clock.elapsed())
             .arg(appearanceTouched ? QStringLiteral(", appearance applied") : QString()));
}

void MainWindow::applyAppearance()
{
    AppConfig &config = AppConfig::instance();

    const QColor seed(config.value(QStringLiteral("appearance/themeSeed"),
                                   QLatin1String(Theme::DefaultSeed)).toString());
    Theme::applySeed(seed.isValid() ? seed : QColor(QLatin1String(Theme::DefaultSeed)));
    Theme::applyPalette();
    wlog(QStringLiteral("ui"), QStringLiteral("applying appearance"));
    qApp->setStyleSheet(Theme::applicationStyleSheet());

    if (m_messageView) {
        m_messageView->document()->setDefaultStyleSheet(
            Theme::messageViewCss(config.value(QStringLiteral("appearance/fontSize"), 15).toInt()));
        m_messageView->setAnimationsEnabled(
            config.value(QStringLiteral("appearance/playAnimations"), true).toBool());
    }
    if (m_backdrop) {
        m_backdrop->setHoleColors(Theme::holeAccent(), Theme::holeDisk(), Theme::holeGrade());
        m_backdrop->setRunning(
            config.value(QStringLiteral("appearance/animatedBackground"), true).toBool());
        applyBackgroundSettings();
        m_backdrop->update();
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
    // Out of the switcher as well, or it could be switched straight back to.
    if (!m_selfUserId.isEmpty())
        TokenStore::forgetAccount(m_selfUserId);
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

    if (auto *max = dynamic_cast<CaptionButton *>(m_captionMax))
        max->setRestore(isMaximized());
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
                    if (widget == m_appMenu || name.startsWith(QLatin1String("Caption"))) {
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
