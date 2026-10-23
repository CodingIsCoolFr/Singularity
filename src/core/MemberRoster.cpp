#include "core/MemberRoster.h"

#include "core/Logger.h"
#include "core/MessageStore.h"
#include "core/RestClient.h"

#include <QDateTime>
#include <QJsonArray>
#include <QSet>
#include <QTimer>

namespace {

// A page a second at most: what a person paging through the Members page
// would ask for, never faster.
constexpr int PageGapMs = 1000;

} // namespace

MemberRoster::MemberRoster(RestClient *rest, MessageStore *store, QObject *parent)
    : QObject(parent)
    , m_rest(rest)
    , m_store(store)
{
}

bool MemberRoster::canSearch(quint64 permissions)
{
    constexpr quint64 KickMembers = 1ull << 1;
    constexpr quint64 BanMembers = 1ull << 2;
    constexpr quint64 Administrator = 1ull << 3;
    constexpr quint64 ManageGuild = 1ull << 5;
    constexpr quint64 ManageNicknames = 1ull << 27;
    constexpr quint64 ManageRoles = 1ull << 28;
    constexpr quint64 ModerateMembers = 1ull << 40;
    constexpr quint64 Any = KickMembers | BanMembers | Administrator | ManageGuild | ManageNicknames | ManageRoles
        | ModerateMembers;
    return (permissions & Any) != 0;
}

QJsonObject MemberRoster::cursorAfter(const QJsonObject &member)
{
    const QDateTime joined =
        QDateTime::fromString(member.value(QStringLiteral("joined_at")).toString(), Qt::ISODateWithMs);
    const QString userId = member.value(QStringLiteral("user")).toObject().value(QStringLiteral("id")).toString();
    if (!joined.isValid() || userId.isEmpty())
        return {};
    return QJsonObject{
        {QStringLiteral("guild_joined_at"), double(joined.toMSecsSinceEpoch())},
        {QStringLiteral("user_id"), userId},
    };
}

bool MemberRoster::complete(const QString &guildId) const
{
    return m_states.value(guildId).complete;
}

bool MemberRoster::loading(const QString &guildId) const
{
    return m_states.value(guildId).loading;
}

void MemberRoster::want(const QString &guildId, bool more)
{
    if (guildId.isEmpty() || !m_store)
        return;
    State &state = m_states[guildId];

    // Friends first, whatever else happens: one question, answered at once.
    if (!state.friendsAsked)
        askAboutFriends(guildId);

    if (!m_rest || state.refused || state.complete || !canSearch(m_store->selfPermissions(guildId)))
        return;
    state.limit = qMax(state.limit, more ? state.loaded + MoreLoad : FirstLoad);
    if (!state.loading && state.loaded < state.limit)
        nextPage(guildId);
}

void MemberRoster::askAboutFriends(const QString &guildId)
{
    State &state = m_states[guildId];
    state.friendsAsked = true;

    const QStringList known = m_store->knownMembers(guildId);
    const QSet<QString> already(known.cbegin(), known.cend());
    QStringList ids;
    for (const UserInfo &friendInfo : m_store->usersWithRelationship(1)) {
        if (!friendInfo.id.isEmpty() && !already.contains(friendInfo.id))
            ids.append(friendInfo.id);
    }
    for (const QString &id : ids)
        state.checked.insert(id);
    if (!ids.isEmpty())
        emit membersWanted(guildId, ids);
}

void MemberRoster::checkPresences(const QString &guildId, const QStringList &userIds)
{
    if (guildId.isEmpty())
        return;
    State &state = m_states[guildId];
    QStringList ids;
    for (const QString &id : userIds) {
        if (!id.isEmpty() && !state.checked.contains(id)) {
            state.checked.insert(id);
            ids.append(id);
        }
    }
    if (!ids.isEmpty())
        emit membersWanted(guildId, ids);
}

void MemberRoster::nextPage(const QString &guildId)
{
    State &state = m_states[guildId];
    state.loading = true;

    m_rest->searchGuildMembers(
        guildId, state.after, PageSize,
        [this, guildId](const QJsonObject &reply) {
            State &state = m_states[guildId];
            const QJsonArray members = reply.value(QStringLiteral("members")).toArray();
            QJsonObject last;
            for (const QJsonValue &value : members) {
                last = value.toObject().value(QStringLiteral("member")).toObject();
                m_store->rememberMember(guildId, last);
            }
            state.loaded += int(members.size());

            const int total = reply.value(QStringLiteral("total_result_count")).toInt(-1);
            const QJsonObject after = members.isEmpty() ? QJsonObject() : cursorAfter(last);
            // A short page is the last one; so is a page with no cursor to go on
            // from, or the next ask would start over.
            state.complete =
                members.size() < PageSize || after.isEmpty() || (total >= 0 && state.loaded >= total);
            state.after = after;

            wlog(QStringLiteral("members"),
                 QStringLiteral("listed %1 of %2 people in guild %3%4")
                     .arg(state.loaded)
                     .arg(total)
                     .arg(guildId, state.complete ? QStringLiteral(", everyone") : QString()));
            emit grew(guildId);

            if (state.complete || state.loaded >= state.limit) {
                state.loading = false;
                return;
            }
            QTimer::singleShot(PageGapMs, this, [this, guildId]() { nextPage(guildId); });
        },
        [this, guildId](const RestClient::Error &error) {
            State &state = m_states[guildId];
            state.loading = false;
            // Not allowed here after all: the friends and people seen stay,
            // and the search is not asked again this run.
            if (error.httpStatus == 401 || error.httpStatus == 403 || error.httpStatus == 404)
                state.refused = true;
            wlog(QStringLiteral("members"), QStringLiteral("could not list the people in guild %1: HTTP %2 %3")
                                                .arg(guildId)
                                                .arg(error.httpStatus)
                                                .arg(error.message));
        });
}
