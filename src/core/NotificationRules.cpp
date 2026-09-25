#include "core/NotificationRules.h"

#include <QDateTime>
#include <QJsonArray>

namespace {

qint64 endOf(const QJsonObject &holder)
{
    const QString end = holder.value(QStringLiteral("mute_config")).toObject()
                            .value(QStringLiteral("end_time")).toString();
    if (end.isEmpty())
        return 0;
    return QDateTime::fromString(end, Qt::ISODateWithMs).toMSecsSinceEpoch();
}

} // namespace

bool NotificationRules::mutedNow(bool muted, qint64 endsMs, qint64 nowMs)
{
    return muted && (endsMs <= 0 || endsMs > nowMs);
}

void NotificationRules::applySettingsEntry(const QJsonObject &entry)
{
    // guild_id is null for the entry that covers direct messages.
    const QString guildId = entry.value(QStringLiteral("guild_id")).toString();

    GuildSettings settings;
    settings.muted = entry.value(QStringLiteral("muted")).toBool();
    settings.muteEndsMs = endOf(entry);
    settings.level = entry.value(QStringLiteral("message_notifications")).toInt(3);
    settings.suppressEveryone = entry.value(QStringLiteral("suppress_everyone")).toBool();
    settings.suppressRoles = entry.value(QStringLiteral("suppress_roles")).toBool();

    // Sent as an array; very old payloads used an object keyed by channel.
    const QJsonValue overrides = entry.value(QStringLiteral("channel_overrides"));
    const auto takeOverride = [&settings](const QJsonObject &one) {
        Override o;
        o.muted = one.value(QStringLiteral("muted")).toBool();
        o.muteEndsMs = endOf(one);
        o.level = one.value(QStringLiteral("message_notifications")).toInt(3);
        settings.channels.insert(one.value(QStringLiteral("channel_id")).toString(), o);
    };
    if (overrides.isArray()) {
        for (const QJsonValue &value : overrides.toArray())
            takeOverride(value.toObject());
    } else if (overrides.isObject()) {
        const QJsonObject map = overrides.toObject();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            QJsonObject one = it.value().toObject();
            if (!one.contains(QStringLiteral("channel_id")))
                one.insert(QStringLiteral("channel_id"), it.key());
            takeOverride(one);
        }
    }

    m_settings.insert(guildId, settings);
}

void NotificationRules::applyGuild(const QJsonObject &guild)
{
    const QString id = guild.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
        return;
    // Newer payloads keep the server's own fields under "properties".
    const QJsonObject properties = guild.value(QStringLiteral("properties")).toObject();
    const QJsonValue level = properties.contains(QStringLiteral("default_message_notifications"))
        ? properties.value(QStringLiteral("default_message_notifications"))
        : guild.value(QStringLiteral("default_message_notifications"));
    if (level.isDouble())
        m_guildDefaults.insert(id, level.toInt());
}

void NotificationRules::setSelfRoles(const QString &guildId, const QStringList &roleIds)
{
    m_selfRoles.insert(guildId, QSet<QString>(roleIds.cbegin(), roleIds.cend()));
}

void NotificationRules::ingestReady(const QJsonObject &ready, const QString &selfUserId)
{
    const QJsonValue settings = ready.value(QStringLiteral("user_guild_settings"));
    const QJsonArray entries = settings.isObject()
        ? settings.toObject().value(QStringLiteral("entries")).toArray()
        : settings.toArray();
    for (const QJsonValue &entry : entries)
        applySettingsEntry(entry.toObject());

    // merged_members runs parallel to guilds: the list at index i holds your
    // own member record in guilds[i], roles included.
    const QJsonArray guilds = ready.value(QStringLiteral("guilds")).toArray();
    const QJsonArray merged = ready.value(QStringLiteral("merged_members")).toArray();
    for (int i = 0; i < guilds.size(); ++i) {
        const QJsonObject guild = guilds.at(i).toObject();
        applyGuild(guild);
        const QString guildId = guild.value(QStringLiteral("id")).toString();
        for (const QJsonValue &value : merged.at(i).toArray()) {
            const QJsonObject member = value.toObject();
            const QString who = member.value(QStringLiteral("user_id")).toString();
            if (!who.isEmpty() && who != selfUserId)
                continue;
            QStringList roles;
            for (const QJsonValue &role : member.value(QStringLiteral("roles")).toArray())
                roles.append(role.toString());
            setSelfRoles(guildId, roles);
        }
    }
}

NotificationRules::Verdict NotificationRules::judge(const QJsonObject &message, const QString &selfUserId,
                                                    const QString &parentId, qint64 nowMs) const
{
    Verdict verdict;
    const QString guildId = message.value(QStringLiteral("guild_id")).toString();
    const QString channelId = message.value(QStringLiteral("channel_id")).toString();

    if (message.value(QStringLiteral("author")).toObject().value(QStringLiteral("id")).toString() == selfUserId) {
        verdict.reason = QStringLiteral("your own message");
        return verdict;
    }

    const GuildSettings settings = m_settings.value(guildId);
    const Override channel = settings.channels.value(channelId);
    const Override parent = parentId.isEmpty() ? Override{} : settings.channels.value(parentId);

    // Direct messages and group chats.
    if (guildId.isEmpty()) {
        if (mutedNow(channel.muted, channel.muteEndsMs, nowMs)) {
            verdict.reason = QStringLiteral("that chat is muted");
            return verdict;
        }
        verdict.notify = true;
        verdict.mentioned = true;
        verdict.reason = QStringLiteral("direct message");
        return verdict;
    }

    // Was I mentioned, in a way this server lets count?
    bool mentioned = false;
    for (const QJsonValue &value : message.value(QStringLiteral("mentions")).toArray()) {
        if (value.toObject().value(QStringLiteral("id")).toString() == selfUserId)
            mentioned = true;
    }
    if (!mentioned && message.value(QStringLiteral("mention_everyone")).toBool() && !settings.suppressEveryone)
        mentioned = true;
    if (!mentioned && !settings.suppressRoles) {
        const QSet<QString> mine = m_selfRoles.value(guildId);
        for (const QJsonValue &role : message.value(QStringLiteral("mention_roles")).toArray()) {
            if (mine.contains(role.toString()))
                mentioned = true;
        }
    }
    verdict.mentioned = mentioned;

    int level = channel.level;
    if (level == 3)
        level = parent.level;
    if (level == 3)
        level = settings.level;
    if (level == 3)
        level = m_guildDefaults.value(guildId, 1);

    if (level == 2) {
        verdict.reason = QStringLiteral("notifications are set to nothing here");
        return verdict;
    }

    const bool muted = mutedNow(settings.muted, settings.muteEndsMs, nowMs)
        || mutedNow(channel.muted, channel.muteEndsMs, nowMs)
        || mutedNow(parent.muted, parent.muteEndsMs, nowMs);
    if (muted && !mentioned) {
        verdict.reason = QStringLiteral("muted and you were not mentioned");
        return verdict;
    }
    if (level == 1 && !mentioned) {
        verdict.reason = QStringLiteral("only @mentions notify here");
        return verdict;
    }

    verdict.notify = true;
    verdict.reason = mentioned ? QStringLiteral("you were mentioned") : QStringLiteral("all messages notify here");
    return verdict;
}
