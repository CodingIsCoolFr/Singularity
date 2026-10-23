#include "ui/PollCard.h"

#include "core/AppConfig.h"
#include "ui/Theme.h"

#include <QLocale>
#include <QUrl>
#include <QUrlQuery>

namespace {

// About as wide as Discord's own poll card.
constexpr int CardWidth = 440;

// Emoji follow the chat text size, a little larger than the letters, as a
// poll's answers show them.
int emojiPixels()
{
    const int textSize =
        qBound(12, AppConfig::instance().value(QStringLiteral("appearance/fontSize"), 15).toInt(), 24);
    return qRound(textSize * 18 / 14.0);
}

// A custom emoji is its picture from Discord's own host, still unless moving
// pictures are on, as reactions show it; any other is the character itself.
QString emojiHtml(const QString &id, const QString &name, bool animated)
{
    if (id.isEmpty())
        return name.toHtmlEscaped();
    const bool animate = AppConfig::instance().value(QStringLiteral("appearance/animate"), true).toBool();
    const QString ext = (animated && animate) ? QStringLiteral("gif") : QStringLiteral("png");
    return QStringLiteral("<img src=\"https://cdn.discordapp.com/emojis/%1.%2?size=48#e%3\" width=\"%3\" "
                          "height=\"%3\" style=\"vertical-align: middle;\">")
        .arg(id.toHtmlEscaped(), ext, QString::number(emojiPixels()));
}

// An answer's share of the votes as a thin bar: the filled part, then the
// rest. A cell of no width still takes room, so an empty or a full bar is a
// single cell.
QString barHtml(int percent, const char *fill)
{
    const QString part = QStringLiteral("<td width=\"%1%\" bgcolor=\"%2\" style=\"font-size: 3px;\">&#160;</td>");
    QString cells;
    if (percent > 0)
        cells += part.arg(QString::number(percent), QLatin1String(fill));
    if (percent < 100)
        cells += part.arg(QString::number(100 - percent), QLatin1String(Theme::SurfaceInput));
    return QStringLiteral("<table class=\"poll-bar\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">"
                          "<tr>%1</tr></table>")
        .arg(cells);
}

QString answerHtml(const PollInfo &poll, const PollInfo::Answer &answer, const QString &messageId, bool closed,
                   int total, int top)
{
    const int percent = total > 0 ? qRound(answer.count * 100.0 / total) : 0;
    const bool won = closed && top > 0 && answer.count == top;

    // A circle where one answer is taken, a box where several are, filled
    // where the vote is yours. A closed poll keeps only your own, ticked.
    QString mark;
    if (!closed) {
        if (poll.allowMultiselect)
            mark = answer.mine ? QStringLiteral("&#9745;") : QStringLiteral("&#9744;");
        else
            mark = answer.mine ? QStringLiteral("&#9673;") : QStringLiteral("&#9675;");
    } else if (answer.mine) {
        mark = QStringLiteral("&#10003;");
    }

    QString label;
    if (!mark.isEmpty()) {
        label += QStringLiteral("<span style=\"font-family: 'Segoe UI Symbol'; color: %1;\">%2</span>&#160;&#160;")
                     .arg(QLatin1String(answer.mine ? Theme::Accent : Theme::TextFaint), mark);
    }
    if (answer.hasEmoji())
        label += emojiHtml(answer.emojiId, answer.emojiName, answer.emojiAnimated) + QStringLiteral("&#160;");
    label += answer.text.toHtmlEscaped();

    // The answer's words are the link. Rich text cannot make a whole row
    // one, and a pill of words is what the reactions already are.
    if (!closed && !messageId.isEmpty()) {
        QUrl press;
        press.setScheme(QStringLiteral("singularity-poll"));
        press.setHost(QStringLiteral("vote"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("m"), messageId);
        query.addQueryItem(QStringLiteral("a"), QString::number(answer.id));
        press.setQuery(query);
        label = QStringLiteral("<a class=\"poll-pick\" href=\"%1\">%2</a>")
                    .arg(press.toString(QUrl::FullyEncoded).toHtmlEscaped(), label);
    } else {
        label = QStringLiteral("<span class=\"poll-pick\">%1</span>").arg(label);
    }

    const QString count = QStringLiteral("<span class=\"poll-count\">%1</span>&#160;&#160;<span class=\"%2\">%3%</span>")
                              .arg(PollCard::votes(answer.count),
                                   won ? QStringLiteral("poll-share-won") : QStringLiteral("poll-share"),
                                   QString::number(percent));

    const char *fill = answer.mine ? Theme::Accent : won ? Theme::LightGray : Theme::TextFaint;
    return QStringLiteral("<table class=\"poll-answer\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                          "<td class=\"%1\">"
                          "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                          "<td valign=\"middle\">%2</td>"
                          "<td align=\"right\" valign=\"middle\" nowrap>%3</td></tr></table>"
                          "%4</td></tr></table>")
        .arg(answer.mine ? QStringLiteral("poll-row-mine") : QStringLiteral("poll-row"), label, count,
             barHtml(percent, fill));
}

} // namespace

QString PollCard::votes(int count)
{
    return count == 1 ? QStringLiteral("1 vote") : QStringLiteral("%1 votes").arg(QLocale().toString(count));
}

QString PollCard::timeLeft(const QDateTime &now, const QDateTime &expiry)
{
    const qint64 seconds = now.secsTo(expiry);
    const auto left = [](qint64 count, const char *one, const char *many) {
        return QStringLiteral("%1 %2 left").arg(QString::number(count), QLatin1String(count == 1 ? one : many));
    };
    if (seconds < 60)
        return QStringLiteral("less than a minute left");
    if (seconds < 3600)
        return left(seconds / 60, "minute", "minutes");
    if (seconds < 86400)
        return left(seconds / 3600, "hour", "hours");
    return left(seconds / 86400, "day", "days");
}

QString PollCard::html(const PollInfo &poll, const QString &messageId, const QDateTime &now)
{
    if (!poll.isSet())
        return {};

    // Past its expiry a poll is closed, even before Discord's update saying
    // so arrives, and shows how it ended.
    const bool closed = poll.isClosed(now);
    const int total = poll.totalVotes();
    int top = 0;
    for (const PollInfo::Answer &answer : poll.answers)
        top = qMax(top, answer.count);

    QString inner = QStringLiteral("<div class=\"poll-question\">%1</div>").arg(poll.question.toHtmlEscaped());
    if (!closed) {
        inner += QStringLiteral("<div class=\"poll-hint\">%1</div>")
                     .arg(poll.allowMultiselect ? QStringLiteral("Select one or more answers")
                                                : QStringLiteral("Select one answer"));
    }
    for (const PollInfo::Answer &answer : poll.answers)
        inner += answerHtml(poll, answer, messageId, closed, total, top);

    QString foot = votes(total);
    if (closed)
        foot += QStringLiteral(" &#183; Poll closed");
    else if (poll.expiry.isValid())
        foot += QStringLiteral(" &#183; ") + timeLeft(now, poll.expiry);
    inner += QStringLiteral("<div class=\"poll-foot\">%1</div>").arg(foot);

    return QStringLiteral("<table class=\"poll\" width=\"%1\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                          "<td class=\"poll-inner\">%2</td></tr></table>")
        .arg(QString::number(CardWidth), inner);
}

QString PollCard::resultHtml(const PollResultInfo &result, const QString &authorName, const QString &messageId,
                             const QDateTime &when)
{
    // The id hidden at the start of every message row: right-click finds the
    // message under the pointer by it.
    const QString idTag = QStringLiteral("<a name=\"msg-%1\">&#8203;</a>").arg(messageId);
    const QString name = authorName.isEmpty() ? QStringLiteral("Someone") : authorName.toHtmlEscaped();
    const QString words = QStringLiteral("<b>%1</b>&#8217;s poll <b>%2</b> has closed.")
                              .arg(name, result.question.toHtmlEscaped());

    QString outcome;
    if (result.hasWinner) {
        QString winner;
        if (!result.winnerEmojiId.isEmpty() || !result.winnerEmojiName.isEmpty())
            winner = emojiHtml(result.winnerEmojiId, result.winnerEmojiName, result.winnerEmojiAnimated)
                     + QStringLiteral("&#160;");
        winner += result.winnerText.toHtmlEscaped();
        const int percent = result.totalVotes > 0 ? qRound(result.winnerVotes * 100.0 / result.totalVotes) : 0;
        outcome = QStringLiteral("<div class=\"poll-question\">%1</div>"
                                 "<div class=\"poll-hint\">Winning answer &#183; %2% &#183; %3</div>")
                      .arg(winner, QString::number(percent), votes(result.winnerVotes));
    } else if (result.totalVotes > 0) {
        outcome = QStringLiteral("<div class=\"poll-question\">The poll ended in a tie</div>"
                                 "<div class=\"poll-hint\">%1</div>")
                      .arg(votes(result.totalVotes));
    } else {
        outcome = QStringLiteral("<div class=\"poll-question\">There were no votes</div>");
    }

    QString stamp;
    if (when.isValid()) {
        const bool today = when.date() == QDate::currentDate();
        stamp = QLocale().toString(when, today ? QStringLiteral("h:mm AP") : QStringLiteral("M/d/yy, h:mm AP"));
    }

    return QStringLiteral("<table class=\"row\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                          "<td width=\"56\" align=\"center\" valign=\"top\">%1"
                          "<span style=\"font-size: 16px;\">&#128202;</span></td>"
                          "<td valign=\"top\"><span style=\"color: %2;\">%3</span>"
                          "<span style=\"color: %4;\"> %5</span>"
                          "<table class=\"poll\" width=\"%6\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                          "<td class=\"poll-inner\">%7</td></tr></table></td></tr></table>")
        .arg(idTag, QLatin1String(Theme::TextPrimary), words, QLatin1String(Theme::TextMuted), stamp.toHtmlEscaped(),
             QString::number(CardWidth), outcome);
}
