#include "core/DiscordIdentity.h"

#include "core/AppConfig.h"
#include "core/Logger.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>

#include <algorithm>
#include <atomic>

namespace DiscordIdentity {

namespace {
// Discord's own desktop app as of 21 October 2026: 1.0.9262 on Electron 42.
constexpr auto kChromeVersion = "148.0.7778.280";
constexpr auto kElectronVersion = "42.11.11";
constexpr auto kClientVersion = "1.0.9262";

// The web build on the same day, the floor for clientBuildNumber().
constexpr int kClientBuildNumber = 643186;

std::atomic<int> g_clientBuild{kClientBuildNumber};

const QString kBuildKey = QStringLiteral("identity/clientBuild");
} // namespace

int clientBuildNumber()
{
    return g_clientBuild.load();
}

void setClientBuildNumber(int build)
{
    g_clientBuild.store(std::max(build, kClientBuildNumber));
}

void refreshClientBuild()
{
    setClientBuildNumber(AppConfig::instance().value(kBuildKey, 0).toInt());

    // One request a start, for a page anybody can fetch. Its GLOBAL_ENV says
    // "BUILD_NUMBER":"643186" - the same number the web app puts in its own
    // super properties.
    auto *network = new QNetworkAccessManager(QCoreApplication::instance());
    QNetworkRequest request(QUrl(QStringLiteral("https://discord.com/app")));
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    QNetworkReply *reply = network->get(request);
    QObject::connect(reply, &QNetworkReply::finished, network, [reply, network]() {
        reply->deleteLater();
        network->deleteLater();
        const QString page = QString::fromUtf8(reply->readAll());
        static const QRegularExpression pattern(QStringLiteral("\"BUILD_NUMBER\"\\s*:\\s*\"(\\d+)\""));
        const QRegularExpressionMatch match = pattern.match(page);
        const int build = match.hasMatch() ? match.captured(1).toInt() : 0;
        if (build < 100000 || build > 100000000) {
            wlog(QStringLiteral("identity"), QStringLiteral("could not read Discord's build number (HTTP %1); "
                                                            "staying on %2")
                                                 .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                                                 .arg(clientBuildNumber()));
            return;
        }
        const int before = clientBuildNumber();
        setClientBuildNumber(build);
        AppConfig::instance().setValue(kBuildKey, build);
        if (build != before)
            wlog(QStringLiteral("identity"), QStringLiteral("Discord's web build is %1 (was %2)").arg(build).arg(before));
    });
}

QString userAgent()
{
    return QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                          "discord/%1 Chrome/%2 Electron/%3 Safari/537.36")
        .arg(QLatin1String(kClientVersion), QLatin1String(kChromeVersion), QLatin1String(kElectronVersion));
}

QJsonObject superProperties()
{
    return QJsonObject{
        {QStringLiteral("os"), QStringLiteral("Windows")},
        {QStringLiteral("browser"), QStringLiteral("Discord Client")},
        {QStringLiteral("release_channel"), QStringLiteral("stable")},
        {QStringLiteral("client_version"), QLatin1String(kClientVersion)},
        {QStringLiteral("os_version"), QStringLiteral("10.0.26100")},
        {QStringLiteral("os_arch"), QStringLiteral("x64")},
        {QStringLiteral("app_arch"), QStringLiteral("x64")},
        {QStringLiteral("system_locale"), QStringLiteral("en-US")},
        {QStringLiteral("has_client_mods"), false},
        {QStringLiteral("browser_user_agent"), userAgent()},
        {QStringLiteral("browser_version"), QLatin1String(kElectronVersion)},
        {QStringLiteral("client_build_number"), clientBuildNumber()},
        {QStringLiteral("native_build_number"), 68493},
        {QStringLiteral("client_event_source"), QJsonValue::Null},
        {QStringLiteral("client_launch_id"), QStringLiteral("")},
        {QStringLiteral("client_heartbeat_session_id"), QStringLiteral("")},
    };
}

QByteArray superPropertiesHeader()
{
    const QJsonDocument doc(superProperties());
    return doc.toJson(QJsonDocument::Compact).toBase64();
}

QString restBase()
{
    return QStringLiteral("https://discord.com/api/v%1").arg(ApiVersion);
}

QString gatewayUrl()
{
    return QStringLiteral("wss://gateway.discord.gg/?v=%1&encoding=json").arg(ApiVersion);
}

} // namespace DiscordIdentity
