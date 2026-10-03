#include "core/MessageStore.h"

#include "core/AppConfig.h"
#include "core/Logger.h"

#include <QElapsedTimer>

#include <algorithm>

MessageStore::MessageStore(QObject *parent)
    : QObject(parent)
{
}

QDateTime UserInfo::createdAt() const
{
    bool ok = false;
    const quint64 snowflake = id.toULongLong(&ok);
    if (!ok || snowflake == 0)
        return {};
    // Discord ids hold milliseconds since 2015-01-01 in the top 42 bits.
    constexpr qint64 DiscordEpochMs = 1420070400000LL;
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(snowflake >> 22) + DiscordEpochMs).toLocalTime();
}

QString MessageStore::userName(const QString &userId) const
{
    return m_users.value(userId).displayName();
}

UserInfo MessageStore::user(const QString &userId) const
{
    UserInfo info = m_users.value(userId);
    if (info.id.isEmpty())
        info.id = userId;
    return info;
}

void MessageStore::rememberUser(const QJsonObject &rawUser)
{
    const QString id = rawUser.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
        return;

    UserInfo info = m_users.value(id);
    info.id = id;

    // Each field is only overwritten when the new payload actually carries it,
    // because partial user objects turn up all over the gateway.
    const QString username = rawUser.value(QStringLiteral("username")).toString();
    if (!username.isEmpty())
        info.username = username;

    if (rawUser.contains(QStringLiteral("global_name")))
        info.globalName = rawUser.value(QStringLiteral("global_name")).toString();
    else if (rawUser.contains(QStringLiteral("display_name")))
        info.globalName = rawUser.value(QStringLiteral("display_name")).toString();

    if (rawUser.contains(QStringLiteral("avatar")))
        info.avatarHash = rawUser.value(QStringLiteral("avatar")).toString();

    if (rawUser.contains(QStringLiteral("bot")))
        info.bot = rawUser.value(QStringLiteral("bot")).toBool();

    if (info.username.isEmpty() && info.globalName.isEmpty())
        return;

    m_users.insert(id, info);
}

void MessageStore::setRelationship(const QString &userId, int type, const QDateTime &since)
{
    if (userId.isEmpty())
        return;
    UserInfo info = m_users.value(userId);
    info.id = userId;
    info.relationship = type;
    if (since.isValid())
        info.friendsSince = since;
    else if (type == 0)
        info.friendsSince = QDateTime();
    m_users.insert(userId, info);
    emit userChanged(userId);
}

QList<UserInfo> MessageStore::usersWithRelationship(int type) const
{
    QList<UserInfo> result;
    for (auto it = m_users.constBegin(); it != m_users.constEnd(); ++it) {
        if (it.value().relationship == type)
            result.append(it.value());
    }

    std::sort(result.begin(), result.end(), [](const UserInfo &a, const UserInfo &b) {
        return a.displayName().localeAwareCompare(b.displayName()) < 0;
    });
    return result;
}

QString MessageStore::directChannelWith(const QString &userId) const
{
    for (const QString &channelId : m_directOrder) {
        const ChannelInfo channel = m_channels.value(channelId);
        // Only a one to one chat counts, not a group.
        if (channel.type == 1 && channel.recipientIds.size() == 1
            && channel.recipientIds.first() == userId) {
            return channelId;
        }
    }
    return {};
}

QList<RoleInfo> MessageStore::resolveRoles(const QString &guildId, const QStringList &roleIds) const
{
    const GuildInfo guild = m_guilds.value(guildId);

    QList<RoleInfo> result;
    for (const QString &roleId : roleIds) {
        // The @everyone role shares the guild id and is never shown.
        if (roleId == guildId)
            continue;
        const RoleInfo role = guild.roles.value(roleId);
        if (!role.id.isEmpty())
            result.append(role);
    }

    // Highest rank first, the way the real client lists them.
    std::sort(result.begin(), result.end(), [](const RoleInfo &a, const RoleInfo &b) {
        return a.position > b.position;
    });
    return result;
}

ActivityInfo MessageStore::parseActivity(const QJsonObject &raw)
{
    ActivityInfo activity;
    activity.type = raw.value(QStringLiteral("type")).toInt();
    activity.name = raw.value(QStringLiteral("name")).toString();
    activity.details = raw.value(QStringLiteral("details")).toString();
    activity.state = raw.value(QStringLiteral("state")).toString();
    activity.applicationId = raw.value(QStringLiteral("application_id")).toString();

    const QJsonObject assets = raw.value(QStringLiteral("assets")).toObject();
    activity.largeImage = assets.value(QStringLiteral("large_image")).toString();
    activity.smallImage = assets.value(QStringLiteral("small_image")).toString();
    activity.largeText = assets.value(QStringLiteral("large_text")).toString();

    const QJsonArray buttons = raw.value(QStringLiteral("buttons")).toArray();
    const QJsonArray urls = raw.value(QStringLiteral("metadata")).toObject()
                                 .value(QStringLiteral("button_urls")).toArray();
    for (int i = 0; i < buttons.size() && activity.buttonLabels.size() < 2; ++i) {
        const QJsonValue value = buttons.at(i);
        if (value.isObject()) {
            const QString label = value.toObject().value(QStringLiteral("label")).toString();
            if (label.isEmpty())
                continue;
            activity.buttonLabels.append(label);
            activity.buttonUrls.append(value.toObject().value(QStringLiteral("url")).toString());
        } else if (!value.toString().isEmpty()) {
            activity.buttonLabels.append(value.toString());
            activity.buttonUrls.append(i < urls.size() ? urls.at(i).toString() : QString());
        }
    }
    if (!activity.buttonLabels.isEmpty()) {
        activity.buttonLabel = activity.buttonLabels.first();
        activity.buttonUrl = activity.buttonUrls.value(0);
    }

    const QJsonObject emoji = raw.value(QStringLiteral("emoji")).toObject();
    activity.emoji = emoji.value(QStringLiteral("name")).toString();

    const QJsonObject timestamps = raw.value(QStringLiteral("timestamps")).toObject();
    activity.startMs = static_cast<qint64>(timestamps.value(QStringLiteral("start")).toDouble());
    activity.endMs = static_cast<qint64>(timestamps.value(QStringLiteral("end")).toDouble());

    return activity;
}

PresenceInfo MessageStore::presence(const QString &userId) const
{
    return m_presences.value(userId);
}

void MessageStore::setPresence(const QString &userId, const QJsonObject &rawPresence)
{
    if (userId.isEmpty())
        return;

    PresenceInfo info;
    info.status = rawPresence.value(QStringLiteral("status")).toString();

    const QJsonArray activities = rawPresence.value(QStringLiteral("activities")).toArray();
    for (const QJsonValue &value : activities) {
        const ActivityInfo activity = parseActivity(value.toObject());

        // One card per app. Discord merges the activities of every session a
        // person has open, and a session that just ended - the copy that
        // closed for an update a moment ago - stays in that list until
        // Discord lets it go. That showed "Playing Singularity" twice, with
        // two versions. The newer of the two (the later start) is the one
        // still running.
        bool replaced = false;
        if (!activity.isCustomStatus() && (!activity.applicationId.isEmpty() || !activity.name.isEmpty())) {
            for (ActivityInfo &kept : info.activities) {
                const bool sameApp = !activity.applicationId.isEmpty()
                    ? kept.applicationId == activity.applicationId
                    : (kept.applicationId.isEmpty() && kept.name == activity.name);
                if (kept.type != activity.type || !sameApp)
                    continue;
                if (activity.startMs > kept.startMs)
                    kept = activity;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            info.activities.append(activity);
    }

    m_presences.insert(userId, info);
    emit userChanged(userId);
}

VoiceStateInfo MessageStore::voiceState(const QString &userId) const
{
    return m_voiceStates.value(userId);
}

void MessageStore::setVoiceState(const QJsonObject &rawState)
{
    const QString userId = applyVoiceState(rawState);
    if (userId.isEmpty())
        return;
    emit userChanged(userId);
    emit voiceStatesChanged();
}

// Many at once, as VOICE_STATE_UPDATE_BATCH delivers them.
//
// Big servers do not send a VOICE_STATE_UPDATE per person any more: joins and
// leaves arrive grouped in this batch event, which Discord's own client feeds
// through exactly the same path as single updates. This client used to ignore
// it, so in a large server everyone who came or went in a batch was simply
// never seen - people who had left stayed listed, and people who had joined
// were missing.
void MessageStore::setVoiceStates(const QJsonArray &states)
{
    for (const QJsonValue &value : states)
        applyVoiceState(value.toObject());
    emit voiceStatesChanged();
}

// Forgets every voice state, before a complete snapshot replaces them.
//
// Discord's client does this when the supplemental half of READY arrives: it
// is the whole picture, so anything not in it is stale - somebody who left
// while this client was disconnected, or frozen, or reconnecting.
void MessageStore::clearVoiceStates()
{
    m_voiceStates.clear();
}

void MessageStore::dropVoiceStatesInChannel(const QString &channelId)
{
    if (channelId.isEmpty())
        return;
    bool changed = false;
    for (auto it = m_voiceStates.begin(); it != m_voiceStates.end();) {
        if (it.value().channelId == channelId) {
            it = m_voiceStates.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed)
        emit voiceStatesChanged();
}

QString MessageStore::applyVoiceState(const QJsonObject &rawState)
{
    const QString userId = rawState.value(QStringLiteral("user_id")).toString();
    if (userId.isEmpty())
        return {};

    // A voice state often carries the person, which is the only name we get
    // for someone who has never typed in a channel we have open.
    const QJsonObject member = rawState.value(QStringLiteral("member")).toObject();
    if (!member.isEmpty())
        rememberUser(member.value(QStringLiteral("user")).toObject());

    const QString channelId = rawState.value(QStringLiteral("channel_id")).toString();
    if (channelId.isEmpty()) {
        // A null channel means they left voice.
        m_voiceStates.remove(userId);
    } else {
        VoiceStateInfo state;
        state.channelId = channelId;
        state.guildId = rawState.value(QStringLiteral("guild_id")).toString();
        state.streaming = rawState.value(QStringLiteral("self_stream")).toBool();
        state.video = rawState.value(QStringLiteral("self_video")).toBool();
        state.muted = rawState.value(QStringLiteral("self_mute")).toBool()
            || rawState.value(QStringLiteral("mute")).toBool();
        state.deafened = rawState.value(QStringLiteral("self_deaf")).toBool()
            || rawState.value(QStringLiteral("deaf")).toBool();
        state.serverMuted = rawState.value(QStringLiteral("mute")).toBool();
        state.serverDeafened = rawState.value(QStringLiteral("deaf")).toBool();

        // Muting yourself sends a fresh voice state for the same channel.
        // Taking the clock from that would restart the timer every time
        // somebody touched a button, so the old one is kept while the channel
        // is the same.
        const VoiceStateInfo previous = m_voiceStates.value(userId);
        state.since = (previous.channelId == channelId && previous.since.isValid())
            ? previous.since
            : QDateTime::currentDateTimeUtc();

        m_voiceStates.insert(userId, state);
    }

    return userId;
}

void MessageStore::replaceGuildVoiceStates(const QString &guildId, const QJsonArray &states)
{
    if (guildId.isEmpty())
        return;

    QStringList gone;
    for (auto it = m_voiceStates.constBegin(); it != m_voiceStates.constEnd(); ++it) {
        if (it.value().guildId == guildId)
            gone.append(it.key());
    }
    for (const QString &userId : gone)
        m_voiceStates.remove(userId);

    for (const QJsonValue &value : states) {
        QJsonObject state = value.toObject();
        if (!state.contains(QStringLiteral("guild_id")))
            state.insert(QStringLiteral("guild_id"), guildId);
        const QString userId = state.value(QStringLiteral("user_id")).toString();
        if (userId.isEmpty() || state.value(QStringLiteral("channel_id")).toString().isEmpty())
            continue;

        const QJsonObject member = state.value(QStringLiteral("member")).toObject();
        if (!member.isEmpty())
            rememberUser(member.value(QStringLiteral("user")).toObject());

        VoiceStateInfo info;
        info.channelId = state.value(QStringLiteral("channel_id")).toString();
        info.guildId = guildId;
        info.streaming = state.value(QStringLiteral("self_stream")).toBool();
        info.video = state.value(QStringLiteral("self_video")).toBool();
        info.muted = state.value(QStringLiteral("self_mute")).toBool()
            || state.value(QStringLiteral("mute")).toBool();
        info.deafened = state.value(QStringLiteral("self_deaf")).toBool()
            || state.value(QStringLiteral("deaf")).toBool();
        info.serverMuted = state.value(QStringLiteral("mute")).toBool();
        info.serverDeafened = state.value(QStringLiteral("deaf")).toBool();
        info.since = QDateTime::currentDateTimeUtc();
        m_voiceStates.insert(userId, info);
    }

    emit voiceStatesChanged();
}

void MessageStore::noteMemberListRequest(const QString &guildId)
{
    if (!guildId.isEmpty())
        m_memberListFresh.insert(guildId);
}

void MessageStore::clearMemberList(const QString &guildId)
{
    if (!m_memberLists.contains(guildId))
        return;
    m_memberLists.remove(guildId);
    emit memberListChanged(guildId);
}

void MessageStore::noteActivity(const QString &userId)
{
    if (userId.isEmpty())
        return;
    m_lastSeenActive.insert(userId, QDateTime::currentDateTime());
    emit userChanged(userId);
}

QDateTime MessageStore::lastSeenActive(const QString &userId) const
{
    return m_lastSeenActive.value(userId);
}

void MessageStore::forgetActivity(const QString &userId)
{
    if (m_lastSeenActive.remove(userId))
        emit userChanged(userId);
}

QString MessageStore::activityHint(const QString &userId) const
{
    // Anything Discord actually reports beats a guess.
    if (presence(userId).isOnline())
        return {};

    const QDateTime seen = m_lastSeenActive.value(userId);
    if (!seen.isValid())
        return {};

    const int window = AppConfig::instance()
                           .pluginValue(QStringLiteral("presence-hints"), QStringLiteral("window"), 10)
                           .toInt();

    const qint64 minutes = seen.secsTo(QDateTime::currentDateTime()) / 60;
    if (minutes > window)
        return {};

    return minutes < 1 ? QStringLiteral("active just now")
                       : QStringLiteral("active %1 min ago").arg(minutes);
}

QString MessageStore::presenceBubble(const QString &userId) const
{
    const PresenceInfo info = presence(userId);
    if (info.isOnline())
        return info.status;
    if (!activityHint(userId).isEmpty())
        return QStringLiteral("hint");
    return info.status;
}

QStringList MessageStore::voiceMembers(const QString &channelId) const
{
    QStringList result;
    if (channelId.isEmpty())
        return result;

    for (auto it = m_voiceStates.constBegin(); it != m_voiceStates.constEnd(); ++it) {
        if (it.value().channelId == channelId)
            result.append(it.key());
    }

    // Stable order, by display name, so the list does not jump around.
    std::sort(result.begin(), result.end(), [this](const QString &a, const QString &b) {
        return userName(a).localeAwareCompare(userName(b)) < 0;
    });
    return result;
}

void MessageStore::clear()
{
    m_users.clear();
    m_presences.clear();
    m_lastSeenActive.clear();
    m_voiceStates.clear();
    m_guilds.clear();
    m_guildOrder.clear();
    m_channels.clear();
    m_directOrder.clear();
    m_messages.clear();
    m_historyLoaded.clear();
    m_reads.clear();
}

void MessageStore::ingestReady(const QJsonObject &readyPayload)
{
    clear();

    // READY carries one flat list of every user the client needs to name.
    // Direct message channels then refer to them by id only, so this has to be
    // read before the channels are.
    const QJsonArray users = readyPayload.value(QStringLiteral("users")).toArray();
    for (const QJsonValue &value : users)
        rememberUser(value.toObject());

    const QJsonArray relationships = readyPayload.value(QStringLiteral("relationships")).toArray();
    for (const QJsonValue &value : relationships) {
        const QJsonObject entry = value.toObject();
        rememberUser(entry.value(QStringLiteral("user")).toObject());

        QString otherId = entry.value(QStringLiteral("user_id")).toString();
        if (otherId.isEmpty())
            otherId = entry.value(QStringLiteral("id")).toString();
        const QDateTime since =
            QDateTime::fromString(entry.value(QStringLiteral("since")).toString(), Qt::ISODateWithMs);
        setRelationship(otherId, entry.value(QStringLiteral("type")).toInt(),
                        since.isValid() ? since.toLocalTime() : QDateTime());
    }

    // Friends' online state arrives up front. Server members only appear once
    // that server is subscribed to.
    const QJsonArray presences = readyPayload.value(QStringLiteral("presences")).toArray();
    for (const QJsonValue &value : presences) {
        const QJsonObject entry = value.toObject();
        QString userId = entry.value(QStringLiteral("user_id")).toString();
        if (userId.isEmpty())
            userId = entry.value(QStringLiteral("user")).toObject().value(QStringLiteral("id")).toString();
        setPresence(userId, entry);
    }

    const QJsonArray guilds = readyPayload.value(QStringLiteral("guilds")).toArray();
    for (const QJsonValue &value : guilds)
        ingestGuild(value.toObject());

    // Your own member record in each server: merged_members runs parallel to
    // guilds, and it is where your roles are.
    m_selfUserId = readyPayload.value(QStringLiteral("user")).toObject().value(QStringLiteral("id")).toString();
    const QJsonArray merged = readyPayload.value(QStringLiteral("merged_members")).toArray();
    for (int i = 0; i < guilds.size() && i < merged.size(); ++i) {
        const QString guildId = guilds.at(i).toObject().value(QStringLiteral("id")).toString();
        for (const QJsonValue &value : merged.at(i).toArray()) {
            const QJsonObject member = value.toObject();
            const QString who = member.value(QStringLiteral("user_id")).toString();
            if (!who.isEmpty() && who != m_selfUserId)
                continue;
            QStringList roles;
            for (const QJsonValue &role : member.value(QStringLiteral("roles")).toArray())
                roles.append(role.toString());
            m_selfRoles.insert(guildId, roles);
        }
    }

    const QJsonArray privateChannels = readyPayload.value(QStringLiteral("private_channels")).toArray();
    for (const QJsonValue &value : privateChannels) {
        const QJsonObject raw = value.toObject();
        ingestChannel(raw, QString());
        const QString id = raw.value(QStringLiteral("id")).toString();
        if (!id.isEmpty() && !m_directOrder.contains(id))
            m_directOrder.append(id);
    }

    // READY lists the chats, not in the order the sidebar uses. Discord puts
    // whoever you spoke to most recently at the top, and a snowflake is that
    // time.
    std::sort(m_directOrder.begin(), m_directOrder.end(), [this](const QString &a, const QString &b) {
        return newerId(m_channels.value(a).lastMessageId, m_channels.value(b).lastMessageId);
    });

    ingestReadState(readyPayload);
}

void MessageStore::ingestReadySupplemental(const QJsonObject &payload)
{
    // The guilds here line up one for one, by position, with the guilds in
    // READY. They carry no names, only ids and state.
    const QJsonArray guilds = payload.value(QStringLiteral("guilds")).toArray();

    // A complete snapshot, so start clean, exactly as Discord's VoiceStateStore
    // does on CONNECTION_OPEN_SUPPLEMENTAL. Adding it on top of what was
    // already known is what left people listed in channels they had left
    // while this client was reconnecting.
    clearVoiceStates();

    for (int index = 0; index < guilds.size(); ++index) {
        const QJsonObject entry = guilds.at(index).toObject();

        QString guildId = entry.value(QStringLiteral("id")).toString();
        if (guildId.isEmpty() && index < m_guildOrder.size())
            guildId = m_guildOrder.at(index);

        const QJsonArray voiceStates = entry.value(QStringLiteral("voice_states")).toArray();
        for (const QJsonValue &value : voiceStates) {
            QJsonObject state = value.toObject();
            if (!state.contains(QStringLiteral("guild_id")) && !guildId.isEmpty())
                state.insert(QStringLiteral("guild_id"), guildId);
            applyVoiceState(state);
        }
    }
    emit voiceStatesChanged();

    // Presences arrive in the same shape: one array per guild, in order.
    const QJsonObject merged = payload.value(QStringLiteral("merged_presences")).toObject();

    const QJsonArray guildPresences = merged.value(QStringLiteral("guilds")).toArray();
    for (const QJsonValue &group : guildPresences) {
        const QJsonArray list = group.toArray();
        for (const QJsonValue &value : list) {
            const QJsonObject presence = value.toObject();
            setPresence(presence.value(QStringLiteral("user_id")).toString(), presence);
        }
    }

    const QJsonArray friendPresences = merged.value(QStringLiteral("friends")).toArray();
    for (const QJsonValue &value : friendPresences) {
        const QJsonObject presence = value.toObject();
        setPresence(presence.value(QStringLiteral("user_id")).toString(), presence);
    }

    // Members ride along too, which is how names appear under a voice channel.
    const QJsonArray mergedMembers = payload.value(QStringLiteral("merged_members")).toArray();
    for (const QJsonValue &group : mergedMembers) {
        const QJsonArray list = group.toArray();
        for (const QJsonValue &value : list)
            rememberUser(value.toObject().value(QStringLiteral("user")).toObject());
    }

    wlog(QStringLiteral("store"), QStringLiteral("supplemental: %1 guilds, %2 people in voice, "
                                                 "%3 presences")
                                      .arg(guilds.size())
                                      .arg(m_voiceStates.size())
                                      .arg(m_presences.size()));

    emit voiceStatesChanged();
}

void MessageStore::ingestMemberListUpdate(const QJsonObject &payload)
{
    const QString guildId = payload.value(QStringLiteral("guild_id")).toString();
    if (guildId.isEmpty())
        return;

    const QString listId = payload.value(QStringLiteral("id")).toString();
    MemberList &list = m_memberLists[guildId];

    // Two subscriptions answer with two list ids. Writing both into one
    // list is how the same person was drawn twice. A channel we just opened
    // may legitimately be a new list; anything else for a different id is
    // the spare copy and is dropped.
    const bool fresh = m_memberListFresh.remove(guildId);
    if (!fresh && !list.listId.isEmpty() && !listId.isEmpty() && listId != list.listId)
        return;

    int learned = 0;

    // These two are the server's own totals and are not the number of rows:
    // only the first hundred rows are ever asked for, so a large server sends
    // a hundred people and a member count in the thousands.
    if (payload.contains(QStringLiteral("online_count")))
        list.onlineCount = payload.value(QStringLiteral("online_count")).toInt();
    if (payload.contains(QStringLiteral("member_count")))
        list.memberCount = payload.value(QStringLiteral("member_count")).toInt();

    // How many are in each group, which arrives beside the rows rather than
    // on them.
    //
    // The headings inside the list carry an id and, often, nothing else -
    // which is why every one of them read "— 0". The real counts are in this
    // separate list, and they are kept by id so a heading can be labelled
    // whenever it is drawn.
    const QJsonArray groups = payload.value(QStringLiteral("groups")).toArray();
    if (!groups.isEmpty()) {
        list.groupCounts.clear();
        for (const QJsonValue &value : groups) {
            const QJsonObject group = value.toObject();
            const QString id = group.value(QStringLiteral("id")).toString();
            if (!id.isEmpty())
                list.groupCounts.insert(id, group.value(QStringLiteral("count")).toInt());
        }
    }

    // The payload is a list of edits to a list. Each edit carries items that
    // are either a group heading or a member, and a member brings a user and
    // their status with it.
    const auto readItem = [this, &learned](const QJsonValue &value) -> MemberRow {
        const QJsonObject item = value.toObject();

        const QJsonObject group = item.value(QStringLiteral("group")).toObject();
        if (!group.isEmpty()) {
            MemberRow row;
            row.heading = true;
            row.groupId = group.value(QStringLiteral("id")).toString();
            row.groupCount = group.value(QStringLiteral("count")).toInt();
            return row;
        }

        const QJsonObject member = item.value(QStringLiteral("member")).toObject();
        if (member.isEmpty())
            return {};

        const QJsonObject user = member.value(QStringLiteral("user")).toObject();
        const QString userId = user.value(QStringLiteral("id")).toString();
        if (userId.isEmpty())
            return {};

        rememberUser(user);

        const QJsonObject presence = member.value(QStringLiteral("presence")).toObject();
        if (!presence.isEmpty()) {
            setPresence(userId, presence);
            ++learned;
        }

        MemberRow row;
        row.userId = userId;
        row.nickname = member.value(QStringLiteral("nick")).toString();

        // Which role decides their colour is not sent, only the roles they
        // hold. Discord's rule is the highest-ranked role that sets one, and
        // the ranking lives on the guild - so the ids are kept and the answer
        // worked out when it is drawn, where the guild is to hand.
        const QJsonArray roles = member.value(QStringLiteral("roles")).toArray();
        if (!roles.isEmpty())
            row.colourRoleId = roles.first().toString();

        return row;
    };

    const QJsonArray ops = payload.value(QStringLiteral("ops")).toArray();
    for (const QJsonValue &opValue : ops) {
        const QJsonObject op = opValue.toObject();
        const QString kind = op.value(QStringLiteral("op")).toString();

        if (kind == QLatin1String("SYNC")) {
            // A whole range at once. An empty one is a repeat of a range we
            // already have, not a cleared list — replacing with it blanked
            // the panel.
            const QJsonArray items = op.value(QStringLiteral("items")).toArray();
            if (items.isEmpty())
                continue;

            QList<MemberRow> rows;
            QSet<QString> seen;
            rows.reserve(items.size());
            for (const QJsonValue &item : items) {
                const MemberRow row = readItem(item);
                if (row.heading) {
                    rows.append(row);
                    continue;
                }
                if (row.userId.isEmpty() || seen.contains(row.userId))
                    continue;
                seen.insert(row.userId);
                rows.append(row);
            }
            list.rows = rows;
            if (!listId.isEmpty())
                list.listId = listId;
            continue;
        }

        // The single-row edits. Discord numbers them against the list as it
        // believes we hold it, so an index past the end is not an error - it
        // means our copy is short, and appending is the closest honest answer.
        const int index = op.value(QStringLiteral("index")).toInt(-1);

        if (kind == QLatin1String("DELETE")) {
            if (index >= 0 && index < list.rows.size())
                list.rows.removeAt(index);
            continue;
        }

        if (kind == QLatin1String("INSERT") || kind == QLatin1String("UPDATE")) {
            const MemberRow row = readItem(op.value(QStringLiteral("item")));
            if (!row.heading && row.userId.isEmpty())
                continue;

            if (kind == QLatin1String("UPDATE") && index >= 0 && index < list.rows.size()) {
                list.rows[index] = row;
            } else if (!row.heading && !row.userId.isEmpty()) {
                // Already on the list. A second insert of the same person is
                // what the panel was drawing as two profiles.
                int existing = -1;
                for (int i = 0; i < list.rows.size(); ++i) {
                    if (!list.rows.at(i).heading && list.rows.at(i).userId == row.userId) {
                        existing = i;
                        break;
                    }
                }
                if (existing >= 0)
                    list.rows[existing] = row;
                else if (index >= 0 && index <= list.rows.size())
                    list.rows.insert(index, row);
                else
                    list.rows.append(row);
            } else if (index >= 0 && index <= list.rows.size()) {
                list.rows.insert(index, row);
            } else {
                list.rows.append(row);
            }
        }
    }

    {
        QList<MemberRow> unique;
        QSet<QString> seen;
        unique.reserve(list.rows.size());
        for (const MemberRow &row : list.rows) {
            if (!row.heading && !row.userId.isEmpty()) {
                if (seen.contains(row.userId))
                    continue;
                seen.insert(row.userId);
            }
            unique.append(row);
        }
        list.rows = unique;
    }

    if (learned > 0) {
        wlog(QStringLiteral("store"),
             QStringLiteral("member list: %1 rows, %2 of %3 online, learned %4 statuses in guild %5")
                 .arg(list.rows.size())
                 .arg(list.onlineCount)
                 .arg(list.memberCount)
                 .arg(learned)
                 .arg(guildId));
    }

    emit memberListChanged(guildId);
}

MemberList MessageStore::memberList(const QString &guildId) const
{
    return m_memberLists.value(guildId);
}

void MessageStore::applyGuild(const QJsonObject &rawGuild)
{
    ingestGuild(rawGuild);
}

void MessageStore::ingestGuild(const QJsonObject &rawGuild)
{
    GuildInfo guild;
    guild.id = rawGuild.value(QStringLiteral("id")).toString();
    if (guild.id.isEmpty())
        return;

    // READY sends guild properties either inline or nested under "properties"
    // depending on which capability flags the client asked for.
    const QJsonObject properties = rawGuild.contains(QStringLiteral("properties"))
        ? rawGuild.value(QStringLiteral("properties")).toObject()
        : rawGuild;

    guild.name = properties.value(QStringLiteral("name")).toString();
    guild.ownerId = properties.value(QStringLiteral("owner_id")).toString();
    guild.iconHash = properties.value(QStringLiteral("icon")).toString();
    guild.bannerHash = properties.value(QStringLiteral("banner")).toString();
    guild.premiumTier = properties.value(QStringLiteral("premium_tier")).toInt();
    guild.boostBarEnabled = properties.value(QStringLiteral("premium_progress_bar_enabled")).toBool();
    for (const QJsonValue &feature : properties.value(QStringLiteral("features")).toArray())
        guild.features.append(feature.toString());

    // The boost count sits beside "properties" in READY, not inside it, and
    // inside the object itself in GUILD_CREATE / GUILD_UPDATE.
    guild.boostCount = rawGuild.contains(QStringLiteral("premium_subscription_count"))
        ? rawGuild.value(QStringLiteral("premium_subscription_count")).toInt()
        : properties.value(QStringLiteral("premium_subscription_count")).toInt();

    // Roles are needed to colour the pills on a profile card.
    const QJsonArray roles = rawGuild.value(QStringLiteral("roles")).toArray();
    for (const QJsonValue &value : roles) {
        const QJsonObject rawRole = value.toObject();
        RoleInfo role;
        role.id = rawRole.value(QStringLiteral("id")).toString();
        if (role.id.isEmpty())
            continue;
        role.name = rawRole.value(QStringLiteral("name")).toString();
        role.colour = rawRole.value(QStringLiteral("color")).toInt();
        role.position = rawRole.value(QStringLiteral("position")).toInt();
        // A string, because the bits do not fit in a JSON number.
        role.permissions = rawRole.value(QStringLiteral("permissions")).toString().toULongLong();
        role.managed = rawRole.value(QStringLiteral("managed")).toBool();
        guild.roles.insert(role.id, role);
    }

    const QJsonArray emojis = rawGuild.value(QStringLiteral("emojis")).toArray();
    for (const QJsonValue &value : emojis) {
        const QJsonObject rawEmoji = value.toObject();
        EmojiInfo emoji;
        emoji.id = rawEmoji.value(QStringLiteral("id")).toString();
        emoji.name = rawEmoji.value(QStringLiteral("name")).toString();
        emoji.animated = rawEmoji.value(QStringLiteral("animated")).toBool();
        if (!emoji.id.isEmpty() && !emoji.name.isEmpty())
            guild.emojis.append(emoji);
    }

    const QJsonArray stickers = rawGuild.value(QStringLiteral("stickers")).toArray();
    for (const QJsonValue &value : stickers) {
        const QJsonObject rawSticker = value.toObject();
        GuildSticker sticker;
        sticker.id = rawSticker.value(QStringLiteral("id")).toString();
        sticker.name = rawSticker.value(QStringLiteral("name")).toString();
        sticker.formatType = rawSticker.value(QStringLiteral("format_type")).toInt(1);
        if (!sticker.id.isEmpty() && sticker.formatType != 3)
            guild.stickers.append(sticker);
    }

    const QJsonArray channels = rawGuild.value(QStringLiteral("channels")).toArray();
    for (const QJsonValue &value : channels) {
        const QJsonObject rawChannel = value.toObject();
        const QString channelId = rawChannel.value(QStringLiteral("id")).toString();
        if (channelId.isEmpty())
            continue;
        ingestChannel(rawChannel, guild.id);
        guild.channelIds.append(channelId);
    }

    const bool known = m_guilds.contains(guild.id);
    m_guilds.insert(guild.id, guild);
    if (!known)
        m_guildOrder.append(guild.id);

    // A later, complete copy of the server replaces the voice snapshot we
    // had. Large servers arrive almost empty and fill in afterwards.
    if (rawGuild.contains(QStringLiteral("voice_states")))
        replaceGuildVoiceStates(guild.id, rawGuild.value(QStringLiteral("voice_states")).toArray());
}

void MessageStore::ingestChannel(const QJsonObject &rawChannel, const QString &guildId)
{
    ChannelInfo channel;
    channel.id = rawChannel.value(QStringLiteral("id")).toString();
    if (channel.id.isEmpty())
        return;

    channel.guildId = guildId;
    channel.parentId = rawChannel.value(QStringLiteral("parent_id")).toString();
    channel.type = rawChannel.value(QStringLiteral("type")).toInt();
    channel.position = rawChannel.value(QStringLiteral("position")).toInt();
    channel.topic = rawChannel.value(QStringLiteral("topic")).toString();
    channel.name = rawChannel.value(QStringLiteral("name")).toString();
    channel.bitrate = rawChannel.value(QStringLiteral("bitrate")).toInt();
    channel.userLimit = rawChannel.value(QStringLiteral("user_limit")).toInt();
    channel.rtcRegion = rawChannel.value(QStringLiteral("rtc_region")).toString();
    channel.lastMessageId = rawChannel.value(QStringLiteral("last_message_id")).toString();

    // Direct messages have no name, so build one from the other people in it.
    if (channel.name.isEmpty() && channel.isDirect()) {
        QStringList names;

        // Newer clients get full user objects here...
        const QJsonArray recipients = rawChannel.value(QStringLiteral("recipients")).toArray();
        for (const QJsonValue &value : recipients) {
            const QJsonObject user = value.toObject();
            rememberUser(user);
            const QString recipientId = user.value(QStringLiteral("id")).toString();
            if (!recipientId.isEmpty())
                channel.recipientIds.append(recipientId);
            const QString display = userName(recipientId);
            if (!display.isEmpty())
                names.append(display);
        }

        // ...but with capabilities set, Discord sends bare ids instead and
        // expects the client to look them up in the READY user list.
        if (names.isEmpty()) {
            const QJsonArray recipientIds = rawChannel.value(QStringLiteral("recipient_ids")).toArray();
            for (const QJsonValue &value : recipientIds) {
                const QString recipientId = value.toString();
                if (recipientId.isEmpty())
                    continue;
                channel.recipientIds.append(recipientId);
                const QString display = userName(recipientId);
                if (!display.isEmpty())
                    names.append(display);
            }
        }

        if (names.isEmpty())
            channel.name = channel.type == 3 ? QStringLiteral("Group chat") : QStringLiteral("Direct message");
        else
            channel.name = names.join(QStringLiteral(", "));
    }

    m_channels.insert(channel.id, channel);
}

bool MessageStore::forgetGuild(const QString &guildId)
{
    if (!m_guilds.contains(guildId))
        return false;

    const GuildInfo guild = m_guilds.take(guildId);
    m_guildOrder.removeAll(guildId);
    for (const QString &channelId : guild.channelIds) {
        m_channels.remove(channelId);
        m_messages.remove(channelId);
        m_reads.remove(channelId);
    }
    return true;
}

bool MessageStore::forgetChannel(const QString &channelId)
{
    if (!m_channels.contains(channelId))
        return false;

    const ChannelInfo info = m_channels.take(channelId);
    m_messages.remove(channelId);
    m_reads.remove(channelId);
    m_directOrder.removeAll(channelId);

    if (!info.guildId.isEmpty() && m_guilds.contains(info.guildId)) {
        GuildInfo guild = m_guilds.value(info.guildId);
        guild.channelIds.removeAll(channelId);
        m_guilds.insert(info.guildId, guild);
    }

    if (info.isDirect())
        emit directOrderChanged();
    return true;
}

void MessageStore::ingestChannelObject(const QJsonObject &rawChannel)
{
    const QString guildId = rawChannel.value(QStringLiteral("guild_id")).toString();
    ingestChannel(rawChannel, guildId);

    const QString id = rawChannel.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
        return;

    const ChannelInfo channel = m_channels.value(id);
    if (channel.isDirect() && !m_directOrder.contains(id)) {
        // Newest chats sit at the top, the same as the real client.
        m_directOrder.prepend(id);
    } else if (!guildId.isEmpty() && m_guilds.contains(guildId)) {
        GuildInfo guild = m_guilds.value(guildId);
        if (!guild.channelIds.contains(id)) {
            guild.channelIds.append(id);
            m_guilds.insert(guildId, guild);
        }
    }
}

QList<GuildInfo> MessageStore::guilds() const
{
    QList<GuildInfo> result;
    result.reserve(m_guildOrder.size());
    for (const QString &id : m_guildOrder) {
        if (m_guilds.contains(id))
            result.append(m_guilds.value(id));
    }
    std::sort(result.begin(), result.end(), [](const GuildInfo &a, const GuildInfo &b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });
    return result;
}

GuildInfo MessageStore::guild(const QString &guildId) const
{
    return m_guilds.value(guildId);
}

ChannelInfo MessageStore::channel(const QString &channelId) const
{
    return m_channels.value(channelId);
}

QList<ChannelInfo> MessageStore::channelsOfGuild(const QString &guildId) const
{
    QList<ChannelInfo> result;
    const GuildInfo info = m_guilds.value(guildId);
    for (const QString &channelId : info.channelIds) {
        const ChannelInfo channel = m_channels.value(channelId);
        if (channel.isTextLike())
            result.append(channel);
    }
    std::sort(result.begin(), result.end(), [](const ChannelInfo &a, const ChannelInfo &b) {
        if (a.position != b.position)
            return a.position < b.position;
        return a.name.localeAwareCompare(b.name) < 0;
    });
    return result;
}

QList<ChannelGroup> MessageStore::groupedChannels(const QString &guildId) const
{
    const GuildInfo info = m_guilds.value(guildId);

    // Split the guild's channels into categories and the rest.
    QList<ChannelInfo> categories;
    QHash<QString, QList<ChannelInfo>> children;
    QList<ChannelInfo> loose;

    for (const QString &channelId : info.channelIds) {
        const ChannelInfo channel = m_channels.value(channelId);
        if (channel.isCategory()) {
            categories.append(channel);
        } else if (channel.isTextLike() || channel.isVoice()) {
            // Voice channels belong in the sidebar too. Modern Discord gives
            // them their own text chat, so they open like any other channel.
            if (channel.parentId.isEmpty())
                loose.append(channel);
            else
                children[channel.parentId].append(channel);
        }
    }

    // Discord lists text channels above voice ones inside each category.
    const auto byPosition = [](const ChannelInfo &a, const ChannelInfo &b) {
        if (a.isVoice() != b.isVoice())
            return b.isVoice();
        if (a.position != b.position)
            return a.position < b.position;
        return a.name.localeAwareCompare(b.name) < 0;
    };

    std::sort(categories.begin(), categories.end(), byPosition);
    std::sort(loose.begin(), loose.end(), byPosition);

    QList<ChannelGroup> result;

    // Channels with no category sit at the top, with no heading.
    if (!loose.isEmpty())
        result.append(ChannelGroup{QString(), loose});

    for (const ChannelInfo &category : categories) {
        QList<ChannelInfo> list = children.value(category.id);
        if (list.isEmpty())
            continue;
        std::sort(list.begin(), list.end(), byPosition);
        result.append(ChannelGroup{category.name, list});
    }

    // A category may have been missing from READY. Do not lose its channels.
    for (auto it = children.constBegin(); it != children.constEnd(); ++it) {
        const bool known = std::any_of(categories.constBegin(), categories.constEnd(),
                                       [&it](const ChannelInfo &c) { return c.id == it.key(); });
        if (known)
            continue;
        QList<ChannelInfo> list = it.value();
        std::sort(list.begin(), list.end(), byPosition);
        result.append(ChannelGroup{QString(), list});
    }

    return result;
}

void MessageStore::bumpDirectChannel(const QString &channelId)
{
    if (!m_channels.value(channelId).isDirect())
        return;

    const int index = m_directOrder.indexOf(channelId);
    if (index == 0)
        return;
    if (index > 0)
        m_directOrder.removeAt(index);
    m_directOrder.prepend(channelId);
    emit directOrderChanged();
}

QList<ChannelInfo> MessageStore::directChannels() const
{
    QList<ChannelInfo> result;
    for (const QString &id : m_directOrder) {
        const ChannelInfo channel = m_channels.value(id);
        if (channel.isTextLike())
            result.append(channel);
    }
    return result;
}

QList<MessageInfo> MessageStore::messages(const QString &channelId) const
{
    return m_messages.value(channelId);
}

bool MessageStore::hasHistory(const QString &channelId) const
{
    return m_historyLoaded.value(channelId, false);
}

MessageInfo MessageStore::parseMessage(const QJsonObject &raw)
{
    MessageInfo message;
    message.id = raw.value(QStringLiteral("id")).toString();
    message.channelId = raw.value(QStringLiteral("channel_id")).toString();
    message.content = raw.value(QStringLiteral("content")).toString();
    message.type = raw.value(QStringLiteral("type")).toInt();

    const QJsonObject author = raw.value(QStringLiteral("author")).toObject();
    message.authorId = author.value(QStringLiteral("id")).toString();
    message.authorName = author.value(QStringLiteral("global_name")).toString();
    if (message.authorName.isEmpty())
        message.authorName = author.value(QStringLiteral("username")).toString();
    message.authorAvatar = author.value(QStringLiteral("avatar")).toString();

    message.timestamp = QDateTime::fromString(raw.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
    if (!message.timestamp.isValid())
        message.timestamp = QDateTime::currentDateTime();
    message.timestamp = message.timestamp.toLocalTime();

    message.edited = !raw.value(QStringLiteral("edited_timestamp")).isNull()
        && raw.contains(QStringLiteral("edited_timestamp"));
    message.applicationId = raw.value(QStringLiteral("application_id")).toString();
    message.flags = raw.value(QStringLiteral("flags")).toInt();

    const QJsonObject call = raw.value(QStringLiteral("call")).toObject();
    if (raw.contains(QStringLiteral("call")) && !raw.value(QStringLiteral("call")).isNull()) {
        message.callPresent = true;
        for (const QJsonValue &id : call.value(QStringLiteral("participants")).toArray()) {
            const QString userId = id.toString();
            if (!userId.isEmpty())
                message.callParticipants.append(userId);
        }
        const QJsonValue ended = call.value(QStringLiteral("ended_timestamp"));
        if (ended.isString() && !ended.toString().isEmpty()) {
            message.callEnded = QDateTime::fromString(ended.toString(), Qt::ISODateWithMs);
            if (!message.callEnded.isValid())
                message.callEnded = QDateTime::fromString(ended.toString(), Qt::ISODate);
            if (message.callEnded.isValid())
                message.callEnded = message.callEnded.toLocalTime();
        }
    }

    const QJsonArray attachments = raw.value(QStringLiteral("attachments")).toArray();
    for (const QJsonValue &value : attachments) {
        const QJsonObject raw = value.toObject();
        Attachment attachment;
        attachment.url = raw.value(QStringLiteral("url")).toString();
        if (attachment.url.isEmpty())
            continue;
        attachment.filename = raw.value(QStringLiteral("filename")).toString();
        attachment.contentType = raw.value(QStringLiteral("content_type")).toString();
        attachment.size = static_cast<qint64>(raw.value(QStringLiteral("size")).toDouble());
        // Only pictures carry width and height, which is how isImage() works.
        attachment.width = raw.value(QStringLiteral("width")).toInt();
        attachment.height = raw.value(QStringLiteral("height")).toInt();
        message.attachments.append(attachment);
    }

    // --- link previews ----------------------------------------------------
    const QJsonArray embeds = raw.value(QStringLiteral("embeds")).toArray();
    for (const QJsonValue &value : embeds) {
        const QJsonObject rawEmbed = value.toObject();

        EmbedInfo embed;
        embed.type = rawEmbed.value(QStringLiteral("type")).toString();
        embed.url = rawEmbed.value(QStringLiteral("url")).toString();
        embed.title = rawEmbed.value(QStringLiteral("title")).toString();
        embed.description = rawEmbed.value(QStringLiteral("description")).toString();
        embed.colour = rawEmbed.value(QStringLiteral("color")).toInt();
        embed.providerName = rawEmbed.value(QStringLiteral("provider"))
                                 .toObject()
                                 .value(QStringLiteral("name"))
                                 .toString();
        embed.authorName = rawEmbed.value(QStringLiteral("author"))
                               .toObject()
                               .value(QStringLiteral("name"))
                               .toString();

        // Prefer "image", fall back to "thumbnail". Always take proxy_url:
        // that address is served by Discord, so the client never has to call
        // out to whatever site the link came from.
        for (const QString &key : {QStringLiteral("image"), QStringLiteral("thumbnail")}) {
            const QJsonObject picture = rawEmbed.value(key).toObject();
            if (picture.isEmpty())
                continue;

            QString url = picture.value(QStringLiteral("proxy_url")).toString();
            if (url.isEmpty())
                url = picture.value(QStringLiteral("url")).toString();
            if (url.isEmpty())
                continue;

            embed.imageUrl = url;
            embed.imageWidth = picture.value(QStringLiteral("width")).toInt();
            embed.imageHeight = picture.value(QStringLiteral("height")).toInt();
            break;
        }

        // A Tenor or Klipy card's picture is a poster. The thing that moves
        // is the video next to it.
        const QJsonObject video = rawEmbed.value(QStringLiteral("video")).toObject();
        QString videoUrl = video.value(QStringLiteral("proxy_url")).toString();
        if (videoUrl.isEmpty())
            videoUrl = video.value(QStringLiteral("url")).toString();
        embed.videoUrl = videoUrl;
        if (!videoUrl.isEmpty()
            && (embed.type == QLatin1String("gifv") || embed.type == QLatin1String("video"))) {
            if (embed.imageWidth <= 0)
                embed.imageWidth = video.value(QStringLiteral("width")).toInt();
            if (embed.imageHeight <= 0)
                embed.imageHeight = video.value(QStringLiteral("height")).toInt();
        }

        if (embed.imageUrl.isEmpty() && embed.title.isEmpty() && embed.description.isEmpty())
            continue;

        message.embeds.append(embed);
    }

    // --- stickers ---------------------------------------------------------
    const QJsonArray stickers = raw.value(QStringLiteral("sticker_items")).toArray();
    for (const QJsonValue &value : stickers) {
        const QJsonObject rawSticker = value.toObject();
        StickerInfo sticker;
        sticker.id = rawSticker.value(QStringLiteral("id")).toString();
        if (sticker.id.isEmpty())
            continue;
        sticker.name = rawSticker.value(QStringLiteral("name")).toString();
        sticker.formatType = rawSticker.value(QStringLiteral("format_type")).toInt(1);
        message.stickers.append(sticker);
    }

    // --- reactions --------------------------------------------------------
    const QJsonArray reactions = raw.value(QStringLiteral("reactions")).toArray();
    for (const QJsonValue &value : reactions) {
        const QJsonObject rawReaction = value.toObject();
        const QJsonObject emoji = rawReaction.value(QStringLiteral("emoji")).toObject();

        ReactionInfo reaction;
        reaction.name = emoji.value(QStringLiteral("name")).toString();
        reaction.id = emoji.value(QStringLiteral("id")).toString();
        reaction.animated = emoji.value(QStringLiteral("animated")).toBool();
        reaction.count = rawReaction.value(QStringLiteral("count")).toInt();
        reaction.mine = rawReaction.value(QStringLiteral("me")).toBool();

        if (reaction.count > 0 && (!reaction.name.isEmpty() || !reaction.id.isEmpty()))
            message.reactions.append(reaction);
    }

    message.components = raw.value(QStringLiteral("components")).toArray();

    // A game invite: empty body, an activity, and the application it belongs to.
    const QJsonObject activity = raw.value(QStringLiteral("activity")).toObject();
    if (!activity.isEmpty()) {
        message.activityType = activity.value(QStringLiteral("type")).toInt();
        message.activityPartyId = activity.value(QStringLiteral("party_id")).toString();
        const QJsonObject application = raw.value(QStringLiteral("application")).toObject();
        message.activityApplicationId = application.value(QStringLiteral("id")).toString();
        if (message.activityApplicationId.isEmpty())
            message.activityApplicationId = message.applicationId;
        message.activityApplicationName = application.value(QStringLiteral("name")).toString();
        message.activityApplicationIcon = application.value(QStringLiteral("icon")).toString();
    }

    // --- forwarded / reply ------------------------------------------------
    // Type 1 is a forward. Type 0, or none, on a reply is the message being
    // answered. Discord sends that message beside it as referenced_message.
    const QJsonObject reference = raw.value(QStringLiteral("message_reference")).toObject();
    if (reference.value(QStringLiteral("type")).toInt() == 1) {
        message.forwardedFromChannelId = reference.value(QStringLiteral("channel_id")).toString();
        message.forwardedFromGuildId = reference.value(QStringLiteral("guild_id")).toString();
        message.forwardedFromMessageId = reference.value(QStringLiteral("message_id")).toString();
    } else if (message.type == 19 || !reference.value(QStringLiteral("message_id")).toString().isEmpty()) {
        message.replyToId = reference.value(QStringLiteral("message_id")).toString();
        if (raw.contains(QStringLiteral("referenced_message"))
            && raw.value(QStringLiteral("referenced_message")).isNull()) {
            message.replyMissing = true;
        } else {
            const QJsonObject referenced = raw.value(QStringLiteral("referenced_message")).toObject();
            if (!referenced.isEmpty()) {
                const QJsonObject repliedAuthor = referenced.value(QStringLiteral("author")).toObject();
                message.replyAuthorId = repliedAuthor.value(QStringLiteral("id")).toString();
                message.replyAuthorName = repliedAuthor.value(QStringLiteral("global_name")).toString();
                if (message.replyAuthorName.isEmpty())
                    message.replyAuthorName = repliedAuthor.value(QStringLiteral("username")).toString();
                message.replyAuthorAvatar = repliedAuthor.value(QStringLiteral("avatar")).toString();
                message.replyContent = referenced.value(QStringLiteral("content")).toString();
                message.replyEdited = referenced.contains(QStringLiteral("edited_timestamp"))
                    && !referenced.value(QStringLiteral("edited_timestamp")).isNull();
                message.replyHasAttachment = !referenced.value(QStringLiteral("attachments")).toArray().isEmpty()
                    || !referenced.value(QStringLiteral("embeds")).toArray().isEmpty()
                    || !referenced.value(QStringLiteral("sticker_items")).toArray().isEmpty();
            }
        }
    }
    const QJsonArray snapshots = raw.value(QStringLiteral("message_snapshots")).toArray();
    for (const QJsonValue &value : snapshots) {
        const QJsonObject inner = value.toObject().value(QStringLiteral("message")).toObject();
        if (!inner.isEmpty())
            message.snapshots.append(parseMessage(inner));
    }

    return message;
}

void MessageStore::setHistory(const QString &channelId, const QJsonArray &rawMessages)
{
    // Timed, because this runs on the thread that draws the window.
    //
    // Fetching ahead means this happens for channels nobody opened - twenty
    // five of them in the first half minute of one session - and every one of
    // those is time the window is not responding. If that is expensive it has
    // to be visible, not inferred.
    QElapsedTimer clock;
    clock.start();

    QList<MessageInfo> list;
    list.reserve(rawMessages.size());
    // Discord returns newest first. The view wants oldest first.
    for (auto it = rawMessages.constEnd(); it != rawMessages.constBegin();) {
        --it;
        rememberUser(it->toObject().value(QStringLiteral("author")).toObject());
        MessageInfo message = parseMessage(it->toObject());
        if (message.channelId.isEmpty())
            message.channelId = channelId;
        list.append(message);
    }

    m_messages.insert(channelId, list);
    m_historyLoaded.insert(channelId, true);
    m_historyOrder.removeOne(channelId);
    m_historyOrder.append(channelId);

    const qint64 parsedMs = clock.elapsed();
    if (parsedMs > 8) {
        wlog(QStringLiteral("store"),
             QStringLiteral("parsed %1 messages in %2 ms").arg(list.size()).arg(parsedMs));
    }

    emit channelHistoryChanged(channelId);
}

int MessageStore::prependHistory(const QString &channelId, const QJsonArray &rawMessages)
{
    if (rawMessages.isEmpty())
        return 0;

    // Discord returns newest first here as well, so this walks backwards to
    // end up oldest first, the same order the view wants.
    QList<MessageInfo> older;
    older.reserve(rawMessages.size());
    for (auto it = rawMessages.constEnd(); it != rawMessages.constBegin();) {
        --it;
        rememberUser(it->toObject().value(QStringLiteral("author")).toObject());
        MessageInfo message = parseMessage(it->toObject());
        if (message.channelId.isEmpty())
            message.channelId = channelId;
        older.append(message);
    }

    QList<MessageInfo> &list = m_messages[channelId];

    // Anything already on screen is dropped from the new batch rather than
    // added twice. Discord can repeat a message at the boundary, and a
    // duplicate reads as the person having said it twice.
    QSet<QString> known;
    known.reserve(list.size());
    for (const MessageInfo &message : list)
        known.insert(message.id);

    QList<MessageInfo> fresh;
    fresh.reserve(older.size());
    for (const MessageInfo &message : older) {
        if (!known.contains(message.id))
            fresh.append(message);
    }

    if (fresh.isEmpty())
        return 0;

    list = fresh + list;
    emit channelHistoryChanged(channelId);
    return fresh.size();
}

QString MessageStore::oldestMessageId(const QString &channelId) const
{
    const QList<MessageInfo> list = m_messages.value(channelId);
    return list.isEmpty() ? QString() : list.first().id;
}

bool MessageStore::newerId(const QString &a, const QString &b)
{
    if (a.isEmpty())
        return false;
    if (b.isEmpty())
        return true;
    return a.toULongLong() > b.toULongLong();
}

void MessageStore::ingestReadState(const QJsonObject &readyPayload)
{
    const QJsonValue raw = readyPayload.value(QStringLiteral("read_state"));
    QJsonArray entries;
    if (raw.isArray())
        entries = raw.toArray();
    else if (raw.isObject())
        entries = raw.toObject().value(QStringLiteral("entries")).toArray();

    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        const QString channelId = entry.value(QStringLiteral("id")).toString();
        if (channelId.isEmpty())
            continue;

        ReadMark mark;
        mark.lastReadId = entry.value(QStringLiteral("last_message_id")).toString();
        mark.mentions = entry.value(QStringLiteral("mention_count")).toInt();
        const QString latest = m_channels.value(channelId).lastMessageId;
        mark.unread = newerId(latest, mark.lastReadId) || mark.mentions > 0;
        m_reads.insert(channelId, mark);
    }
}

void MessageStore::noteIncoming(const QString &channelId, const QString &messageId, bool mention,
                                bool seen)
{
    if (channelId.isEmpty() || messageId.isEmpty())
        return;

    if (m_channels.contains(channelId))
        m_channels[channelId].lastMessageId = messageId;
    // The message is stored just before this runs, so the id already matches
    // and a "is this newer" check never succeeds. A direct chat still jumps
    // to the top, including one you just sent yourself.
    bumpDirectChannel(channelId);

    ReadMark mark = m_reads.value(channelId);
    if (seen) {
        mark.lastReadId = messageId;
        mark.mentions = 0;
        mark.unread = false;
    } else if (newerId(messageId, mark.lastReadId)) {
        mark.unread = true;
        if (mention)
            ++mark.mentions;
    }
    m_reads.insert(channelId, mark);
    emit readStateChanged();
}

void MessageStore::markChannelRead(const QString &channelId, const QString &messageId)
{
    if (channelId.isEmpty())
        return;

    ReadMark mark = m_reads.value(channelId);
    if (!messageId.isEmpty())
        mark.lastReadId = messageId;
    mark.mentions = 0;
    mark.unread = false;
    m_reads.insert(channelId, mark);
    emit readStateChanged();
}

void MessageStore::markChannelsRead(const QList<QPair<QString, QString>> &channelsAndMessages)
{
    // The same as markChannelRead for each, with one change signal at the
    // end. One per channel redrew the rail and channel list 3,394 times for
    // Read All and froze the window for half a second.
    for (const auto &pair : channelsAndMessages) {
        if (pair.first.isEmpty())
            continue;
        ReadMark mark = m_reads.value(pair.first);
        if (!pair.second.isEmpty())
            mark.lastReadId = pair.second;
        mark.mentions = 0;
        mark.unread = false;
        m_reads.insert(pair.first, mark);
    }
    if (!channelsAndMessages.isEmpty())
        emit readStateChanged();
}

bool MessageStore::isUnread(const QString &channelId) const
{
    const ReadMark mark = m_reads.value(channelId);
    if (mark.unread || mark.mentions > 0)
        return true;
    return newerId(m_channels.value(channelId).lastMessageId, mark.lastReadId);
}

void MessageStore::setSelfRoles(const QString &guildId, const QStringList &roleIds)
{
    if (!guildId.isEmpty())
        m_selfRoles.insert(guildId, roleIds);
}

RolePower MessageStore::rolePower(const QString &guildId) const
{
    constexpr quint64 Administrator = 1ull << 3;
    constexpr quint64 ManageRoles = 1ull << 28;

    RolePower power;
    const GuildInfo guild = m_guilds.value(guildId);
    if (guild.id.isEmpty())
        return power;
    power.owner = !m_selfUserId.isEmpty() && guild.ownerId == m_selfUserId;

    quint64 permissions = guild.roles.value(guildId).permissions;   // @everyone
    for (const QString &roleId : m_selfRoles.value(guildId)) {
        const RoleInfo role = guild.roles.value(roleId);
        permissions |= role.permissions;
        power.highest = qMax(power.highest, role.position);
    }
    power.canManage = power.owner || (permissions & (Administrator | ManageRoles)) != 0;
    return power;
}

quint64 MessageStore::selfPermissions(const QString &guildId) const
{
    constexpr quint64 Administrator = 1ull << 3;

    const GuildInfo guild = m_guilds.value(guildId);
    if (guild.id.isEmpty())
        return 0;
    if (!m_selfUserId.isEmpty() && guild.ownerId == m_selfUserId)
        return ~0ull;

    quint64 permissions = guild.roles.value(guildId).permissions;   // @everyone
    for (const QString &roleId : m_selfRoles.value(guildId))
        permissions |= guild.roles.value(roleId).permissions;
    if (permissions & Administrator)
        return ~0ull;
    return permissions;
}

QList<QPair<QString, QString>> MessageStore::unreadChannels() const
{
    QList<QPair<QString, QString>> found;
    for (auto it = m_channels.constBegin(); it != m_channels.constEnd(); ++it) {
        if (it.value().lastMessageId.isEmpty() || !isUnread(it.key()))
            continue;
        found.append({it.key(), it.value().lastMessageId});
    }
    return found;
}

int MessageStore::mentionCount(const QString &channelId) const
{
    return m_reads.value(channelId).mentions;
}

bool MessageStore::guildHasUnread(const QString &guildId) const
{
    const GuildInfo guild = m_guilds.value(guildId);
    for (const QString &channelId : guild.channelIds) {
        if (isUnread(channelId))
            return true;
    }
    return false;
}

int MessageStore::guildMentionCount(const QString &guildId) const
{
    int total = 0;
    const GuildInfo guild = m_guilds.value(guildId);
    for (const QString &channelId : guild.channelIds)
        total += mentionCount(channelId);
    return total;
}

void MessageStore::applyReaction(const QJsonObject &data, bool added, const QString &selfUserId)
{
    const QString channelId = data.value(QStringLiteral("channel_id")).toString();
    const QString messageId = data.value(QStringLiteral("message_id")).toString();
    if (channelId.isEmpty() || messageId.isEmpty())
        return;

    QList<MessageInfo> &list = m_messages[channelId];
    const auto target = std::find_if(list.begin(), list.end(), [&messageId](const MessageInfo &item) {
        return item.id == messageId;
    });
    // A reaction on a message we never loaded has nowhere to go. Discord sends
    // these for the whole channel, not only for what is on screen.
    if (target == list.end())
        return;

    const QJsonObject emoji = data.value(QStringLiteral("emoji")).toObject();

    ReactionInfo incoming;
    incoming.name = emoji.value(QStringLiteral("name")).toString();
    incoming.id = emoji.value(QStringLiteral("id")).toString();
    incoming.animated = emoji.value(QStringLiteral("animated")).toBool();

    const bool isMine = !selfUserId.isEmpty()
        && data.value(QStringLiteral("user_id")).toString() == selfUserId;

    QList<ReactionInfo> &reactions = target->reactions;
    const auto existing = std::find_if(reactions.begin(), reactions.end(),
                                       [&incoming](const ReactionInfo &item) {
                                           return item.matches(incoming);
                                       });

    if (added) {
        if (existing == reactions.end()) {
            incoming.count = 1;
            incoming.mine = isMine;
            reactions.append(incoming);
        } else {
            ++existing->count;
            if (isMine)
                existing->mine = true;
        }
    } else {
        if (existing == reactions.end())
            return;

        --existing->count;
        if (isMine)
            existing->mine = false;

        // The last one going means the pill goes with it.
        if (existing->count <= 0)
            reactions.erase(existing);
    }

    emit messageChanged(channelId, messageId);
}

void MessageStore::clearReactions(const QJsonObject &data)
{
    const QString channelId = data.value(QStringLiteral("channel_id")).toString();
    const QString messageId = data.value(QStringLiteral("message_id")).toString();
    if (channelId.isEmpty() || messageId.isEmpty())
        return;

    QList<MessageInfo> &list = m_messages[channelId];
    const auto target = std::find_if(list.begin(), list.end(), [&messageId](const MessageInfo &item) {
        return item.id == messageId;
    });
    if (target == list.end() || target->reactions.isEmpty())
        return;

    // REMOVE_EMOJI names one; REMOVE_ALL names none and takes the lot.
    const QJsonObject emoji = data.value(QStringLiteral("emoji")).toObject();
    if (emoji.isEmpty()) {
        target->reactions.clear();
    } else {
        ReactionInfo wanted;
        wanted.name = emoji.value(QStringLiteral("name")).toString();
        wanted.id = emoji.value(QStringLiteral("id")).toString();

        target->reactions.removeIf([&wanted](const ReactionInfo &item) { return item.matches(wanted); });
    }

    emit messageChanged(channelId, messageId);
}

int MessageStore::appendMessage(const QJsonObject &rawMessage)
{
    rememberUser(rawMessage.value(QStringLiteral("author")).toObject());
    const MessageInfo message = parseMessage(rawMessage);
    if (message.channelId.isEmpty() || message.id.isEmpty())
        return -1;

    QList<MessageInfo> &list = m_messages[message.channelId];
    const auto existing = std::find_if(list.begin(), list.end(), [&message](const MessageInfo &item) {
        return item.id == message.id;
    });
    if (existing != list.end())
        return -1;

    list.append(message);
    if (m_channels.contains(message.channelId))
        m_channels[message.channelId].lastMessageId = message.id;

    // Keep memory flat on busy channels. The caller has to move its window
    // back by the same number, or the next redraw points at the wrong message.
    int dropped = 0;
    if (list.size() > 500) {
        dropped = list.size() - 500;
        list.remove(0, dropped);
    }

    emit messageAdded(message.channelId, message);
    return dropped;
}

void MessageStore::updateMessage(const QJsonObject &rawMessage)
{
    const QString channelId = rawMessage.value(QStringLiteral("channel_id")).toString();
    const QString messageId = rawMessage.value(QStringLiteral("id")).toString();
    if (!m_messages.contains(channelId))
        return;

    QList<MessageInfo> &list = m_messages[channelId];
    for (MessageInfo &item : list) {
        if (item.id != messageId)
            continue;
        // MESSAGE_UPDATE is a partial object. Only touch fields that arrived.
        if (rawMessage.contains(QStringLiteral("content")))
            item.content = rawMessage.value(QStringLiteral("content")).toString();
        if (rawMessage.contains(QStringLiteral("components")))
            item.components = rawMessage.value(QStringLiteral("components")).toArray();
        if (rawMessage.contains(QStringLiteral("application_id")))
            item.applicationId = rawMessage.value(QStringLiteral("application_id")).toString();
        if (rawMessage.contains(QStringLiteral("flags")))
            item.flags = rawMessage.value(QStringLiteral("flags")).toInt();
        if (rawMessage.contains(QStringLiteral("call")) && !rawMessage.value(QStringLiteral("call")).isNull()) {
            const MessageInfo parsed = parseMessage(rawMessage);
            item.callPresent = parsed.callPresent;
            item.callParticipants = parsed.callParticipants;
            item.callEnded = parsed.callEnded;
        }
        // A link preview arrives here, after the message, once Discord has
        // fetched the page. That update is not an edit. Treating it as one
        // drew "(edited)" and dropped the card, so a YouTube link stayed a
        // bare address.
        if (rawMessage.contains(QStringLiteral("embeds"))) {
            const MessageInfo parsed = parseMessage(rawMessage);
            item.embeds = parsed.embeds;
            wlog(QStringLiteral("ui"),
                 QStringLiteral("link preview on %1: %2").arg(messageId).arg(item.embeds.size()));
        }
        if (rawMessage.contains(QStringLiteral("edited_timestamp")))
            item.edited = !rawMessage.value(QStringLiteral("edited_timestamp")).isNull();
        emit messageChanged(channelId, messageId);
        return;
    }
}

void MessageStore::endCall(const QString &channelId)
{
    if (!m_messages.contains(channelId))
        return;
    QList<MessageInfo> &list = m_messages[channelId];
    const QDateTime now = QDateTime::currentDateTime();
    for (MessageInfo &item : list) {
        if (item.type != 3 || item.callEnded.isValid())
            continue;
        item.callPresent = true;
        item.callEnded = now;
        emit messageChanged(channelId, item.id);
    }
}

QString MessageStore::memorySummary() const
{
    int messages = 0;
    for (auto it = m_messages.constBegin(); it != m_messages.constEnd(); ++it)
        messages += it.value().size();

    return QStringLiteral("%1 channels holding %2 messages, %3 users known")
        .arg(m_messages.size())
        .arg(messages)
        .arg(m_users.size());
}

void MessageStore::trimHistories(const QString &keepChannelId)
{
    // Enough for every channel somebody is actually moving between, and for
    // everything fetched ahead in the server they are in. Well past that, a
    // channel nobody has opened in a long while can be fetched again in the
    // third of a second it took the first time.
    constexpr int KeepChannels = 24;

    while (m_messages.size() > KeepChannels && !m_historyOrder.isEmpty()) {
        const QString oldest = m_historyOrder.constFirst();
        m_historyOrder.removeFirst();

        // Never the one on screen.
        if (oldest == keepChannelId) {
            m_historyOrder.append(oldest);
            // Everything else was already tried; stop rather than spin.
            if (m_historyOrder.size() <= 1)
                break;
            continue;
        }

        m_messages.remove(oldest);
        m_historyLoaded.remove(oldest);
    }
}

void MessageStore::markDeleted(const QString &channelId, const QString &messageId)
{
    if (!m_messages.contains(channelId))
        return;
    QList<MessageInfo> &list = m_messages[channelId];
    for (MessageInfo &item : list) {
        if (item.id != messageId)
            continue;
        item.deleted = true;
        emit messageChanged(channelId, messageId);
        return;
    }
}

void MessageStore::removeMessage(const QString &channelId, const QString &messageId)
{
    if (!m_messages.contains(channelId))
        return;
    QList<MessageInfo> &list = m_messages[channelId];
    const auto it = std::find_if(list.begin(), list.end(), [&messageId](const MessageInfo &item) {
        return item.id == messageId;
    });
    if (it == list.end())
        return;
    list.erase(it);
    emit channelHistoryChanged(channelId);
}
