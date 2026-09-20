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
#include <QTimer>

int main(int argc, char *argv[])
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapInterval(1);
    format.setDepthBufferSize(0);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Singularity"));
    app.setOrganizationName(QStringLiteral("Singularity"));
    app.setApplicationVersion(QStringLiteral("0.1.5"));
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

    // A quiet look for a newer version, a few seconds in.
    //
    // Started here rather than from the main window, because the main window
    // is not built at all until somebody signs in. That is exactly the run
    // that most needs an update: whoever is stuck on the sign-in screen may be
    // stuck on something a newer version fixes.
    //
    // The sign-in window runs its own event loop, so this still fires while it
    // is open. It is quiet, so a program that is already current says nothing.
    // An updater that interrupts to report that nothing has happened is one
    // people learn to dismiss without reading, which is exactly the habit you
    // do not want when it eventually has something to say.
    QTimer::singleShot(8000, &app, []() { UpdateFlow::run(true, nullptr); });

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
