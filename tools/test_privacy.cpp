// What the Anonymous plugin's privacy page promises, checked against the code.
//
// The page once said Singularity sent no read receipts, no real Windows
// version and looked at no programs; all three had stopped being true while
// nobody was looking. So each promise that a test can check is checked here,
// and the release build runs this.
//
//   pictures   only from the hosts the page names, never a look-alike
//   identity   the client details carry no identifier of their own, and only
//              the fields known today; a new one fails here until it is
//              looked at and the page says what it is
//   telemetry  no request path for Discord's analytics, metrics or Sentry
//              anywhere in the source
//   programs   with games off and no page watching, nothing lists them
//
// Nothing here talks to the network: every Qt request goes to a closed port,
// and settings live in a temporary folder.

#include "core/DiscordIdentity.h"
#include "core/GameActivity.h"
#include "core/Logger.h"
#include "ui/MediaCache.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdio>
#include <memory>

static int g_failed = 0;
static int g_passed = 0;

static void expect(bool ok, const char *name)
{
    if (ok) {
        ++g_passed;
        std::printf("ok   %s\n", name);
    } else {
        ++g_failed;
        std::printf("FAIL %s\n", name);
    }
}

namespace {

bool allowed(const char *url)
{
    return MediaCache::isAllowedHost(QUrl(QString::fromLatin1(url)));
}

void pictureTests()
{
    expect(allowed("https://cdn.discordapp.com/avatars/1/2.png"), "pictures: Discord's CDN");
    expect(allowed("https://media.discordapp.net/attachments/1/2/a.png"), "pictures: Discord's media proxy");
    expect(allowed("https://discord.com/assets/badge.png"), "pictures: discord.com itself");
    expect(allowed("https://media.tenor.com/x.gif"), "pictures: Tenor");
    expect(allowed("https://i.scdn.co/image/abc"), "pictures: Spotify album art");
    expect(allowed("https://singularitycord.pages.dev/mark.png"), "pictures: Singularity's site");
    expect(allowed("https://abc123.singularitycord.pages.dev/mark.png"), "pictures: a preview of Singularity's site");

    // Look-alikes. The first passed until 0.9.3: the suffix had no dot.
    expect(!allowed("https://evilsingularitycord.pages.dev/x.png"), "pictures: refuses a look-alike Pages site");
    expect(!allowed("https://evil.pages.dev/x.png"), "pictures: refuses any other Pages site");
    expect(!allowed("https://notdiscord.com/x.png"), "pictures: refuses notdiscord.com");
    expect(!allowed("https://discordapp.com.evil.net/x.png"), "pictures: refuses a name that only starts like Discord's");
    expect(!allowed("https://xdiscordapp.net/x.png"), "pictures: refuses xdiscordapp.net");
    expect(!allowed("http://cdn.discordapp.com/x.png"), "pictures: refuses plain http");
    expect(!allowed("https://example.com/x.png"), "pictures: refuses an ordinary site");
}

void identityTests()
{
    const QJsonObject props = DiscordIdentity::superProperties();

    expect(props.value(QStringLiteral("client_launch_id")).toString().isEmpty(),
           "identity: no launch id ties sessions together");
    expect(props.value(QStringLiteral("client_heartbeat_session_id")).toString().isEmpty(),
           "identity: no heartbeat session id");
    expect(props.value(QStringLiteral("system_locale")).toString() == QLatin1String("en-US"),
           "identity: the language is always en-US");

    // Exactly the fields the privacy page accounts for. Adding one is fine;
    // adding one without saying what it is on the page is what this stops.
    const QSet<QString> known{
        QStringLiteral("os"), QStringLiteral("browser"), QStringLiteral("release_channel"),
        QStringLiteral("client_version"), QStringLiteral("os_version"), QStringLiteral("os_arch"),
        QStringLiteral("app_arch"), QStringLiteral("system_locale"), QStringLiteral("has_client_mods"),
        QStringLiteral("browser_user_agent"), QStringLiteral("browser_version"),
        QStringLiteral("client_build_number"), QStringLiteral("native_build_number"),
        QStringLiteral("client_event_source"), QStringLiteral("client_launch_id"),
        QStringLiteral("client_heartbeat_session_id"),
    };
    QStringList unknown;
    for (auto it = props.begin(); it != props.end(); ++it) {
        if (!known.contains(it.key()))
            unknown << it.key();
    }
    if (!unknown.isEmpty())
        std::printf("     new fields: %s\n", qPrintable(unknown.join(QStringLiteral(", "))));
    expect(unknown.isEmpty(), "identity: no field the privacy page does not account for");
}

void telemetryTests()
{
    // Request paths are written as string literals ("/channels/%1/..."), so
    // a call to one of these would put its path in quotes in the source.
    const QStringList forbidden{
        QStringLiteral("\"/science"),
        QStringLiteral("\"/metrics"),
        QStringLiteral("/track\""),
        QStringLiteral("ingest.sentry.io"),
        QStringLiteral("sentry.io/api"),
        QStringLiteral("sentry_key="),
    };
    const QString root = QStringLiteral(SINGULARITY_SOURCE_DIR "/src");
    int files = 0;
    QStringList found;
    QDirIterator it(root, {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        // The speech engine's bundled C API header is someone else's code.
        if (path.contains(QStringLiteral("/sherpa/")))
            continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        ++files;
        const QString text = QString::fromUtf8(file.readAll());
        for (const QString &needle : forbidden) {
            if (text.contains(needle))
                found << QStringLiteral("%1 in %2").arg(needle, path.mid(root.size() + 1));
        }
    }
    for (const QString &hit : found)
        std::printf("     %s\n", qPrintable(hit));
    expect(files > 100, "telemetry: the source tree was found and read");
    expect(found.isEmpty(), "telemetry: no analytics, metrics or Sentry request path in the source");
}

void programTests()
{
    // Games off is the default. The settings file is a fresh temporary one.
    auto games = std::make_unique<GameActivity>();
    games->setShown(false);
    games->start(nullptr);
    expect(games->running().isEmpty(), "programs: games off, nothing open: no program is listed");

    // The Activity page open: the list is kept, and dropped when it closes.
    auto page = std::make_unique<QObject>();
    games->watchRunning(page.get());
    page.reset();
    expect(games->running().isEmpty(), "programs: the page closed: the list is dropped again");
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SingularityTests"));
    QCoreApplication::setApplicationName(QStringLiteral("test_privacy"));

    QTemporaryDir scratch;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.path());
    Logger::useSideFile();
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 9));

    pictureTests();
    identityTests();
    telemetryTests();
    programTests();

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
