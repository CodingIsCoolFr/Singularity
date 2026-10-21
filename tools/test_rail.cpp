// The server rail and the server organizer have no Discord traffic and no
// window of their own, so they are checked here: drags must never lose a
// folder (21 October: a closed folder came back empty from every drag, and
// Discord was told there were no folders), the organizer must put servers
// where a person would, and Discord's own cache must read back.
//
// Settings and the log go to a temporary folder, never the real ones.

#include "core/AppConfig.h"
#include "core/DiscordAppCache.h"
#include "core/Logger.h"
#include "plugin/builtin/ServerCategories.h"
#include "ui/RailLayout.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

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

using Entry = RailLayout::Entry;

static Entry loose(const char *id)
{
    Entry entry;
    entry.guildId = QString::fromLatin1(id);
    return entry;
}

static Entry folder(const char *id, bool open, QStringList guilds)
{
    Entry entry;
    entry.isFolder = true;
    entry.folder.id = QString::fromLatin1(id);
    entry.folder.name = QStringLiteral("Folder ") + entry.folder.id;
    entry.folder.open = open;
    entry.folder.hasColor = true;
    entry.folder.color = 0x3498DB;
    entry.folder.guildIds = guilds;
    return entry;
}

// "g1 [F1: a b c] [F2: d e] g2 g3", one letter or name per tile.
static QString shape(const QList<Entry> &entries)
{
    QStringList parts;
    for (const Entry &entry : entries) {
        if (entry.isFolder)
            parts << QStringLiteral("[%1: %2]").arg(entry.folder.id, entry.folder.guildIds.join(QLatin1Char(' ')));
        else
            parts << entry.guildId;
    }
    return parts.join(QLatin1Char(' '));
}

static RailLayout start()
{
    // F1 is closed: its servers have no rows on screen, which is what used
    // to empty it.
    RailLayout rail;
    rail.setEntries({loose("g1"), folder("101", false, {"a", "b", "c"}), folder("102", true, {"d", "e"}),
                     loose("g2"), loose("g3")});
    return rail;
}

static void check(const RailLayout &rail, const char *want, const char *name)
{
    const QString got = shape(rail.entries());
    const bool ok = got == QLatin1String(want);
    expect(ok, name);
    if (!ok)
        std::printf("     got  %s\n     want %s\n", qPrintable(got), want);
}

static void railTests()
{
    {
        RailLayout rail = start();
        expect(rail.moveGuildBeside("g3", "g1", false, false), "a loose server moves above another");
        check(rail, "g3 g1 [101: a b c] [102: d e] g2", "and the closed folder keeps all three servers");
    }
    {
        RailLayout rail = start();
        rail.moveGuildBeside("g1", "e", false, true);
        check(rail, "[101: a b c] [102: d e g1] g2 g3", "after a server in a folder joins that folder");
    }
    {
        RailLayout rail = start();
        rail.moveGuildBeside("g2", "d", false, false);
        check(rail, "g1 [101: a b c] [102: g2 d e] g3", "before a server in a folder joins it there");
    }
    {
        RailLayout rail = start();
        rail.moveGuildBeside("d", "g2", false, false);
        check(rail, "g1 [101: a b c] [102: e] d g2 g3", "out of a folder, above a loose server");
    }
    {
        RailLayout rail = start();
        rail.moveGuildBeside("g2", "102", true, true);
        check(rail, "g1 [101: a b c] [102: g2 d e] g3", "just under an open folder's tile is first inside it");
    }
    {
        RailLayout rail = start();
        rail.moveGuildBeside("g2", "101", true, true);
        check(rail, "g1 [101: a b c] g2 [102: d e] g3", "just under a closed folder's tile is after it");
    }
    {
        RailLayout rail = start();
        rail.moveGuildBeside("g3", "101", true, false);
        check(rail, "g1 g3 [101: a b c] [102: d e] g2", "above a folder's tile is before it");
    }
    {
        RailLayout rail = start();
        rail.moveGuildBeside("b", QString(), false, true);
        check(rail, "g1 [101: a c] [102: d e] g2 g3 b", "below every tile is the very end");
        rail.moveGuildBeside("b", QString(), false, false);
        check(rail, "b g1 [101: a c] [102: d e] g2 g3", "over direct messages is the very top");
    }
    {
        RailLayout rail = start();
        expect(!rail.moveGuildBeside("g2", "g2", false, true), "dropped on itself moves nothing");
        expect(!rail.moveGuildBeside("g2", "g3", false, false), "dropped just above the next server, where it is, moves nothing");
        check(rail, "g1 [101: a b c] [102: d e] g2 g3", "and the rail is as it was");
        expect(rail.moveGuildBeside("g2", "e", false, true), "under the last server of an open folder is a move into it");
    }
    {
        RailLayout rail;
        rail.setEntries({folder("201", false, {"x"}), loose("y")});
        expect(!rail.moveGuildBeside("x", "201", true, true), "a folder's only server dropped by its own tile stays");
        check(rail, "[201: x] y", "and the folder is still there");
        rail.moveGuildBeside("x", "y", false, true);
        check(rail, "y x", "the last server out of a folder takes the folder away");
    }
    {
        RailLayout rail = start();
        expect(rail.moveFolderBeside("101", "g3", false, true), "a folder moves after a loose server");
        check(rail, "g1 [102: d e] g2 g3 [101: a b c]", "with every server still inside");
        rail.moveFolderBeside("101", "d", false, false);
        check(rail, "g1 [102: d e] [101: a b c] g2 g3", "against a server in another folder it goes after that folder");
    }
    {
        // Every drag that used to empty a closed folder, then reconcile, then
        // what goes to Discord: the folder count must not drop.
        RailLayout rail = start();
        rail.moveGuildBeside("g3", "g1", false, false);
        rail.moveGuildBeside("g1", "g2", false, true);
        rail.moveGuildBeside("d", "g3", false, false);
        rail.moveGuildBeside("g2", "101", true, false);
        rail.reconcile({"g1", "g2", "g3", "a", "b", "c", "d", "e"});
        int folders = 0;
        for (const QJsonValue &value : rail.toDiscordFolders())
            folders += value.toObject().value(QStringLiteral("id")).isNull() ? 0 : 1;
        expect(folders == 2, "after many drags Discord is still sent both folders");
        expect(rail.folder(QStringLiteral("101")) && rail.folder(QStringLiteral("101"))->guildIds.size() == 3,
               "and the closed one still holds a, b and c");
    }
}

static ServerCategories::Facts facts(const char *name, QStringList channels, int members = 0, double voice = 0,
                                     QStringList features = {})
{
    ServerCategories::Facts f;
    f.name = QString::fromUtf8(name);
    f.channelNames = channels;
    f.channelCount = int(channels.size());
    f.memberCount = members;
    f.usualVoice = voice;
    f.features = features;
    return f;
}

static void expectCategory(const ServerCategories::Facts &f, const char *want, const char *name)
{
    const ServerCategories::Verdict verdict = ServerCategories::classify(f);
    const bool ok = verdict.categoryId == QLatin1String(want);
    expect(ok, name);
    if (!ok || true)
        std::printf("     %-14s %s\n", qPrintable(verdict.categoryId), qPrintable(verdict.reason));
}

static void gameTests()
{
    using ServerCategories::gamesNamedIn;
    // A slice of Discord's list, with a few that are also everyday words.
    ServerCategories::setKnownGames({"Project Zomboid", "Sons of the Forest", "The Forest", "s&box", "Subnautica",
                                     "Schedule I", "RV There Yet?", "Grounded", "Lethal Company", "VRChat",
                                     "Call of Duty", "Rocket League", "Matrix", "Chill", "Survival", "Social",
                                     "Minecraft", "VALORANT", "Beat Saber"});

    expect(gamesNamedIn("Subnautica (Official)") == QStringList{"Subnautica"}, "Subnautica (Official) is Subnautica");
    expect(gamesNamedIn("Sons Of | The Forest") == QStringList{"Sons of the Forest"},
           "Sons Of | The Forest is Sons of the Forest, not also The Forest");
    expect(gamesNamedIn("Unofficial Lethal Company Community") == QStringList{"Lethal Company"},
           "the words around a game's name are ignored");
    expect(gamesNamedIn("RV There Yet? Official Server") == QStringList{"RV There Yet?"}, "punctuation in names");
    expect(gamesNamedIn("s&box") == QStringList{"s&box"}, "s&box");
    expect(gamesNamedIn("Call of Duty Zombies") == QStringList{"Call of Duty"}, "a game inside a longer name");
    expect(gamesNamedIn("Matrix of Minds").isEmpty(), "a short game name inside a longer one does not count");
    expect(gamesNamedIn("ChillZone | Active").isEmpty(), "a game called Chill is not every chill server");
    expect(gamesNamedIn("Social").isEmpty(), "nor is one called Social");
    expect(gamesNamedIn("Survival Friends").isEmpty(), "an everyday word is not a game inside a name");
    expect(ServerCategories::isVrGame("VRChat") && ServerCategories::isVrGame("Beat Saber")
               && !ServerCategories::isVrGame("Subnautica"),
           "VR games are told apart");
}

static void categoryTests()
{
    using ServerCategories::fold;
    expect(fold(QStringLiteral("✝⋆-Counting-⋆✝")) == QLatin1String(" counting "), "decoration around a channel name goes");
    expect(fold(QString::fromUtf8("𝓖𝓸𝓽𝓱")) == QLatin1String(" goth "), "script letters read as plain ones");
    expect(fold(QString::fromUtf8("ɢᴏᴛʜ")) == QLatin1String(" goth "), "small capitals read as plain ones");
    expect(fold(QString::fromUtf8("Ｇｏｔｈ Café")) == QLatin1String(" goth cafe "), "full width and accents too");

    const QStringList gothChannels = {
        "✝⋆-General-✝⋆", "⋆✝-MEMES-✝⋆", "⋆✝-NSFW-✝⋆", "⋆✝-Art-✝⋆", "⋆✝-Gaming-✝⋆", "⋆✝-Food-porn-✝⋆",
        "⋆✝-Pet-Pics-✝⋆", "⋆✝-Commands-✝⋆", "✝⋆-Venting-⋆✝", "✝⋆-Face-Reveals-⋆✝", "✝⋆-Media-⋆✝",
        "✝⋆-Fact-of-the-Day-⋆✝", "✝⋆-Introductions-⋆✝", "✝⋆-Counting-⋆✝", "Waifu-Rolls", "Waifu-Trades",
        "Bot-Commands", "⋆✝-Levels-✝⋆", "✝⋆-Level-Chats-✝⋆", "dyno-updates", "scam-log"};
    expectCategory(facts("Goth", gothChannels, 1208, 6, {"COMMUNITY"}), "social",
                   "Goth, with its venting and face reveals, is Social");
    expectCategory(facts("Call of Duty Zombies", {"general", "lfg", "clips", "zombies-strats", "memes"}, 40000, 30,
                         {"COMMUNITY", "DISCOVERABLE"}),
                   "gaming", "Call of Duty Zombies is Gaming even with a crowd in voice");
    expectCategory(facts("the boys", {"general", "vc", "memes"}, 9, 2), "friends", "nine people is Friends");
    expectCategory(facts("CR", {"general", "clips", "voice"}, 14), "friends",
                   "a small private server with gaming clips is still Friends");
    expectCategory(facts("Anime Soul", {"general", "anime-discussion", "manga", "waifu-wars"}, 90000, 4,
                         {"COMMUNITY"}),
                   "anime", "Anime Soul is Anime & Art");
    expectCategory(facts("Lofi Beats Cafe", {"music-share", "producers", "playlists"}, 5000, 0, {"COMMUNITY"}),
                   "music", "a lofi server is Music");
    expectCategory(facts("Python Discord", {"help-python", "web-development", "career"}, 400000, 3, {"COMMUNITY"}),
                   "tech", "Python Discord is Tech");
    expectCategory(facts("Crypto Alpha Calls", {"btc", "eth", "trading-chat"}, 3000, 0, {"COMMUNITY"}), "money",
                   "a trading server is Money & Crypto");
    expectCategory(facts("Dyno", {"support", "faq", "bug-reports", "suggestions"}, 80000, 0,
                         {"COMMUNITY", "DEVELOPER_SUPPORT_SERVER"}),
                   "support", "a bot's support server is Apps & Support");
    expectCategory(facts("Rocket League Garage", {"trading", "general"}, 50000, 0, {"COMMUNITY"}), "gaming",
                   "Rocket League is not Music because it says rock");
    expectCategory(facts("Artillery Clan", {"general", "ranked"}, 300), "gaming",
                   "Artillery is not Art");
    expectCategory(facts("Active India", {"general", "chat", "voice-1", "voice-2"}, 20000, 12, {"COMMUNITY"}),
                   "social", "a big server where people sit in voice is Social");
    expectCategory(facts("Active India", {"general", "chat"}, 20000, 0, {"COMMUNITY"}), "communities",
                   "the same server with nobody in voice is just a Community");
    expectCategory(facts("zzz", {"general"}, 200), "other", "nothing to go on is Other");

    // What went wrong on 21 October, with the games list known.
    expectCategory(facts("Project Zomboid", {"general", "voice", "mods"}, 0, 3), "gaming",
                   "Project Zomboid with three people in voice is Gaming, not Social");
    expectCategory(facts("Sons Of | The Forest", {"general"}, 0, 1), "gaming", "Sons of the Forest is Gaming");
    expectCategory(facts("Subnautica (Official)", {"announcements", "support", "bug-reports", "faq", "feedback"},
                         90000, 0, {"COMMUNITY", "VERIFIED"}),
                   "gaming", "an official game server is Gaming, not Apps & Support");
    expectCategory(facts("Schedule I Official", {"general", "suggestions", "bug-reports"}, 40000, 1, {"COMMUNITY"}),
                   "gaming", "Schedule I Official is Gaming");
    expectCategory(facts("VRChat", {"general", "avatars", "worlds"}, 100000, 2, {"COMMUNITY"}), "vr", "VRChat is VR");
    expectCategory(facts("Standable", {"support", "announcements"}, 3000), "vr", "Standable is VR");
    expectCategory(facts("XSOverlay", {"support", "faq"}, 8000), "vr", "XSOverlay is VR");
    expectCategory(facts("Paper_Tracker", {"general", "setup-help"}, 2000), "vr", "Paper_Tracker is VR");
    expectCategory(facts("Vencord 🎃", {"support", "plugin-development", "faq"}, 60000, 0, {"COMMUNITY"}), "support",
                   "a client mod's server is Apps & Support");
    expectCategory(facts("Daniel's Mod", {"general", "downloads"}, 900), "support", "a mod's server is Apps & Support");
    expectCategory(facts("Matrix of Minds", {"general", "voice"}, 20), "friends",
                   "Matrix of Minds is not the game called Matrix");

    ServerCategories::Facts crimson = facts("King Crimson", {"general", "showcase"}, 400);
    crimson.linkedGames = {"VRChat"};
    expectCategory(crimson, "vr", "a server whose profile is linked to VRChat is VR");
    ServerCategories::Facts kitten = facts("Kitten Space Agency", {"general", "screenshots"}, 15000, 0, {"COMMUNITY"});
    kitten.linkedGames = {"a game"};
    expectCategory(kitten, "gaming", "a game Discord's list does not have is still a game through the profile");
    ServerCategories::Facts goth = facts("Goth", gothChannels, 1208, 6, {"COMMUNITY"});
    goth.linkedGames = {"VALORANT", "Minecraft"};
    expectCategory(goth, "social", "a hangout that plays games is still a hangout");
    ServerCategories::Facts juice = facts("Library of Juice WRLD", {"general", "unreleased"}, 3000, 0, {"COMMUNITY"});
    juice.traits = {"Music", "Hip Hop", "Leaks"};
    expectCategory(juice, "music", "a profile tagged Music and Hip Hop is Music");
}

// Every server in a file, one "name<TAB>usual voice" per line, sorted with
// Discord's real list of games: what the organizer would do with names alone.
static int sortFile(const QString &namesPath, const QString &gamesPath)
{
    QFile games(gamesPath);
    if (games.open(QIODevice::ReadOnly)) {
        QStringList names;
        const QJsonObject catalog = QJsonDocument::fromJson(games.readAll()).object();
        for (auto it = catalog.constBegin(); it != catalog.constEnd(); ++it) {
            const QJsonArray rows = it.value().isArray() ? it.value().toArray() : QJsonArray{it.value()};
            for (const QJsonValue &row : rows)
                names.append(row.toObject().value(QStringLiteral("n")).toString());
        }
        ServerCategories::setKnownGames(names);
    }
    std::printf("%d games known\n", ServerCategories::knownGameCount());
    QFile list(namesPath);
    if (!list.open(QIODevice::ReadOnly | QIODevice::Text))
        return 1;
    for (const QString &line : QString::fromUtf8(list.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList parts = line.split(QLatin1Char('\t'));
        ServerCategories::Facts f;
        f.name = parts.value(0).trimmed();
        f.usualVoice = parts.value(1).toDouble();
        const ServerCategories::Verdict verdict = ServerCategories::classify(f);
        std::printf("%-12s %-50s %s\n", qPrintable(verdict.categoryId), f.name.left(50).toUtf8().constData(),
                    qPrintable(verdict.reason));
    }
    return 0;
}

static void cacheTests()
{
    // Whatever this machine has. On the build machine Discord's own app has
    // been used, so the copies are listed for a person to compare.
    const QList<DiscordAppCache::FolderSnapshot> snapshots = DiscordAppCache::folderSnapshots();
    std::printf("     the Discord app's cache holds %d copies\n", int(snapshots.size()));
    for (const DiscordAppCache::FolderSnapshot &snapshot : snapshots) {
        std::printf("     %s %s: %d folders, %d servers\n", qPrintable(snapshot.app),
                    qPrintable(snapshot.savedAt.toString(Qt::ISODate)), snapshot.folderCount(),
                    snapshot.serverCount());
    }
    bool sane = true;
    for (const DiscordAppCache::FolderSnapshot &snapshot : snapshots)
        sane = sane && snapshot.serverCount() > 0 && snapshot.folderCount() <= snapshot.folders.size();
    expect(sane, "every copy read back has servers in it");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SingularityTests"));
    QCoreApplication::setApplicationName(QStringLiteral("test_rail"));

    QTemporaryDir scratch;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.path());
    Logger::useSideFile();

    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--sort"))
        return sortFile(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));

    railTests();
    gameTests();
    categoryTests();
    cacheTests();

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
