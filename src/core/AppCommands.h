#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>

// Discord's application commands - "slash commands" - read and sent the way
// its own client does (web bundle, APPLICATION_COMMAND_INDEX_* and
// handleCommand).
//
// Where they come from:
//   GET /guilds/{id}/application-command-index     the apps in a server
//   GET /channels/{id}/application-command-index   the apps in a DM or group
//   GET /users/@me/application-command-index       apps you installed yourself
// A 202 means the index is still being built: Discord's client asks again
// after five seconds.
//
// How one runs: POST /interactions with type 2, the app, the channel, the
// gateway session and a nonce, and `data` naming the command (id, version,
// name, type), the filled-in options, the whole command as it came from the
// index, and an empty attachment list. A sub command is sent as the root
// command with the sub command as its only option, the filled-in options
// inside that. The answer comes back on the gateway as an ordinary message.
namespace AppCommands {

enum OptionType {
    SubCommand = 1,
    SubCommandGroup = 2,
    String = 3,
    Integer = 4,
    Boolean = 5,
    User = 6,
    Channel = 7,
    Role = 8,
    Mentionable = 9,
    Number = 10,
    Attachment = 11,
};

struct Choice
{
    QString name;        // what is shown and typed
    QJsonValue value;    // what is sent
};

struct Option
{
    int type = String;
    QString name;            // untranslated, which is what is sent
    QString displayName;     // what is shown
    QString description;
    bool required = false;
    bool autocomplete = false;
    QList<Choice> choices;
    QList<Option> options;   // a sub command's own options
    bool hasMin = false;
    bool hasMax = false;
    double minValue = 0;
    double maxValue = 0;
    int minLength = -1;
    int maxLength = -1;
};

struct App
{
    QString id;
    QString name;
    QString icon;
    QString botId;
    QJsonObject permissions;   // the app's own overrides, used when a command has none
};

struct Command
{
    // The root id, then "\0" and the name of each sub command level. That is
    // Discord's own key for a sub command ("counter create"), and the key its
    // Frequently Used list is stored under.
    QString id;
    QString rootId;
    QString applicationId;
    QString version;
    QString guildId;          // only a command made for one server has one
    int type = 1;             // 1 typed with "/", 2 on a person, 3 on a message
    QString name;             // "counter create", untranslated
    QString displayName;
    QString description;
    QList<Option> options;    // what you fill in
    QList<QPair<QString, int>> subPath;   // ("create", SubCommand), outermost first
    QJsonObject root;         // the root command as the index sent it
    QString defaultMemberPermissions;     // empty and !hasDefault: anyone
    bool hasDefaultPermissions = false;
    bool dmPermission = true;
    bool nsfw = false;
    bool hasContexts = false;
    QList<int> contexts;      // 0 server, 1 DM with the app's bot, 2 any other DM
    QList<int> integrationTypes;   // 0 added to a server, 1 added to a person
    QJsonObject permissions;  // per-command overrides: user, roles, channels
    bool builtIn = false;
    bool userInstalled = false;    // came from /users/@me
};

struct Index
{
    QList<App> apps;
    QList<Command> commands;
};

// One index response. Sub commands are flattened into their own entries, the
// way Discord lists "/counter create" and "/counter delete" separately.
Index parseIndex(const QJsonObject &raw, bool userInstalled = false);

// Where you are, for the permission check.
struct Place
{
    QString guildId;          // empty in a DM
    QString channelId;
    QString parentId;         // a thread's channel
    QStringList recipientIds; // a DM's other people
    QString selfId;
    QStringList roleIds;      // your roles in the server
    quint64 permissions = 0;  // your permission bits in the server
    bool owner = false;
};

// Whether the command shows here. Discord's order: where it may be used
// (contexts, or dm_permission for old commands), then a server's owner and
// administrators see everything, then a channel override, then a person,
// role or @everyone override, then default_member_permissions.
bool allowed(const Command &command, const App *app, const Place &place);

// The text after "/name ", split at each "option:" label. A label counts
// only for an option the command has, and only the first time it appears.
// Text before the first label is left loose; *looseEnd says where it stops.
struct Span
{
    int option = -1;     // index into the command's options
    int labelStart = 0;
    int valueStart = 0;
    int valueEnd = 0;    // where the next label starts, or the end; trim the value
};
QList<Span> findSpans(const QString &text, const QList<Option> &options, int *looseEnd);

// Typed text to the value Discord wants. *error says why when it does not
// fit; the value is then undefined.
struct Resolver
{
    QString guildId;
    std::function<QString(const QString &name)> channelByName;   // "#general" -> id
};
QJsonValue toValue(const Option &option, const QString &text, const Resolver &resolver, QString *error);

// `data` for POST /interactions.
QJsonObject buildData(const Command &command, const QJsonArray &options, const QString &targetId = {});

// A snowflake made from the clock, which is what Discord's client uses as a
// nonce. The autocomplete answer comes back carrying it.
QString makeNonce();

// The commands Discord's client runs itself, without an app.
QList<Command> builtIns();
QString runBuiltIn(const Command &command, const QString &text);

// The key Discord's Frequently Used list stores a command under: its id, plus
// ":<server id>" for a command made for one server.
QString frecencyKey(const Command &command);

} // namespace AppCommands
