#include "plugin/builtin/NitroWatchPlugin.h"

#include "core/Logger.h"
#include "core/RestClient.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

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
        || lower == QLatin1String("www.discordapp.com");
}

// Real gift links, and only those.
const QRegularExpression &giftPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral(R"((?:https?://)?(?:www\.)?(?:discord\.gift/|discord(?:app)?\.com/gifts/)([A-Za-z0-9]{12,24}))"),
        QRegularExpression::CaseInsensitiveOption);
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

    const QString content = data.value(QStringLiteral("content")).toString();
    if (content.isEmpty() || !watchEnabled())
        return;

    const QString channelId = data.value(QStringLiteral("channel_id")).toString();
    const QString authorId =
        data.value(QStringLiteral("author")).toObject().value(QStringLiteral("id")).toString();

    bool foundReal = false;

    auto matches = giftPattern().globalMatch(content);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString code = match.captured(1);

        // Check the host of what actually matched, not the pattern's promise.
        // A link like evil.com/discord.gift/abc would otherwise slip through.
        const QUrl url(match.captured().startsWith(QLatin1String("http"))
                           ? match.captured()
                           : QStringLiteral("https://") + match.captured());
        if (!isOfficialGiftHost(url.host()))
            continue;

        foundReal = true;

        if (m_seen.contains(code))
            continue;
        m_seen.insert(code);

        offer(code, authorId, channelId);
    }

    if (foundReal || !warnFakes())
        return;

    // Nothing real in this message. If something in it is dressed up as a
    // gift, say so once, because that is the actual danger here.
    const QRegularExpressionMatch fake = lookalikePattern().match(content);
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

    auto *alert = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    m_alert = alert;
    alert->setAttribute(Qt::WA_DeleteOnClose);
    alert->setAttribute(Qt::WA_StyledBackground, true);
    alert->setFixedWidth(340);
    alert->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid %2; border-radius: 10px;")
                             .arg(QLatin1String(Theme::SurfaceInput), QLatin1String(Theme::Border)));

    auto *layout = new QVBoxLayout(alert);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(4);

    layout->addWidget(line(QStringLiteral("Nitro gift"), Theme::TextPrimary, 15, true, alert));
    layout->addWidget(line(QStringLiteral("%1 posted one%2").arg(who, where), Theme::TextMuted, 12, false, alert));
    layout->addWidget(line(QStringLiteral("Claim it yourself. Wisp will not do it for you, and that "
                                          "is what keeps this account looking ordinary."),
                           Theme::TextFaint, 11, false, alert));

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

    QObject::connect(claimButton, &QPushButton::clicked, alert, [this, code, alert]() {
        claim(code);
        alert->close();
    });
    QObject::connect(ignore, &QPushButton::clicked, alert, &QWidget::close);

    // Top right of the screen, clear of the window.
    alert->adjustSize();
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        alert->move(area.right() - alert->width() - 24, area.top() + 24);
    }

    alert->show();
    alert->raise();

    // Gone after a minute if nobody touches it, so a missed gift does not
    // leave a panel sitting there for the rest of the day.
    QTimer::singleShot(60000, alert, &QWidget::close);
}

void NitroWatchPlugin::claim(const QString &code)
{
    if (!context() || !context()->rest())
        return;

    wlog(QStringLiteral("nitro"), QStringLiteral("claiming a gift, because the button was pressed"));

    context()->rest()->redeemGift(
        code,
        [this](const QJsonObject &) {
            wlog(QStringLiteral("nitro"), QStringLiteral("gift claimed"));
            if (context())
                context()->log(QStringLiteral("Nitro gift claimed."));
        },
        [this](const RestClient::Error &error) {
            // Usually means somebody else was quicker, or the code was never
            // real in the first place.
            wlog(QStringLiteral("nitro"),
                 QStringLiteral("gift could not be claimed: HTTP %1 %2")
                     .arg(error.httpStatus)
                     .arg(error.message));
            if (context())
                context()->log(QStringLiteral("Could not claim it: %1").arg(error.message));
        });
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
        "So this automates the slow part, which is noticing. Wisp watches every channel at once, "
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
