#include "core/AppConfig.h"
#include "core/GatewayClient.h"
#include "core/Logger.h"
#include "core/MessageStore.h"
#include "core/RestClient.h"
#include "plugin/PluginHost.h"
#include "ui/LoginDialog.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QIcon>
#include <QSurfaceFormat>

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
    app.setApplicationVersion(QStringLiteral("0.1.1"));
    app.setWindowIcon(QIcon(QStringLiteral(":/brand/singularity.png")));

    Theme::applySeed(QColor(AppConfig::instance().value(QStringLiteral("appearance/themeSeed"),
                                                       QLatin1String(Theme::DefaultSeed)).toString()));
    app.setStyleSheet(Theme::applicationStyleSheet());

    // The title bar belongs to Windows, not to Qt, so it has to be coloured
    // separately or it sits above the app as a paler strip.
    Theme::installDarkTitleBars();

    // Touch the log first so the file exists even if startup fails early.
    wlog(QStringLiteral("app"), QStringLiteral("Singularity %1 starting").arg(app.applicationVersion()));

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
