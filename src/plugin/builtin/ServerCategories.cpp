#include "plugin/builtin/ServerCategories.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ServerCategories {

namespace {

// A word or phrase to look for. Ending in "=" means the whole word only;
// otherwise a word of four letters or more also matches as the start of a
// longer one ("game" finds "games" and "gamer"). Three letters or fewer is
// always whole: "art" must not find "artillery", nor "rap" "rapid".
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
          "palworld", "helldivers", "elden ring", "dark souls", "warframe", "tarkov", "dayz", "ark=", "osu=",
          "terraria", "stardew", "sims=", "hoi4", "chess=", "vrchat", "vrc", "beat saber", "among us",
          "phasmophobia", "lethal company", "rocket league", "rainbow six", "r6", "siege=", "pubg",
          "battlefield", "splatoon", "zelda", "mario", "final fantasy", "ffxiv", "warcraft", "hearthstone",
          "diablo", "path of exile", "runescape", "osrs", "mmo", "mmorpg", "rpg", "fps", "moba",
          "battle royale", "twitch", "streamer", "clips", "smp", "bedwars", "skyblock", "hypixel", "modpack",
          "brawl stars", "clash royale", "clash of clans", "geometry dash", "dead by daylight", "dbd",
          "the finals", "marvel snap", "arc raiders", "repo=", "peak="}},
        {"social",
         {"social", "hangout", "hang out", "chill", "vibe", "lounge", "cafe", "make friends", "new friends",
          "meet people", "dating", "e girl", "egirl", "eboy", "e boy", "goth", "emo", "alt", "alternative",
          "grunge", "teen", "late night", "night owl", "insomnia", "vc", "voice chat", "talk", "yap",
          "selfie", "face reveal", "faces=", "introduc", "intro", "intros=", "venting", "vent=",
          "confession", "qotd", "question of the day", "counting", "pet", "pets=", "food", "cooking",
          "meme", "media=", "pics=", "irl", "outfit", "fashion", "fit check", "nsfw", "simp", "hot take",
          "debate", "rant=", "rants=", "sleep call", "sleep vc", "cuddle", "lgbt", "queer", "pride=",
          "trans=", "relationship"}},
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
          "guitar", "piano", "drum", "orchestra", "kpop", "k pop", "jpop"}},
        {"tech",
         {"coding", "code=", "coder", "programming", "programmer", "developer", "dev", "devs",
          "development", "software", "python", "javascript", "typescript", "java", "cpp", "csharp",
          "golang", "rust=", "linux", "windows=", "android", "ios", "macos", "hacking", "hacker", "cyber",
          "infosec", "tech=", "technology", "technical", "pc build", "pc help", "hardware", "overclock",
          "gpu", "cpu", "ai", "artificial intelligence", "machine learning", "llm", "llms", "chatgpt",
          "openai", "claude", "midjourney", "stable diffusion", "comfyui", "github", "open source", "unity=",
          "unreal", "godot", "gamedev", "game dev", "blender", "3d", "web dev", "webdev", "api", "homelab",
          "raspberry", "arduino", "electronics", "engineer", "math", "maths", "science", "physics",
          "robotic", "hosting", "vps"}},
        {"money",
         {"crypto", "bitcoin", "btc", "eth", "ethereum", "solana", "nft", "nfts", "web3", "defi", "airdrop",
          "memecoin", "meme coin", "trading", "trader", "stocks", "stock market", "forex", "invest",
          "finance", "money", "hustle", "entrepreneur", "business", "ecommerce", "dropship", "reselling",
          "resell", "sneaker", "casino", "gambling", "betting"}},
        {"support",
         {"support", "official", "helpdesk", "help desk", "help=", "bug report", "bug", "bugs", "feedback",
          "suggestion", "changelog", "status=", "faq", "docs", "documentation", "premium", "dashboard",
          "bot", "bots", "beta=", "tester", "testing", "app=", "apps=", "application", "dyno", "mee6",
          "carl bot", "tatsu", "dank memer", "wallpaper engine", "vencord", "betterdiscord", "plugin"}},
    };
    return table;
}

struct Needle
{
    QString text;   // " word" - a space in front so it only starts a word
    bool whole = false;
};

QHash<QString, QList<Needle>> compileNeedles()
{
    QHash<QString, QList<Needle>> needles;
    for (const Keywords &group : keywordTable()) {
        QList<Needle> &list = needles[QString::fromLatin1(group.category)];
        for (const char *raw : group.words) {
            QString word = QString::fromLatin1(raw);
            Needle needle;
            needle.whole = word.endsWith(QLatin1Char('='));
            if (needle.whole)
                word.chop(1);
            const QString lastWord = word.section(QLatin1Char(' '), -1);
            if (lastWord.size() <= 3)
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

// The words of one category found in one piece of text.
QStringList hits(const QString &folded, const QString &category)
{
    QStringList words;
    for (const Needle &needle : needles().value(category)) {
        if (found(folded, needle))
            words.append(needle.text.mid(1));
    }
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

} // namespace

const QList<Category> &all()
{
    static const QList<Category> categories = {
        {QStringLiteral("social"), QStringLiteral("Social"), 0xE91E63},
        {QStringLiteral("friends"), QStringLiteral("Friends"), 0xF1C40F},
        {QStringLiteral("gaming"), QStringLiteral("Gaming"), 0x2ECC71},
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

Verdict classify(const Facts &facts)
{
    struct Tally
    {
        double score = 0;
        QStringList nameWords;
        int channels = 0;
    };
    QHash<QString, Tally> tally;

    // Each source adds `weight` per hit, up to `cap` hits, so fifty channels
    // named after one game cannot drown out the server's own name.
    const auto count = [&](const QStringList &texts, double weight, int cap, bool keepWords, bool perText) {
        QStringList folded;
        folded.reserve(texts.size());
        for (const QString &text : texts)
            folded.append(fold(text));
        for (const Category &category : all()) {
            int taken = 0;
            for (const QString &text : std::as_const(folded)) {
                if (taken >= cap)
                    break;
                const QStringList words = hits(text, category.id);
                if (words.isEmpty())
                    continue;
                const int used = perText ? 1 : std::min(int(words.size()), cap - taken);
                // Housekeeping channels - #bot-commands, #faq, #suggestions -
                // are in nearly every server, so they say little about it.
                const double scale = (perText && category.id == QLatin1String("support")) ? 0.5 : 1.0;
                Tally &t = tally[category.id];
                t.score += weight * used * scale;
                taken += used;
                if (keepWords)
                    t.nameWords += words.mid(0, used);
                else if (perText)
                    ++t.channels;
            }
        }
    };

    count({facts.name}, 6.0, 2, true, false);
    count({facts.description}, 2.5, 3, true, false);
    count(facts.categoryNames, 1.5, 3, false, true);
    count(facts.channelNames, 1.0, 5, false, true);
    count(facts.roleNames, 0.4, 5, false, false);

    const bool community = hasFeature(facts.features, "COMMUNITY");
    const bool discoverable = hasFeature(facts.features, "DISCOVERABLE");

    // People talking in voice is what makes a hangout. Counted on a curve:
    // one person is a hint, four or more is as strong as a word in the name.
    double voiceBonus = 0;
    if (facts.usualVoice > 0.05) {
        voiceBonus = std::min(6.0, 2.5 * std::log2(1.0 + facts.usualVoice));
        tally[QStringLiteral("social")].score += voiceBonus;
    }

    // A small private server is most likely a group of friends, whatever
    // they play.
    double sizeBonus = 0;
    if (!community) {
        if (facts.memberCount > 0 && facts.memberCount <= 30)
            sizeBonus = 7;
        else if (facts.memberCount > 30 && facts.memberCount <= 80)
            sizeBonus = 3;
        else if (facts.memberCount == 0 && !discoverable && facts.channelCount > 0 && facts.channelCount <= 12)
            sizeBonus = 3;
        tally[QStringLiteral("friends")].score += sizeBonus;
    }

    // Discord marks the servers apps run for their users.
    if (hasFeature(facts.features, "DEVELOPER_SUPPORT_SERVER"))
        tally[QStringLiteral("support")].score += 8;
    if (hasFeature(facts.features, "VERIFIED"))
        tally[QStringLiteral("support")].score += 1.5;

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
    if (!t.nameWords.isEmpty()) {
        QStringList unique;
        for (const QString &word : t.nameWords) {
            if (!unique.contains(word))
                unique << word;
        }
        why << QStringLiteral("says %1").arg(unique.mid(0, 3).join(QStringLiteral(", ")));
    }
    if (t.channels > 0)
        why << QStringLiteral("%1 matching channel%2")
                   .arg(t.channels)
                   .arg(QLatin1String(t.channels == 1 ? "" : "s"));
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
