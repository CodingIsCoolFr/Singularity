#include "core/DiscordIdentity.h"

#include <QJsonDocument>

namespace DiscordIdentity {

namespace {
constexpr auto kChromeVersion = "136.0.0.0";
constexpr auto kElectronVersion = "35.3.0";
constexpr auto kClientVersion = "1.0.9202";
constexpr int kClientBuildNumber = 419246;
} // namespace

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
        {QStringLiteral("client_build_number"), kClientBuildNumber},
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
