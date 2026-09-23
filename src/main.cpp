#include "core/AppConfig.h"
#include "core/GatewayClient.h"
#include "core/Logger.h"
#include "core/MessageStore.h"
#include "core/RestClient.h"
#include "plugin/PluginHost.h"
#include "ui/LoginDialog.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/UpdateFlow.h"

#include <QApplication>
#include <QFontDatabase>
#include <QIcon>
#include <QSurfaceFormat>

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
static void holdRunningMutex()
{
#ifdef Q_OS_WIN
    static HANDLE held = nullptr;
    if (!held)
        held = CreateMutexW(nullptr, FALSE, L"Local\\SingularityRunning");
#endif
}

int main(int argc, char *argv[])
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapInterval(1);
    format.setDepthBufferSize(0);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    holdRunningMutex();
    app.setApplicationName(QStringLiteral("Singularity"));
    app.setOrganizationName(QStringLiteral("Singularity"));
    app.setApplicationVersion(QStringLiteral("0.6.66"));
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
    app.setStyleSheet(Theme::applicationStyleSheet());

    // The title bar belongs to Windows, not to Qt, so it has to be coloured
    // separately or it sits above the app as a paler strip.
    Theme::installDarkTitleBars();

    // Touch the log first so the file exists even if startup fails early.
    wlog(QStringLiteral("app"), QStringLiteral("Singularity %1 starting").arg(app.applicationVersion()));

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

    if (token.isEmpty()) {
        wlog(QStringLiteral("app"), QStringLiteral("no saved token, showing sign-in"));
        LoginDialog login(&rest);
        if (login.exec() != QDialog::Accepted) {
            wlog(QStringLiteral("app"), QStringLiteral("sign-in cancelled"));
            return 0;
        }
        token = login.token();
        wlog(QStringLiteral("app"), QStringLiteral("signed in, remembered=%1")
                                        .arg(!AppConfig::instance().token().isEmpty()));
    } else {
        wlog(QStringLiteral("app"), QStringLiteral("using saved token"));
    }

    rest.setToken(token);

    MainWindow window(&rest, &gateway, &store, &plugins);
    wlog(QStringLiteral("app"), QStringLiteral("main window constructed"));
    window.show();
    wlog(QStringLiteral("app"), QStringLiteral("main window show() returned"));
    window.startSession(token);

    return app.exec();
}
