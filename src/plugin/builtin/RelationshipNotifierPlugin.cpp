#include "plugin/builtin/RelationshipNotifierPlugin.h"

#include "core/Logger.h"
#include "core/RestClient.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Discord's relationship types.
constexpr int Friend = 1;
constexpr int Incoming = 3;
constexpr int Outgoing = 4;

constexpr int GroupDm = 3;   // channel type

struct Toggle {
    const char *key;
    const char *label;
};

// Every kind of notice can be switched off on its own.
constexpr Toggle kToggles[] = {
    {"friendRemoved", "A friend removes you"},
    {"requestReceived", "Someone sends you a friend request"},
    {"requestAccepted", "Someone accepts your friend request"},
    {"requestCancelled", "Someone cancels the friend request they sent you"},
    {"serverRemoved", "You are removed from a server"},
    {"groupRemoved", "You are removed from a group chat"},
    {"whileAway", "Tell me what changed while Singularity was closed"},
};

QJsonObject toJson(const QHash<QString, QString> &map)
{
    QJsonObject out;
    for (auto it = map.cbegin(); it != map.cend(); ++it)
        out.insert(it.key(), it.value());
    return out;
}

QHash<QString, QString> fromJson(const QJsonObject &object)
{
    QHash<QString, QString> out;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        out.insert(it.key(), it.value().toString());
    return out;
}

} // namespace

void RelationshipNotifierPlugin::onLoad(PluginContext *context)
{
    Plugin::onLoad(context);
    m_ready = false;
}

void RelationshipNotifierPlugin::onUnload()
{
    m_relationship.clear();
    m_lists = {};
    m_groupsAnnounced.clear();
    m_ready = false;
    Plugin::onUnload();
}

bool RelationshipNotifierPlugin::wants(const char *setting) const
{
    return settingValue(QString::fromLatin1(setting), true).toBool();
}

void RelationshipNotifierPlugin::tell(const char *setting, const QString &title, const QString &text)
{
    if (!wants(setting) || !context())
        return;
    wlog(QStringLiteral("relationships"), QStringLiteral("%1: %2").arg(title, text));
    context()->notify(title, text);
}

QString RelationshipNotifierPlugin::personName(const QString &userId, const QJsonObject &user) const
{
    const QString global = user.value(QStringLiteral("global_name")).toString();
    if (!global.isEmpty())
        return global;
    const QString username = user.value(QStringLiteral("username")).toString();
    if (!username.isEmpty())
        return username;

    if (context() && context()->store()) {
        const UserInfo info = context()->store()->user(userId);
        if (!info.displayName().isEmpty())
            return info.displayName();
        if (!info.username.isEmpty())
            return info.username;
    }

    // Someone already gone from the store is still in our own lists.
    for (const auto *map : {&m_lists.friends, &m_lists.incoming, &m_lists.outgoing}) {
        const QString known = map->value(userId);
        if (!known.isEmpty())
            return known;
    }
    return QStringLiteral("Someone");
}

QString RelationshipNotifierPlugin::groupName(const QString &channelId) const
{
    if (context() && context()->store()) {
        const ChannelInfo channel = context()->store()->channel(channelId);
        if (!channel.name.isEmpty())
            return channel.name;

        QStringList names;
        for (const QString &id : channel.recipientIds)
            names << personName(id);
        if (!names.isEmpty())
            return names.join(QStringLiteral(", "));
    }
    const QString known = m_lists.groups.value(channelId);
    return known.isEmpty() ? QStringLiteral("a group chat") : known;
}

void RelationshipNotifierPlugin::onGatewayEvent(const QString &eventType, const QJsonObject &data)
{
    if (eventType == QLatin1String("READY")) {
        onReady(data);
        return;
    }
    if (!m_ready)
        return;

    // Plugins see each event before the client updates its store, so what a
    // person or server *was* is still there to be read.

    if (eventType == QLatin1String("RELATIONSHIP_ADD")) {
        const QString userId = data.value(QStringLiteral("id")).toString();
        const int type = data.value(QStringLiteral("type")).toInt();
        const QJsonObject user = data.value(QStringLiteral("user")).toObject();
        const int was = m_relationship.value(userId, 0);
        const QString name = personName(userId, user);

        if (type == Incoming && was != Incoming)
            tell("requestReceived", QStringLiteral("Friend request"),
                 QStringLiteral("%1 sent you a friend request.").arg(name));
        else if (type == Friend && was == Outgoing)
            tell("requestAccepted", QStringLiteral("Friend request accepted"),
                 QStringLiteral("%1 accepted your friend request.").arg(name));

        m_relationship.insert(userId, type);
        m_lists.friends.remove(userId);
        m_lists.incoming.remove(userId);
        m_lists.outgoing.remove(userId);
        if (type == Friend)
            m_lists.friends.insert(userId, name);
        else if (type == Incoming)
            m_lists.incoming.insert(userId, name);
        else if (type == Outgoing)
            m_lists.outgoing.insert(userId, name);
        save();
        return;
    }

    if (eventType == QLatin1String("RELATIONSHIP_REMOVE")) {
        const QString userId = data.value(QStringLiteral("id")).toString();
        const int was = m_relationship.value(userId, data.value(QStringLiteral("type")).toInt());
        const bool ours = context() && context()->rest() && context()->rest()->removedByUs(userId);

        if (!ours) {
            const QString name = personName(userId);
            if (was == Friend)
                tell("friendRemoved", QStringLiteral("Relationship notifier"),
                     QStringLiteral("%1 removed you as a friend.").arg(name));
            else if (was == Incoming)
                tell("requestCancelled", QStringLiteral("Relationship notifier"),
                     QStringLiteral("%1 cancelled their friend request.").arg(name));
            // An outgoing request that vanishes is not reported: Discord does
            // not say whether it was declined, and guessing would be wrong.
        }

        m_relationship.remove(userId);
        m_lists.friends.remove(userId);
        m_lists.incoming.remove(userId);
        m_lists.outgoing.remove(userId);
        save();
        return;
    }

    if (eventType == QLatin1String("GUILD_CREATE")) {
        const QString guildId = data.value(QStringLiteral("id")).toString();
        const QJsonObject props = data.contains(QStringLiteral("properties"))
            ? data.value(QStringLiteral("properties")).toObject()
            : data;
        if (!guildId.isEmpty()) {
            m_lists.guilds.insert(guildId, props.value(QStringLiteral("name")).toString());
            save();
        }
        return;
    }

    if (eventType == QLatin1String("GUILD_DELETE")) {
        // "unavailable" is an outage, and the server comes back by itself.
        if (data.value(QStringLiteral("unavailable")).toBool())
            return;
        const QString guildId = data.value(QStringLiteral("id")).toString();
        QString name = context() && context()->store() ? context()->store()->guild(guildId).name : QString();
        if (name.isEmpty())
            name = m_lists.guilds.value(guildId, QStringLiteral("a server"));
        tell("serverRemoved", QStringLiteral("Relationship notifier"),
             QStringLiteral("You are no longer in %1.").arg(name));
        m_lists.guilds.remove(guildId);
        save();
        return;
    }

    if (eventType == QLatin1String("CHANNEL_CREATE")
        && data.value(QStringLiteral("type")).toInt() == GroupDm) {
        const QString channelId = data.value(QStringLiteral("id")).toString();
        m_groupsAnnounced.remove(channelId);
        QTimer::singleShot(0, [this, channelId]() {
            if (m_ready) {
                m_lists.groups.insert(channelId, groupName(channelId));
                save();
            }
        });
        return;
    }

    // Removed from a group: Discord may say so either way, so whichever
    // arrives first is announced and the other is ignored.
    const bool recipientGone = eventType == QLatin1String("CHANNEL_RECIPIENT_REMOVE")
        && data.value(QStringLiteral("user")).toObject().value(QStringLiteral("id")).toString() == m_selfId;
    const bool groupGone = eventType == QLatin1String("CHANNEL_DELETE")
        && data.value(QStringLiteral("type")).toInt() == GroupDm;
    if (recipientGone || groupGone) {
        const QString channelId = recipientGone ? data.value(QStringLiteral("channel_id")).toString()
                                                : data.value(QStringLiteral("id")).toString();
        const bool ours = context() && context()->rest() && context()->rest()->removedByUs(channelId);
        if (!ours && !m_groupsAnnounced.contains(channelId)) {
            m_groupsAnnounced.insert(channelId);
            tell("groupRemoved", QStringLiteral("Relationship notifier"),
                 QStringLiteral("You were removed from the group %1.").arg(groupName(channelId)));
        }
        m_lists.groups.remove(channelId);
        save();
    }
}

void RelationshipNotifierPlugin::onReady(const QJsonObject &data)
{
    m_selfId = data.value(QStringLiteral("user")).toObject().value(QStringLiteral("id")).toString();
    m_relationship.clear();
    m_groupsAnnounced.clear();

    // Ids straight from READY, which lists every server - including ones in
    // an outage - so nothing is mistaken for having been removed.
    Lists now;
    for (const QJsonValue &value : data.value(QStringLiteral("relationships")).toArray()) {
        const QJsonObject rel = value.toObject();
        const QString id = rel.value(QStringLiteral("id")).toString();
        const int type = rel.value(QStringLiteral("type")).toInt();
        m_relationship.insert(id, type);
        if (type == Friend)
            now.friends.insert(id, QString());
        else if (type == Incoming)
            now.incoming.insert(id, QString());
        else if (type == Outgoing)
            now.outgoing.insert(id, QString());
    }
    for (const QJsonValue &value : data.value(QStringLiteral("guilds")).toArray())
        now.guilds.insert(value.toObject().value(QStringLiteral("id")).toString(), QString());
    for (const QJsonValue &value : data.value(QStringLiteral("private_channels")).toArray()) {
        const QJsonObject channel = value.toObject();
        if (channel.value(QStringLiteral("type")).toInt() == GroupDm)
            now.groups.insert(channel.value(QStringLiteral("id")).toString(), QString());
    }

    // Names once the client has taken READY in, which happens right after
    // plugins see it.
    QTimer::singleShot(0, [this, now]() mutable {
        if (!context())
            return;
        for (auto *map : {&now.friends, &now.incoming, &now.outgoing})
            for (auto it = map->begin(); it != map->end(); ++it)
                it.value() = personName(it.key());
        for (auto it = now.guilds.begin(); it != now.guilds.end(); ++it)
            it.value() = context()->store()->guild(it.key()).name;
        for (auto it = now.groups.begin(); it != now.groups.end(); ++it)
            it.value() = groupName(it.key());

        compareWithLastRun(now);
        m_lists = now;
        m_ready = true;
        save();
    });
}

// "While you were away": what the last run knew, against what READY says now.
void RelationshipNotifierPlugin::compareWithLastRun(const Lists &now)
{
    const Lists before = loadSaved();
    const bool haveBefore = !before.friends.isEmpty() || !before.guilds.isEmpty()
        || !before.incoming.isEmpty() || !before.groups.isEmpty();
    if (!haveBefore || !wants("whileAway"))
        return;   // first run for this account: nothing to compare yet

    const auto gone = [](const QHash<QString, QString> &was, const QHash<QString, QString> &is) {
        QStringList names;
        for (auto it = was.cbegin(); it != was.cend(); ++it)
            if (!is.contains(it.key()))
                names << (it.value().isEmpty() ? it.key() : it.value());
        return names;
    };
    const auto arrived = [](const QHash<QString, QString> &was, const QHash<QString, QString> &is) {
        QStringList names;
        for (auto it = is.cbegin(); it != is.cend(); ++it)
            if (!was.contains(it.key()))
                names << (it.value().isEmpty() ? it.key() : it.value());
        return names;
    };

    const QString title = QStringLiteral("While you were away");
    for (const QString &name : gone(before.friends, now.friends))
        tell("friendRemoved", title, QStringLiteral("%1 is no longer your friend.").arg(name));
    for (const QString &name : arrived(before.incoming, now.incoming))
        tell("requestReceived", title, QStringLiteral("%1 sent you a friend request.").arg(name));
    for (const QString &id : now.friends.keys())
        if (before.outgoing.contains(id))
            tell("requestAccepted", title,
                 QStringLiteral("%1 accepted your friend request.").arg(now.friends.value(id)));
    for (const QString &name : gone(before.guilds, now.guilds))
        tell("serverRemoved", title, QStringLiteral("You are no longer in %1.").arg(name));
    for (const QString &name : gone(before.groups, now.groups))
        tell("groupRemoved", title, QStringLiteral("You are no longer in the group %1.").arg(name));
}

void RelationshipNotifierPlugin::save() const
{
    if (m_selfId.isEmpty())
        return;
    const QJsonObject all{
        {QStringLiteral("friends"), toJson(m_lists.friends)},
        {QStringLiteral("incoming"), toJson(m_lists.incoming)},
        {QStringLiteral("outgoing"), toJson(m_lists.outgoing)},
        {QStringLiteral("guilds"), toJson(m_lists.guilds)},
        {QStringLiteral("groups"), toJson(m_lists.groups)},
    };
    const_cast<RelationshipNotifierPlugin *>(this)->setSettingValue(
        QStringLiteral("lists/") + m_selfId,
        QString::fromUtf8(QJsonDocument(all).toJson(QJsonDocument::Compact)));
}

RelationshipNotifierPlugin::Lists RelationshipNotifierPlugin::loadSaved() const
{
    Lists out;
    const QJsonObject all =
        QJsonDocument::fromJson(settingValue(QStringLiteral("lists/") + m_selfId).toString().toUtf8()).object();
    out.friends = fromJson(all.value(QStringLiteral("friends")).toObject());
    out.incoming = fromJson(all.value(QStringLiteral("incoming")).toObject());
    out.outgoing = fromJson(all.value(QStringLiteral("outgoing")).toObject());
    out.guilds = fromJson(all.value(QStringLiteral("guilds")).toObject());
    out.groups = fromJson(all.value(QStringLiteral("groups")).toObject());
    return out;
}

QWidget *RelationshipNotifierPlugin::createSettingsWidget(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    for (const Toggle &toggle : kToggles) {
        auto *box = new QCheckBox(QString::fromLatin1(toggle.label), page);
        box->setChecked(settingValue(QString::fromLatin1(toggle.key), true).toBool());
        const QString key = QString::fromLatin1(toggle.key);
        QObject::connect(box, &QCheckBox::toggled, page, [this, key](bool on) { setSettingValue(key, on); });
        layout->addWidget(box);
    }

    auto *note = new QLabel(
        QStringLiteral("Things you do yourself in Singularity never notify you. The same action "
                       "taken on another device - unfriending someone from your phone, say - "
                       "looks to Discord exactly like the other person doing it, so that one "
                       "can notify you."),
        page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(QLatin1String(Theme::TextFaint)));
    layout->addWidget(note);
    layout->addStretch(1);
    return page;
}
