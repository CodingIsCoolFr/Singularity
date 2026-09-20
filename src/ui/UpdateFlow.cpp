#include "ui/UpdateFlow.h"

#include "core/Logger.h"
#include "core/Updater.h"

#include <QApplication>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>

namespace {

// The window the current check was asked from, if there is one.
//
// Held weakly, because the sign-in window is gone by the time a download that
// started there finishes, and a dialog parented to a deleted window crashes.
// A null parent gives a top level dialog, which is the right answer on the
// startup path where no window exists yet.
QPointer<QWidget> g_owner;

// Shown while the installer downloads, so a slow connection looks like
// progress rather than like nothing happening.
QPointer<QProgressDialog> g_progress;

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
        if (!g_progress || total <= 0)
            return;
        // Kilobytes, because the byte counts overflow the int these take.
        g_progress->setMaximum(static_cast<int>(total / 1024));
        g_progress->setValue(static_cast<int>(got / 1024));
    });

    QObject::connect(instance, &Updater::upToDate, qApp, []() {
        QMessageBox::information(g_owner, QStringLiteral("Up to date"),
                                 QStringLiteral("Singularity %1 is the newest version.")
                                     .arg(QApplication::applicationVersion()));
    });

    QObject::connect(instance, &Updater::failed, qApp, [](const QString &reason) {
        if (g_progress)
            g_progress->close();
        QMessageBox::warning(g_owner, QStringLiteral("Could not check for updates"), reason);
    });

    QObject::connect(instance, &Updater::updateAvailable, qApp,
                     [](const QString &version, const QString &notes, qint64 bytes) {
                         QString text = QStringLiteral("Singularity %1 is available. You have %2.\n\n")
                                            .arg(version, QApplication::applicationVersion());
                         if (!notes.isEmpty())
                             text += notes.left(600) + QStringLiteral("\n\n");
                         text += QStringLiteral("Download it now? It is about %1 MB.")
                                     .arg(bytes / (1024 * 1024));

                         if (QMessageBox::question(g_owner, QStringLiteral("Update available"), text)
                             != QMessageBox::Yes) {
                             wlog(QStringLiteral("update"),
                                  QStringLiteral("%1 offered and declined").arg(version));
                             return;
                         }

                         g_progress = new QProgressDialog(
                             QStringLiteral("Downloading Singularity %1...").arg(version),
                             QString(), 0, 0, g_owner);
                         g_progress->setWindowTitle(QStringLiteral("Update"));
                         g_progress->setAttribute(Qt::WA_DeleteOnClose);
                         g_progress->setMinimumDuration(0);
                         g_progress->setValue(0);

                         updater()->download();
                     });

    QObject::connect(instance, &Updater::readyToInstall, qApp, [](const QString &path) {
        if (g_progress)
            g_progress->close();

        const auto answer = QMessageBox::question(
            g_owner, QStringLiteral("Ready to install"),
            QStringLiteral("The update has been downloaded. Singularity will close while it "
                           "installs, and the installer will offer to start it again.\n\n"
                           "Install now?"));
        if (answer != QMessageBox::Yes)
            return;

        // Started before closing, because once this process is gone there is
        // nothing left to start anything.
        if (!QProcess::startDetached(path, {})) {
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
    u->check(quiet);
}
