#include "core/DiscordWatch.h"

#include "core/Logger.h"

#include <QCoreApplication>
#include <QRegularExpression>

namespace {

// Where a report goes, when somebody chooses to send one.
constexpr char SourceRepository[] = "CodingIsCoolFr/Singularity";

// How many times a thing has to happen before it counts as broken. One odd
// answer can be a fluke on Discord's side; the same answer twice from the
// same place is how it behaves now.
constexpr int MajorAtOnce = 1;
constexpr int MajorAtTwice = 2;
constexpr int MajorAtThrice = 3;
constexpr int NeverMajor = 0;

} // namespace

DiscordWatch &DiscordWatch::instance()
{
    static DiscordWatch watch;
    return watch;
}

QString DiscordWatch::routeShape(const QString &path)
{
    QString bare = path.section(QLatin1Char('?'), 0, 0);
    static const QRegularExpression version(QStringLiteral("^/api/v\\d+"));
    bare.remove(version);

    static const QRegularExpression word(QStringLiteral("^[a-z@][a-z0-9_-]*$"));
    static const QRegularExpression number(QStringLiteral("^\\d+$"));
    QStringList parts = bare.split(QLatin1Char('/'));
    for (QString &part : parts) {
        if (part.isEmpty() || word.match(part).hasMatch())
            continue;
        part = number.match(part).hasMatch() ? QStringLiteral("{id}") : QStringLiteral("{x}");
    }
    return parts.join(QLatin1Char('/'));
}

void DiscordWatch::record(const QString &key, const QString &what, int majorAt)
{
    const QDateTime now = QDateTime::currentDateTime();
    Finding &finding = m_findings[key];
    if (finding.count == 0) {
        finding.key = key;
        finding.what = what;
        finding.first = now;
        m_order.append(key);
        wlog(QStringLiteral("discord"), QStringLiteral("may have changed: %1 (%2)").arg(what, key));
    }
    ++finding.count;
    finding.last = now;

    if (!finding.major && majorAt != NeverMajor && finding.count >= majorAt) {
        finding.major = true;
        wlog(QStringLiteral("discord"),
             QStringLiteral("broken by a change on Discord's side: %1 (%2, seen %3 times)")
                 .arg(what, key)
                 .arg(finding.count));
        emit broke(what);
    }
}

void DiscordWatch::noteRestFailure(const QString &method, const QString &path, int httpStatus, int discordCode)
{
    const QString key = QStringLiteral("%1 %2").arg(method.toUpper(), routeShape(path));

    // Discord's answer for a thing that does not exist carries a code of its
    // own (10003 Unknown Channel and the like). A route it no longer has
    // carries none.
    if (httpStatus == 404 && discordCode == 0) {
        record(key, QStringLiteral("Discord no longer has an address Singularity uses"), MajorAtTwice);
        return;
    }
    if (httpStatus == 405) {
        record(key, QStringLiteral("Discord changed how an address it has is used"), MajorAtTwice);
        return;
    }
    if (httpStatus == 410) {
        record(key, QStringLiteral("Discord retired an address Singularity uses"), MajorAtOnce);
        return;
    }
    if (discordCode == 50041) {
        record(key, QStringLiteral("Discord no longer accepts the API version Singularity speaks"), MajorAtOnce);
        return;
    }
    if (discordCode == 50109) {
        record(key, QStringLiteral("Discord could not read a request Singularity sent"), MajorAtTwice);
        return;
    }
    // A message that is too long is this same code, so only a repeat from
    // the same place counts.
    if (httpStatus == 400 && discordCode == 50035) {
        record(key, QStringLiteral("Discord expects a request to be shaped differently"), MajorAtThrice);
        return;
    }
}

void DiscordWatch::noteGatewayClose(int code)
{
    const QString key = QStringLiteral("gateway close %1").arg(code);
    switch (code) {
    case 4001:
        record(key, QStringLiteral("Discord's gateway no longer knows a command Singularity sends"), MajorAtTwice);
        break;
    case 4002:
        record(key, QStringLiteral("Discord's gateway could not read what Singularity sent"), MajorAtTwice);
        break;
    case 4010:
    case 4011:
        record(key, QStringLiteral("Discord's gateway changed how it splits up connections"), MajorAtOnce);
        break;
    case 4012:
        record(key, QStringLiteral("Discord's gateway no longer accepts the version Singularity speaks"), MajorAtOnce);
        break;
    case 4013:
    case 4014:
        record(key, QStringLiteral("Discord's gateway no longer accepts what Singularity asks to be sent"),
               MajorAtOnce);
        break;
    default:
        break;
    }
}

void DiscordWatch::noteUnknownOpcode(int op)
{
    // New, not broken: a message type Discord added. Kept so a report shows
    // it, never shown on its own.
    record(QStringLiteral("gateway opcode %1").arg(op),
           QStringLiteral("Discord's gateway sent a kind of message Singularity has never seen"), NeverMajor);
}

void DiscordWatch::noteReady(const QJsonObject &ready)
{
    // Without these there is no account, no session to resume and no servers:
    // nothing else in the window can work.
    for (const char *name : {"user", "session_id", "guilds"}) {
        if (!ready.contains(QLatin1String(name))) {
            record(QStringLiteral("READY.%1").arg(QLatin1String(name)),
                   QStringLiteral("Discord's sign-in no longer carries what Singularity is built on"), MajorAtOnce);
        }
    }
    // Without these, parts go missing: direct messages, names, unread marks,
    // settings and server folders.
    for (const char *name : {"private_channels", "users", "read_state", "user_settings_proto"}) {
        if (!ready.contains(QLatin1String(name))) {
            record(QStringLiteral("READY.%1").arg(QLatin1String(name)),
                   QStringLiteral("Discord's sign-in left out something Singularity reads"), MajorAtTwice);
        }
    }
}

QList<DiscordWatch::Finding> DiscordWatch::findings() const
{
    QList<Finding> out;
    out.reserve(m_order.size());
    for (const QString &key : m_order)
        out.append(m_findings.value(key));
    return out;
}

bool DiscordWatch::hasMajor() const
{
    for (const Finding &finding : m_findings) {
        if (finding.major)
            return true;
    }
    return false;
}

QString DiscordWatch::report() const
{
    QStringList lines;
    lines << QStringLiteral("Singularity %1 on Windows").arg(QCoreApplication::applicationVersion());
    lines << QString();
    for (const Finding &finding : findings()) {
        lines << QStringLiteral("%1 %2").arg(finding.major ? QStringLiteral("[broken]") : QStringLiteral("[noted] "),
                                             finding.what);
        lines << QStringLiteral("    %1 - seen %2 time%3, first %4, last %5")
                     .arg(finding.key)
                     .arg(finding.count)
                     .arg(finding.count == 1 ? QString() : QStringLiteral("s"))
                     .arg(finding.first.toString(Qt::ISODate), finding.last.toString(Qt::ISODate));
    }
    if (m_order.isEmpty())
        lines << QStringLiteral("Nothing noticed.");
    return lines.join(QLatin1Char('\n'));
}

QUrl DiscordWatch::issueUrl() const
{
    QString title = QStringLiteral("Discord change broke something");
    for (const Finding &finding : findings()) {
        if (finding.major) {
            title = QStringLiteral("Discord change: %1").arg(finding.what);
            break;
        }
    }
    const QString body =
        QStringLiteral("Singularity noticed that Discord has changed something it relies on.\n\n"
                       "```\n%1\n```\n\n"
                       "What I was doing when it happened:\n\n")
            .arg(report());

    // Encoded by hand, every reserved character included, so a newline, a
    // "+" or an "&" in the report arrives on the page as itself.
    const auto encode = [](const QString &text) { return QString::fromLatin1(QUrl::toPercentEncoding(text)); };
    QUrl url(QStringLiteral("https://github.com/%1/issues/new").arg(QLatin1String(SourceRepository)));
    url.setQuery(QStringLiteral("title=%1&body=%2&labels=discord-change").arg(encode(title), encode(body)),
                 QUrl::StrictMode);
    return url;
}
