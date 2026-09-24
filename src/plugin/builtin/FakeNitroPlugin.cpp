#include "plugin/builtin/FakeNitroPlugin.h"

#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace {

constexpr int DefaultSize = 48;

} // namespace

void FakeNitroPlugin::onGatewayEvent(const QString &eventType, const QJsonObject &data)
{
    // Any kind of Nitro already lets you use every emoji, so the plugin then
    // stays out of the way.
    if (eventType == QLatin1String("READY")) {
        m_premiumType = data.value(QStringLiteral("user")).toObject()
                            .value(QStringLiteral("premium_type")).toInt();
    } else if (eventType == QLatin1String("USER_UPDATE") && data.contains(QStringLiteral("premium_type"))) {
        m_premiumType = data.value(QStringLiteral("premium_type")).toInt();
    }
}

QString FakeNitroPlugin::rewrite(const QString &content, int premiumType, const QStringList &guildEmojiIds,
                                 int size, bool shortLinks)
{
    if (premiumType != 0)
        return content;

    static const QRegularExpression emojiRe(QStringLiteral("<(a?):([A-Za-z0-9_]+):(\\d+)>"));

    QString out;
    int last = 0;
    auto it = emojiRe.globalMatch(content);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const bool animated = !match.captured(1).isEmpty();
        const QString name = match.captured(2);
        const QString id = match.captured(3);

        // Without Nitro you may send a still emoji from the server you are
        // in. Those go through untouched, as real emoji.
        const bool allowed = !animated && guildEmojiIds.contains(id);

        out += content.mid(last, match.capturedStart() - last);
        if (allowed) {
            out += match.captured(0);
        } else {
            // The same address Vencord's FakeNitro sends, so each client shows
            // the other's fake emoji: .gif when it moves, .png when it does not.
            const QString url = QStringLiteral("https://cdn.discordapp.com/emojis/%1.%2?size=%3&name=%4&lossless=true")
                                    .arg(id, animated ? QStringLiteral("gif") : QStringLiteral("png"))
                                    .arg(size)
                                    .arg(name);
            out += shortLinks ? QStringLiteral("[%1](%2)").arg(name, url) : url;
        }
        last = match.capturedEnd();
    }
    out += content.mid(last);
    return out;
}

bool FakeNitroPlugin::onOutgoingMessage(QString &content, const QString &channelId)
{
    if (!context() || !context()->store())
        return true;

    MessageStore *store = context()->store();
    QStringList guildEmojiIds;
    const QString guildId = store->channel(channelId).guildId;
    if (!guildId.isEmpty()) {
        for (const EmojiInfo &emoji : store->guild(guildId).emojis)
            guildEmojiIds.append(emoji.id);
    }

    content = rewrite(content, m_premiumType, guildEmojiIds,
                      settingValue(QStringLiteral("size"), DefaultSize).toInt(),
                      settingValue(QStringLiteral("shortLinks"), true).toBool());
    return true;
}

QWidget *FakeNitroPlugin::createSettingsWidget(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *form = new QFormLayout;
    auto *size = new QComboBox(page);
    for (int value : {32, 48, 64, 96, 128})
        size->addItem(QStringLiteral("%1 pixels").arg(value), value);
    const int saved = settingValue(QStringLiteral("size"), DefaultSize).toInt();
    size->setCurrentIndex(qMax(0, size->findData(saved)));
    QObject::connect(size, &QComboBox::currentIndexChanged, page, [this, size](int) {
        setSettingValue(QStringLiteral("size"), size->currentData());
    });
    form->addRow(QStringLiteral("Emoji size"), size);
    layout->addLayout(form);

    auto *shortLinks = new QCheckBox(QStringLiteral("Hide the long address behind the emoji's name"), page);
    shortLinks->setChecked(settingValue(QStringLiteral("shortLinks"), true).toBool());
    QObject::connect(shortLinks, &QCheckBox::toggled, page, [this](bool on) {
        setSettingValue(QStringLiteral("shortLinks"), on);
    });
    layout->addWidget(shortLinks);

    auto *note = new QLabel(
        QStringLiteral("This is not real Nitro. Nothing changes on your account. Emoji you are not "
                       "allowed to send are sent as a link to their picture. People using Singularity "
                       "see them as normal emoji. People on the normal Discord app see a small picture "
                       "under your message.\n\n"
                       "Still emoji from the server you are in are sent as real emoji. If you have "
                       "Nitro, this plugin does nothing.\n\n"
                       "Screen share at 1080p and 60 frames already works without Nitro in Singularity."),
        page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(QLatin1String(Theme::TextFaint)));
    layout->addWidget(note);

    layout->addStretch(1);
    return page;
}
