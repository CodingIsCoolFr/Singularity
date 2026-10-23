#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class MessageStore;
class RestClient;

// Everyone in a server, for the member list's Offline section.
//
// Discord's member list (GUILD_MEMBER_LIST_UPDATE) leaves the offline out of
// any server over 1,000 members, so its own client shows only who is online
// there. It does list everyone, offline too, on its Members page (Server
// Settings), which it opens to anyone holding one of the moderator
// permissions (canSearch); that page reads POST /guilds/{id}/members-search
// 250 people at a time. This does the same for the member list where that
// page would be open to you, a page at a time a second apart.
//
// Everywhere else it asks the gateway which of your friends are in the
// server (op 8 by user id, as the client does for people it needs to draw),
// and the list shows them and anyone else already seen there.
class MemberRoster : public QObject
{
    Q_OBJECT

public:
    MemberRoster(RestClient *rest, MessageStore *store, QObject *parent = nullptr);

    // Discord's chunk for its Members page: max(5 x page size, 250).
    static constexpr int PageSize = 250;
    // Listed by itself on opening a server. More arrive as the end of what
    // is listed is scrolled to (want with `more`).
    static constexpr int FirstLoad = 5000;
    static constexpr int MoreLoad = 2500;

    // Fill in a server's people, or carry on doing so.
    void want(const QString &guildId, bool more = false);

    // Whether these people are online, for rows about to be drawn: the
    // search does not say, and somebody online but further down Discord's
    // list than has been sent would otherwise be drawn as offline. Each
    // person is asked about once a run.
    void checkPresences(const QString &guildId, const QStringList &userIds);

    // Whether every member has been listed: the search reached its end.
    bool complete(const QString &guildId) const;
    // Whether the search is running for this server now.
    bool loading(const QString &guildId) const;

    // Discord's Members page is open to any one of: Administrator, Manage
    // Server, Ban Members, Kick Members, Timeout Members, Manage Roles,
    // Manage Nicknames (its MOD_DASH permissions).
    static bool canSearch(quint64 permissions);

    // Where the next page starts: Discord's cursor from the last member of a
    // page, {"guild_joined_at": ms, "user_id": id}. Empty without a join time.
    static QJsonObject cursorAfter(const QJsonObject &member);

signals:
    // People were added to the store for this server.
    void grew(const QString &guildId);
    // Which of these are members here and online (op 8 with user_ids and
    // presences, Discord's requestMembersById).
    void membersWanted(const QString &guildId, const QStringList &userIds);

private:
    struct State
    {
        QJsonObject after;
        int loaded = 0;
        int limit = 0;
        bool loading = false;
        bool complete = false;
        bool refused = false;   // the search said no; not asked again this run
        bool friendsAsked = false;
        QSet<QString> checked;   // asked about with presences
    };

    void nextPage(const QString &guildId);
    void askAboutFriends(const QString &guildId);

    RestClient *m_rest = nullptr;
    MessageStore *m_store = nullptr;
    QHash<QString, State> m_states;
};
