// Polls are checked here against Discord's own shapes, with no Discord
// traffic: a poll message as the API sends it, the vote events the gateway
// sends, the closing notice, and the vote and new-poll requests, which a
// stand-in for the network catches instead of sending. The payloads follow
// the poll schema in Discord's own client (poll.question, answers[].answer_id
// / poll_media, expiry, allow_multiselect, results.answer_counts[].id / count
// / me_voted, results.is_finalized), its PUT .../polls/{message}/answers/@me
// with {"answer_ids": [...]}, and its poll message: no words and a `poll` of
// question, answers, allow_multiselect, duration and layout_type.
//
// `test_polls --render <png>` draws a few polls the way the chat would, to
// look at. Settings and the log go to a temporary folder, never the real ones.

#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/MessageStore.h"
#include "core/RestClient.h"
#include "ui/PollCard.h"
#include "ui/PollDialog.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTimeZone>

#include <cstdio>
#include <memory>

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

static const QString Channel = QStringLiteral("1200000000000000001");
static const QString PollId = QStringLiteral("1300000000000000001");
static const QString Me = QStringLiteral("1100000000000000001");
static const QString Someone = QStringLiteral("1100000000000000002");

static QJsonObject json(const char *text)
{
    return QJsonDocument::fromJson(QByteArray(text)).object();
}

// A one-answer poll as GET /channels/{id}/messages returns it: three answers
// (a character emoji, a moving custom emoji, none), you on the first, and an
// answer whose text would break HTML or QString::arg if not escaped.
static QJsonObject pollMessage()
{
    return json(R"({
        "id": "1300000000000000001",
        "channel_id": "1200000000000000001",
        "type": 0,
        "content": "",
        "author": {"id": "1100000000000000003", "username": "pollster", "global_name": "Poll Ster", "avatar": null},
        "timestamp": "2026-10-22T18:00:00.000000+00:00",
        "edited_timestamp": null,
        "attachments": [], "embeds": [], "mentions": [], "components": [],
        "poll": {
            "question": {"text": "Pizza or tacos?"},
            "answers": [
                {"answer_id": 1, "poll_media": {"text": "Pizza", "emoji": {"id": null, "name": "🍕"}}},
                {"answer_id": 2, "poll_media": {"text": "Tacos",
                                                "emoji": {"id": "1234567890123456789", "name": "taco", "animated": true}}},
                {"answer_id": 3, "poll_media": {"text": "<b>Both</b> & %1"}}
            ],
            "expiry": "2026-10-23T18:00:00.000000+00:00",
            "allow_multiselect": false,
            "layout_type": 1,
            "results": {
                "is_finalized": false,
                "answer_counts": [
                    {"id": 1, "count": 3, "me_voted": true},
                    {"id": 3, "count": 1, "me_voted": false}
                ]
            }
        }
    })");
}

static void parseTests()
{
    const MessageInfo message = MessageStore::parseMessage(pollMessage());
    const PollInfo &poll = message.poll;

    expect(poll.isSet() && poll.answers.size() == 3, "a poll message has its three answers");
    expect(poll.question == QLatin1String("Pizza or tacos?"), "the question is read");
    expect(!poll.allowMultiselect, "one answer only");
    expect(poll.expiry == QDateTime(QDate(2026, 10, 23), QTime(18, 0), QTimeZone::UTC), "the expiry is read");
    expect(poll.answers.at(0).id == 1 && poll.answers.at(1).id == 2 && poll.answers.at(2).id == 3,
           "answer ids are Discord's");
    expect(poll.answers.at(0).emojiId.isEmpty() && poll.answers.at(0).emojiName == QStringLiteral("\U0001F355"),
           "a character emoji is its character");
    expect(poll.answers.at(1).emojiId == QLatin1String("1234567890123456789") && poll.answers.at(1).emojiAnimated,
           "a custom emoji keeps its id and that it moves");
    expect(!poll.answers.at(2).hasEmoji(), "an answer without an emoji has none");
    expect(poll.answers.at(0).count == 3 && poll.answers.at(0).mine, "counts and your vote are read");
    expect(poll.answers.at(1).count == 0 && !poll.answers.at(1).mine, "an answer missing from the counts has none");
    expect(poll.totalVotes() == 4, "the total is every vote");
    expect(poll.myAnswers() == QList<int>{1}, "your answers");
    expect(poll.resultsSent && !poll.finalized, "results were sent, not final");
    expect(!poll.isClosed(QDateTime(QDate(2026, 10, 23), QTime(17, 0), QTimeZone::UTC)), "open before its expiry");
    expect(poll.isClosed(QDateTime(QDate(2026, 10, 23), QTime(18, 1), QTimeZone::UTC)), "closed after its expiry");

    // Without results Discord's client reads every answer as no votes.
    QJsonObject raw = pollMessage();
    QJsonObject rawPoll = raw.value(QStringLiteral("poll")).toObject();
    rawPoll.remove(QStringLiteral("results"));
    raw.insert(QStringLiteral("poll"), rawPoll);
    const PollInfo bare = MessageStore::parseMessage(raw).poll;
    expect(bare.isSet() && bare.totalVotes() == 0 && !bare.resultsSent, "a poll without results has no votes yet");

    // A message without a poll has none.
    QJsonObject plain = pollMessage();
    plain.remove(QStringLiteral("poll"));
    plain.insert(QStringLiteral("content"), QStringLiteral("hi"));
    expect(!MessageStore::parseMessage(plain).poll.isSet(), "an ordinary message has no poll");
}

static void clickTests()
{
    PollInfo single = MessageStore::parseMessage(pollMessage()).poll;
    expect(single.selectionAfterClick(2) == QList<int>{2}, "one answer: a click moves your vote");
    expect(single.selectionAfterClick(1).isEmpty(), "one answer: a second click takes it back");
    expect(single.selectionAfterClick(9) == QList<int>{1}, "a click on an answer that is not there changes nothing");

    PollInfo multi = single;
    multi.allowMultiselect = true;
    expect(multi.selectionAfterClick(3) == (QList<int>{1, 3}), "several answers: a click adds one");
    expect(multi.selectionAfterClick(1).isEmpty(), "several answers: a click takes that one back");
    multi.setMine({1, 2, 3});
    expect(multi.selectionAfterClick(2) == (QList<int>{1, 3}), "several answers: only the clicked one goes");

    PollInfo counted = single;
    expect(counted.setMine({2}), "moving your vote changes the poll");
    expect(counted.answers.at(0).count == 2 && !counted.answers.at(0).mine && counted.answers.at(1).count == 1
               && counted.answers.at(1).mine,
           "moving your vote moves its count");
    expect(!counted.setMine({2}), "setting the same vote again changes nothing");
    expect(counted.setMine({}) && counted.totalVotes() == 3 && counted.myAnswers().isEmpty(),
           "taking your vote back takes its count");
}

static void storeTests()
{
    MessageStore store;
    store.setHistory(Channel, QJsonArray{pollMessage()});

    int changes = 0;
    QObject::connect(&store, &MessageStore::messageChanged, [&changes](const QString &, const QString &) {
        ++changes;
    });
    const auto poll = [&store]() {
        const QList<MessageInfo> messages = store.messages(Channel);
        return messages.isEmpty() ? PollInfo() : messages.first().poll;
    };
    const auto vote = [](const QString &user, int answer) {
        return QJsonObject{{QStringLiteral("user_id"), user},
                           {QStringLiteral("channel_id"), Channel},
                           {QStringLiteral("message_id"), PollId},
                           {QStringLiteral("guild_id"), QStringLiteral("1000000000000000001")},
                           {QStringLiteral("answer_id"), answer}};
    };

    // MESSAGE_POLL_VOTE_ADD from someone else.
    store.applyPollVote(vote(Someone, 2), true, Me);
    expect(poll().answers.at(1).count == 1 && !poll().answers.at(1).mine && changes == 1,
           "someone's vote is counted and redrawn");

    // Your click: counted at once, then Discord's echo of it changes nothing.
    store.setPollVotes(Channel, PollId, {2});
    expect(poll().answers.at(0).count == 2 && poll().answers.at(1).count == 2 && poll().answers.at(1).mine,
           "your click moves your vote at once");
    const int before = changes;
    store.applyPollVote(vote(Me, 1), false, Me);
    store.applyPollVote(vote(Me, 2), true, Me);
    expect(poll().answers.at(0).count == 2 && poll().answers.at(1).count == 2 && changes == before,
           "the echo of your own vote is not counted twice");

    // Your vote from another device is news here.
    store.applyPollVote(vote(Me, 3), true, Me);
    expect(poll().answers.at(2).count == 2 && poll().answers.at(2).mine, "your vote from elsewhere is counted");

    // MESSAGE_POLL_VOTE_REMOVE.
    store.applyPollVote(vote(Someone, 2), false, Me);
    expect(poll().answers.at(1).count == 1, "a vote taken back is taken off");

    // MESSAGE_POLL_VOTE_ADD_MANY: each answer with everyone who chose it.
    store.applyPollVote(json(R"({"channel_id": "1200000000000000001", "message_id": "1300000000000000001",
                                 "votes": [{"answer_id": 1, "users": ["1100000000000000004", "1100000000000000005"]},
                                           {"answer_id": 3, "users": ["1100000000000000006"]}]})"),
                        true, Me);
    expect(poll().answers.at(0).count == 4 && poll().answers.at(2).count == 3, "a batch of votes is counted");

    // Votes with nowhere to go.
    const int quiet = changes;
    QJsonObject elsewhere = vote(Someone, 1);
    elsewhere.insert(QStringLiteral("message_id"), QStringLiteral("1300000000000000099"));
    store.applyPollVote(elsewhere, true, Me);
    store.applyPollVote(vote(Someone, 7), true, Me);
    expect(changes == quiet && poll().totalVotes() == 8, "a vote for another message or answer changes nothing");

    // The poll closes: MESSAGE_UPDATE with the final count.
    QJsonObject closed = pollMessage();
    QJsonObject closedPoll = closed.value(QStringLiteral("poll")).toObject();
    closedPoll.insert(QStringLiteral("results"),
                      json(R"({"is_finalized": true, "answer_counts": [
                               {"id": 1, "count": 5, "me_voted": false},
                               {"id": 2, "count": 2, "me_voted": true}]})"));
    closed.insert(QStringLiteral("poll"), closedPoll);
    store.updateMessage(closed);
    expect(poll().finalized && poll().answers.at(0).count == 5 && poll().answers.at(2).count == 0
               && poll().myAnswers() == QList<int>{2},
           "the final count replaces ours");
    expect(poll().isClosed(QDateTime(QDate(2026, 10, 22), QTime(19, 0), QTimeZone::UTC)),
           "a finalized poll is closed before its expiry");

    // An update that carries the poll without results keeps the count.
    QJsonObject partial = pollMessage();
    QJsonObject partialPoll = partial.value(QStringLiteral("poll")).toObject();
    partialPoll.remove(QStringLiteral("results"));
    partial.insert(QStringLiteral("poll"), partialPoll);
    store.updateMessage(partial);
    expect(poll().finalized && poll().answers.at(0).count == 5, "an update without results keeps the count");
}

static void resultTests()
{
    // The notice Discord posts when a poll ends: type 46, a reference to the
    // poll, and a "poll_result" embed of named fields.
    const MessageInfo notice = MessageStore::parseMessage(json(R"({
        "id": "1300000000000000002",
        "channel_id": "1200000000000000001",
        "type": 46,
        "content": "",
        "author": {"id": "1100000000000000003", "username": "pollster", "global_name": "Poll Ster", "avatar": null},
        "timestamp": "2026-10-23T18:00:01.000000+00:00",
        "message_reference": {"type": 0, "channel_id": "1200000000000000001", "message_id": "1300000000000000001"},
        "embeds": [{
            "type": "poll_result",
            "fields": [
                {"name": "poll_question_text", "value": "Pizza or tacos?", "inline": false},
                {"name": "victor_answer_votes", "value": "5", "inline": false},
                {"name": "total_votes", "value": "8", "inline": false},
                {"name": "victor_answer_id", "value": "1", "inline": false},
                {"name": "victor_answer_text", "value": "Pizza", "inline": false},
                {"name": "victor_answer_emoji_name", "value": "🍕", "inline": false}
            ]
        }]
    })"));
    const PollResultInfo &result = notice.pollResult;
    expect(result.isSet && notice.embeds.isEmpty(), "the closing notice is read, not drawn as a card");
    expect(result.question == QLatin1String("Pizza or tacos?") && result.winnerText == QLatin1String("Pizza")
               && result.winnerEmojiName == QStringLiteral("\U0001F355") && result.hasWinner,
           "the closing notice names the winner");
    expect(result.winnerVotes == 5 && result.totalVotes == 8, "the closing notice has the votes");

    const QString html = PollCard::resultHtml(result, notice.authorName, notice.id, notice.timestamp);
    expect(html.contains(QLatin1String("msg-1300000000000000002")), "the notice row carries its message id");
    expect(html.contains(QLatin1String("Winning answer &#183; 63%")), "the notice says how much the winner had");

    PollResultInfo tie;
    tie.isSet = true;
    tie.question = QStringLiteral("Up or down?");
    tie.totalVotes = 4;
    expect(PollCard::resultHtml(tie, QStringLiteral("Poll Ster"), QStringLiteral("1"), {}).contains(
               QLatin1String("ended in a tie")),
           "a tie is a tie");
    tie.totalVotes = 0;
    expect(PollCard::resultHtml(tie, QStringLiteral("Poll Ster"), QStringLiteral("1"), {}).contains(
               QLatin1String("no votes")),
           "nobody voting is said");
}

// Requests, caught before they leave.
class CatchingRest : public RestClient
{
public:
    int sent = 0;
    QByteArray verb;
    QString path;
    QJsonObject body;
    QString captchaKey;

protected:
    void sendJson(const QByteArray &how, const QString &to, const QByteArray &json, const CaptchaProof &captcha,
                  ObjectHandler onOk, ErrorHandler) override
    {
        ++sent;
        verb = how;
        path = to;
        body = QJsonDocument::fromJson(json).object();
        captchaKey = captcha.key;
        // Discord's answer to a vote: 204, no body.
        if (onOk)
            onOk(QJsonObject{});
    }
};

static void requestTests()
{
    CatchingRest rest;
    bool ok = false;
    rest.votePoll(Channel, PollId, {QStringLiteral("1"), QStringLiteral("3")},
                  [&ok](const QJsonObject &) { ok = true; }, nullptr);
    expect(rest.sent == 1 && rest.verb == "PUT", "a vote is one PUT");
    expect(rest.path == QLatin1String("/channels/1200000000000000001/polls/1300000000000000001/answers/@me"),
           "a vote goes to the poll's answers/@me");
    expect(rest.body == json(R"({"answer_ids": ["1", "3"]})"), "the body is every chosen answer id, as text");
    expect(ok, "Discord's empty answer is success");

    rest.votePoll(Channel, PollId, {}, nullptr, nullptr);
    expect(rest.body == json(R"({"answer_ids": []})"), "no answers takes the vote back");

    // A new poll, from the composer's dialog.
    const QJsonObject poll = PollDialog::pollBody(
        QStringLiteral("  Pizza or tacos?  "),
        {QStringLiteral("Pizza"), QStringLiteral("   "), QStringLiteral(" Tacos "), QString(80, QLatin1Char('x'))},
        24, true);
    QJsonObject shape = json(R"({"question": {"text": "Pizza or tacos?"},
                                 "answers": [{"poll_media": {"text": "Pizza"}}, {"poll_media": {"text": "Tacos"}}],
                                 "allow_multiselect": true, "duration": 24, "layout_type": 1})");
    QJsonArray answers = shape.value(QStringLiteral("answers")).toArray();
    answers.append(QJsonObject{{QStringLiteral("poll_media"),
                                QJsonObject{{QStringLiteral("text"), QString(55, QLatin1Char('x'))}}}});
    shape.insert(QStringLiteral("answers"), answers);
    expect(poll == shape, "a new poll is Discord's shape, blank answers left out and long ones cut to 55");

    QStringList eleven;
    for (int i = 1; i <= 11; ++i)
        eleven.append(QString::number(i));
    expect(PollDialog::pollBody(QStringLiteral("Q"), eleven, 1, false).value(QStringLiteral("answers")).toArray().size()
               == 10,
           "a poll has ten answers at most");

    rest.sendPoll(Channel, poll, nullptr, nullptr);
    expect(rest.verb == "POST" && rest.path == QLatin1String("/channels/1200000000000000001/messages"),
           "a poll is posted as a message");
    expect(rest.body.value(QStringLiteral("poll")).toObject() == poll
               && rest.body.value(QStringLiteral("content")).toString().isEmpty()
               && !rest.body.value(QStringLiteral("nonce")).toString().isEmpty()
               && rest.body.value(QStringLiteral("tts")) == QJsonValue(false),
           "the message has no words, a nonce, and the poll");
    RestClient::CaptchaProof proof;
    proof.key = QStringLiteral("solved");
    rest.sendPoll(Channel, poll, nullptr, nullptr, proof);
    expect(rest.captchaKey == QLatin1String("solved"), "a retry after a check carries its proof");
}

static void cardTests()
{
    const PollInfo poll = MessageStore::parseMessage(pollMessage()).poll;
    const QDateTime now(QDate(2026, 10, 23), QTime(13, 0), QTimeZone::UTC);

    const QString open = PollCard::html(poll, PollId, now);
    expect(open.contains(QLatin1String("Pizza or tacos?")), "the card shows the question");
    expect(open.contains(QLatin1String("Select one answer")), "the card says how many may be chosen");
    expect(open.contains(QLatin1String("singularity-poll://vote?m=1300000000000000001&amp;a=2")),
           "an answer is a link that votes for it");
    expect(open.contains(QLatin1String("&lt;b&gt;Both&lt;/b&gt; &amp; %1")), "answer words are escaped and kept");
    expect(open.contains(QLatin1String("cdn.discordapp.com/emojis/1234567890123456789.")),
           "a custom emoji is its picture");
    expect(open.contains(QLatin1String("75%")) && open.contains(QLatin1String("3 votes"))
               && open.contains(QLatin1String("25%")) && open.contains(QLatin1String("1 vote<")),
           "each answer has its votes and share");
    expect(open.contains(QLatin1String("width=\"75%\"")), "the bar is as long as the share");
    expect(open.contains(QLatin1String("poll-row-mine")) && open.contains(QLatin1String("&#9673;")),
           "your answer is marked");
    expect(open.contains(QLatin1String("4 votes &#183; 5 hours left")), "the foot has the total and the time left");

    PollInfo multi = poll;
    multi.allowMultiselect = true;
    const QString several = PollCard::html(multi, PollId, now);
    expect(several.contains(QLatin1String("Select one or more answers")) && several.contains(QLatin1String("&#9745;")),
           "a multi-answer poll says so and uses boxes");

    const QString done = PollCard::html(poll, PollId, now.addDays(2));
    expect(done.contains(QLatin1String("Poll closed")) && !done.contains(QLatin1String("singularity-poll:"))
               && !done.contains(QLatin1String("Select one")),
           "a closed poll shows its result and takes no votes");
    expect(done.contains(QLatin1String("poll-share-won")), "a closed poll marks its winner");

    const QString forwarded = PollCard::html(poll, QString(), now);
    expect(!forwarded.contains(QLatin1String("singularity-poll:")), "a forwarded poll is only to read");
    expect(PollCard::html(PollInfo(), PollId, now).isEmpty(), "no poll, no card");

    expect(PollCard::timeLeft(now, now.addSecs(30)) == QLatin1String("less than a minute left"), "seconds left");
    expect(PollCard::timeLeft(now, now.addSecs(60)) == QLatin1String("1 minute left"), "a minute left");
    expect(PollCard::timeLeft(now, now.addSecs(7200 + 59)) == QLatin1String("2 hours left"), "hours left");
    expect(PollCard::timeLeft(now, now.addDays(3)) == QLatin1String("3 days left"), "days left");
    expect(PollCard::votes(1) == QLatin1String("1 vote") && PollCard::votes(0) == QLatin1String("0 votes"),
           "vote counts read as words");
}

// The cards on the chat's own stylesheet, drawn to a picture to look at.
static int render(const QString &file)
{
    PollInfo poll = MessageStore::parseMessage(pollMessage()).poll;
    const QDateTime now(QDate(2026, 10, 23), QTime(13, 0), QTimeZone::UTC);
    PollInfo multi = poll;
    multi.allowMultiselect = true;
    multi.setMine({1, 3});

    QString html = PollCard::html(poll, PollId, now) + PollCard::html(multi, PollId, now)
                   + PollCard::html(poll, PollId, now.addDays(2));
    PollResultInfo result;
    result.isSet = true;
    result.question = poll.question;
    result.hasWinner = true;
    result.winnerText = QStringLiteral("Pizza");
    result.winnerEmojiName = QStringLiteral("\U0001F355");
    result.winnerVotes = 5;
    result.totalVotes = 8;
    html += PollCard::resultHtml(result, QStringLiteral("Poll Ster"), QStringLiteral("2"), now.toLocalTime());

    QTextDocument document;
    document.setDefaultStyleSheet(Theme::messageViewCss());
    document.setHtml(QStringLiteral("<body>%1</body>").arg(html));
    document.setTextWidth(620);

    QImage image(QSize(620, int(document.size().height()) + 16), QImage::Format_ARGB32);
    image.fill(QColor(Theme::SurfaceChat));
    QPainter painter(&image);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, QColor(Theme::TextPrimary));
    document.documentLayout()->draw(&painter, context);
    painter.end();
    return image.save(file) ? 0 : 1;
}

int main(int argc, char **argv)
{
    const bool rendering = argc == 3 && QByteArray(argv[1]) == "--render";
    std::unique_ptr<QCoreApplication> app(rendering ? new QGuiApplication(argc, argv)
                                                    : new QCoreApplication(argc, argv));
    QCoreApplication::setOrganizationName(QStringLiteral("SingularityTests"));
    QCoreApplication::setApplicationName(QStringLiteral("test_polls"));

    QTemporaryDir scratch;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.path());
    Logger::useSideFile();

    if (rendering)
        return render(QString::fromLocal8Bit(argv[2]));

    parseTests();
    clickTests();
    storeTests();
    resultTests();
    requestTests();
    cardTests();

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
