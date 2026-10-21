#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Works out what kind of server something is - gaming, a hangout, anime and
// art, and so on - from what the client already knows about it. Nothing is
// asked of Discord and nothing leaves the machine.
//
// It reads the server's name and description, its channel and category names,
// its role names, how big it is and how many people usually sit in voice. The
// name counts most, channels a little each, roles barely: a hangout with one
// #gaming channel is still a hangout. Voice is what tells a social server
// apart, which is what makes "Social" more than a list of words.
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
    QStringList channelNames;    // text and voice
    QStringList categoryNames;   // the headings channels sit under
    QStringList roleNames;
    QStringList features;        // Discord's: "COMMUNITY", "DISCOVERABLE", ...
    int memberCount = 0;         // 0: not known
    int channelCount = 0;
    double usualVoice = 0;       // people usually in voice, you left out
};

struct Verdict
{
    QString categoryId;
    QString reason;   // a few words for the settings list: "name: valorant; 4 gaming channels"
};

Verdict classify(const Facts &facts);

// Lower case plain letters and digits, single spaces between words and one at
// each end. Fancy lettering people put in names - bold script, full width,
// small capitals, accents - is read as the plain letters it stands for.
QString fold(const QString &text);

} // namespace ServerCategories
