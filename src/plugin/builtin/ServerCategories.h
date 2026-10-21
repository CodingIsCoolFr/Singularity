#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Works out what kind of server something is - gaming, a hangout, VR, anime
// and art, and so on - from what the client knows about it. Nothing is asked
// of Discord here; the organizer gathers the facts and this weighs them.
//
// Strongest first:
//   - the games the server is linked to on its Discord profile, and its name
//     being a game's name (Discord's own list of about ten thousand games);
//   - its name, and the "traits" its admins tagged its profile with;
//   - its description, its channel and category names, its roles;
//   - how big it is, and how many people usually sit in voice.
// A hangout with one #gaming channel is still a hangout, and an official game
// server is a game server whatever it says about "support".
namespace ServerCategories {

struct Category
{
    QString id;      // stored in settings, so never renamed
    QString name;    // the folder's name
    quint32 color;   // the folder's colour, 0xRRGGBB, from Discord's own palette
};

// In the order the folders go on the rail.
const QList<Category> &all();
const Category *find(const QString &id);

// What the organizer knows about one server.
struct Facts
{
    QString name;
    QString description;
    QStringList traits;          // the profile's tags: "Chill", "Anime", "VRChat"...
    QStringList linkedGames;     // the profile's games, by name
    int gameActivity = 0;        // the profile's busiest game: 0 none, higher is busier
    QStringList channelNames;    // text and voice
    QStringList categoryNames;   // the headings channels sit under
    QStringList roleNames;
    QStringList features;        // Discord's: "COMMUNITY", "DISCOVERABLE", "VERIFIED"...
    int memberCount = 0;         // 0: not known
    int channelCount = 0;
    double usualVoice = 0;       // people usually in voice, you left out
};

struct Verdict
{
    QString categoryId;
    QString reason;   // a few words for the settings list: "a game: Subnautica; 3 matching channels"
};

Verdict classify(const Facts &facts);

// Discord's list of games, by name. Set once the list is on disk; until then
// a server is only matched to games by the words below.
void setKnownGames(const QStringList &names);
int knownGameCount();

// The known games a piece of text is named after or names: "Subnautica
// (Official)" is Subnautica, "Sons Of | The Forest" is Sons of the Forest.
// Short or everyday names only count when they are the whole name.
QStringList gamesNamedIn(const QString &text);

// A game played in VR, which puts its server in VR rather than Gaming.
bool isVrGame(const QString &name);

// Lower case plain letters and digits, single spaces between words and one at
// each end. Fancy lettering people put in names - bold script, full width,
// small capitals, accents - is read as the plain letters it stands for.
QString fold(const QString &text);

} // namespace ServerCategories
