#include "core/DiscordIdentity.h"

#include "core/AppConfig.h"
#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QSysInfo>
#include <QThreadPool>
#include <QTimer>
#include <QVersionNumber>

#include <algorithm>
#include <functional>

namespace DiscordIdentity {

namespace {

// What Discord's own desktop app was on 21 October 2026. Only ever a floor:
// everything below is kept current by refreshIdentity(), and nothing it
// finds is allowed to move a number backwards.
struct Identity
{
    int clientBuild = 643186;             // the web build, discord.com/app
    QString hostVersion = QStringLiteral("1.0.9264");   // the desktop app, Discord's update server
    QString chrome = QStringLiteral("148.0.7778.280");  // from the official app, when installed
    QString electron = QStringLiteral("42.11.11");
    int nativeBuild = 94186;
};

QMutex g_lock;
Identity g_identity;

// How often a long session looks again. Discord ships a web build most days
// and the desktop app every week or two.
constexpr int RefreshMs = 6 * 60 * 60 * 1000;

const QString kClientBuildKey = QStringLiteral("identity/clientBuild");
const QString kHostVersionKey = QStringLiteral("identity/hostVersion");
const QString kChromeKey = QStringLiteral("identity/chrome");
const QString kElectronKey = QStringLiteral("identity/electron");
const QString kNativeBuildKey = QStringLiteral("identity/nativeBuild");
const QString kScannedAppKey = QStringLiteral("identity/scannedApp");

Identity snapshot()
{
    QMutexLocker locker(&g_lock);
    return g_identity;
}

bool newer(const QString &candidate, const QString &current)
{
    const QVersionNumber a = QVersionNumber::fromString(candidate);
    return !a.isNull() && a > QVersionNumber::fromString(current);
}

// Each of these takes a value only when it moves forward, remembers it for
// the next start, and says so in the log with where it came from.
void raiseClientBuild(int build, const char *source)
{
    if (build < 100000 || build > 100000000)
        return;
    {
        QMutexLocker locker(&g_lock);
        if (build <= g_identity.clientBuild)
            return;
        g_identity.clientBuild = build;
    }
    AppConfig::instance().setValue(kClientBuildKey, build);
    wlog(QStringLiteral("identity"), QStringLiteral("web build %1, from %2").arg(build).arg(QLatin1String(source)));
}

void raiseNativeBuild(int build, const char *source)
{
    if (build <= 0 || build > 100000000)
        return;
    {
        QMutexLocker locker(&g_lock);
        if (build <= g_identity.nativeBuild)
            return;
        g_identity.nativeBuild = build;
    }
    AppConfig::instance().setValue(kNativeBuildKey, build);
    wlog(QStringLiteral("identity"), QStringLiteral("native build %1, from %2").arg(build).arg(QLatin1String(source)));
}

void raiseVersion(QString Identity::*field, const QString &key, const QString &value, const char *what,
                  const char *source)
{
    {
        QMutexLocker locker(&g_lock);
        if (!newer(value, g_identity.*field))
            return;
        g_identity.*field = value;
    }
    AppConfig::instance().setValue(key, value);
    wlog(QStringLiteral("identity"), QStringLiteral("%1 %2, from %3")
                                         .arg(QLatin1String(what), value, QLatin1String(source)));
}

QNetworkAccessManager *network()
{
    static QPointer<QNetworkAccessManager> manager;
    if (!manager)
        manager = new QNetworkAccessManager(QCoreApplication::instance());
    return manager;
}

void fetch(const QUrl &url, std::function<void(const QByteArray &, int)> done)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    QNetworkReply *reply = network()->get(request);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done = std::move(done)]() {
        reply->deleteLater();
        done(reply->readAll(), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
    });
}

// The web build: discord.com/app is the web client's own page, and its
// GLOBAL_ENV says "BUILD_NUMBER":"643186" - the number it puts in its super
// properties.
void refreshWebBuild()
{
    fetch(QUrl(QStringLiteral("https://discord.com/app")), [](const QByteArray &page, int status) {
        static const QRegularExpression pattern(QStringLiteral("\"BUILD_NUMBER\"\\s*:\\s*\"(\\d+)\""));
        const QRegularExpressionMatch match = pattern.match(QString::fromUtf8(page));
        if (!match.hasMatch()) {
            wlog(QStringLiteral("identity"), QStringLiteral("could not read the web build (HTTP %1); staying on %2")
                                                 .arg(status)
                                                 .arg(clientBuildNumber()));
            return;
        }
        raiseClientBuild(match.captured(1).toInt(), "discord.com");
    });
}

// The desktop app's version: the manifest Discord's own updater reads,
// {"full": {"host_version": [1, 0, 9264], ...}, ...}.
void refreshHostVersion()
{
    const QUrl url(QStringLiteral("https://discord.com/api/updates/distributions/app/manifests/latest"
                                  "?channel=stable&platform=win&arch=x64"));
    fetch(url, [](const QByteArray &body, int status) {
        const QJsonArray parts =
            QJsonDocument::fromJson(body).object().value(QStringLiteral("full")).toObject().value(QStringLiteral("host_version")).toArray();
        QStringList numbers;
        for (const QJsonValue &part : parts)
            numbers << QString::number(part.toInt());
        if (numbers.size() != 3) {
            wlog(QStringLiteral("identity"), QStringLiteral("could not read the desktop version (HTTP %1)").arg(status));
            return;
        }
        raiseVersion(&Identity::hostVersion, kHostVersionKey, numbers.join(QLatin1Char('.')), "desktop app",
                     "Discord's update server");
    });
}

// What only the app itself knows - the Chrome and Electron it runs on and
// its native build - read from Discord's own app when it is installed here.
// Discord.exe is a couple of hundred megabytes, so it is read once per app
// version, off the window's thread.
void readInstalledApp()
{
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    const QString appData = qEnvironmentVariable("APPDATA");
    if (localAppData.isEmpty())
        return;

    const QDir root(localAppData + QStringLiteral("/Discord"));
    QString newest;
    for (const QString &dir : root.entryList({QStringLiteral("app-*")}, QDir::Dirs)) {
        if (newest.isEmpty() || newer(dir.mid(4), newest.mid(4)))
            newest = dir;
    }
    if (newest.isEmpty())
        return;   // no official app on this computer: the remembered values stand
    const QString appDir = root.filePath(newest);
    const QString version = newest.mid(4);
    const bool alreadyRead = AppConfig::instance().value(kScannedAppKey).toString() == version;

    QThreadPool::globalInstance()->start([appDir, appData, version, alreadyRead]() {
        QString chrome;
        QString electron;
        if (!alreadyRead) {
            QFile exe(appDir + QStringLiteral("/Discord.exe"));
            if (exe.open(QIODevice::ReadOnly)) {
                if (uchar *bytes = exe.map(0, exe.size())) {
                    const QByteArrayView all(reinterpret_cast<const char *>(bytes), exe.size());
                    static const QRegularExpression ua(QStringLiteral("Chrome/(\\d+\\.\\d+\\.\\d+\\.\\d+) Electron/(\\d+\\.\\d+\\.\\d+)"));
                    // The user agent template sits in the text section; look
                    // at each "Chrome/" rather than decoding the whole file.
                    for (qsizetype at = all.indexOf("Chrome/"); at >= 0 && chrome.isEmpty();
                         at = all.indexOf("Chrome/", at + 7)) {
                        const QString near = QString::fromLatin1(all.mid(at, 64).toByteArray());
                        const QRegularExpressionMatch match = ua.match(near);
                        if (match.hasMatch() && match.capturedStart() == 0) {
                            chrome = match.captured(1);
                            electron = match.captured(2);
                        }
                    }
                    exe.unmap(bytes);
                }
            }
        }

        // Its voice engine writes the native build into its log at start.
        int nativeBuild = 0;
        QFile log(appData + QStringLiteral("/discord/logs/discord_media_rCURRENT.log"));
        if (log.open(QIODevice::ReadOnly) && log.size() < 64LL * 1024 * 1024) {
            static const QRegularExpression native(QStringLiteral("nativeBuildNumber\"\\s*:\\s*(\\d+)"));
            auto it = native.globalMatch(QString::fromLatin1(log.readAll()));
            while (it.hasNext())
                nativeBuild = std::max(nativeBuild, it.next().captured(1).toInt());
        }

        QMetaObject::invokeMethod(QCoreApplication::instance(), [version, chrome, electron, nativeBuild, alreadyRead]() {
            raiseVersion(&Identity::hostVersion, kHostVersionKey, version, "desktop app", "the installed Discord");
            if (!chrome.isEmpty()) {
                raiseVersion(&Identity::chrome, kChromeKey, chrome, "Chrome", "the installed Discord");
                raiseVersion(&Identity::electron, kElectronKey, electron, "Electron", "the installed Discord");
            }
            if (!alreadyRead)
                AppConfig::instance().setValue(kScannedAppKey, version);
            raiseNativeBuild(nativeBuild, "the installed Discord's log");
        });
    });
}

void refreshAll()
{
    refreshWebBuild();
    refreshHostVersion();
    readInstalledApp();
}

} // namespace

int clientBuildNumber()
{
    return snapshot().clientBuild;
}

void refreshIdentity()
{
    // Last time's values first, so this start already speaks as current a
    // client as the last one did.
    AppConfig &config = AppConfig::instance();
    raiseClientBuild(config.value(kClientBuildKey, 0).toInt(), "last time");
    raiseNativeBuild(config.value(kNativeBuildKey, 0).toInt(), "last time");
    raiseVersion(&Identity::hostVersion, kHostVersionKey, config.value(kHostVersionKey).toString(), "desktop app",
                 "last time");
    raiseVersion(&Identity::chrome, kChromeKey, config.value(kChromeKey).toString(), "Chrome", "last time");
    raiseVersion(&Identity::electron, kElectronKey, config.value(kElectronKey).toString(), "Electron", "last time");

    refreshAll();

    // A session left open for days keeps up as well.
    auto *timer = new QTimer(QCoreApplication::instance());
    timer->setInterval(RefreshMs);
    QObject::connect(timer, &QTimer::timeout, timer, []() { refreshAll(); });
    timer->start();

    const Identity now = snapshot();
    wlog(QStringLiteral("identity"), QStringLiteral("speaking as Discord %1 (web build %2, native %3), Electron %4, "
                                                    "Windows %5; checking for newer")
                                         .arg(now.hostVersion)
                                         .arg(now.clientBuild)
                                         .arg(now.nativeBuild)
                                         .arg(now.electron, QSysInfo::kernelVersion()));
}

QString userAgent()
{
    const Identity now = snapshot();
    return QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                          "discord/%1 Chrome/%2 Electron/%3 Safari/537.36")
        .arg(now.hostVersion, now.chrome, now.electron);
}

QJsonObject superProperties()
{
    const Identity now = snapshot();
    return QJsonObject{
        {QStringLiteral("os"), QStringLiteral("Windows")},
        {QStringLiteral("browser"), QStringLiteral("Discord Client")},
        {QStringLiteral("release_channel"), QStringLiteral("stable")},
        {QStringLiteral("client_version"), now.hostVersion},
        // This computer's own, as Discord's app reports it: "10.0.26300".
        {QStringLiteral("os_version"), QSysInfo::kernelVersion()},
        {QStringLiteral("os_arch"), QStringLiteral("x64")},
        {QStringLiteral("app_arch"), QStringLiteral("x64")},
        {QStringLiteral("system_locale"), QStringLiteral("en-US")},
        {QStringLiteral("has_client_mods"), false},
        {QStringLiteral("browser_user_agent"), userAgent()},
        {QStringLiteral("browser_version"), now.electron},
        {QStringLiteral("client_build_number"), now.clientBuild},
        {QStringLiteral("native_build_number"), now.nativeBuild},
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
