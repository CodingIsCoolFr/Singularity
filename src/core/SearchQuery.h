#pragma once

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// Discord's message search, as its search box writes it and as its API
// answers it.
//
// The box takes words and filters on one line: "cake from:alex has:image
// before:2024-05-01". The API takes the same thing as query parameters, with
// people and channels by id and dates turned into message ids. parse() splits
// the line, the window puts ids to the names, and toQuery() writes the
// parameters Discord's own client sends to /messages/search.
//
// Read from Discord's web client and docs.discord.food: arrays go as the same
// name repeated, a page is 25, and offset stops at 9975.
namespace SearchQuery {

constexpr int PageSize = 25;
constexpr int MaxOffset = 9975;

// One filter in the box. `key` is lower case, without the colon.
struct Filter
{
    QString key;     // from, mentions, in, has, before, during, after, pinned
    QString value;   // as typed, without quotes, @ or #
    QString id;      // the person's or channel's id, once one is known
};

struct Parsed
{
    QString content;          // the words, with the filters taken out
    QList<Filter> filters;

    bool isEmpty() const { return content.trimmed().isEmpty() && filters.isEmpty(); }
};

// The filters Discord's box offers, in the order its list shows them, with
// the hint it shows beside each.
QStringList filterKeys();
QString filterHint(const QString &key);

// What has: takes, by the word the box shows. "forward" is sent as
// "snapshot", which is what Discord calls a forwarded message.
QStringList hasValues();

Parsed parse(const QString &text);

// from, mentions and in name a person or a channel, so they need an id.
bool needsId(const QString &key);

enum class Sort { Newest, Oldest, Relevant };

struct Request
{
    Parsed query;
    Sort sort = Sort::Newest;
    int offset = 0;
    bool includeNsfw = false;   // servers only, and only for accounts allowed to see it
};

// The query parameters, in Discord's names. Dates are days on this machine's
// clock, as the official client reads them: before: is before that day
// starts, after: is after it ends, during: is the day itself. Anything that
// cannot be sent - a date it cannot read, a person with no id yet - is put in
// `problem`, in words for the person, and the list comes back empty.
QList<QPair<QString, QString>> toQuery(const Request &request, QString *problem = nullptr);

// A date the way the box takes one: 2024-05-01, 2024/05/01, 5/1/2024,
// today or yesterday. Invalid when it is none of those.
QDate readDate(const QString &text, const QDate &today = QDate::currentDate());

// The smallest message id Discord could have handed out at that moment.
QString snowflakeAt(const QDateTime &when);

// One page of Discord's answer.
struct Page
{
    int total = 0;
    QList<QJsonObject> messages;           // the hits, in Discord's order
    QHash<QString, QJsonObject> threads;   // threads the hits sit in, by id
};

// Reads {"total_results", "messages": [[message], ...], "threads"}. Each
// inner list used to carry the messages around the hit as well, with the hit
// marked "hit": true; the hit is taken either way.
Page readPage(const QJsonObject &reply);

// Searching every direct and group chat at once, as Discord's home page
// does: POST /users/@me/messages/search/tabs (SEARCH_TABS_DMS in Discord's
// own route table). There is no flat GET for it. The same filters go in a
// JSON body, inside one "tab":
//   {"tabs": {"messages": {"content": "...", "author_id": [...], "has": [...],
//              "sort_by": ..., "sort_order": ..., "offset": n, "limit": 25}},
//    "track_exact_total_hits": true}
// Names that can repeat in the query (author_id, mentions, channel_id, has)
// become arrays; offset and limit numbers; pinned and include_nsfw booleans.
QJsonObject toTabBody(const QList<QPair<QString, QString>> &query);

// The "messages" tab of a tab search's answer, which has the flat search's
// shape ({"messages", "total_results", "threads"}), so readPage reads it.
// Empty when the answer has no such tab: a 202 "not indexed yet" instead.
QJsonObject messagesTab(const QJsonObject &reply);

// The words a result is marked at: the search's words, without quotes and
// lower case.
QStringList highlightTerms(const QString &content);

// `plain` as rich text, with every word that starts with one of `terms`
// wrapped in <span class="hit">. Discord matches whole words and their
// endings ("cake" finds "cakes"), not the middle of one, and so does this.
// Line breaks become <br>.
QString highlightHtml(const QString &plain, const QStringList &terms);

} // namespace SearchQuery
