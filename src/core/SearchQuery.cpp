#include "core/SearchQuery.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QTime>

#include <algorithm>

namespace SearchQuery {

namespace {

// The words the box knows as filters. "on" is Discord's other spelling of
// during.
QString canonicalKey(const QString &key)
{
    const QString lower = key.toLower();
    if (lower == QLatin1String("on"))
        return QStringLiteral("during");
    if (filterKeys().contains(lower))
        return lower;
    return {};
}

// Splits on spaces, keeping a "quoted run" in one piece with its quotes.
QStringList tokens(const QString &text)
{
    QStringList out;
    QString current;
    bool quoted = false;
    for (const QChar c : text) {
        if (c == QLatin1Char('"'))
            quoted = !quoted;
        if (c.isSpace() && !quoted) {
            if (!current.isEmpty())
                out.append(current);
            current.clear();
            continue;
        }
        current.append(c);
    }
    if (!current.isEmpty())
        out.append(current);
    return out;
}

QString unquote(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
        value = value.mid(1, value.size() - 2);
    else if (value.startsWith(QLatin1Char('"')))
        value = value.mid(1);
    return value.trimmed();
}

// A filter token, split. Empty key when it is not one.
QPair<QString, QString> splitFilter(const QString &token)
{
    static const QRegularExpression pattern(QStringLiteral(R"(^([A-Za-z]+):(.*)$)"),
                                            QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = pattern.match(token);
    if (!match.hasMatch())
        return {};
    const QString key = canonicalKey(match.captured(1));
    if (key.isEmpty())
        return {};
    return {key, match.captured(2)};
}

Filter makeFilter(const QString &key, const QString &raw)
{
    Filter filter;
    filter.key = key;
    QString value = unquote(raw);

    // A mention pasted in, or a plain id, already says who or where.
    static const QRegularExpression userMention(QStringLiteral(R"(^<@!?(\d+)>$)"));
    static const QRegularExpression channelMention(QStringLiteral(R"(^<#(\d+)>$)"));
    static const QRegularExpression bareId(QStringLiteral(R"(^\d{15,21}$)"));
    if (key == QLatin1String("from") || key == QLatin1String("mentions")) {
        const QRegularExpressionMatch match = userMention.match(value);
        if (match.hasMatch())
            filter.id = match.captured(1);
        if (value.startsWith(QLatin1Char('@')))
            value = value.mid(1);
    } else if (key == QLatin1String("in")) {
        const QRegularExpressionMatch match = channelMention.match(value);
        if (match.hasMatch())
            filter.id = match.captured(1);
        if (value.startsWith(QLatin1Char('#')))
            value = value.mid(1);
    }
    if (needsId(key) && filter.id.isEmpty() && bareId.match(value).hasMatch())
        filter.id = value;

    filter.value = value;
    return filter;
}

// What has: sends, from what the box shows. Plurals are taken too, since
// that is how people type them.
QString hasParameter(const QString &value)
{
    QString word = value.toLower().trimmed();
    if (word == QLatin1String("forward") || word == QLatin1String("forwards")
        || word == QLatin1String("forwarded") || word == QLatin1String("snapshot"))
        return QStringLiteral("snapshot");
    if (word.endsWith(QLatin1Char('s')) && word != QLatin1String("snapshots"))
        word.chop(1);
    static const QStringList known{
        QStringLiteral("link"),  QStringLiteral("embed"), QStringLiteral("file"),    QStringLiteral("image"),
        QStringLiteral("video"), QStringLiteral("sound"), QStringLiteral("sticker"), QStringLiteral("poll"),
    };
    return known.contains(word) ? word : QString();
}

QDateTime startOfDay(const QDate &day)
{
    return QDateTime(day, QTime(0, 0));
}

bool isWordChar(const QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

} // namespace

QStringList filterKeys()
{
    return {
        QStringLiteral("from"),   QStringLiteral("mentions"), QStringLiteral("has"),
        QStringLiteral("before"), QStringLiteral("during"),   QStringLiteral("after"),
        QStringLiteral("pinned"), QStringLiteral("in"),
    };
}

QString filterHint(const QString &key)
{
    if (key == QLatin1String("from") || key == QLatin1String("mentions"))
        return QStringLiteral("user");
    if (key == QLatin1String("has"))
        return QStringLiteral("link, embed or file");
    if (key == QLatin1String("before") || key == QLatin1String("during") || key == QLatin1String("after"))
        return QStringLiteral("specific date");
    if (key == QLatin1String("pinned"))
        return QStringLiteral("true or false");
    if (key == QLatin1String("in"))
        return QStringLiteral("channel");
    return {};
}

QStringList hasValues()
{
    return {
        QStringLiteral("link"),  QStringLiteral("embed"),   QStringLiteral("file"), QStringLiteral("image"),
        QStringLiteral("video"), QStringLiteral("sound"),   QStringLiteral("sticker"),
        QStringLiteral("poll"),  QStringLiteral("forward"),
    };
}

bool needsId(const QString &key)
{
    return key == QLatin1String("from") || key == QLatin1String("mentions") || key == QLatin1String("in");
}

Parsed parse(const QString &text)
{
    Parsed parsed;
    QStringList words;
    const QStringList parts = tokens(text);
    for (int i = 0; i < parts.size(); ++i) {
        const QPair<QString, QString> filter = splitFilter(parts.at(i));
        if (filter.first.isEmpty()) {
            words.append(parts.at(i));
            continue;
        }
        QString value = filter.second;
        // "from: alex", the way the box shows a chosen filter, with a space.
        if (value.isEmpty() && i + 1 < parts.size() && splitFilter(parts.at(i + 1)).first.isEmpty())
            value = parts.at(++i);
        if (unquote(value).isEmpty())
            continue;
        parsed.filters.append(makeFilter(filter.first, value));
    }
    parsed.content = words.join(QLatin1Char(' ')).trimmed();
    return parsed;
}

QDate readDate(const QString &text, const QDate &today)
{
    const QString value = text.trimmed().toLower();
    if (value == QLatin1String("today"))
        return today;
    if (value == QLatin1String("yesterday"))
        return today.addDays(-1);

    static const QStringList formats{
        QStringLiteral("yyyy-M-d"), QStringLiteral("yyyy/M/d"), QStringLiteral("M/d/yyyy"),
        QStringLiteral("M-d-yyyy"),
    };
    for (const QString &format : formats) {
        const QDate date = QDate::fromString(value, format);
        if (date.isValid())
            return date;
    }
    // Two-digit years, 5/1/24, are this century.
    const QDate shortYear = QDate::fromString(value, QStringLiteral("M/d/yy"));
    if (shortYear.isValid())
        return shortYear.year() < 2000 ? shortYear.addYears(100) : shortYear;
    return {};
}

QString snowflakeAt(const QDateTime &when)
{
    constexpr qint64 DiscordEpochMs = 1420070400000LL;
    const qint64 ms = when.toMSecsSinceEpoch() - DiscordEpochMs;
    if (ms <= 0)
        return QStringLiteral("0");
    return QString::number(quint64(ms) << 22);
}

QList<QPair<QString, QString>> toQuery(const Request &request, QString *problem)
{
    const auto fail = [problem](const QString &why) {
        if (problem)
            *problem = why;
        return QList<QPair<QString, QString>>{};
    };

    if (request.query.isEmpty())
        return fail(QStringLiteral("Type something to search for."));

    QList<QPair<QString, QString>> query;
    const QString content = request.query.content.trimmed();
    if (!content.isEmpty())
        query.append({QStringLiteral("content"), content.left(1024)});

    // The narrowest window every date asks for.
    QDateTime earliest;   // min: after this
    QDateTime latest;     // max: before this

    for (const Filter &filter : request.query.filters) {
        const QString &key = filter.key;
        if (needsId(key) && filter.id.isEmpty()) {
            if (key == QLatin1String("in"))
                return fail(QStringLiteral("There is no channel called \"%1\" here.").arg(filter.value));
            return fail(QStringLiteral("Nobody here goes by \"%1\".").arg(filter.value));
        }

        if (key == QLatin1String("from")) {
            query.append({QStringLiteral("author_id"), filter.id});
        } else if (key == QLatin1String("mentions")) {
            query.append({QStringLiteral("mentions"), filter.id});
        } else if (key == QLatin1String("in")) {
            query.append({QStringLiteral("channel_id"), filter.id});
        } else if (key == QLatin1String("has")) {
            const QString has = hasParameter(filter.value);
            if (has.isEmpty())
                return fail(QStringLiteral("has: takes link, embed, file, image, video, sound, sticker, "
                                           "poll or forward."));
            query.append({QStringLiteral("has"), has});
        } else if (key == QLatin1String("pinned")) {
            const QString value = filter.value.toLower();
            if (value == QLatin1String("true") || value == QLatin1String("yes"))
                query.append({QStringLiteral("pinned"), QStringLiteral("true")});
            else if (value == QLatin1String("false") || value == QLatin1String("no"))
                query.append({QStringLiteral("pinned"), QStringLiteral("false")});
            else
                return fail(QStringLiteral("pinned: takes true or false."));
        } else {
            const QDate day = readDate(filter.value);
            if (!day.isValid())
                return fail(QStringLiteral("%1: wants a date like %2, not \"%3\".")
                                .arg(key, QDate::currentDate().toString(Qt::ISODate), filter.value));
            const QDateTime from = startOfDay(day);
            const QDateTime to = startOfDay(day.addDays(1));
            if (key == QLatin1String("before") || key == QLatin1String("during")) {
                const QDateTime end = key == QLatin1String("before") ? from : to;
                if (!latest.isValid() || end < latest)
                    latest = end;
            }
            if (key == QLatin1String("after") || key == QLatin1String("during")) {
                const QDateTime start = key == QLatin1String("after") ? to : from;
                if (!earliest.isValid() || start > earliest)
                    earliest = start;
            }
        }
    }

    if (earliest.isValid())
        query.append({QStringLiteral("min_id"), snowflakeAt(earliest)});
    if (latest.isValid())
        query.append({QStringLiteral("max_id"), snowflakeAt(latest)});

    switch (request.sort) {
    case Sort::Newest:
        query.append({QStringLiteral("sort_by"), QStringLiteral("timestamp")});
        query.append({QStringLiteral("sort_order"), QStringLiteral("desc")});
        break;
    case Sort::Oldest:
        query.append({QStringLiteral("sort_by"), QStringLiteral("timestamp")});
        query.append({QStringLiteral("sort_order"), QStringLiteral("asc")});
        break;
    case Sort::Relevant:
        query.append({QStringLiteral("sort_by"), QStringLiteral("relevance")});
        query.append({QStringLiteral("sort_order"), QStringLiteral("desc")});
        break;
    }

    if (request.offset > 0)
        query.append({QStringLiteral("offset"), QString::number(qBound(0, request.offset, MaxOffset))});
    if (request.includeNsfw)
        query.append({QStringLiteral("include_nsfw"), QStringLiteral("true")});

    if (problem)
        problem->clear();
    return query;
}

Page readPage(const QJsonObject &reply)
{
    Page page;
    page.total = reply.value(QStringLiteral("total_results")).toInt();

    for (const QJsonValue &entry : reply.value(QStringLiteral("messages")).toArray()) {
        QJsonObject hit;
        if (entry.isArray()) {
            const QJsonArray group = entry.toArray();
            for (const QJsonValue &candidate : group) {
                if (candidate.toObject().value(QStringLiteral("hit")).toBool()) {
                    hit = candidate.toObject();
                    break;
                }
            }
            if (hit.isEmpty() && !group.isEmpty())
                hit = group.first().toObject();
        } else {
            hit = entry.toObject();
        }
        if (!hit.value(QStringLiteral("id")).toString().isEmpty())
            page.messages.append(hit);
    }

    for (const QJsonValue &entry : reply.value(QStringLiteral("threads")).toArray()) {
        const QJsonObject thread = entry.toObject();
        const QString id = thread.value(QStringLiteral("id")).toString();
        if (!id.isEmpty())
            page.threads.insert(id, thread);
    }
    return page;
}

QStringList highlightTerms(const QString &content)
{
    QStringList terms;
    for (QString word : content.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts)) {
        word.remove(QLatin1Char('"'));
        // Punctuation at either end is not part of what was looked for.
        while (!word.isEmpty() && !isWordChar(word.front()))
            word.remove(0, 1);
        while (!word.isEmpty() && !isWordChar(word.back()))
            word.chop(1);
        word = word.toLower();
        if (!word.isEmpty() && !terms.contains(word))
            terms.append(word);
    }
    // Longest first, so "cakes" is not cut short by "cake".
    std::sort(terms.begin(), terms.end(),
              [](const QString &a, const QString &b) { return a.size() > b.size(); });
    return terms;
}

QString highlightHtml(const QString &plain, const QStringList &terms)
{
    const auto escape = [](const QString &text) {
        return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    };

    QString html;
    html.reserve(plain.size() + 64);
    int done = 0;   // everything before this is in html already
    for (int i = 0; i < plain.size(); ++i) {
        const bool wordStart = isWordChar(plain.at(i)) && (i == 0 || !isWordChar(plain.at(i - 1)));
        if (!wordStart)
            continue;
        for (const QString &term : terms) {
            if (term.isEmpty() || QStringView(plain).mid(i, term.size()).compare(term, Qt::CaseInsensitive) != 0)
                continue;
            // The rest of the word goes with it: "cake" marks all of "cakes".
            int end = i + term.size();
            while (end < plain.size() && isWordChar(plain.at(end)))
                ++end;
            html += escape(plain.mid(done, i - done));
            html += QStringLiteral("<span class=\"hit\">") + escape(plain.mid(i, end - i))
                + QStringLiteral("</span>");
            done = end;
            i = end - 1;
            break;
        }
    }
    html += escape(plain.mid(done));
    return html;
}

} // namespace SearchQuery
