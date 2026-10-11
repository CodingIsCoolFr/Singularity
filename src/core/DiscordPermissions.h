#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Discord's permission bits, and the overwrite order from
// https://docs.discord.com/developers/topics/permissions
//
// The client has to compute these itself. Gateway READY still lists every
// channel in a server, including ones this account cannot view. Treating
// "it is in the sidebar" as "GET /channels/{id}/messages will work" is how
// a private channel opens and then 403s.
namespace DiscordPermissions {

constexpr quint64 Administrator = 1ull << 3;
constexpr quint64 ViewChannel = 1ull << 10;
constexpr quint64 SendMessages = 1ull << 11;
constexpr quint64 ReadMessageHistory = 1ull << 16;
constexpr quint64 Connect = 1ull << 20;
constexpr quint64 All = ~0ull;

// One entry from a channel's permission_overwrites array.
// type 0 is a role (the guild id is @everyone), type 1 is a member.
struct Overwrite
{
    QString id;
    int type = 0;
    quint64 allow = 0;
    quint64 deny = 0;
};

inline bool has(quint64 bits, quint64 flag)
{
    return (bits & flag) == flag;
}

// Guild-level bits: owner and Administrator are every bit. Otherwise
// @everyone OR'd with every role the member holds.
inline quint64 computeBase(bool owner, quint64 everyone, const QList<quint64> &rolePerms)
{
    if (owner)
        return All;

    quint64 permissions = everyone;
    for (quint64 role : rolePerms)
        permissions |= role;
    if (has(permissions, Administrator))
        return All;
    return permissions;
}

// Channel overwrites, in Discord's documented order: @everyone, then all
// matching roles (deny OR'd, then allow OR'd), then the member.
inline quint64 applyOverwrites(quint64 base, const QString &guildId, const QString &userId,
                               const QStringList &roleIds, const QList<Overwrite> &overwrites)
{
    if (has(base, Administrator))
        return All;

    quint64 permissions = base;
    const Overwrite *everyone = nullptr;
    const Overwrite *member = nullptr;
    QList<const Overwrite *> roles;
    for (const Overwrite &overwrite : overwrites) {
        if (overwrite.type == 1) {
            if (overwrite.id == userId)
                member = &overwrite;
        } else if (overwrite.id == guildId) {
            everyone = &overwrite;
        } else if (roleIds.contains(overwrite.id)) {
            roles.append(&overwrite);
        }
    }

    if (everyone) {
        permissions &= ~everyone->deny;
        permissions |= everyone->allow;
    }

    quint64 allow = 0;
    quint64 deny = 0;
    for (const Overwrite *role : roles) {
        allow |= role->allow;
        deny |= role->deny;
    }
    permissions &= ~deny;
    permissions |= allow;

    if (member) {
        permissions &= ~member->deny;
        permissions |= member->allow;
    }
    return permissions;
}

// What a failed GET /channels/{id}/messages actually is. 403 is not the
// only refusal, and 401 is a dead session, not a missing channel bit.
enum class HistoryFailure {
    RateLimit,
    Auth,
    MissingChannel,
    MissingAccess,
    MissingPermissions,
    Forbidden,
    Other,
};

inline HistoryFailure classifyHistoryFailure(int httpStatus, int discordCode)
{
    if (httpStatus == 429)
        return HistoryFailure::RateLimit;
    if (httpStatus == 401)
        return HistoryFailure::Auth;
    if (httpStatus == 404)
        return HistoryFailure::MissingChannel;
    if (httpStatus == 403 && discordCode == 50001)
        return HistoryFailure::MissingAccess;
    if (httpStatus == 403 && discordCode == 50013)
        return HistoryFailure::MissingPermissions;
    if (httpStatus == 403)
        return HistoryFailure::Forbidden;
    return HistoryFailure::Other;
}

} // namespace DiscordPermissions
