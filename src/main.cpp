#include "core/AppConfig.h"
#include "core/CrashLog.h"
#include "core/GatewayClient.h"
#include "core/HangWatch.h"
#include "core/InstallTidy.h"
#include "core/Logger.h"
#include "core/MessageStore.h"
#include "core/RestClient.h"
#include "core/SingleInstance.h"
#include "core/TokenStore.h"
#include "plugin/PluginHost.h"
#include "ui/LoginDialog.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/UpdateFlow.h"

#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QTextStream>
#include <QFontDatabase>
#include <QIcon>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTimer>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Held until this process exits. The installer waits for the name to
// disappear and then copies files. It does not ask Windows to close whoever
// has the program open, because Explorer keeps the shortcut's target open to
// draw its icon, and closing Explorer is the taskbar not taking clicks.
//
// Returns true when another copy already held it.
static bool holdRunningMutex()
{
#ifdef Q_OS_WIN
    static HANDLE held = nullptr;
    if (!held) {
        held = CreateMutexW(nullptr, FALSE, L"Local\\SingularityRunning");
        return held && GetLastError() == ERROR_ALREADY_EXISTS;
    }
#endif
    return false;
}

int main(int argc, char *argv[])
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapInterval(1);
    format.setDepthBufferSize(0);
    QSurfaceFormat::setDefaultFormat(format);

    // Settings -> Appearance -> App zoom. Qt reads its scale factor once, while
    // the application is being built, so it has to be in place before that.
    // Someone who set QT_SCALE_FACTOR themselves keeps theirs.
    int zoom = 100;
    const bool ownScale = qEnvironmentVariableIsSet("QT_SCALE_FACTOR");
    if (!ownScale) {
        const QSettings stored(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Singularity"),
                               QStringLiteral("Singularity"));
        zoom = qBound(80, stored.value(QStringLiteral("appearance/zoom"), 100).toInt(), 200);
        if (zoom != 100)
            qputenv("QT_SCALE_FACTOR", QByteArray::number(zoom / 100.0));
    }

    QApplication app(argc, argv);

    // Taken back out once read: every program started from here (a restart,
    // the updater, a browser) would inherit it, and the next copy decides its
    // zoom for itself.
    if (!ownScale)
        qunsetenv("QT_SCALE_FACTOR");
    app.setProperty("singularityZoom", zoom);

    // Before the first log line: a second launch that is only going to wake
    // the running copy must not start its log on top of that copy's.
    const bool anotherCopy = holdRunningMutex();
    if (anotherCopy && !app.arguments().contains(QStringLiteral("--replace"))
        && !app.arguments().contains(QStringLiteral("--add-account")))
        Logger::useSideFile();

    CrashLog::install();
    app.setApplicationName(QStringLiteral("Singularity"));
    app.setOrganizationName(QStringLiteral("Singularity"));
    app.setApplicationVersion(QStringLiteral("0.8.7"));
    app.setWindowIcon(QIcon(QStringLiteral(":/brand/singularity.png")));

    Theme::applySeed(QColor(AppConfig::instance().value(QStringLiteral("appearance/themeSeed"),
                                                       QLatin1String(Theme::DefaultSeed)).toString()));

    // What the seed actually produced.
    //
    // The palette is derived at runtime, so a control that comes out invisible
    // cannot be explained by reading the stylesheet: the colours are not in
    // it. One line here turns "the button vanished" into a number.
    wlog(QStringLiteral("theme"),
         QStringLiteral("seed %1 -> accent %2, surface %3, text %4, ground %5")
             .arg(Theme::seedColor().name(), QLatin1String(Theme::Accent),
                  QLatin1String(Theme::SurfaceChat), QLatin1String(Theme::TextPrimary),
                  QLatin1String(Theme::Dark)));
    Theme::applyPalette();
    app.setStyleSheet(Theme::applicationStyleSheet());

    // The title bar belongs to Windows, not to Qt, so it has to be coloured
    // separately or it sits above the app as a paler strip.
    Theme::installDarkTitleBars();

    // Every drop-down list gets a solid background. Without this the Windows 11
    // style leaves them see-through under our style sheet, and their items
    // float over whatever is behind the window.
    Theme::installPopupFix();

    // Touch the log first so the file exists even if startup fails early.
    wlog(QStringLiteral("app"), QStringLiteral("Singularity %1 starting").arg(app.applicationVersion()));
    if (zoom != 100)
        wlog(QStringLiteral("app"), QStringLiteral("app zoom %1%").arg(zoom));

    // --layout-report <file>: builds the main window without signing in or
    // connecting to anything, writes the narrowest width each part of it will
    // accept, and exits. For finding what stops the window fitting a narrow
    // (portrait) monitor. Nothing here touches the account.
    {
        const int at = int(app.arguments().indexOf(QStringLiteral("--layout-report")));
        if (at > 0 && at + 1 < app.arguments().size()) {
            RestClient rest;
            MessageStore store;
            GatewayClient gateway;
            PluginHost plugins(&rest, &gateway, &store);
            MainWindow window(&rest, &gateway, &store, &plugins);
            // Never shown: sizes are known without drawing, and showing it would
            // start the background's OpenGL on a screen that may not exist.
            window.resize(900, 800);
            QFile out(app.arguments().at(at + 1));
            if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream text(&out);
                text << "window minimumSizeHint " << window.minimumSizeHint().width() << " minimumWidth "
                     << window.minimumWidth() << " actual " << window.width() << "\n";
                const QList<QWidget *> all = window.findChildren<QWidget *>();
                for (QWidget *w : all) {
                    const int least = qMax(w->minimumSizeHint().width(), w->minimumWidth());
                    if (least < 200)
                        continue;
                    int depth = 0;
                    for (QWidget *p = w->parentWidget(); p && p != &window; p = p->parentWidget())
                        ++depth;
                    text << depth << " " << w->metaObject()->className() << " '" << w->objectName()
                         << "' min " << least << " now " << w->width() << "\n";
                }
            }
            return 0;
        }
    }

    // One copy at a time. A second launch brings the running copy forward and
    // leaves; a newer version (from the updater) or a restart for an account
    // switch asks the running copy to close and takes over.
    const bool addingAccount = app.arguments().contains(QStringLiteral("--add-account"));
    const bool replaceRunning = addingAccount || app.arguments().contains(QStringLiteral("--replace"));
    if (!SingleInstance::claim(app.applicationVersion(), replaceRunning))
        return 0;

    // This copy stays. If it began on the side log, singularity.log is its.
    Logger::instance().claimMainFile();

    // Names whatever freezes the window, in the log, the moment it happens.
    HangWatch::start();

    // A quiet look for a newer version, a few seconds in, and again while the
    // program stays open. The sign-in window runs its own event loop, so the
    // first look still fires there. Already being current says nothing.
    UpdateFlow::watch();

    RestClient rest;
    MessageStore store;
    GatewayClient gateway;
    PluginHost plugins(&rest, &gateway, &store);
    plugins.registerBuiltins();

    // A remembered token skips the sign-in window. If it turns out to be stale
    // the main window says so and offers Log out.
    QString token = AppConfig::instance().token();

    // Signed out of one account but others are still remembered: carry on as
    // the most recent of them, the way the official client does, rather than
    // asking for a password that is not needed.
    if (token.isEmpty()) {
        const QList<TokenStore::Account> saved = TokenStore::accounts();
        if (!saved.isEmpty()) {
            token = saved.first().token;
            AppConfig::instance().setToken(token);
            wlog(QStringLiteral("app"), QStringLiteral("no current session, using the most recent saved account"));
        }
    }

    // "Add an account" restarts with --add-account. The sign-in window shows
    // even though a session is saved; cancelling it goes back to that session.
    const auto bringForward = [](QWidget *window) {
        if (!window)
            return;
        if (window->isMinimized())
            window->showNormal();
        window->show();
        window->raise();
        window->activateWindow();
    };

    if (token.isEmpty() || addingAccount) {
        wlog(QStringLiteral("app"), addingAccount ? QStringLiteral("adding an account, showing sign-in")
                                                  : QStringLiteral("no saved token, showing sign-in"));
        LoginDialog login(&rest);

        // Shown as an ordinary window and waited for, not run with exec().
        //
        // exec() makes a dialog application-modal: while it is open Qt throws
        // away every click on any other window of the program. The update box
        // is another window, so its Update button did nothing while the
        // sign-in window was up. exec() also ends the moment the dialog is
        // hidden, and the updater hides every window to show its install
        // screen - which read as "cancelled" and quit halfway through the
        // download. Waiting on finished() has neither problem: the dialog
        // only finishes when it is accepted or really closed.
        QEventLoop waiting;
        QObject::connect(&login, &QDialog::finished, &waiting, &QEventLoop::quit);
        SingleInstance::setShowHandler([&login, bringForward]() { bringForward(&login); });
        login.show();
        waiting.exec();
        SingleInstance::setShowHandler(nullptr);
        const int answer = login.result();

        // A newer copy took over while this one sat at sign-in.
        if (SingleInstance::replaced())
            return 0;

        // The loop also ends when a finished update quits the program so the
        // new copy can start. That is not a cancelled sign-in either.
        if (answer != QDialog::Accepted && UpdateFlow::updating())
            return 0;

        if (answer != QDialog::Accepted) {
            wlog(QStringLiteral("app"), QStringLiteral("sign-in cancelled"));
            if (token.isEmpty())
                return 0;
            rest.setToken(token);
        } else {
            token = login.token();
        }
        wlog(QStringLiteral("app"), QStringLiteral("signed in, remembered=%1")
                                        .arg(!AppConfig::instance().token().isEmpty()));
    } else {
        wlog(QStringLiteral("app"), QStringLiteral("using saved token"));
    }

    rest.setToken(token);

    MainWindow window(&rest, &gateway, &store, &plugins);
    wlog(QStringLiteral("app"), QStringLiteral("main window constructed"));
    SingleInstance::setShowHandler([&window, bringForward]() { bringForward(&window); });
    window.show();
    wlog(QStringLiteral("app"), QStringLiteral("main window show() returned"));
    window.startSession(token);

    // Twenty seconds in, by when an update's old copy has closed: point every
    // shortcut (the taskbar pin included) at this copy and clear out old ones.
    QTimer::singleShot(20000, &app, []() { InstallTidy::run(); });

    const int result = app.exec();

    // Every way out - Quit, sign-out, an update, a newer copy taking over -
    // ends up here, so the goodbye to Discord is said once, in one place.
    gateway.shutdown();

    SingleInstance::setShowHandler(nullptr);
    HangWatch::stop();
    return result;
}
