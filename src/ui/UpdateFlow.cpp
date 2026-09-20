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
                         QMessageBox box(g_owner);
                         box.setWindowTitle(QStringLiteral("Update available"));
                         box.setIcon(QMessageBox::Question);
                         box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
                         box.setDefaultButton(QMessageBox::Yes);

                         box.setText(QStringLiteral("Singularity %1 is available. You have %2.\n\n"
                                                    "Download it now? It is about %3 MB.")
                                         .arg(version, QApplication::applicationVersion())
                                         .arg(bytes / (1024 * 1024)));

                         // Release notes are written in Markdown, so they were
                         // being shown with their asterisks and hashes still in
                         // them. Qt can render Markdown; it just has to be told
                         // that is what this is.
                         if (!notes.isEmpty()) {
                             box.setTextFormat(Qt::MarkdownText);
                             box.setInformativeText(summarise(notes));
                         }

                         if (box.exec() != QMessageBox::Yes) {
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
