#include "plugin/builtin/KeywordAlertsPlugin.h"

#include "core/GatewayClient.h"
#include "core/Logger.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QDateTime>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// A channel that keeps saying the word alerts once in this long.
constexpr qint64 ChannelQuietMs = 30 * 1000;

// How much of the message the notification shows.
constexpr int ExcerptChars = 180;

QRegularExpression patternFor(const QString &word, bool wholeWords)
{
    const QString escaped = QRegularExpression::escape(word);
    // Not inside a longer word on either side; works for words that begin or
    // end with a symbol ("c++"), where \b would not.
    const QString pattern = wholeWords
        ? QStringLiteral("(?<![\\p{L}\\p{N}_])%1(?![\\p{L}\\p{N}_])").arg(escaped)
        : escaped;
    return QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption
                                           | QRegularExpression::UseUnicodePropertiesOption);
}

// What a message says: its text, and the title and description of any embed
// (a bot's message is often nothing but an embed).
QString messageText(const QJsonObject &message)
{
    QStringList parts{message.value(QStringLiteral("content")).toString()};
    for (const QJsonValue &value : message.value(QStringLiteral("embeds")).toArray()) {
        const QJsonObject embed = value.toObject();
        parts << embed.value(QStringLiteral("title")).toString() << embed.value(QStringLiteral("description")).toString();
    }
    parts.removeAll(QString());
    return parts.join(QLatin1Char('\n'));
}

} // namespace

QStringList KeywordAlertsPlugin::parseWords(const QString &lines)
{
    QStringList words;
    QSet<QString> seen;
    for (const QString &line : lines.split(QLatin1Char('\n'))) {
        const QString word = line.simplified();
        if (word.isEmpty() || seen.contains(word.toCaseFolded()))
            continue;
        seen.insert(word.toCaseFolded());
        words.append(word);
    }
    return words;
}

QStringList KeywordAlertsPlugin::matches(const QString &text, const QStringList &words, bool wholeWords)
{
    QStringList found;
    for (const QString &word : words) {
        if (patternFor(word, wholeWords).match(text).hasMatch())
            found.append(word);
    }
    return found;
}

void KeywordAlertsPlugin::onLoad(PluginContext *context)
{
    Plugin::onLoad(context);
    rebuild();
    if (context->store())
        m_selfId = context->store()->selfUserId();
    watchEveryServer();
}

void KeywordAlertsPlugin::rebuild()
{
    m_words = parseWords(settingValue(QStringLiteral("words")).toString());
    const bool whole = settingValue(QStringLiteral("wholeWords"), true).toBool();
    m_patterns.clear();
    for (const QString &word : std::as_const(m_words))
        m_patterns.append(patternFor(word, whole));
}

void KeywordAlertsPlugin::watchEveryServer()
{
    // Nothing to listen for, nothing to ask for.
    if (m_words.isEmpty() || !context() || !context()->gateway() || !context()->store()
        || context()->gateway()->state() != GatewayClient::State::Ready)
        return;
    QStringList ids;
    for (const GuildInfo &guild : context()->store()->guilds())
        ids.append(guild.id);
    // Servers already subscribed, by Nitro watch or by being open, are skipped.
    context()->gateway()->keepGuildsSubscribed(ids);
}

void KeywordAlertsPlugin::onServerRail(const QStringList &joinedGuildIds)
{
    Q_UNUSED(joinedGuildIds)
    // After signing in, and after joining a server: the store has them all.
    watchEveryServer();
}

void KeywordAlertsPlugin::onGatewayEvent(const QString &eventType, const QJsonObject &data)
{
    if (eventType == QLatin1String("READY")) {
        m_selfId = data.value(QStringLiteral("user")).toObject().value(QStringLiteral("id")).toString();
        m_lastAlert.clear();
    } else if (eventType == QLatin1String("MESSAGE_CREATE")) {
        consider(data);
    }
}

void KeywordAlertsPlugin::consider(const QJsonObject &message)
{
    if (m_patterns.isEmpty() || !context() || !context()->store())
        return;
    const QJsonObject author = message.value(QStringLiteral("author")).toObject();
    const QString authorId = author.value(QStringLiteral("id")).toString();
    if (authorId.isEmpty() || authorId == m_selfId || authorId == context()->store()->selfUserId())
        return;
    if (author.value(QStringLiteral("bot")).toBool() && settingValue(QStringLiteral("ignoreBots"), true).toBool())
        return;
    const QString guildId = message.value(QStringLiteral("guild_id")).toString();
    if (guildId.isEmpty() && !settingValue(QStringLiteral("directMessages"), true).toBool())
        return;

    const QString text = messageText(message);
    QString found;
    for (int i = 0; i < m_patterns.size() && found.isEmpty(); ++i) {
        if (m_patterns.at(i).match(text).hasMatch())
            found = m_words.at(i);
    }
    if (found.isEmpty())
        return;

    const QString channelId = message.value(QStringLiteral("channel_id")).toString();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastAlert.value(channelId, 0) < ChannelQuietMs)
        return;
    m_lastAlert.insert(channelId, now);

    MessageStore *store = context()->store();
    const QString nick = message.value(QStringLiteral("member")).toObject().value(QStringLiteral("nick")).toString();
    const QString globalName = author.value(QStringLiteral("global_name")).toString();
    const QString who = !nick.isEmpty() ? nick
        : !globalName.isEmpty()         ? globalName
                                        : author.value(QStringLiteral("username")).toString();
    QString excerpt = message.value(QStringLiteral("content")).toString().simplified();
    if (excerpt.isEmpty())
        excerpt = text.simplified();
    if (excerpt.size() > ExcerptChars)
        excerpt = excerpt.left(ExcerptChars - 1) + QChar(0x2026);

    QString title;
    QString body;
    if (guildId.isEmpty()) {
        title = QStringLiteral("\"%1\" from %2").arg(found, who);
        body = excerpt;
    } else {
        const QString channel = store->channel(channelId).name;
        const QString server = store->guild(guildId).name;
        title = QStringLiteral("\"%1\" in #%2").arg(found, channel.isEmpty() ? QStringLiteral("a channel") : channel);
        if (!server.isEmpty())
            title += QStringLiteral(" · ") + server;
        body = QStringLiteral("%1: %2").arg(who, excerpt);
    }
    context()->notify(title, body, channelId);
    wlog(QStringLiteral("keywords"), QStringLiteral("alerted for \"%1\" in channel %2").arg(found, channelId));
}

QWidget *KeywordAlertsPlugin::createSettingsWidget(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    layout->addWidget(new QLabel(QStringLiteral("Words and phrases, one a line:"), page));
    auto *words = new QPlainTextEdit(page);
    words->setPlaceholderText(QStringLiteral("your name\nthe game you play\ngiveaway"));
    words->setPlainText(settingValue(QStringLiteral("words")).toString());
    words->setFixedHeight(120);
    layout->addWidget(words);

    // Saved a moment after the typing stops, not on every key.
    auto *save = new QTimer(page);
    save->setSingleShot(true);
    save->setInterval(500);
    QObject::connect(words, &QPlainTextEdit::textChanged, save, qOverload<>(&QTimer::start));
    QObject::connect(save, &QTimer::timeout, page, [this, words]() {
        setSettingValue(QStringLiteral("words"), words->toPlainText());
        rebuild();
        watchEveryServer();
    });

    struct Toggle
    {
        const char *key;
        const char *label;
    };
    static const Toggle toggles[] = {
        {"wholeWords", "Whole words only (\"art\" does not match \"start\")"},
        {"directMessages", "Direct and group messages too"},
        {"ignoreBots", "Ignore bots"},
    };
    for (const Toggle &toggle : toggles) {
        const QString key = QString::fromLatin1(toggle.key);
        auto *box = new QCheckBox(QString::fromLatin1(toggle.label), page);
        box->setChecked(settingValue(key, true).toBool());
        QObject::connect(box, &QCheckBox::toggled, page, [this, key](bool on) {
            setSettingValue(key, on);
            rebuild();
        });
        layout->addWidget(box);
    }

    auto *note = new QLabel(
        QStringLiteral("Capital letters never matter. Your own messages never alert you, and a channel that "
                       "keeps saying the word alerts once every half minute. Clicking the notification opens "
                       "the channel.\n\nDiscord sends no messages from a server of more than 250 members until "
                       "the app subscribes to it, so while this is on and has words, Singularity subscribes to "
                       "every server you are in, with the same request opening each one sends, as Nitro watch "
                       "does. Busy servers then send a little more traffic."),
        page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(QLatin1String(Theme::TextFaint)));
    layout->addWidget(note);
    layout->addStretch(1);
    return page;
}
