#include "plugin/builtin/NitroWatchPlugin.h"

#include "core/Logger.h"
#include "ui/CaptchaDialog.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <iterator>

namespace {

// The only two places a real gift link can live.
//
// Everything else claiming to be one is a phishing page. This list is
// deliberately short and exact: a "close enough" match here is how people lose
// their account, because the scam domains are chosen to read like the real one
// at a glance (dlscord.gift, discord-nitro.com, discordgift.site).
bool isOfficialGiftHost(const QString &host)
{
    const QString lower = host.toLower();
    return lower == QLatin1String("discord.gift")
        || lower == QLatin1String("discord.com")
        || lower == QLatin1String("www.discord.com")
        || lower == QLatin1String("discordapp.com")
        || lower == QLatin1String("www.discordapp.com")
        || lower == QLatin1String("ptb.discord.com")
        || lower == QLatin1String("canary.discord.com");
}

// Any web address in a piece of text, taken whole.
//
// Whole is the important word. An earlier version of this looked for the gift
// part directly, which meant that in
//
//     https://evil.com/discord.gift/aBcDeF1234567890
//
// the pattern matched from "discord.gift/" onwards, and the address it then
// checked was one it had invented rather than the one that was posted. It
// passed. Its own check caught it. An address has to be read from its start.
const QRegularExpression &urlPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral(R"((?:https?://)?[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)+(?:/[^\s<>"']*)?)"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

// A gift code on its own: letters and digits, nothing else.
const QRegularExpression &codePattern()
{
    static const QRegularExpression pattern(QStringLiteral(R"(^[A-Za-z0-9]{12,24}$)"));
    return pattern;
}

// Anything that is trying to look like a gift link. Used only to warn.
const QRegularExpression &lookalikePattern()
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(https?://[^\s<>"']*(?:gift|nitro)[^\s<>"']*)"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

QLabel *line(const QString &text, const char *colour, int size, bool bold, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: %2px; font-weight: %3; background: transparent;")
                             .arg(QLatin1String(colour))
                             .arg(size)
                             .arg(bold ? 600 : 400));
    return label;
}

} // namespace

QStringList NitroWatchPlugin::findGiftCodes(const QString &text)
{
    QStringList codes;

    auto matches = urlPattern().globalMatch(text);
    while (matches.hasNext()) {
        QString whole = matches.next().captured();

        // Trailing punctuation belongs to the sentence.
        while (!whole.isEmpty() && QStringLiteral(".,;:!?)]}").contains(whole.back()))
            whole.chop(1);

        const QUrl url(whole.startsWith(QLatin1String("http"), Qt::CaseInsensitive)
                           ? whole
                           : QStringLiteral("https://") + whole);
        if (!url.isValid())
            continue;

        const QString host = url.host().toLower();
        if (!isOfficialGiftHost(host))
            continue;

        // The address points at Discord. Now the path has to be a gift and
        // nothing else, because discord.com serves plenty of other things.
        QString code;
        const QString path = url.path();

        if (host == QLatin1String("discord.gift")) {
            // The whole path is the code.
            if (path.startsWith(QLatin1Char('/')))
                code = path.mid(1);
        } else if (path.startsWith(QLatin1String("/gifts/"), Qt::CaseInsensitive)) {
            code = path.mid(7);
        } else if (path.startsWith(QLatin1String("/gift/"), Qt::CaseInsensitive)) {
            code = path.mid(6);
        }

        if (code.endsWith(QLatin1Char('/')))
            code.chop(1);

        // Anything left over means this was a longer path that merely began
        // like a gift link, so it is not one.
        if (!codePattern().match(code).hasMatch())
            continue;

        codes << code;
    }

    return codes;
}

QString NitroWatchPlugin::messageText(const QJsonObject &data)
{
    QStringList parts;
    const auto take = [&](const QJsonObject &obj, const QString &key) {
        const QString s = obj.value(key).toString();
        if (!s.isEmpty())
            parts << s;
    };

    take(data, QStringLiteral("content"));

    for (const QJsonValue &v : data.value(QStringLiteral("embeds")).toArray()) {
        const QJsonObject embed = v.toObject();
        take(embed, QStringLiteral("url"));
        take(embed, QStringLiteral("title"));
        take(embed, QStringLiteral("description"));
        for (const QJsonValue &f : embed.value(QStringLiteral("fields")).toArray()) {
            const QJsonObject field = f.toObject();
            take(field, QStringLiteral("name"));
            take(field, QStringLiteral("value"));
        }
    }

    for (const QJsonValue &row : data.value(QStringLiteral("components")).toArray()) {
        const QJsonObject rowObj = row.toObject();
        take(rowObj, QStringLiteral("url"));
        for (const QJsonValue &child : rowObj.value(QStringLiteral("components")).toArray())
            take(child.toObject(), QStringLiteral("url"));
    }

    return parts.join(QLatin1Char('\n'));
}

void NitroWatchPlugin::onLoad(PluginContext *context)
{
    Plugin::onLoad(context);
    selfCheck();
}

// Checks the link matcher against cases that must pass and cases that must
// fail. The failures matter more: treating a phishing domain as a real gift is
// how somebody loses an account, so a wrong answer here switches the watcher
// off rather than let it guess.
void NitroWatchPlugin::selfCheck()
{
    struct Case
    {
        const char *text;
        const char *expected;   // empty means nothing should match
    };

    static const Case cases[] = {
        // The two real shapes.
        {"free nitro https://discord.gift/aBcDeF1234567890 go", "aBcDeF1234567890"},
        {"https://discord.com/gifts/aBcDeF1234567890", "aBcDeF1234567890"},
        // Written without the scheme, as people paste it.
        {"discord.gift/aBcDeF1234567890", "aBcDeF1234567890"},
        // A real path on somebody else's domain. Must not match.
        {"https://evil.com/discord.gift/aBcDeF1234567890", ""},
        // A lookalike domain, the usual phishing shape. Must not match.
        {"https://dlscord.gift/aBcDeF1234567890", ""},
        {"https://discord-nitro.com/gifts/aBcDeF1234567890", ""},
        // Too short to be a code.
        {"discord.gift/short", ""},
        // Ordinary text.
        {"nothing to see here", ""},
        // A real Discord address that is not a gift.
        {"https://discord.com/channels/123/456", ""},
        // The gift part buried deeper in somebody else's path.
        {"https://evil.com/a/b/discord.gift/aBcDeF1234567890", ""},
        // A subdomain dressed up to read as the real host.
        {"https://discord.gift.evil.com/aBcDeF1234567890", ""},
        // Two real ones in one message.
        {"discord.gift/aBcDeF1234567890 and discord.gift/ZyXwVu0987654321",
         "aBcDeF1234567890,ZyXwVu0987654321"},
        // A full stop ending the sentence, not the link.
        {"here: https://discord.gift/aBcDeF1234567890.", "aBcDeF1234567890"},
        // Official client paths that are not discord.gift.
        {"https://discord.com/gift/aBcDeF1234567890", "aBcDeF1234567890"},
        {"https://ptb.discord.com/gifts/aBcDeF1234567890", "aBcDeF1234567890"},
    };

    QStringList failures;
    for (const Case &test : cases) {
        const QStringList got = findGiftCodes(QString::fromUtf8(test.text));
        const QString want = QString::fromUtf8(test.expected);

        const QString actual = got.join(QStringLiteral(","));
        if (actual != want) {
            failures << QStringLiteral("\"%1\" gave \"%2\", wanted \"%3\"")
                            .arg(QString::fromUtf8(test.text), actual, want);
        }
    }

    {
        QJsonObject msg;
        msg.insert(QStringLiteral("content"), QString());
        QJsonObject embed;
        embed.insert(QStringLiteral("url"), QStringLiteral("https://discord.gift/aBcDeF1234567890"));
        msg.insert(QStringLiteral("embeds"), QJsonArray{embed});
        const QStringList got = findGiftCodes(messageText(msg));
        if (got.join(QLatin1Char(',')) != QLatin1String("aBcDeF1234567890")) {
            failures << QStringLiteral("empty content with gift embed url was missed (got \"%1\")")
                            .arg(got.join(QLatin1Char(',')));
        }
    }

    if (failures.isEmpty()) {
        wlog(QStringLiteral("nitro"),
             QStringLiteral("link matcher passed its own check, %1 cases")
                 .arg(static_cast<int>(std::size(cases))));
        return;
    }

    m_matcherTrusted = false;
    wlog(QStringLiteral("nitro"),
         QStringLiteral("link matcher failed its own check, so nothing will be offered: %1")
             .arg(failures.join(QStringLiteral("; "))));
}

void NitroWatchPlugin::onUnload()
{
    if (m_alert)
        m_alert->deleteLater();
    m_seen.clear();
    Plugin::onUnload();
}

bool NitroWatchPlugin::watchEnabled() const
{
    return context() && context()->setting(QStringLiteral("watch"), true).toBool();
}

bool NitroWatchPlugin::warnFakes() const
{
    return !context() || context()->setting(QStringLiteral("warnFakes"), true).toBool();
}

void NitroWatchPlugin::onGatewayEvent(const QString &eventType, const QJsonObject &data)
{
    if (eventType != QLatin1String("MESSAGE_CREATE") && eventType != QLatin1String("MESSAGE_UPDATE"))
        return;
    if (!watchEnabled())
        return;

    // Native gift cards often have empty content; the link is on the embed.
    const QString text = messageText(data);
    if (text.isEmpty())
        return;

    const QString channelId = data.value(QStringLiteral("channel_id")).toString();
    const QString authorId =
        data.value(QStringLiteral("author")).toObject().value(QStringLiteral("id")).toString();

    const QStringList codes = m_matcherTrusted ? findGiftCodes(text) : QStringList();

    for (const QString &code : codes) {
        // A gift posted twice, or edited afterwards, must not raise two alerts.
        if (m_seen.contains(code))
            continue;
        m_seen.insert(code);

        offer(code, authorId, channelId);
    }

    if (!codes.isEmpty() || !warnFakes())
        return;

    // Nothing real in this message. If something in it is dressed up as a
    // gift, say so once, because that is the actual danger here.
    const QRegularExpressionMatch fake = lookalikePattern().match(text);
    if (fake.hasMatch()) {
        const QUrl url(fake.captured());
        if (!url.host().isEmpty() && !isOfficialGiftHost(url.host()))
            warnAboutFake(url.host());
    }
}

void NitroWatchPlugin::warnAboutFake(const QString &host)
{
    wlog(QStringLiteral("nitro"),
         QStringLiteral("a link on %1 is dressed up as a Nitro gift and is not one, ignoring it")
             .arg(host));

    if (context())
        context()->log(QStringLiteral("Ignored a fake Nitro link on %1").arg(host));
}

void NitroWatchPlugin::offer(const QString &code, const QString &fromUserId, const QString &channelId)
{
    wlog(QStringLiteral("nitro"), QStringLiteral("gift spotted in channel %1").arg(channelId));

    QString who = QStringLiteral("Someone");
    QString where;
    if (context() && context()->store()) {
        const QString name = context()->store()->userName(fromUserId);
        if (!name.isEmpty())
            who = name;

        const ChannelInfo channel = context()->store()->channel(channelId);
        if (!channel.name.isEmpty())
            where = QStringLiteral(" in #%1").arg(channel.name);
    }

    // One alert at a time. A newer gift is the one worth looking at.
    if (m_alert)
        m_alert->deleteLater();

    QWidget *parent = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (qobject_cast<QMainWindow *>(w) && w->isVisible()) {
            parent = w;
            break;
        }
    }

    // Dialog + a real owner, not a parentless Qt::Tool window. Those get
    // created on Windows and then never appear.
    auto *alert = new QWidget(parent, Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    m_alert = alert;
    alert->setAttribute(Qt::WA_DeleteOnClose);
    alert->setAttribute(Qt::WA_StyledBackground, true);
    alert->setWindowModality(Qt::NonModal);
    alert->setWindowTitle(QStringLiteral("Nitro gift"));
    alert->setFixedWidth(340);
    alert->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid %2; border-radius: 10px;")
                             .arg(QLatin1String(Theme::SurfaceInput), QLatin1String(Theme::Border)));

    auto *layout = new QVBoxLayout(alert);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(4);

    layout->addWidget(line(QStringLiteral("Nitro gift"), Theme::TextPrimary, 15, true, alert));
    layout->addWidget(line(QStringLiteral("%1 posted one%2").arg(who, where), Theme::TextMuted, 12, false, alert));
    layout->addWidget(line(QStringLiteral("Claim it yourself. Singularity will not do it for you, and that "
                                          "is what keeps this account looking ordinary."),
                           Theme::TextFaint, 11, false, alert));

    auto *status = line(QString(), Theme::TextMuted, 12, false, alert);
    status->hide();
    layout->addWidget(status);

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 10, 0, 0);
    buttons->setSpacing(8);

    auto *claimButton = new QPushButton(QStringLiteral("Claim"), alert);
    claimButton->setFixedHeight(30);
    claimButton->setCursor(Qt::PointingHandCursor);
    claimButton->setStyleSheet(QStringLiteral("QPushButton { background-color: %1; color: %2; border: none; "
                                        "border-radius: 6px; font-weight: 600; padding: 0 16px; } "
                                        "QPushButton:hover { background-color: %3; }")
                             .arg(QLatin1String(Theme::Accent), QLatin1String(Theme::Dark),
                                  QLatin1String(Theme::LightGray)));
    buttons->addWidget(claimButton);

    auto *ignore = new QPushButton(QStringLiteral("Ignore"), alert);
    ignore->setFixedHeight(30);
    ignore->setCursor(Qt::PointingHandCursor);
    ignore->setStyleSheet(QStringLiteral("QPushButton { background-color: transparent; color: %1; "
                                         "border: 1px solid %2; border-radius: 6px; padding: 0 14px; } "
                                         "QPushButton:hover { background-color: %3; }")
                              .arg(QLatin1String(Theme::TextMuted), QLatin1String(Theme::Border),
                                   QLatin1String(Theme::SurfaceHover)));
    buttons->addWidget(ignore);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    QObject::connect(claimButton, &QPushButton::clicked, alert, [this, code, channelId, status, claimButton]() {
        claim(code, channelId, status, claimButton);
    });
    QObject::connect(ignore, &QPushButton::clicked, alert, &QWidget::close);

    // Sit on the Singularity window if we have one, otherwise the primary screen.
    alert->adjustSize();
    if (parent) {
        const QPoint topRight = parent->mapToGlobal(QPoint(parent->width() - 24, 24));
        alert->move(topRight.x() - alert->width(), topRight.y());
    } else if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        alert->move(area.right() - alert->width() - 24, area.top() + 24);
    }

    alert->show();
    alert->raise();
    alert->activateWindow();

    // Gone after a minute if nobody touches it, so a missed gift does not
    // leave a panel sitting there for the rest of the day.
    QTimer::singleShot(60000, alert, &QWidget::close);
}

void NitroWatchPlugin::claim(const QString &code, const QString &channelId, QLabel *status,
                             QPushButton *button, const RestClient::CaptchaProof &proof)
{
    if (!context() || !context()->rest())
        return;

    QPointer<QLabel> statusLabel = status;
    QPointer<QPushButton> claimButton = button;
    if (claimButton)
        claimButton->setEnabled(false);
    if (statusLabel) {
        statusLabel->setText(QStringLiteral("Claiming..."));
        statusLabel->show();
    }

    wlog(QStringLiteral("nitro"), QStringLiteral("claiming a gift, because the button was pressed"));

    context()->rest()->redeemGift(
        code, channelId,
        [this, statusLabel](const QJsonObject &) {
            wlog(QStringLiteral("nitro"), QStringLiteral("gift claimed"));
            if (statusLabel) {
                statusLabel->setText(QStringLiteral("Claimed."));
                statusLabel->show();
            }
            if (context())
                context()->log(QStringLiteral("Nitro gift claimed."));
        },
        [this, code, channelId, statusLabel, claimButton, proof](const RestClient::Error &error) {
            const QJsonArray keys = error.body.value(QStringLiteral("captcha_key")).toArray();
            bool needsCheck = error.body.contains(QStringLiteral("captcha_sitekey"));
            for (const QJsonValue &key : keys) {
                if (key.toString() == QLatin1String("captcha-required"))
                    needsCheck = true;
            }
            // One check, solved by the person at the keyboard, then one retry.
            // A second demand is reported instead of looping.
            if (needsCheck && proof.key.isEmpty()) {
                QWidget *parent = nullptr;
                for (QWidget *w : QApplication::topLevelWidgets()) {
                    if (qobject_cast<QMainWindow *>(w) && w->isVisible()) {
                        parent = w;
                        break;
                    }
                }
                const QString token = CaptchaDialog::solve(
                    parent, error.body.value(QStringLiteral("captcha_sitekey")).toString(),
                    error.body.value(QStringLiteral("captcha_rqdata")).toString());
                if (token.isEmpty()) {
                    if (statusLabel) {
                        statusLabel->setText(QStringLiteral("Discord asked for a check, and it was not finished."));
                        statusLabel->show();
                    }
                    if (claimButton)
                        claimButton->setEnabled(true);
                    return;
                }
                RestClient::CaptchaProof next;
                next.key = token;
                next.rqtoken = error.body.value(QStringLiteral("captcha_rqtoken")).toString();
                next.sessionId = error.body.value(QStringLiteral("captcha_session_id")).toString();
                claim(code, channelId, statusLabel, claimButton, next);
                return;
            }

            wlog(QStringLiteral("nitro"),
                 QStringLiteral("gift could not be claimed: HTTP %1 %2")
                     .arg(error.httpStatus)
                     .arg(error.message));
            const QString text = error.message.isEmpty()
                                     ? QStringLiteral("Could not claim it (HTTP %1).").arg(error.httpStatus)
                                     : QStringLiteral("Could not claim it: %1").arg(error.message);
            if (statusLabel) {
                statusLabel->setText(text);
                statusLabel->show();
            }
            if (claimButton)
                claimButton->setEnabled(true);
            if (context())
                context()->log(text);
        },
        proof);
}

QWidget *NitroWatchPlugin::createSettingsWidget(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *watch = new QCheckBox(QStringLiteral("Alert me when a gift is posted"), page);
    watch->setChecked(watchEnabled());
    layout->addWidget(watch);

    auto *fakes = new QCheckBox(QStringLiteral("Tell me when a fake gift link is going around"), page);
    fakes->setChecked(warnFakes());
    layout->addWidget(fakes);

    layout->addWidget(line(QStringLiteral(
        "Why there is no automatic claim.\n\n"
        "Discord does not catch automated accounts by timing your click. It catches them on the "
        "shape of the account: a redeem arriving from a session that never opened the channel, "
        "never scrolled, never moved a pointer, and that does it again next week at four in the "
        "morning. A random delay hides none of that, because none of it is about timing.\n\n"
        "Dead codes are also posted on purpose to see who bites. A person glances at a suspicious "
        "link and moves on. Anything claiming on its own takes the bait every time, and marks "
        "itself doing it.\n\n"
        "So this automates the slow part, which is noticing. Singularity watches every channel at once, "
        "including the ones you are not looking at, and gets you from \"a gift exists\" to one "
        "button straight away. You are still faster than anyone reading their messages. The press "
        "stays yours, which is the part that keeps the account ordinary."),
        Theme::TextFaint, 11, false, page));

    layout->addStretch(1);

    QObject::connect(watch, &QCheckBox::toggled, page, [this](bool on) {
        if (context())
            context()->setSetting(QStringLiteral("watch"), on);
    });
    QObject::connect(fakes, &QCheckBox::toggled, page, [this](bool on) {
        if (context())
            context()->setSetting(QStringLiteral("warnFakes"), on);
    });

    return page;
}
