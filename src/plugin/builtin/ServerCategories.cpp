#include "plugin/builtin/ServerCategories.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ServerCategories {

namespace {

// A word or phrase to look for.
//
//   "word"   a word of four letters or more also matches as the start of a
//            longer one ("game" finds "games" and "gamer"); three letters or
//            fewer only ever matches the whole word, so "art" never finds
//            "artillery" nor "rap" "rapid".
//   "word="  the whole word only, whatever its length.
//   "^word"  only in the server's own words - its name, description and
//            traits - never in channel or role names. "mod" says a lot about
//            a server called "Daniel's Mod" and nothing about one that has a
//            #mod-log, which is every server.
struct Keywords
{
    const char *category;
    QList<const char *> words;
};

const QList<Keywords> &keywordTable()
{
    static const QList<Keywords> table = {
        {"gaming",
         {"gaming", "game", "gamer", "esport", "lfg", "looking for group", "clan", "tournament", "scrim",
          "ranked", "speedrun", "patch notes", "valorant", "fortnite", "minecraft", "roblox", "overwatch",
          "apex", "league of legends", "genshin", "honkai", "pokemon", "call of duty", "cod", "warzone",
          "zombies", "destiny", "halo=", "csgo", "counter strike", "cs2", "dota", "smash", "nintendo",
          "playstation", "xbox", "steam=", "rust=", "gta", "fivem", "red dead", "marvel rivals", "deadlock=",
          "elden ring", "dark souls", "warframe", "tarkov", "dayz", "osu=", "terraria", "stardew", "hoi4",
          "among us", "rocket league", "rainbow six", "r6", "pubg", "battlefield", "splatoon", "zelda",
          "final fantasy", "ffxiv", "warcraft", "diablo", "path of exile", "runescape", "osrs", "mmo", "mmorpg",
          "rpg", "fps", "moba", "battle royale", "twitch", "streamer", "clips", "smp", "bedwars", "skyblock",
          "hypixel", "modpack", "dead by daylight", "dbd", "the finals", "speedrunning", "^modding",
          "^server ip", "^gamers"}},
        {"vr",
         {"vr", "vrchat", "vrc", "virtual reality", "steamvr", "^oculus", "^meta quest", "^quest 2", "^quest 3",
          "^index", "fbt", "full body", "slimevr", "^tundra", "^vive", "^pico", "xsoverlay", "^ovr", "ovr toolkit",
          "alvr", "virtual desktop", "standable", "vrcx", "face track", "eye track", "facial track", "^babble",
          "paper tracker", "haritora", "mocopi", "chilloutvr", "resonite", "neosvr", "rec room", "beat saber",
          "gorilla tag", "avatar world", "vrc avatar", "^avatars"}},
        {"social",
         {"social", "hangout", "hang out", "chill", "vibe", "lounge", "cafe", "make friends", "new friends",
          "meet people", "dating", "e girl", "egirl", "eboy", "e boy", "goth", "emo", "alt", "alternative",
          "grunge", "teen", "late night", "night owl", "insomnia", "vc", "voice chat", "talk", "yap",
          "selfie", "face reveal", "faces=", "introduc", "intro", "intros=", "venting", "vent=",
          "confession", "qotd", "question of the day", "counting", "pet", "pets=", "food", "cooking",
          "meme", "media=", "pics=", "irl", "outfit", "fashion", "fit check", "nsfw", "simp", "hot take",
          "debate", "rant=", "rants=", "sleep call", "sleep vc", "cuddle", "lgbt", "queer", "pride=",
          "trans=", "relationship", "active vc", "active voice", "active chat", "chatting", "^chat="}},
        {"friends",
         {"friends", "friend group", "bestie", "besties", "bff", "bffs", "homie", "homies", "the boys",
          "the gang", "fam", "family", "squad", "crew=", "gang=", "bros", "brothers"}},
        {"anime",
         {"anime", "manga", "otaku", "weeb", "waifu", "husbando", "kawaii", "cosplay", "vtuber", "hololive",
          "nijisanji", "jujutsu", "naruto", "one piece", "demon slayer", "dragon ball", "attack on titan",
          "chainsaw man", "jojo", "mudae", "karuta", "sofi", "gacha", "art", "arts", "artist", "artwork",
          "drawing", "draw", "sketch", "doodle", "commission", "illustrat", "animation", "animator",
          "fanart", "fan art", "digital art", "oc", "ocs", "furry", "furries", "fursona", "pixel art",
          "writing", "writer", "poetry", "roleplay", "rp", "photography"}},
        {"music",
         {"music", "musician", "producer", "producing", "beats", "beatmaker", "hip hop", "hiphop", "rap",
          "rapper", "rnb", "rock=", "metal=", "punk", "edm", "techno", "house music", "lofi", "lo fi", "dj",
          "djs", "spotify", "soundcloud", "bandcamp", "band=", "bands=", "singer", "singing", "vocal",
          "lyric", "song", "playlist", "concert", "album", "fl studio", "ableton", "mixing", "mastering",
          "guitar", "piano", "drum", "orchestra", "kpop", "k pop", "jpop", "^leaks", "^snippets"}},
        {"tech",
         {"coding", "code=", "coder", "programming", "programmer", "developer", "dev", "devs",
          "development", "software", "python", "javascript", "typescript", "java", "cpp", "csharp",
          "golang", "rust=", "linux", "windows=", "android", "ios", "macos", "hacking", "hacker", "cyber",
          "infosec", "tech=", "technology", "technical", "pc build", "pc help", "hardware", "overclock",
          "gpu", "cpu", "ai", "artificial intelligence", "machine learning", "llm", "llms", "chatgpt",
          "openai", "claude", "anthropic", "midjourney", "stable diffusion", "comfyui", "github", "open source",
          "unity=", "unreal", "godot", "gamedev", "game dev", "blender", "3d", "web dev", "webdev", "api",
          "homelab", "raspberry", "arduino", "electronics", "engineer", "math", "maths", "science", "physics",
          "robotic", "hosting", "vps"}},
        {"money",
         {"crypto", "bitcoin", "btc", "eth", "ethereum", "solana", "nft", "nfts", "web3", "defi", "airdrop",
          "memecoin", "meme coin", "trading", "trader", "stocks", "stock market", "forex", "invest",
          "finance", "money", "hustle", "entrepreneur", "business", "ecommerce", "dropship", "reselling",
          "resell", "sneaker", "casino", "gambling", "betting"}},
        // "Official" is not here: every game's own server says it, and those
        // are game servers.
        {"support",
         {"support", "helpdesk", "help desk", "bug report", "feedback", "suggestion", "changelog", "faq",
          "docs", "documentation", "premium", "dashboard", "bot", "bots", "^beta=", "^tester", "dyno", "mee6",
          "carl bot", "tatsu", "dank memer", "wallpaper engine", "vencord", "betterdiscord", "plugin",
          "^mod=", "^mods=", "^client", "^tool", "^tools", "^app=", "^apps=", "^launcher", "^extension"}},
    };
    return table;
}

struct Needle
{
    QString text;   // " word" - a space in front so it only starts a word
    bool whole = false;
    bool ownOnly = false;
};

QHash<QString, QList<Needle>> compileNeedles()
{
    QHash<QString, QList<Needle>> needles;
    for (const Keywords &group : keywordTable()) {
        QList<Needle> &list = needles[QString::fromLatin1(group.category)];
        for (const char *raw : group.words) {
            QString word = QString::fromLatin1(raw);
            Needle needle;
            needle.ownOnly = word.startsWith(QLatin1Char('^'));
            if (needle.ownOnly)
                word.remove(0, 1);
            needle.whole = word.endsWith(QLatin1Char('='));
            if (needle.whole)
                word.chop(1);
            if (word.section(QLatin1Char(' '), -1).size() <= 3)
                needle.whole = true;
            needle.text = QLatin1Char(' ') + word;
            list.append(needle);
        }
    }
    return needles;
}

const QHash<QString, QList<Needle>> &needles()
{
    static const QHash<QString, QList<Needle>> compiled = compileNeedles();
    return compiled;
}

bool found(const QString &haystack, const Needle &needle)
{
    qsizetype from = 0;
    for (;;) {
        const qsizetype at = haystack.indexOf(needle.text, from);
        if (at < 0)
            return false;
        const qsizetype end = at + needle.text.size();
        if (!needle.whole || (end < haystack.size() && haystack.at(end) == QLatin1Char(' ')))
            return true;
        from = at + 1;
    }
}

// The words of one category found in one piece of folded text.
QStringList hits(const QString &folded, const QString &category, bool ownWords)
{
    QStringList words;
    for (const Needle &needle : needles().value(category)) {
        if ((ownWords || !needle.ownOnly) && found(folded, needle))
            words.append(needle.text.mid(1));
    }
    return words;
}

// Every single word any category looks for. A game named after one of them -
// there are games called "Social" and "Chill" - is never matched by that word
// alone, or every hangout would be a game server.
const QSet<QString> &keywordWords()
{
    static const QSet<QString> words = [] {
        QSet<QString> out;
        for (const Keywords &group : keywordTable()) {
            for (const char *raw : group.words) {
                QString word = QString::fromLatin1(raw);
                word.remove(QLatin1Char('^'));
                word.remove(QLatin1Char('='));
                if (!word.contains(QLatin1Char(' ')))
                    out.insert(word);
            }
        }
        return out;
    }();
    return words;
}

// Small capitals have no plain letter to fold to in Unicode, so they are
// mapped by hand: ɢᴏᴛʜ is "goth".
QChar unsmallCap(QChar c)
{
    switch (c.unicode()) {
    case 0x1D00: return QLatin1Char('a');
    case 0x0299: return QLatin1Char('b');
    case 0x1D04: return QLatin1Char('c');
    case 0x1D05: return QLatin1Char('d');
    case 0x1D07: return QLatin1Char('e');
    case 0xA730: return QLatin1Char('f');
    case 0x0262: return QLatin1Char('g');
    case 0x029C: return QLatin1Char('h');
    case 0x026A: return QLatin1Char('i');
    case 0x1D0A: return QLatin1Char('j');
    case 0x1D0B: return QLatin1Char('k');
    case 0x029F: return QLatin1Char('l');
    case 0x1D0D: return QLatin1Char('m');
    case 0x0274: return QLatin1Char('n');
    case 0x1D0F: return QLatin1Char('o');
    case 0x1D18: return QLatin1Char('p');
    case 0x0280: return QLatin1Char('r');
    case 0xA731: return QLatin1Char('s');
    case 0x1D1B: return QLatin1Char('t');
    case 0x1D1C: return QLatin1Char('u');
    case 0x1D20: return QLatin1Char('v');
    case 0x1D21: return QLatin1Char('w');
    case 0x028F: return QLatin1Char('y');
    case 0x1D22: return QLatin1Char('z');
    default: return c;
    }
}

bool hasFeature(const QStringList &features, const char *name)
{
    return features.contains(QLatin1String(name));
}

// ---------------------------------------------------------------------------
// Known games
// ---------------------------------------------------------------------------

struct GameIndex
{
    QHash<QString, QString> byWords;   // folded words -> the game's own name
    int longest = 1;                   // most words in any one name
};

GameIndex &gameIndex()
{
    static GameIndex index;
    return index;
}

// Words a server puts around its game's name: "Subnautica (Official)",
// "Unofficial Lethal Company Community", "RV There Yet? Official Server".
const QSet<QString> &fillerWords()
{
    static const QSet<QString> words = {
        QStringLiteral("official"), QStringLiteral("unofficial"), QStringLiteral("community"),
        QStringLiteral("server"),   QStringLiteral("discord"),    QStringLiteral("hub"),
        QStringLiteral("fan"),      QStringLiteral("fans"),       QStringLiteral("club"),
        QStringLiteral("group"),    QStringLiteral("the"),        QStringLiteral("game"),
        QStringLiteral("players"),  QStringLiteral("wiki"),       QStringLiteral("en"),
    };
    return words;
}

// Long single words that are also game names but mean nothing on their own
// inside a longer server name.
const QSet<QString> &everydayWords()
{
    static const QSet<QString> words = {
        QStringLiteral("survival"),  QStringLiteral("paradise"),   QStringLiteral("together"),
        QStringLiteral("creative"),  QStringLiteral("infinity"),   QStringLiteral("adventure"),
        QStringLiteral("champions"), QStringLiteral("simulator"),  QStringLiteral("multiplayer"),
        QStringLiteral("roleplay"),  QStringLiteral("community"),  QStringLiteral("everything"),
        QStringLiteral("anything"),  QStringLiteral("something"),  QStringLiteral("hangout"),
        QStringLiteral("friends"),   QStringLiteral("socialize"),  QStringLiteral("graveyard"),
        QStringLiteral("universe"),  QStringLiteral("kingdom"),    QStringLiteral("generation"),
        QStringLiteral("evolution"), QStringLiteral("revolution"), QStringLiteral("dimension"),
    };
    return words;
}

// Whether a name found by its words counts. The whole of a server's name may
// be any game four letters or longer that is not also one of our keywords; a
// part of it must be long or several words, so "Matrix of Minds" is not taken
// for a game called "Matrix".
bool acceptable(const QString &key, bool wholeName)
{
    const bool oneWord = !key.contains(QLatin1Char(' '));
    if (oneWord && keywordWords().contains(key))
        return false;
    if (wholeName)
        return key.size() >= 4;
    if (oneWord)
        return key.size() >= 8 && !everydayWords().contains(key);
    return key.size() >= 6;
}

} // namespace

const QList<Category> &all()
{
    static const QList<Category> categories = {
        {QStringLiteral("social"), QStringLiteral("Social"), 0xE91E63},
        {QStringLiteral("friends"), QStringLiteral("Friends"), 0xF1C40F},
        {QStringLiteral("gaming"), QStringLiteral("Gaming"), 0x2ECC71},
        {QStringLiteral("vr"), QStringLiteral("VR"), 0xE74C3C},
        {QStringLiteral("anime"), QStringLiteral("Anime & Art"), 0x9B59B6},
        {QStringLiteral("music"), QStringLiteral("Music"), 0xE67E22},
        {QStringLiteral("tech"), QStringLiteral("Tech"), 0x3498DB},
        {QStringLiteral("money"), QStringLiteral("Money & Crypto"), 0x1ABC9C},
        {QStringLiteral("support"), QStringLiteral("Apps & Support"), 0x95A5A6},
        {QStringLiteral("communities"), QStringLiteral("Communities"), 0x206694},
        {QStringLiteral("other"), QStringLiteral("Other"), 0x979C9F},
    };
    return categories;
}

const Category *find(const QString &id)
{
    for (const Category &category : all()) {
        if (category.id == id)
            return &category;
    }
    return nullptr;
}

QString fold(const QString &text)
{
    // Compatibility decomposition turns 𝓖𝓸𝓽𝓱, Ｇｏｔｈ and Ⓖⓞⓣⓗ into plain
    // letters and splits accents off, to be dropped below.
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);

    QString out(1, QLatin1Char(' '));
    out.reserve(decomposed.size() + 2);
    bool space = true;
    for (QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing)
            continue;
        c = unsmallCap(c);
        if (c.isLetterOrNumber()) {
            out += c.toLower();
            space = false;
        } else if (!space) {
            out += QLatin1Char(' ');
            space = true;
        }
    }
    if (!space)
        out += QLatin1Char(' ');
    return out;
}

void setKnownGames(const QStringList &names)
{
    GameIndex index;
    for (const QString &name : names) {
        const QString key = fold(name).trimmed();
        if (key.size() < 3)
            continue;
        if (!index.byWords.contains(key))
            index.byWords.insert(key, name);
        index.longest = std::max(index.longest, int(key.count(QLatin1Char(' '))) + 1);
    }
    gameIndex() = std::move(index);
}

int knownGameCount()
{
    return int(gameIndex().byWords.size());
}

QStringList gamesNamedIn(const QString &text)
{
    const GameIndex &index = gameIndex();
    if (index.byWords.isEmpty())
        return {};
    const QStringList words = fold(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty())
        return {};

    QStringList games;
    const auto take = [&](const QString &key, bool wholeName) {
        const auto it = index.byWords.constFind(key);
        if (it == index.byWords.constEnd() || !acceptable(key, wholeName) || games.contains(it.value()))
            return;
        // "The Forest" inside "Sons of the Forest" is the same server.
        for (const QString &already : std::as_const(games)) {
            if (fold(already).contains(QLatin1Char(' ') + key + QLatin1Char(' ')))
                return;
        }
        games.append(it.value());
    };

    QStringList core;
    for (const QString &word : words) {
        if (!fillerWords().contains(word))
            core.append(word);
    }
    if (!core.isEmpty())
        take(core.join(QLatin1Char(' ')), true);
    take(words.join(QLatin1Char(' ')), true);

    for (int length = std::min(index.longest, int(words.size())); length >= 1; --length) {
        for (int start = 0; start + length <= words.size(); ++start)
            take(words.mid(start, length).join(QLatin1Char(' ')), false);
    }
    return games;
}

bool isVrGame(const QString &name)
{
    static const QSet<QString> vrGames = {
        QStringLiteral("vrchat"),         QStringLiteral("chilloutvr"),     QStringLiteral("resonite"),
        QStringLiteral("neos vr"),        QStringLiteral("neosvr"),         QStringLiteral("rec room"),
        QStringLiteral("beat saber"),     QStringLiteral("gorilla tag"),    QStringLiteral("half life alyx"),
        QStringLiteral("boneworks"),      QStringLiteral("bonelab"),        QStringLiteral("pavlov"),
        QStringLiteral("blade sorcery"),  QStringLiteral("h3vr"),           QStringLiteral("vtol vr"),
        QStringLiteral("contractors"),    QStringLiteral("onward"),         QStringLiteral("population one"),
        QStringLiteral("walkabout mini golf"), QStringLiteral("superhot vr"), QStringLiteral("job simulator"),
        QStringLiteral("into the radius"), QStringLiteral("animal company"), QStringLiteral("gym class vr"),
        QStringLiteral("virtual desktop"), QStringLiteral("steamvr"),
    };
    const QString key = fold(name);
    return vrGames.contains(key.trimmed()) || key.contains(QLatin1String(" vr ")) || key.endsWith(QLatin1String("vr "));
}

Verdict classify(const Facts &facts)
{
    struct Tally
    {
        double score = 0;
        QStringList ownWords;   // words found in the server's own words, for the reason
        int channels = 0;
        QString game;           // the game that decided it, if one did
    };
    QHash<QString, Tally> tally;

    // Each source adds `weight` per hit, up to `cap` hits, so fifty channels
    // named after one game cannot drown out the server's own name.
    //   own:     the server's own words, where "^" keywords count too
    //   perText: one hit per text at most - a channel is one vote
    const auto count = [&](const QStringList &texts, double weight, int cap, bool own, bool perText) {
        QStringList folded;
        folded.reserve(texts.size());
        for (const QString &text : texts)
            folded.append(fold(text));
        for (const Category &category : all()) {
            int taken = 0;
            for (const QString &text : std::as_const(folded)) {
                if (taken >= cap)
                    break;
                const QStringList words = hits(text, category.id, own);
                if (words.isEmpty())
                    continue;
                const int used = perText ? 1 : std::min(int(words.size()), cap - taken);
                // Housekeeping channels - #bot-commands, #faq, #suggestions -
                // are in nearly every server, so they say little about it.
                const double scale = (!own && category.id == QLatin1String("support")) ? 0.5 : 1.0;
                Tally &t = tally[category.id];
                t.score += weight * used * scale;
                taken += used;
                if (own)
                    t.ownWords += words.mid(0, used);
                else
                    ++t.channels;
            }
        }
    };

    count({facts.name}, 6.0, 2, true, false);
    count(facts.traits, 4.0, 3, true, true);
    count({facts.description}, 2.5, 3, true, false);
    count(facts.categoryNames, 1.5, 3, false, true);
    count(facts.channelNames, 1.0, 5, false, true);
    count(facts.roleNames, 0.4, 5, false, false);

    const bool community = hasFeature(facts.features, "COMMUNITY");
    const bool discoverable = hasFeature(facts.features, "DISCOVERABLE");
    const bool verified = hasFeature(facts.features, "VERIFIED");

    // Games: the strongest thing a server can say about itself. Named after
    // one, it is that game's server; linked to some on its profile, it plays
    // them. A VR game makes it a VR server instead.
    const auto creditGame = [&](const QString &game, double weight) {
        Tally &t = tally[isVrGame(game) ? QStringLiteral("vr") : QStringLiteral("gaming")];
        t.score += weight;
        if (t.game.isEmpty())
            t.game = game;
    };
    const QStringList named = gamesNamedIn(facts.name);
    if (!named.isEmpty())
        creditGame(named.first(), named.first().contains(QLatin1Char(' ')) || named.first().size() >= 6 ? 9.0 : 6.0);
    for (const QString &trait : facts.traits) {
        const QStringList inTrait = gamesNamedIn(trait);
        if (!inTrait.isEmpty()) {
            creditGame(inTrait.first(), 3.0);
            break;
        }
    }
    if (!facts.description.isEmpty()) {
        const QStringList inDescription = gamesNamedIn(facts.description);
        if (!inDescription.isEmpty())
            creditGame(inDescription.first(), 2.5);
    }
    int linkedFlat = 0;
    bool linkedVr = false;
    for (const QString &game : facts.linkedGames) {
        if (isVrGame(game)) {
            linkedVr = true;
            if (tally.value(QStringLiteral("vr")).game.isEmpty())
                tally[QStringLiteral("vr")].game = game;
        } else {
            ++linkedFlat;
            if (tally.value(QStringLiteral("gaming")).game.isEmpty())
                tally[QStringLiteral("gaming")].game = game;
        }
    }
    if (linkedVr)
        tally[QStringLiteral("vr")].score += 7.0;
    if (linkedFlat > 0)
        tally[QStringLiteral("gaming")].score += 5.0 + std::min(linkedFlat - 1, 3);
    if (facts.gameActivity > 0 && (linkedFlat > 0 || !named.isEmpty()))
        tally[QStringLiteral("gaming")].score += std::min(facts.gameActivity, 3);
    // A verified server named after a game is that game's own.
    if (verified && (!named.isEmpty() || linkedFlat > 0))
        tally[QStringLiteral("gaming")].score += 2.0;

    // People talking in voice is what makes a hangout, but one or two people
    // in a game's voice channel do not. Counted on a curve: three people is a
    // hint, a crowd is as strong as a word in the name.
    double voiceBonus = 0;
    if (facts.usualVoice > 0.05) {
        voiceBonus = std::clamp(1.5 * std::log2(1.0 + facts.usualVoice) - 1.0, 0.0, 6.0);
        tally[QStringLiteral("social")].score += voiceBonus;
    }

    // A small private server is most likely a group of friends - unless its
    // own name says what it is about.
    double sizeBonus = 0;
    if (!community) {
        if (facts.memberCount > 0 && facts.memberCount <= 30)
            sizeBonus = 5.5;
        else if (facts.memberCount > 30 && facts.memberCount <= 80)
            sizeBonus = 2.5;
        else if (facts.memberCount == 0 && !discoverable && facts.channelCount > 0 && facts.channelCount <= 12)
            sizeBonus = 2.5;
        tally[QStringLiteral("friends")].score += sizeBonus;
    }

    // Discord marks the servers apps run for their users.
    if (hasFeature(facts.features, "DEVELOPER_SUPPORT_SERVER"))
        tally[QStringLiteral("support")].score += 8;

    QString best;
    double bestScore = 0;
    for (const Category &category : all()) {
        const double score = tally.value(category.id).score;
        if (score > bestScore + 1e-9) {
            best = category.id;
            bestScore = score;
        }
    }

    Verdict verdict;
    if (bestScore < 3.0) {
        const bool big = community || discoverable || facts.memberCount >= 500;
        verdict.categoryId = big ? QStringLiteral("communities") : QStringLiteral("other");
        verdict.reason = big ? QStringLiteral("a public server with nothing more specific")
                             : QStringLiteral("nothing to go on");
        return verdict;
    }

    verdict.categoryId = best;
    const Tally t = tally.value(best);
    QStringList why;
    if (!t.game.isEmpty())
        why << QStringLiteral("plays %1").arg(t.game);
    if (!t.ownWords.isEmpty()) {
        QStringList unique;
        for (const QString &word : t.ownWords) {
            if (!unique.contains(word))
                unique << word;
        }
        why << QStringLiteral("says %1").arg(unique.mid(0, 3).join(QStringLiteral(", ")));
    }
    if (t.channels > 0)
        why << QStringLiteral("%1 matching channel%2").arg(t.channels).arg(QLatin1String(t.channels == 1 ? "" : "s"));
    if (best == QLatin1String("social") && voiceBonus > 0)
        why << QStringLiteral("about %1 in voice").arg(qRound(facts.usualVoice));
    if (best == QLatin1String("friends") && sizeBonus > 0)
        why << (facts.memberCount > 0 ? QStringLiteral("%1 members").arg(facts.memberCount)
                                      : QStringLiteral("small and private"));
    if (best == QLatin1String("support") && hasFeature(facts.features, "DEVELOPER_SUPPORT_SERVER"))
        why << QStringLiteral("an app's support server");
    verdict.reason = why.join(QStringLiteral("; "));
    return verdict;
}

} // namespace ServerCategories
