#include "ui/UpdateFlow.h"

#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/Updater.h"

#include "ui/Theme.h"

#include <QEvent>
#include <functional>

#include <QCoreApplication>
#include <QDir>
#include <QApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QEventLoop>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

// The check that is in flight. A quiet one stays silent when nothing is new
// and does not ask again about a version that was already declined.
bool g_quiet = false;

// The window the current check was asked from, if there is one.
//
// Held weakly, because the sign-in window is gone by the time a download that
// started there finishes, and a dialog parented to a deleted window crashes.
// A null parent gives a top level panel, which is the right answer on the
// startup path where no window exists yet.
QPointer<QWidget> g_owner;

// A dialog left the main window disabled, and Windows then kept every
// taskbar click until that window was enabled again. Clearing it here is
// what makes a click during the download open the program then, rather
// than a minute later.
QString summarise(const QString &notes);

// Above this program, not above every other window. A no-activate show was
// leaving the card behind the main window, which is why Check for updates
// looked like it had done nothing.
void liftOverApp(QWidget *panel)
{
    if (!panel)
        return;
    panel->setAttribute(Qt::WA_ShowWithoutActivating, false);
    panel->setWindowModality(Qt::NonModal);
    panel->show();
    panel->raise();
    panel->activateWindow();
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(panel->winId());
    if (hwnd) {
        // Top of the always-on-top band: above Singularity and every other
        // program, so an update is never waiting where nobody can see it.
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
#endif
}

void releaseForegroundLock()
{
    const auto windows = QApplication::topLevelWidgets();
    for (QWidget *widget : windows) {
        if (!widget->isWindow())
            continue;
        if (widget->windowModality() != Qt::NonModal)
            widget->setWindowModality(Qt::NonModal);
        if (!widget->isEnabled())
            widget->setEnabled(true);
#ifdef Q_OS_WIN
        const HWND hwnd = reinterpret_cast<HWND>(widget->effectiveWinId());
        if (hwnd)
            EnableWindow(hwnd, TRUE);
#endif
    }
}

class UpdateOffer : public QWidget
{
public:
    // A window of its own that stays above everything, Singularity and every
    // other program alike. It used to be a tool window owned by whichever
    // Singularity window was active - and with none active it had no owner at
    // all, so it could open underneath the program where nobody saw it. It
    // also went away whenever its owner was minimised. The owner is kept only
    // to centre the box over it.
    UpdateOffer(const QString &version, const QString &notes, qint64 bytes, QWidget *owner)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
        , m_version(version)
        , m_owner(owner)
    {
        setObjectName(QStringLiteral("UpdateOffer"));
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowModality(Qt::NonModal);
        setFixedWidth(420);
        setStyleSheet(QStringLiteral(
            "#UpdateOffer { background: %1; border: 1px solid %2; border-radius: 12px; }"
            "QLabel { color: %3; background: transparent; }"
            "QPushButton { background: %4; color: %3; border: none; border-radius: 8px; padding: 6px 14px; }"
            "QPushButton#UpdateYes { background: %2; }"
            "QProgressBar { background: %4; border: none; border-radius: 3px; min-height: 6px; max-height: 6px; }"
            "QProgressBar::chunk { background: %2; border-radius: 3px; }")
                          .arg(QLatin1String(Theme::SurfaceChat), QLatin1String(Theme::Accent),
                               QLatin1String(Theme::Light), QLatin1String(Theme::SurfaceInput)));

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->setSpacing(8);

        m_title = new QLabel(QStringLiteral("Update available"), this);
        m_title->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 600;"));

        m_body = new QLabel(this);
        m_body->setWordWrap(true);
        m_body->setText(QStringLiteral("Singularity %1 is available. You have %2.\n\n"
                                       "About %3 MB. Singularity will close, update itself, and start again.")
                            .arg(version, QApplication::applicationVersion())
                            .arg(qMax(qint64(1), bytes / (1024 * 1024))));

        m_notes = new QLabel(this);
        // Release notes are written in Markdown; shown as Markdown, so "##"
        // and "**" become a heading and bold rather than sitting there as
        // symbols. summarise() has already taken out every angle bracket, so
        // nothing in them can be HTML.
        m_notes->setTextFormat(Qt::MarkdownText);
        m_notes->setText(summarise(notes));
        m_notes->setWordWrap(true);
        m_notes->setVisible(!m_notes->text().isEmpty());
        m_notes->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::MidGray)));

        m_bar = new QProgressBar(this);
        m_bar->setTextVisible(false);
        m_bar->setRange(0, 1);
        m_bar->hide();

        auto *row = new QHBoxLayout;
        row->addStretch(1);
        m_no = new QPushButton(QStringLiteral("Not now"), this);
        m_yes = new QPushButton(QStringLiteral("Update"), this);
        m_yes->setObjectName(QStringLiteral("UpdateYes"));
        m_yes->setCursor(Qt::PointingHandCursor);
        m_no->setCursor(Qt::PointingHandCursor);
        row->addWidget(m_no);
        row->addWidget(m_yes);

        layout->addWidget(m_title);
        layout->addWidget(m_body);
        layout->addWidget(m_notes);
        layout->addWidget(m_bar);
        layout->addLayout(row);

        connect(m_yes, &QPushButton::clicked, this, [this]() {
            if (m_accept)
                m_accept();
        });
        connect(m_no, &QPushButton::clicked, this, [this]() {
            if (m_decline)
                m_decline();
        });

        if (owner)
            owner->installEventFilter(this);
    }

    void setAccept(std::function<void()> accept) { m_accept = std::move(accept); }
    void setDecline(std::function<void()> decline) { m_decline = std::move(decline); }

    void place()
    {
        adjustSize();
        QRect area;
        if (QWidget *owner = m_owner.data(); owner && owner->isVisible() && !owner->isMinimized())
            area = QRect(owner->mapToGlobal(QPoint(0, 0)), owner->size());
        else if (QScreen *screen = QGuiApplication::primaryScreen())
            area = screen->availableGeometry();
        if (!area.isNull())
            move(area.center() - QPoint(width() / 2, height() / 2));
        liftOverApp(this);
    }

    void showProgress()
    {
        m_yes->hide();
        m_no->hide();
        m_notes->hide();
        m_title->setText(QStringLiteral("Updating"));
        m_body->setText(QStringLiteral("Downloading Singularity %1. The taskbar stays usable.").arg(m_version));
        m_bar->show();
        place();
        releaseForegroundLock();
    }

    void showInstalling()
    {
        m_yes->hide();
        m_no->hide();
        m_notes->hide();
        m_bar->hide();
        m_title->setText(QStringLiteral("Installing"));
        m_body->setText(QStringLiteral("Installing Singularity %1.\n\n"
                                       "The new copy is being written beside this one, "
                                       "so this window stays open and the taskbar keeps working.")
                            .arg(m_version));
        place();
        releaseForegroundLock();
    }

    void setProgress(qint64 got, qint64 total)
    {
        if (total <= 0)
            return;
        m_bar->setRange(0, static_cast<int>(total / 1024));
        m_bar->setValue(static_cast<int>(got / 1024));
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_owner.data() && event->type() == QEvent::Resize)
            place();
        return QWidget::eventFilter(watched, event);
    }

private:
    QString m_version;
    QPointer<QWidget> m_owner;
    QLabel *m_title = nullptr;
    QLabel *m_body = nullptr;
    QLabel *m_notes = nullptr;
    QProgressBar *m_bar = nullptr;
    QPushButton *m_yes = nullptr;
    QPushButton *m_no = nullptr;
    std::function<void()> m_accept;
    std::function<void()> m_decline;
};

QPointer<UpdateOffer> g_offer;

// The installer's old window: just the hole, the name, and "Installing...".
// Nothing of the open app is behind it. This process keeps running, so the
// taskbar is not waiting on a window that has stopped answering.
class InstallScreen : public QWidget
{
public:
    InstallScreen()
        : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    {
        setObjectName(QStringLiteral("InstallScreen"));
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowModality(Qt::NonModal);
        setFixedSize(420, 310);
        setStyleSheet(QStringLiteral(
            "#InstallScreen { background: #07090e; }"
            "QLabel { background: transparent; color: #eef4fb; }"));

        m_hole = new QLabel(this);
        m_hole->setFixedSize(96, 96);
        m_hole->move((width() - 96) / 2, 38);
        m_hole->setAlignment(Qt::AlignCenter);

        auto *title = new QLabel(QStringLiteral("Singularity"), this);
        title->setStyleSheet(QStringLiteral(
            "color: #eef4fb; font-family: 'Segoe UI Light'; font-size: 22px;"));
        title->adjustSize();
        title->move((width() - title->width()) / 2, 150);

        m_step = new QLabel(QStringLiteral("Installing..."), this);
        m_step->setStyleSheet(QStringLiteral("color: #8b95a8; font-family: 'Segoe UI'; font-size: 9pt;"));
        m_step->adjustSize();
        m_step->move((width() - m_step->width()) / 2, 200);

        m_track = new QWidget(this);
        m_track->setGeometry(40, 234, width() - 80, 3);
        m_track->setStyleSheet(QStringLiteral("background: #1a2230;"));
        m_fill = new QWidget(m_track);
        m_fill->setGeometry(0, 0, 0, 3);
        m_fill->setStyleSheet(QStringLiteral("background: #d5dde8;"));

        for (int i = 0; i < 24; ++i) {
            const QPixmap source(QStringLiteral(":/spinner/spin%1.bmp").arg(i, 2, 10, QLatin1Char('0')));
            m_frames.append(source.isNull()
                                ? source
                                : source.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        if (!m_frames.isEmpty() && !m_frames.first().isNull())
            m_hole->setPixmap(m_frames.first());

        connect(&m_timer, &QTimer::timeout, this, [this]() { tick(); });
        m_timer.start(40);
    }

    void place()
    {
        QScreen *screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect area = screen->availableGeometry();
            move(area.center() - QPoint(width() / 2, height() / 2));
        }
        liftOverApp(this);
    }

    void setStep(const QString &text)
    {
        m_step->setText(text);
        m_step->adjustSize();
        m_step->move((width() - m_step->width()) / 2, 200);
    }

    // Real progress while the file is coming down. After that the bar eases
    // the rest of the way on its own, because the install does not report one.
    void setFraction(qint64 got, qint64 total)
    {
        if (total <= 0)
            return;
        m_real = true;
        const int full = m_track->width();
        const int filled = static_cast<int>(qBound(qint64(0), got * full / total, qint64(full)));
        m_filled = qMax(m_filled, filled);
        m_fill->setGeometry(0, 0, m_filled, 3);
    }

private:
    void tick()
    {
        if (!m_frames.isEmpty()) {
            m_frame = (m_frame + 1) % m_frames.size();
            const QPixmap &frame = m_frames.at(m_frame);
            if (!frame.isNull())
                m_hole->setPixmap(frame);
        }
        if (m_real)
            return;
        const int full = m_track->width();
        const int cap = full * 9 / 10;
        if (m_filled < cap)
            m_filled = qMin(cap, m_filled + qMax(1, (cap - m_filled) / 40));
        m_fill->setGeometry(0, 0, m_filled, 3);
    }

    QLabel *m_hole = nullptr;
    QLabel *m_step = nullptr;
    QWidget *m_track = nullptr;
    QWidget *m_fill = nullptr;
    QTimer m_timer;
    QList<QPixmap> m_frames;
    int m_frame = 0;
    int m_filled = 0;
    bool m_real = false;
};

QPointer<InstallScreen> g_install;
QList<QPointer<QWidget>> g_hidden;
bool g_updating = false;

// The open program goes away for the whole update, download included.
// The hole screen is the only thing left on screen.
void presentInstallScreen(const QString &step)
{
    if (!g_install)
        g_install = new InstallScreen;
    g_install->setStep(step);
    const auto tops = QApplication::topLevelWidgets();
    for (QWidget *widget : tops) {
        if (!widget->isWindow() || widget == g_install || !widget->isVisible())
            continue;
        g_hidden.append(widget);
        widget->hide();
    }
    g_install->place();
    releaseForegroundLock();
}

void restoreHidden()
{
    // Every way an update can fail comes through here.
    g_updating = false;
    for (const QPointer<QWidget> &widget : g_hidden) {
        if (widget)
            widget->show();
    }
    g_hidden.clear();
    if (g_install)
        g_install->close();
}

// Trims release notes down to something that fits in a dialog.
//
// The old version took the first 600 characters flat, which landed in the
// middle of a word: the box ended "...written on the repositor" and stopped,
// reading like the program had broken rather than run out of room. The cut is
// now moved back to a paragraph break, or failing that a line break, and says
// that it happened.
//
// Angle brackets are dropped first. Notes are the body of a release, which is
// text fetched from a web service, and it is about to be handed to a renderer
// that understands HTML. Markdown needs no angle brackets, so removing them
// costs nothing and takes away the only thing in there that could reach back
// out to the network for an image.
QString summarise(const QString &notes)
{
    QString text = notes;
    text.remove(QLatin1Char('<'));
    text.remove(QLatin1Char('>'));
    // A byte-order mark at the front of the notes (the release script's text
    // files carried one) sits before the first "##", so the heading was not
    // recognised and showed as raw symbols.
    text.remove(QChar(0xFEFF));
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text = text.trimmed();

    constexpr int Limit = 700;
    if (text.size() <= Limit)
        return text;

    QString cut = text.left(Limit);

    int end = cut.lastIndexOf(QLatin1String("\n\n"));
    if (end < Limit / 3)
        end = cut.lastIndexOf(QLatin1Char('\n'));
    if (end < Limit / 3)
        end = cut.lastIndexOf(QLatin1Char(' '));
    if (end > 0)
        cut.truncate(end);

    return cut.trimmed() + QStringLiteral("\n\nThe rest is on the release page.");
}

// One updater for the whole run, owned by the application.
//
// The flow is a chain of network replies, so the object has to outlive the
// call that started it. Tying it to a window would put it back in the trap
// this file exists to get out of.
Updater *updater()
{
    static Updater *instance = nullptr;
    if (instance)
        return instance;

    instance = new Updater(qApp);

    QObject::connect(instance, &Updater::progress, qApp, [](qint64 got, qint64 total) {
        if (g_install)
            g_install->setFraction(got, total);
        else if (g_offer)
            g_offer->setProgress(got, total);
    });

    QObject::connect(instance, &Updater::upToDate, qApp, []() {
        QMessageBox::information(g_owner, QStringLiteral("Up to date"),
                                 QStringLiteral("Singularity %1 is the newest version.")
                                     .arg(QApplication::applicationVersion()));
    });

    QObject::connect(instance, &Updater::failed, qApp, [](const QString &reason) {
        restoreHidden();
        QMessageBox::warning(g_owner, QStringLiteral("Could not check for updates"), reason);
    });

    QObject::connect(instance, &Updater::updateAvailable, qApp,
                     [](const QString &version, const QString &notes, qint64 bytes) {
                         // No on one version should not ask about that same
                         // version again. A newer tag is a different string,
                         // so it still asks. The menu check always asks.
                         if (g_quiet
                             && AppConfig::instance().value(QStringLiteral("update/declined")).toString()
                                    == version) {
                             return;
                         }

                         if (g_offer) {
                             g_offer->place();
                             return;
                         }

                         // Not a dialog. A dialog, even one marked non-modal,
                         // left this program holding Windows' foreground lock
                         // for the whole download. Clicks on the taskbar were
                         // kept and delivered together when the dialog closed.
                         QWidget *owner = g_owner ? g_owner.data() : QApplication::activeWindow();
                         g_offer = new UpdateOffer(version, notes, bytes, owner);
                         g_offer->setDecline([version]() {
                             AppConfig::instance().setValue(QStringLiteral("update/declined"), version);
                             wlog(QStringLiteral("update"),
                                  QStringLiteral("%1 offered and declined").arg(version));
                             if (g_offer)
                                 g_offer->close();
                         });
                         g_offer->setAccept([]() {
                             g_updating = true;
                             if (g_offer)
                                 g_offer->close();
                             presentInstallScreen(QStringLiteral("Downloading..."));
                             updater()->download();
                         });
                         g_offer->place();
                         releaseForegroundLock();
                     });

    QObject::connect(instance, &Updater::readyToInstall, qApp, [](const QString &path) {
        releaseForegroundLock();

        // The folder this copy lives in. A copy already under versions\<n>
        // belongs to the folder above that, so the next one is a sibling
        // rather than nested inside it.
        QDir root(QCoreApplication::applicationDirPath());
        const QString here = QDir::cleanPath(root.absolutePath());
        if (root.cdUp() && root.dirName() == QLatin1String("versions"))
            root.cdUp();
        else
            root.setPath(here);

        QString version = updater()->latestVersion();
        if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
            version.remove(0, 1);
        const QString dest = QDir::cleanPath(root.filePath(QStringLiteral("versions/") + version));
        QDir().mkpath(dest);

        if (g_offer)
            g_offer->close();
        // Same screen as the download. The app is already hidden.
        presentInstallScreen(QStringLiteral("Installing..."));

        // Discord writes the new version beside the one that is open and
        // never puts an installer in front. This copy keeps drawing and
        // keeps taking clicks. The installer has no window of its own, and
        // it does not wait for this process to exit, because it is not
        // replacing these files.
        auto *install = new QProcess(qApp);
        install->setProgram(path);
        install->setArguments({
            QStringLiteral("/VERYSILENT"),
            QStringLiteral("/SUPPRESSMSGBOXES"),
            QStringLiteral("/NORESTART"),
            QStringLiteral("/NOCLOSEAPPLICATIONS"),
            QStringLiteral("/NORESTARTAPPLICATIONS"),
            QStringLiteral("/SKIPWAIT=1"),
            QStringLiteral("/DIR=") + QDir::toNativeSeparators(dest),
        });
        QObject::connect(install, &QProcess::finished, qApp, [install](int code, QProcess::ExitStatus) {
            install->deleteLater();
            if (g_install)
                g_install->close();
            releaseForegroundLock();
            if (code != 0) {
                restoreHidden();
                QMessageBox::warning(g_owner, QStringLiteral("Could not install the update"),
                                     QStringLiteral("The installer stopped before it finished."));
                return;
            }
            // The installer's own last step starts the new copy.
            qApp->quit();
        });
        QObject::connect(install, &QProcess::errorOccurred, qApp, [install](QProcess::ProcessError) {
            restoreHidden();
            QMessageBox::warning(g_owner, QStringLiteral("Could not start the installer"),
                                 install->errorString());
            install->deleteLater();
        });

        wlog(QStringLiteral("update"),
             QStringLiteral("installing %1 beside the running copy, into %2").arg(version, dest));
        install->start();
    });

    return instance;
}

} // namespace

void UpdateFlow::run(bool quiet, QWidget *parent)
{
    Updater *u = updater();

    // A second check while one is still running would talk over the first.
    if (u->busy())
        return;

    g_owner = parent;
    g_quiet = quiet;
    u->check(quiet);
}

bool UpdateFlow::updating()
{
    return g_updating;
}

void UpdateFlow::waitForUpdate()
{
    // A plain event loop, checked a few times a second. qApp->quit() at the
    // end of a good install ends this loop too, along with every other one.
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(250);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&loop]() {
        if (!g_updating)
            loop.quit();
    });
    poll.start();
    if (g_updating)
        loop.exec();
}

void UpdateFlow::watch()
{
    // Once the window exists, then every two minutes after that. The first
    // look is what a launch finds. The repeat is an update published while
    // the program is already open, which otherwise sits there until somebody
    // opens the menu.
    QTimer::singleShot(8000, qApp, []() { UpdateFlow::run(true, nullptr); });

    auto *timer = new QTimer(qApp);
    timer->setInterval(2 * 60 * 1000);
    QObject::connect(timer, &QTimer::timeout, qApp, []() { UpdateFlow::run(true, nullptr); });
    timer->start();
}
