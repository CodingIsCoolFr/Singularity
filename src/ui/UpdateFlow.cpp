#include "ui/UpdateFlow.h"

#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/Updater.h"

#include "ui/Theme.h"

#include <QEvent>
#include <functional>

#include <QApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
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
    UpdateOffer(const QString &version, const QString &notes, qint64 bytes, QWidget *owner)
        : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint)
        , m_version(version)
    {
        setObjectName(QStringLiteral("UpdateOffer"));
        setAttribute(Qt::WA_DeleteOnClose);
        setAttribute(Qt::WA_ShowWithoutActivating);
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

        m_notes = new QLabel(summarise(notes), this);
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
        if (QWidget *owner = parentWidget(); owner && owner->isVisible())
            area = QRect(owner->mapToGlobal(QPoint(0, 0)), owner->size());
        else if (QScreen *screen = QGuiApplication::primaryScreen())
            area = screen->availableGeometry();
        if (!area.isNull())
            move(area.center() - QPoint(width() / 2, height() / 2));
        setWindowModality(Qt::NonModal);
        if (!isVisible())
            show();
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
        if (watched == parentWidget() && event->type() == QEvent::Resize)
            place();
        return QWidget::eventFilter(watched, event);
    }

private:
    QString m_version;
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
        if (g_offer)
            g_offer->setProgress(got, total);
    });

    QObject::connect(instance, &Updater::upToDate, qApp, []() {
        QMessageBox::information(g_owner, QStringLiteral("Up to date"),
                                 QStringLiteral("Singularity %1 is the newest version.")
                                     .arg(QApplication::applicationVersion()));
    });

    QObject::connect(instance, &Updater::failed, qApp, [](const QString &reason) {
        if (g_offer)
            g_offer->close();
        releaseForegroundLock();
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
                             if (!g_offer)
                                 return;
                             g_offer->showProgress();
                             updater()->download();
                         });
                         g_offer->place();
                         releaseForegroundLock();
                     });

    QObject::connect(instance, &Updater::readyToInstall, qApp, [](const QString &path) {
        if (g_offer)
            g_offer->close();
        releaseForegroundLock();

        // No installer window. /SILENT still shows one, and that window is
        // the foreground while the files unpack. It stops answering for
        // that stretch, so the taskbar holds every click until it finishes.
        const QStringList switches{
            QStringLiteral("/VERYSILENT"),
            QStringLiteral("/SUPPRESSMSGBOXES"),
            QStringLiteral("/NORESTART"),

            // This copy quits on its own, and the installer waits for that.
            // Asking Windows to close whoever has the exe open shuts Explorer
            // down with it, which is the taskbar freezing after every update.
            QStringLiteral("/NOCLOSEAPPLICATIONS"),

            // The installer starts Singularity again itself, from its [Run]
            // section. Letting Windows restart it as well would leave two.
            QStringLiteral("/NORESTARTAPPLICATIONS"),
        };

        wlog(QStringLiteral("update"), QStringLiteral("installing silently from %1").arg(path));

        // Started before closing, because once this process is gone there is
        // nothing left to start anything.
        if (!QProcess::startDetached(path, switches)) {
            QMessageBox::warning(g_owner, QStringLiteral("Could not start the installer"),
                                 QStringLiteral("It was downloaded to:\n%1").arg(path));
            return;
        }

        qApp->closeAllWindows();
        qApp->quit();
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
