#pragma once

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

// Decides whether a new message deserves a desktop notification, by the rules
// Discord's own client uses (checked against docs.discord.food, "User Guild
// Settings").
//
// Everything comes from what Discord already sends: READY's
// user_guild_settings (one entry per server, and one with guild_id null for
// direct messages), each server's default_message_notifications, and your own
// roles in each server from merged_members. USER_GUILD_SETTINGS_UPDATE keeps
// it current when you change a setting in any client.
//
// The rules, in order:
//   - A direct message or group DM notifies unless that chat is muted.
//   - In a server the level is the first that is set of: the channel's
//     override, its category's override, your setting for the server, the
//     server's default. 0 = all messages, 1 = only @mentions, 2 = nothing.
//   - A mention is you by name, @everyone/@here unless the server has them
//     suppressed, or a role of yours unless role mentions are suppressed.
//   - Muting a server or channel stops everything except mentions - Discord's
//     own wording: "unless you are mentioned". A mute with an end time that has
//     passed no longer counts.
class NotificationRules
{
public:
    void ingestReady(const QJsonObject &ready, const QString &selfUserId);
    void applySettingsEntry(const QJsonObject &entry);    // one user_guild_settings entry
    void applyGuild(const QJsonObject &guild);            // GUILD_CREATE / GUILD_UPDATE
    void setSelfRoles(const QString &guildId, const QStringList &roleIds);

    struct Verdict
    {
        bool notify = false;
        bool mentioned = false;
        QString reason;   // for the log: why not, or why yes
    };

    // `message` is the MESSAGE_CREATE payload. `parentId` is the channel's
    // category (or, for a thread, its parent channel), if known.
    Verdict judge(const QJsonObject &message, const QString &selfUserId, const QString &parentId,
                  qint64 nowMs) const;

private:
    struct Override
    {
        bool muted = false;
        qint64 muteEndsMs = 0;   // 0 = no end
        int level = 3;           // 3 = inherit
    };
    struct GuildSettings
    {
        bool muted = false;
        qint64 muteEndsMs = 0;
        int level = 3;
        bool suppressEveryone = false;
        bool suppressRoles = false;
        QHash<QString, Override> channels;
    };

    static bool mutedNow(bool muted, qint64 endsMs, qint64 nowMs);

    QHash<QString, GuildSettings> m_settings;   // "" = direct messages
    QHash<QString, int> m_guildDefaults;        // default_message_notifications
    QHash<QString, QSet<QString>> m_selfRoles;
};
