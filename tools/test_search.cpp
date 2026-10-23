// Message search, checked against Discord's own shapes with nothing sent to
// Discord.
//
// A stand-in RestClient answers every GET from a list of replies written the
// way Discord's API writes them - a page of results, the 202 "not indexed
// yet", a 429, a member search - and keeps every path it was asked for. So
// the line the box writes, the query that goes out, the waiting, the results
// pane and the jump it asks for are all checked here. On top of that every
// Qt network request is pointed at a closed port on this machine, so a
// picture or a call that slips past the stand-in goes nowhere.
//
// Settings and the log go to a temporary folder, never the real ones.

#include "core/MessageStore.h"
#include "core/RestClient.h"
#include "core/SearchQuery.h"
#include "core/Logger.h"
#include "ui/MediaCache.h"
#include "ui/SearchPanel.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMouseEvent>
#include <QNetworkProxy>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <functional>

static int g_failed = 0;
static int g_passed = 0;

static void expect(bool ok, const char *name)
{
    if (ok) {
        ++g_passed;
        std::printf("ok   %s\n", name);
    } else {
        ++g_failed;
        std::printf("FAIL %s\n", name);
    }
}

namespace {

const QString Guild = QStringLiteral("900000000000000001");
const QString General = QStringLiteral("900000000000000010");
const QString Baking = QStringLiteral("900000000000000011");
const QString Thread = QStringLiteral("900000000000000099");
const QString Direct = QStringLiteral("900000000000000050");
const QString Alex = QStringLiteral("800000000000000001");
const QString Sam = QStringLiteral("800000000000000002");
const QString Robin = QStringLiteral("800000000000000003");
const QString Self = QStringLiteral("800000000000000009");

// Runs the event loop until `done` or the time is up.
bool waitFor(const std::function<bool()> &done, int ms = 3000)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

// A RestClient whose GETs never leave: each is answered from `replies`, the
// first whose path start matches, used once. Anything unmatched is Discord's
// 404. Answers come on the next pass of the event loop, as a reply does.
class StandInRest : public RestClient
{
public:
    struct Reply
    {
        QString pathStart;
        int status = 200;
        QJsonDocument body;
    };
    QList<Reply> replies;
    QStringList asked;
    QStringList posted;   // "POST /path {json}"

    void reply(const QString &pathStart, int status, const QJsonObject &body)
    {
        replies.append({pathStart, status, QJsonDocument(body)});
    }
    void reply(const QString &pathStart, const QJsonArray &body)
    {
        replies.append({pathStart, 200, QJsonDocument(body)});
    }

protected:
    void sendGet(const QString &path, ObjectHandler onObject, ArrayHandler onArray, ErrorHandler onError) override
    {
        asked.append(path);
        Reply answer{QString(), 404,
                     QJsonDocument(QJsonObject{{QStringLiteral("message"), QStringLiteral("404: Not Found")},
                                               {QStringLiteral("code"), 0}})};
        for (int i = 0; i < replies.size(); ++i) {
            if (path.startsWith(replies.at(i).pathStart)) {
                answer = replies.takeAt(i);
                break;
            }
        }
        QTimer::singleShot(0, this, [answer, onObject, onArray, onError]() {
            if (answer.status >= 200 && answer.status < 300) {
                if (answer.body.isArray() && onArray)
                    onArray(answer.body.array());
                else if (onObject)
                    onObject(answer.body.object());
                return;
            }
            Error error;
            error.httpStatus = answer.status;
            error.body = answer.body.object();
            error.message = error.body.value(QStringLiteral("message")).toString();
            if (onError)
                onError(error);
        });
    }

    // A POST is answered from the same list; what it would have sent is kept
    // in `posted`.
    void sendJson(const QByteArray &verb, const QString &path, const QByteArray &json, const CaptchaProof &,
                  ObjectHandler onOk, ErrorHandler onError) override
    {
        posted.append(QString::fromLatin1(verb) + QLatin1Char(' ') + path + QLatin1Char(' ') + QString::fromUtf8(json));
        sendGet(path, std::move(onOk), nullptr, std::move(onError));
    }
};

// A message as Discord's search hands it back.
QJsonObject message(const QString &id, const QString &channelId, const QString &authorId, const QString &username,
                    const QString &content, const QString &when, bool hit = true)
{
    QJsonObject author{
        {QStringLiteral("id"), authorId},
        {QStringLiteral("username"), username},
        {QStringLiteral("global_name"), QJsonValue::Null},
        {QStringLiteral("avatar"), QJsonValue::Null},
        {QStringLiteral("discriminator"), QStringLiteral("0")},
        {QStringLiteral("public_flags"), 0},
    };
    QJsonObject raw{
        {QStringLiteral("id"), id},
        {QStringLiteral("type"), 0},
        {QStringLiteral("content"), content},
        {QStringLiteral("channel_id"), channelId},
        {QStringLiteral("author"), author},
        {QStringLiteral("attachments"), QJsonArray{}},
        {QStringLiteral("embeds"), QJsonArray{}},
        {QStringLiteral("mentions"), QJsonArray{}},
        {QStringLiteral("mention_roles"), QJsonArray{}},
        {QStringLiteral("pinned"), false},
        {QStringLiteral("mention_everyone"), false},
        {QStringLiteral("tts"), false},
        {QStringLiteral("timestamp"), when},
        {QStringLiteral("edited_timestamp"), QJsonValue::Null},
        {QStringLiteral("flags"), 0},
        {QStringLiteral("components"), QJsonArray{}},
    };
    if (hit)
        raw.insert(QStringLiteral("hit"), true);
    return raw;
}

// A page of results, each hit in a list of its own, as Discord sends it now.
QJsonObject page(int total, const QList<QJsonObject> &hits, const QJsonArray &threads = {})
{
    QJsonArray groups;
    for (const QJsonObject &hit : hits)
        groups.append(QJsonArray{hit});
    QJsonObject body{
        {QStringLiteral("total_results"), total},
        {QStringLiteral("messages"), groups},
        {QStringLiteral("analytics_id"), QStringLiteral("2c8d6a0f3b1e4b7a9a6e0c2f5d4e3b21")},
        {QStringLiteral("doing_deep_historical_index"), false},
    };
    if (!threads.isEmpty())
        body.insert(QStringLiteral("threads"), threads);
    return body;
}

// Discord's answer for a place it has not indexed yet (HTTP 202).
QJsonObject notIndexed()
{
    return {
        {QStringLiteral("message"), QStringLiteral("Index not yet available. Try again later")},
        {QStringLiteral("code"), 110000},
        {QStringLiteral("documents_indexed"), 0},
        {QStringLiteral("retry_after"), 0},
    };
}

QJsonObject tooFast()
{
    return {
        {QStringLiteral("message"), QStringLiteral("You are being rate limited.")},
        {QStringLiteral("retry_after"), 0.05},
        {QStringLiteral("global"), false},
    };
}

bool hasParam(const QList<QPair<QString, QString>> &query, const QString &key, const QString &value)
{
    return query.contains({key, value});
}

QString paramValue(const QList<QPair<QString, QString>> &query, const QString &key)
{
    for (const auto &pair : query) {
        if (pair.first == key)
            return pair.second;
    }
    return {};
}

// --- the line and the query --------------------------------------------------

void queryTests()
{
    using namespace SearchQuery;

    const Parsed parsed = parse(QStringLiteral("chocolate cake from:alex has:image before:2024-05-01 pinned:true "
                                               "in:general"));
    expect(parsed.content == QLatin1String("chocolate cake"), "words are what is left without the filters");
    expect(parsed.filters.size() == 5, "five filters read");
    expect(parsed.filters.value(0).key == QLatin1String("from") && parsed.filters.value(0).value == QLatin1String("alex"),
           "from: and its name");
    expect(parsed.filters.value(4).key == QLatin1String("in") && parsed.filters.value(4).value == QLatin1String("general"),
           "in: and its channel");

    const Parsed spaced = parse(QStringLiteral("from: alex cake"));
    expect(spaced.filters.size() == 1 && spaced.filters.first().value == QLatin1String("alex")
               && spaced.content == QLatin1String("cake"),
           "\"from: alex\", as the box shows a chosen filter, is one filter");

    const Parsed quoted = parse(QStringLiteral("in:\"voice chat\" \"exact words\""));
    expect(quoted.filters.size() == 1 && quoted.filters.first().value == QLatin1String("voice chat"),
           "a quoted channel name keeps its space");
    expect(quoted.content == QLatin1String("\"exact words\""), "quoted words stay quoted for Discord");

    const Parsed pasted = parse(QStringLiteral("mentions:<@800000000000000001> in:<#900000000000000010> from:@sam"));
    expect(pasted.filters.value(0).id == Alex, "a pasted mention is already an id");
    expect(pasted.filters.value(1).id == General, "a pasted channel is already an id");
    expect(pasted.filters.value(2).value == QLatin1String("sam") && pasted.filters.value(2).id.isEmpty(),
           "@ is not part of the name");

    const Parsed upper = parse(QStringLiteral("FROM:Alex On:2024-01-02 notafilter:x"));
    expect(upper.filters.size() == 2 && upper.filters.value(1).key == QLatin1String("during"),
           "keys are any case, and on: is during:");
    expect(upper.content == QLatin1String("notafilter:x"), "an unknown key is just a word");

    // The query, with ids in.
    Request request;
    request.query = parsed;
    request.query.filters[0].id = Alex;
    request.query.filters[4].id = General;
    QString problem;
    QList<QPair<QString, QString>> query = toQuery(request, &problem);
    expect(problem.isEmpty() && !query.isEmpty(), "a complete line makes a query");
    expect(hasParam(query, QStringLiteral("content"), QStringLiteral("chocolate cake")), "content");
    expect(hasParam(query, QStringLiteral("author_id"), Alex), "from: is author_id");
    expect(hasParam(query, QStringLiteral("channel_id"), General), "in: is channel_id");
    expect(hasParam(query, QStringLiteral("has"), QStringLiteral("image")), "has:image");
    expect(hasParam(query, QStringLiteral("pinned"), QStringLiteral("true")), "pinned:true");
    expect(paramValue(query, QStringLiteral("max_id")) == snowflakeAt(QDateTime(QDate(2024, 5, 1), QTime(0, 0))),
           "before: is max_id at the start of that day");
    expect(hasParam(query, QStringLiteral("sort_by"), QStringLiteral("timestamp"))
               && hasParam(query, QStringLiteral("sort_order"), QStringLiteral("desc")),
           "newest first by default");
    expect(paramValue(query, QStringLiteral("offset")).isEmpty(), "no offset on the first page");
    expect(paramValue(query, QStringLiteral("include_nsfw")).isEmpty(), "include_nsfw only when allowed");

    request.offset = 25;
    request.sort = Sort::Relevant;
    request.includeNsfw = true;
    query = toQuery(request);
    expect(hasParam(query, QStringLiteral("offset"), QStringLiteral("25")), "the second page is offset 25");
    expect(hasParam(query, QStringLiteral("sort_by"), QStringLiteral("relevance")), "most relevant");
    expect(hasParam(query, QStringLiteral("include_nsfw"), QStringLiteral("true")), "include_nsfw");
    request.offset = 50000;
    expect(hasParam(toQuery(request), QStringLiteral("offset"), QStringLiteral("9975")), "offset stops at 9975");

    Request dates;
    dates.query = parse(QStringLiteral("after:2024-05-01 before:2024-06-01"));
    query = toQuery(dates);
    expect(paramValue(query, QStringLiteral("min_id")) == snowflakeAt(QDateTime(QDate(2024, 5, 2), QTime(0, 0))),
           "after: is min_id at the end of that day");
    dates.query = parse(QStringLiteral("during:2024-05-01"));
    query = toQuery(dates);
    expect(paramValue(query, QStringLiteral("min_id")) == snowflakeAt(QDateTime(QDate(2024, 5, 1), QTime(0, 0)))
               && paramValue(query, QStringLiteral("max_id"))
                      == snowflakeAt(QDateTime(QDate(2024, 5, 2), QTime(0, 0))),
           "during: is that day, both ends");
    dates.query = parse(QStringLiteral("has:videos has:forward"));
    query = toQuery(dates);
    expect(hasParam(query, QStringLiteral("has"), QStringLiteral("video"))
               && hasParam(query, QStringLiteral("has"), QStringLiteral("snapshot")),
           "plurals, and forward is sent as snapshot");

    const QDate today(2026, 10, 22);
    expect(readDate(QStringLiteral("yesterday"), today) == QDate(2026, 10, 21), "yesterday");
    expect(readDate(QStringLiteral("5/1/2024"), today) == QDate(2024, 5, 1), "5/1/2024");
    expect(readDate(QStringLiteral("5/1/24"), today) == QDate(2024, 5, 1), "two-digit years are this century");
    expect(!readDate(QStringLiteral("soon"), today).isValid(), "not a date");

    Request bad;
    bad.query = parse(QStringLiteral("cake before:soon"));
    expect(toQuery(bad, &problem).isEmpty() && problem.contains(QLatin1String("soon")), "a bad date is explained");
    bad.query = parse(QStringLiteral("has:pdf"));
    expect(toQuery(bad, &problem).isEmpty() && problem.startsWith(QLatin1String("has:")), "an unknown has: kind");
    bad.query = parse(QStringLiteral("from:nobody"));
    expect(toQuery(bad, &problem).isEmpty() && problem.contains(QLatin1String("nobody")),
           "a person with no id is explained");
    bad.query = parse(QStringLiteral("   "));
    expect(toQuery(bad, &problem).isEmpty() && !problem.isEmpty(), "an empty line asks for words");

    // Discord's own example id: 175928847299117063 was made at 1462015105796.
    const QDateTime made = QDateTime::fromMSecsSinceEpoch(1462015105796LL);
    expect(snowflakeAt(made) == QString::number((175928847299117063ULL >> 22) << 22),
           "a moment as a message id matches Discord's");

    // Both shapes of page.
    const QJsonObject before = message(QStringLiteral("1"), General, Alex, QStringLiteral("alex"),
                                       QStringLiteral("before"), QStringLiteral("2024-05-01T10:00:00+00:00"), false);
    const QJsonObject found = message(QStringLiteral("2"), General, Alex, QStringLiteral("alex"),
                                      QStringLiteral("cake"), QStringLiteral("2024-05-01T10:01:00+00:00"));
    // Built by appending: QJsonArray{QJsonArray{...}} is a copy, not a nest.
    QJsonArray groups;
    groups.append(QJsonArray{before, found, before});
    const QJsonObject oldShape{
        {QStringLiteral("total_results"), 1},
        {QStringLiteral("messages"), groups},
    };
    Page read = readPage(oldShape);
    expect(read.total == 1 && read.messages.size() == 1
               && read.messages.first().value(QStringLiteral("id")).toString() == QLatin1String("2"),
           "the hit is taken from among the messages around it");
    read = readPage(page(60, {found}, QJsonArray{QJsonObject{{QStringLiteral("id"), Thread},
                                                              {QStringLiteral("name"), QStringLiteral("cake talk")},
                                                              {QStringLiteral("type"), 11}}}));
    expect(read.total == 60 && read.messages.size() == 1, "a page in today's shape");
    expect(read.threads.value(Thread).value(QStringLiteral("name")).toString() == QLatin1String("cake talk"),
           "threads by id");

    // Marking the hits.
    const QStringList terms = highlightTerms(QStringLiteral("\"Cake,\" bake"));
    expect(terms == QStringList({QStringLiteral("cake"), QStringLiteral("bake")}), "terms without quotes or commas");
    const QString html = highlightHtml(QStringLiteral("Cakes & cake-pops, not pancakes\nbake"), terms);
    expect(html.contains(QLatin1String("<span class=\"hit\">Cakes</span>")), "a hit takes the rest of its word");
    expect(html.contains(QLatin1String("<span class=\"hit\">cake</span>-pops")), "a hyphen ends the word");
    expect(!html.contains(QLatin1String("pan<span")), "not the middle of a word");
    expect(html.contains(QLatin1String("&amp;")) && html.contains(QLatin1String("<br>")),
           "everything else is escaped, lines kept");
}

// --- the store ---------------------------------------------------------------

void storeTests()
{
    MessageStore store;
    store.applyGuild(QJsonObject{
        {QStringLiteral("id"), Guild},
        {QStringLiteral("name"), QStringLiteral("Cake Club")},
        {QStringLiteral("channels"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), General}, {QStringLiteral("name"), QStringLiteral("general")},
                                {QStringLiteral("type"), 0}}}},
    });
    store.rememberMember(Guild, QJsonObject{
        {QStringLiteral("user"), QJsonObject{{QStringLiteral("id"), Alex}, {QStringLiteral("username"), QStringLiteral("alex")},
                                             {QStringLiteral("global_name"), QStringLiteral("Alexandra")}}},
        {QStringLiteral("nick"), QStringLiteral("Baker Alex")},
        {QStringLiteral("roles"), QJsonArray{}},
    });
    store.rememberMember(Guild, QJsonObject{
        {QStringLiteral("user"), QJsonObject{{QStringLiteral("id"), Sam}, {QStringLiteral("username"), QStringLiteral("alexis_sam")}}},
        {QStringLiteral("nick"), QJsonValue::Null},
        {QStringLiteral("roles"), QJsonArray{}},
    });
    store.rememberUser(QJsonObject{{QStringLiteral("id"), Robin}, {QStringLiteral("username"), QStringLiteral("alex")}});

    const QList<UserInfo> found = store.findUsers(QStringLiteral("alex"), Guild);
    expect(found.size() == 2 && found.first().id == Alex, "an exact name first, then the start of one");
    expect(store.findUsers(QStringLiteral("baker"), Guild).value(0).id == Alex, "a nickname in that server counts");
    expect(store.findUsers(QStringLiteral("sam"), Guild).value(0).id == Sam, "the middle of a name counts too");
    bool outsider = false;
    for (const UserInfo &info : found)
        outsider = outsider || info.id == Robin;
    expect(!outsider, "only that server's members");
    expect(store.findUsers(QStringLiteral("alex"), QString()).size() == 3, "everyone known, outside a server");

    // Around a message, then reading on toward the present.
    const QJsonArray around{
        message(QStringLiteral("103"), General, Alex, QStringLiteral("alex"), QStringLiteral("c"), QStringLiteral("2024-05-01T10:03:00+00:00"), false),
        message(QStringLiteral("102"), General, Alex, QStringLiteral("alex"), QStringLiteral("b"), QStringLiteral("2024-05-01T10:02:00+00:00"), false),
        message(QStringLiteral("101"), General, Alex, QStringLiteral("alex"), QStringLiteral("a"), QStringLiteral("2024-05-01T10:01:00+00:00"), false),
    };
    store.setHistory(General, around);
    const QJsonArray after{
        message(QStringLiteral("105"), General, Alex, QStringLiteral("alex"), QStringLiteral("e"), QStringLiteral("2024-05-01T10:05:00+00:00"), false),
        message(QStringLiteral("104"), General, Alex, QStringLiteral("alex"), QStringLiteral("d"), QStringLiteral("2024-05-01T10:04:00+00:00"), false),
        message(QStringLiteral("103"), General, Alex, QStringLiteral("alex"), QStringLiteral("c"), QStringLiteral("2024-05-01T10:03:00+00:00"), false),
    };
    int signalled = 0;
    QObject::connect(&store, &MessageStore::channelHistoryChanged, [&signalled](const QString &) { ++signalled; });
    const int added = store.appendHistory(General, after);
    QStringList ids;
    for (const MessageInfo &info : store.messages(General))
        ids.append(info.id);
    expect(added == 2, "newer messages are added once each");
    expect(ids == QStringList({QStringLiteral("101"), QStringLiteral("102"), QStringLiteral("103"),
                               QStringLiteral("104"), QStringLiteral("105")}),
           "oldest first, whatever order Discord listed them in");
    expect(signalled == 1, "the change is told once");
    expect(store.appendHistory(General, after) == 0, "nothing new is nothing added");

    store.forgetHistory(General);
    expect(!store.hasHistory(General) && store.messages(General).isEmpty(), "a channel's past can be let go of");
}

// --- the calls ---------------------------------------------------------------

void restTests()
{
    StandInRest rest;
    const QJsonObject hit = message(QStringLiteral("1100000000000000001"), General, Alex, QStringLiteral("alex"),
                                    QStringLiteral("cake"), QStringLiteral("2024-05-01T18:22:10.123000+00:00"));

    // Where it goes, and how the words are written.
    rest.reply(QStringLiteral("/guilds/"), 200, page(1, {hit}));
    bool answered = false;
    int total = 0;
    rest.searchMessages(Guild, QString(),
                        {{QStringLiteral("content"), QStringLiteral("chocolate cake & tea")},
                         {QStringLiteral("author_id"), Alex}},
                        [&](const QJsonObject &body) {
                            answered = true;
                            total = body.value(QStringLiteral("total_results")).toInt();
                        },
                        [](const RestClient::Error &) {});
    waitFor([&]() { return answered; });
    expect(answered && total == 1, "a server search answers with its page");
    expect(rest.asked.value(0)
               == QStringLiteral("/guilds/%1/messages/search?content=chocolate%20cake%20%26%20tea&author_id=%2")
                      .arg(Guild, Alex),
           "GET /guilds/{id}/messages/search, the words encoded");

    rest.asked.clear();
    answered = false;
    rest.reply(QStringLiteral("/channels/"), 200, page(0, {}));
    rest.searchMessages(QString(), Direct, {{QStringLiteral("content"), QStringLiteral("hi")}},
                        [&](const QJsonObject &) { answered = true; }, [](const RestClient::Error &) {});
    waitFor([&]() { return answered; });
    expect(rest.asked.value(0) == QStringLiteral("/channels/%1/messages/search?content=hi").arg(Direct),
           "a chat is GET /channels/{id}/messages/search");

    // Every chat at once, as Discord's home page searches: a POST with the
    // filters in the "messages" tab, answered with that tab.
    {
        const QList<QPair<QString, QString>> query{
            {QStringLiteral("content"), QStringLiteral("cake")},
            {QStringLiteral("author_id"), Alex},
            {QStringLiteral("author_id"), Sam},
            {QStringLiteral("has"), QStringLiteral("image")},
            {QStringLiteral("pinned"), QStringLiteral("true")},
            {QStringLiteral("sort_by"), QStringLiteral("timestamp")},
            {QStringLiteral("sort_order"), QStringLiteral("desc")},
            {QStringLiteral("offset"), QStringLiteral("25")},
        };
        const QJsonObject body = SearchQuery::toTabBody(query);
        const QJsonObject tab = body.value(QStringLiteral("tabs")).toObject().value(QStringLiteral("messages")).toObject();
        expect(body.value(QStringLiteral("track_exact_total_hits")).toBool(), "every chat: exact totals asked for");
        expect(tab.value(QStringLiteral("content")).toString() == QLatin1String("cake"), "every chat: the words in the tab");
        expect(tab.value(QStringLiteral("author_id")).toArray() == QJsonArray{Alex, Sam}, "every chat: from: as an array");
        expect(tab.value(QStringLiteral("has")).toArray() == QJsonArray{QStringLiteral("image")}, "every chat: has: as an array");
        expect(tab.value(QStringLiteral("pinned")).isBool() && tab.value(QStringLiteral("pinned")).toBool(),
               "every chat: pinned: as a boolean");
        expect(tab.value(QStringLiteral("offset")).toInt() == 25 && tab.value(QStringLiteral("offset")).isDouble(),
               "every chat: offset as a number");
        expect(tab.value(QStringLiteral("limit")).toInt() == SearchQuery::PageSize, "every chat: a page of 25");
        expect(tab.value(QStringLiteral("sort_order")).toString() == QLatin1String("desc"), "every chat: the sort kept");
        expect(!body.contains(QStringLiteral("include_nsfw")), "every chat: no include_nsfw unless asked");

        rest.asked.clear();
        rest.posted.clear();
        bool got = false;
        int count = 0;
        // Discord's answer: the flat search's page, inside tabs.messages.
        rest.reply(QStringLiteral("/users/@me/messages/search/tabs"), 202, notIndexed());
        rest.reply(QStringLiteral("/users/@me/messages/search/tabs"), 200,
                   QJsonObject{{QStringLiteral("analytics_id"), QStringLiteral("x")},
                               {QStringLiteral("tabs"), QJsonObject{{QStringLiteral("messages"), page(1, {hit})}}}});
        rest.searchAllDirect(body,
                             [&](const QJsonObject &answer) {
                                 got = true;
                                 count = SearchQuery::readPage(answer).messages.size();
                             },
                             [](const RestClient::Error &) {});
        waitFor([&]() { return got; }, 6000);
        expect(rest.posted.value(0).startsWith(QLatin1String("POST /users/@me/messages/search/tabs {\"tabs\":{\"messages\":")),
               "every chat is POST /users/@me/messages/search/tabs");
        expect(rest.posted.size() == 2, "every chat: the 202 is waited out and asked again, with the same body");
        expect(rest.posted.value(0) == rest.posted.value(1), "every chat: the second ask is the same as the first");
        expect(got && count == 1, "every chat: the messages tab is read as a page");
        expect(SearchQuery::messagesTab(notIndexed()).isEmpty(), "every chat: a 202 has no tab");
    }

    // A video's placeholder while its still is made: ThumbHash. Built by
    // hand, bit by bit: landscape, 7 by 4 lightness factors, no alpha, every
    // scale zero, lightness full (white) or none (black), colour centred.
    {
        const QImage white = MediaCache::fromThumbHash(QStringLiteral("PwgCBIAAAAAAAAAAAAAAAAAAAA=="));
        expect(white.size() == QSize(32, 18), "placeholder: a 7:4 landscape hash is 32 by 18");
        const QColor middle = white.isNull() ? QColor() : white.pixelColor(16, 9);
        expect(middle.red() > 240 && middle.green() > 240 && middle.blue() > 240, "placeholder: full lightness is white");
        const QImage black = MediaCache::fromThumbHash(QStringLiteral("AAgCBIAAAAAAAAAAAAAAAAAAAA=="));
        const QColor dark = black.isNull() ? QColor(255, 255, 255) : black.pixelColor(16, 9);
        expect(dark.red() < 16 && dark.green() < 16 && dark.blue() < 16, "placeholder: no lightness is black");
        expect(MediaCache::fromThumbHash(QStringLiteral("AAA=")).isNull(), "placeholder: too short to be one is nothing");
        expect(MediaCache::fromThumbHash(QString()).isNull(), "placeholder: none is nothing");
    }

    // 202: not indexed yet, then the page.
    rest.asked.clear();
    answered = false;
    rest.reply(QStringLiteral("/guilds/"), 202, notIndexed());
    rest.reply(QStringLiteral("/guilds/"), 200, page(1, {hit}));
    QElapsedTimer clock;
    clock.start();
    rest.searchMessages(Guild, QString(), {{QStringLiteral("content"), QStringLiteral("cake")}},
                        [&](const QJsonObject &body) { answered = body.contains(QStringLiteral("messages")); },
                        [](const RestClient::Error &) {});
    waitFor([&]() { return answered; });
    expect(answered && rest.asked.size() == 2, "a 202 is asked again, and the page comes");
    expect(clock.elapsed() >= 450, "after waiting, not at once");

    // 429: waited out.
    rest.asked.clear();
    answered = false;
    rest.reply(QStringLiteral("/guilds/"), 429, tooFast());
    rest.reply(QStringLiteral("/guilds/"), 200, page(1, {hit}));
    rest.searchMessages(Guild, QString(), {{QStringLiteral("content"), QStringLiteral("cake")}},
                        [&](const QJsonObject &) { answered = true; }, [](const RestClient::Error &) {});
    waitFor([&]() { return answered; });
    expect(answered && rest.asked.size() == 2, "a 429 is waited out and asked again");

    // 429 after 429: given up on after three waits.
    rest.asked.clear();
    int failedWith = 0;
    for (int i = 0; i < 4; ++i)
        rest.reply(QStringLiteral("/guilds/"), 429, tooFast());
    rest.searchMessages(Guild, QString(), {{QStringLiteral("content"), QStringLiteral("cake")}},
                        [](const QJsonObject &) {},
                        [&](const RestClient::Error &error) { failedWith = error.httpStatus; });
    waitFor([&]() { return failedWith != 0; }, 6000);
    expect(failedWith == 429 && rest.asked.size() == 4, "a fourth 429 is the caller's to explain");

    // A newer search takes over: the older one is not asked again.
    rest.asked.clear();
    bool olderAnswered = false;
    bool newerAnswered = false;
    rest.reply(QStringLiteral("/guilds/"), 202, notIndexed());
    rest.reply(QStringLiteral("/guilds/"), 200, page(1, {hit}));
    rest.searchMessages(Guild, QString(), {{QStringLiteral("content"), QStringLiteral("old")}},
                        [&](const QJsonObject &) { olderAnswered = true; }, [&](const RestClient::Error &) {
                            olderAnswered = true;
                        });
    rest.searchMessages(Guild, QString(), {{QStringLiteral("content"), QStringLiteral("new")}},
                        [&](const QJsonObject &) { newerAnswered = true; }, [](const RestClient::Error &) {});
    waitFor([&]() { return newerAnswered; });
    waitFor([]() { return false; }, 900);
    expect(newerAnswered && !olderAnswered && rest.asked.size() == 2, "only the newest search is asked again");

    // Still not indexed after a long while: said so.
    rest.asked.clear();
    QString why;
    for (int i = 0; i < 13; ++i)
        rest.reply(QStringLiteral("/guilds/"), 202, notIndexed());
    rest.searchMessages(Guild, QString(), {{QStringLiteral("content"), QStringLiteral("cake")}},
                        [](const QJsonObject &) {},
                        [&](const RestClient::Error &error) { why = error.message; });
    waitFor([&]() { return !why.isEmpty(); }, 12000);
    expect(why.contains(QLatin1String("still")) && rest.asked.size() == 13, "a place never indexed is given up on");

    // The history a jump asks for.
    rest.asked.clear();
    bool around = false;
    rest.reply(QStringLiteral("/channels/"), QJsonArray{hit});
    rest.fetchMessagesAround(General, QStringLiteral("1100000000000000001"), 50,
                             [&](const QJsonArray &list) { around = list.size() == 1; },
                             [](const RestClient::Error &) {});
    rest.fetchMessagesAfter(General, QStringLiteral("1100000000000000001"), 50, [](const QJsonArray &) {},
                            [](const RestClient::Error &) {});
    waitFor([&]() { return around && rest.asked.size() == 2; });
    expect(rest.asked.value(0)
               == QStringLiteral("/channels/%1/messages?around=1100000000000000001&limit=50").arg(General),
           "around a message");
    expect(rest.asked.value(1)
               == QStringLiteral("/channels/%1/messages?after=1100000000000000001&limit=50").arg(General),
           "after a message");
}

// --- the box and the pane ----------------------------------------------------

QPushButton *buttonNamed(QWidget *in, const QString &text)
{
    for (QPushButton *button : in->findChildren<QPushButton *>()) {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

QLabel *pageLabel(QWidget *in)
{
    for (QLabel *label : in->findChildren<QLabel *>()) {
        if (label->text().startsWith(QLatin1String("Page ")))
            return label;
    }
    return nullptr;
}

void pressKey(QWidget *to, int key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(to, &press);
}

void panelTests(const QString &shotPath)
{
    StandInRest rest;
    MessageStore store;
    store.applyGuild(QJsonObject{
        {QStringLiteral("id"), Guild},
        {QStringLiteral("name"), QStringLiteral("Cake Club")},
        {QStringLiteral("channels"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), General}, {QStringLiteral("name"), QStringLiteral("general")},
                                {QStringLiteral("type"), 0}},
                    QJsonObject{{QStringLiteral("id"), Baking}, {QStringLiteral("name"), QStringLiteral("baking")},
                                {QStringLiteral("type"), 0}}}},
    });
    store.rememberMember(Guild, QJsonObject{
        {QStringLiteral("user"), QJsonObject{{QStringLiteral("id"), Alex}, {QStringLiteral("username"), QStringLiteral("alex")},
                                             {QStringLiteral("global_name"), QStringLiteral("Alexandra")}}},
        {QStringLiteral("nick"), QStringLiteral("Baker Alex")},
        {QStringLiteral("roles"), QJsonArray{}},
    });
    store.ingestChannelObject(QJsonObject{
        {QStringLiteral("id"), Direct},
        {QStringLiteral("type"), 1},
        {QStringLiteral("recipients"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), Robin}, {QStringLiteral("username"), QStringLiteral("robin")}}}},
    });

    QWidget window;
    window.resize(980, 760);
    auto *layout = new QVBoxLayout(&window);
    auto *box = new SearchBox(&store, &rest, &window);
    auto *panel = new SearchPanel(&rest, &store, &window);
    layout->addWidget(box);
    layout->addWidget(panel, 1);
    // Before showing: the window hands the box focus as soon as it is active.
    box->setScope(Guild, QString());
    window.show();
    window.activateWindow();
    waitFor([&window]() { return window.isActiveWindow(); }, 500);

    QString jumpedChannel;
    QString jumpedMessage;
    QObject::connect(panel, &SearchPanel::jumpRequested, [&](const QString &channelId, const QString &messageId) {
        jumpedChannel = channelId;
        jumpedMessage = messageId;
    });
    QString searched;
    QObject::connect(box, &SearchBox::searchRequested, [&](const QString &text) { searched = text; });

    // The box: filters offered, a person and a kind picked, Enter searches.
    expect(box->placeholderText() == QLatin1String("Search Cake Club"), "the box names the server");
    box->setFocus();
    waitFor([box]() { return box->hasFocus(); }, 500);
    expect(box->hasFocus(), "the box takes focus");
    auto *popup = window.findChild<QListWidget *>(QStringLiteral("SearchPopup"));
    expect(popup && popup->isVisible() && popup->count() == 9, "focus shows Discord's eight filters under a heading");

    box->insert(QStringLiteral("from:ba"));
    popup = window.findChild<QListWidget *>(QStringLiteral("SearchPopup"));
    expect(popup && popup->isVisible() && popup->count() == 2
               && popup->item(1)->text().startsWith(QLatin1String("Baker Alex")),
           "from: offers the member by nickname");
    if (!shotPath.isEmpty()) {
        QImage shot(window.size(), QImage::Format_ARGB32);
        shot.fill(QColor(QLatin1String("#101014")));
        window.render(&shot);
        shot.save(QString(shotPath).replace(QLatin1String(".png"), QLatin1String("-box.png")));
    }
    pressKey(box, Qt::Key_Return);
    expect(box->text() == QLatin1String("from:alex "), "Enter writes the pick in");
    expect(box->picked().value(QStringLiteral("from:alex")) == Alex, "and remembers who it was");
    box->insert(QStringLiteral("has:im"));
    pressKey(box, Qt::Key_Return);
    expect(box->text() == QLatin1String("from:alex has:image "), "has: offers its kinds");
    box->insert(QStringLiteral("cake"));
    pressKey(box, Qt::Key_Return);
    expect(searched == QLatin1String("from:alex has:image cake"), "Enter on words searches");

    // A click anywhere else closes the list, as Discord's does - even on
    // something that takes no focus, which used to leave it open for good.
    {
        const auto pressAt = [](QWidget *on, const QPoint &local) {
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(on->mapToGlobal(local)), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(on, &press);
        };
        const QString kept = box->text();
        box->clear();   // emptied with focus: the filters come up again
        popup = window.findChild<QListWidget *>(QStringLiteral("SearchPopup"));
        expect(popup && popup->isVisible(), "click away: the list is open to begin with");
        if (popup && popup->isVisible()) {
            pressAt(popup->viewport(), QPoint(20, 40));
            expect(popup->isVisible(), "click away: a click in the list keeps it open");
            pressAt(box, QPoint(10, box->height() / 2));
            expect(popup->isVisible(), "click away: a click in the box keeps it open");
            panel->setFocusPolicy(Qt::NoFocus);
            pressAt(panel, QPoint(panel->width() / 2, panel->height() / 2));
            expect(!popup->isVisible(), "click away: a click on something that takes no focus closes it");
            expect(!box->hasFocus(), "click away: and the box lets go, as Discord's does");
        }
        box->setText(kept);
    }

    // The pane: a search, a page, the hits marked.
    const QList<QJsonObject> hits{
        message(QStringLiteral("1100000000000000003"), General, Alex, QStringLiteral("alex"),
                QStringLiteral("The cake is in the oven"), QStringLiteral("2024-05-01T18:22:10.123000+00:00")),
        message(QStringLiteral("1100000000000000002"), General, Alex, QStringLiteral("alex"),
                QStringLiteral("Cakes for everyone on <t:1714600000:d>, ask <@800000000000000001>"),
                QStringLiteral("2024-05-01T18:20:00+00:00")),
        message(QStringLiteral("1100000000000000001"), Thread, Alex, QStringLiteral("alex"),
                QStringLiteral("pancake? no, cake"), QStringLiteral("2024-04-30T09:00:00+00:00")),
    };
    const QJsonArray threads{QJsonObject{{QStringLiteral("id"), Thread}, {QStringLiteral("name"), QStringLiteral("cake talk")},
                                         {QStringLiteral("type"), 11}, {QStringLiteral("parent_id"), Baking}}};
    rest.asked.clear();
    rest.reply(QStringLiteral("/guilds/"), 202, notIndexed());
    rest.reply(QStringLiteral("/guilds/"), 200, page(60, hits, threads));
    panel->search(Guild, QString(), searched, box->picked());
    auto *list = panel->findChild<QListWidget *>(QStringLiteral("SearchResults"));
    waitFor([list]() { return list->count() == 3; });
    expect(list->count() == 3, "three results listed, after Discord finished indexing");
    expect(rest.asked.size() == 2, "the 202 was asked again");
    const QString path = rest.asked.value(1);
    expect(path.startsWith(QStringLiteral("/guilds/%1/messages/search?").arg(Guild)) && path.contains(QLatin1String("content=cake"))
               && path.contains(QStringLiteral("author_id=") + Alex) && path.contains(QLatin1String("has=image")),
           "the query names the person by id");
    auto *count = panel->findChild<QLabel *>(QStringLiteral("SearchCount"));
    expect(count && count->text() == QLatin1String("60 results"), "how many there are");

    const auto text = [list](int row, int role) { return list->item(row)->data(role).toString(); };
    constexpr int Place = Qt::UserRole + 7;
    constexpr int Html = Qt::UserRole + 8;
    constexpr int Author = Qt::UserRole + 3;
    expect(text(0, Place) == QLatin1String("# general") && text(1, Place).isEmpty()
               && text(2, Place) == QLatin1String("# cake talk"),
           "a channel is named over the first of its run, a thread by Discord's name for it");
    expect(text(0, Author) == QLatin1String("Baker Alex"), "the author by their name in this server");
    expect(text(0, Html).contains(QLatin1String("<span class=\"hit\">cake</span>")), "the hit is marked");
    expect(text(1, Html).contains(QLatin1String("@Baker Alex")) && !text(1, Html).contains(QLatin1String("&lt;@")),
           "a mention reads as a name");
    expect(text(1, Html).contains(QLocale().toString(QDateTime::fromSecsSinceEpoch(1714600000).toLocalTime(),
                                                     QStringLiteral("M/d/yyyy"))),
           "a <t:...:d> timestamp reads as the chat writes it");
    expect(text(2, Html).contains(QLatin1String("pancake?")) && text(2, Html).contains(QLatin1String("<span class=\"hit\">cake</span>")),
           "only whole words are marked");

    QLabel *pages = pageLabel(panel);
    expect(pages && pages->text() == QLatin1String("Page 1 of 3") && pages->isVisible(), "pages of 25");

    if (!shotPath.isEmpty()) {
        QImage shot(window.size(), QImage::Format_ARGB32);
        shot.fill(QColor(QLatin1String("#101014")));
        window.render(&shot);
        shot.save(shotPath);
    }

    // A click asks for the jump.
    emit list->itemClicked(list->item(2));
    expect(jumpedChannel == Thread && jumpedMessage == QLatin1String("1100000000000000001"),
           "a click asks to open that message in its channel");

    // The next page, then another order.
    rest.asked.clear();
    rest.reply(QStringLiteral("/guilds/"), 200, page(60, {hits.first()}));
    buttonNamed(panel, QStringLiteral("Next"))->click();
    waitFor([list]() { return list->count() == 1; });
    expect(rest.asked.value(0).contains(QLatin1String("offset=25")), "Next asks for offset 25");
    expect(pageLabel(panel) && pageLabel(panel)->text() == QLatin1String("Page 2 of 3"), "on page 2");

    rest.asked.clear();
    rest.reply(QStringLiteral("/guilds/"), 200, page(60, hits));
    buttonNamed(panel, QStringLiteral("Oldest"))->click();
    waitFor([list]() { return list->count() == 3; });
    expect(rest.asked.value(0).contains(QLatin1String("sort_order=asc")) && !rest.asked.value(0).contains(QLatin1String("offset=")),
           "another order starts again from the first page");

    // A name nobody here knows goes to Discord's member search.
    rest.asked.clear();
    rest.reply(QStringLiteral("/guilds/%1/members/search").arg(Guild),
               QJsonArray{QJsonObject{
                   {QStringLiteral("user"), QJsonObject{{QStringLiteral("id"), Sam}, {QStringLiteral("username"), QStringLiteral("sam")}}},
                   {QStringLiteral("nick"), QJsonValue::Null},
                   {QStringLiteral("roles"), QJsonArray{}},
               }});
    rest.reply(QStringLiteral("/guilds/%1/messages/search").arg(Guild), 200, page(1, {hits.first()}));
    panel->search(Guild, QString(), QStringLiteral("from:sam"));
    waitFor([&rest]() { return rest.asked.size() == 2; });
    waitFor([list]() { return list->count() == 1; });
    expect(rest.asked.value(0) == QStringLiteral("/guilds/%1/members/search?query=sam&limit=10").arg(Guild),
           "a stranger's name is looked up first");
    expect(rest.asked.value(1).contains(QStringLiteral("author_id=") + Sam), "and searched for by id");

    rest.asked.clear();
    rest.reply(QStringLiteral("/guilds/%1/members/search").arg(Guild), QJsonArray{});
    panel->search(Guild, QString(), QStringLiteral("cake from:nobody"));
    auto *status = panel->findChild<QLabel *>(QStringLiteral("SearchStatus"));
    waitFor([status]() { return status->text().startsWith(QLatin1String("Nobody")); });
    expect(status->isVisible() && status->text() == QLatin1String("Nobody here goes by \"nobody\"."),
           "nobody by that name is said, and nothing is searched");
    expect(rest.asked.size() == 1, "only the member lookup went out");

    // A chat: its own endpoint, its own people.
    rest.asked.clear();
    rest.reply(QStringLiteral("/channels/%1/messages/search").arg(Direct), 200, page(0, {}));
    panel->search(QString(), Direct, QStringLiteral("hello from:robin"));
    waitFor([status]() { return status->text().startsWith(QLatin1String("Nothing matched")); });
    expect(rest.asked.value(0).startsWith(QStringLiteral("/channels/%1/messages/search?content=hello&author_id=%2")
                                              .arg(Direct, Robin)),
           "a chat's search names its own people");
    expect(status->isVisible(), "no results is said plainly");

    // Refused.
    rest.reply(QStringLiteral("/guilds/"), 403,
               QJsonObject{{QStringLiteral("message"), QStringLiteral("Missing Access")}, {QStringLiteral("code"), 50001}});
    panel->search(Guild, QString(), QStringLiteral("cake"));
    waitFor([status]() { return status->text().contains(QLatin1String("can't")); });
    expect(status->text() == QLatin1String("You can't search here."), "a 403 is explained");

    // Cleared: an answer still on its way is dropped.
    rest.reply(QStringLiteral("/guilds/"), 200, page(60, hits));
    panel->search(Guild, QString(), QStringLiteral("cake"));
    panel->clear();
    waitFor([]() { return false; }, 100);
    expect(list->count() == 0, "an answer for a search that was ended is dropped");
}

} // namespace

int main(int argc, char **argv)
{
    // No window on anyone's screen, and the same everywhere.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SingularityTests"));
    QCoreApplication::setApplicationName(QStringLiteral("test_search"));

    QTemporaryDir scratch;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.path());
    Logger::useSideFile();

    // Nothing leaves this machine: every request goes to a port nothing
    // listens on.
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 9));

    // `test_search <png>` also saves what the pane looked like.
    const QString shot = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();

    // Dressed as the program is, so the picture is what a person would see.
    Theme::applyPalette();
    app.setStyleSheet(Theme::applicationStyleSheet());

    queryTests();
    storeTests();
    restTests();
    panelTests(shot);

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
