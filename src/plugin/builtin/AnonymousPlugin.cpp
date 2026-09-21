#include "plugin/builtin/AnonymousPlugin.h"

#include "core/AppConfig.h"
#include "core/GatewayClient.h"
#include "core/Logger.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QLabel>

#include <iterator>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {

// Query parameters that exist to tell somebody where you came from.
//
// Every name here is a known tracking parameter from a named source, not a
// guess. Anything merely suspicious is left alone, because a link that stops
// working is worse than a link that carries a tracking code: the first is
// obvious and the second is at least still a link.
//
// `ref` on its own is deliberately absent. Several sites use it to choose what
// to show you, so removing it can change where the link lands.
const QSet<QString> &trackingParameters()
{
    static const QSet<QString> names{
        // Google's campaign tags, used by almost everyone.
        QStringLiteral("utm_source"), QStringLiteral("utm_medium"),
        QStringLiteral("utm_campaign"), QStringLiteral("utm_term"),
        QStringLiteral("utm_content"), QStringLiteral("utm_id"),
        QStringLiteral("utm_name"), QStringLiteral("utm_cid"),
        QStringLiteral("utm_reader"), QStringLiteral("utm_referrer"),
        QStringLiteral("utm_social"), QStringLiteral("utm_brand"),

        // Click identifiers. These are the ones that name you personally.
        QStringLiteral("fbclid"),    // Facebook
        QStringLiteral("gclid"),     // Google Ads
        QStringLiteral("dclid"),     // Google Display
        QStringLiteral("gbraid"), QStringLiteral("wbraid"),
        QStringLiteral("msclkid"),   // Microsoft
        QStringLiteral("yclid"),     // Yandex
        QStringLiteral("twclid"),    // Twitter
        QStringLiteral("ttclid"),    // TikTok
        QStringLiteral("igshid"), QStringLiteral("igsh"),   // Instagram
        QStringLiteral("mibextid"),  // Meta
        QStringLiteral("rb_clickid"),
        QStringLiteral("s_kwcid"),
        QStringLiteral("vero_conv"), QStringLiteral("vero_id"),
        QStringLiteral("wickedid"),

        // Share identifiers. YouTube and Spotify put one on every share link,
        // and it ties the person who opens it back to the person who sent it.
        QStringLiteral("si"),
        QStringLiteral("share_id"), QStringLiteral("share_source"),

        // Mailing lists.
        QStringLiteral("mc_cid"), QStringLiteral("mc_eid"),
        QStringLiteral("_hsenc"), QStringLiteral("_hsmi"),
        QStringLiteral("hsa_acc"), QStringLiteral("hsa_cam"), QStringLiteral("hsa_grp"),
        QStringLiteral("hsa_ad"), QStringLiteral("hsa_src"), QStringLiteral("hsa_tgt"),
        QStringLiteral("hsa_kw"), QStringLiteral("hsa_mt"), QStringLiteral("hsa_net"),
        QStringLiteral("hsa_ver"),
        QStringLiteral("ck_subscriber_id"),
        QStringLiteral("oly_anon_id"), QStringLiteral("oly_enc_id"),

        // Analytics packages.
        QStringLiteral("_ga"), QStringLiteral("_gl"),
        QStringLiteral("pk_campaign"), QStringLiteral("pk_kwd"),
        QStringLiteral("piwik_campaign"), QStringLiteral("piwik_kwd"),

        // Site specific.
        QStringLiteral("ref_src"), QStringLiteral("ref_url"),   // Twitter
        QStringLiteral("__tn__"), QStringLiteral("__cft__"),    // Facebook
        QStringLiteral("at_medium"), QStringLiteral("at_campaign"),  // BBC
        QStringLiteral("spm"),                                  // AliExpress
    };
    return names;
}

// Matches a bare http or https link. Deliberately plain: anything clever here
// risks eating punctuation that belongs to the sentence around the link.
const QRegularExpression &linkPattern()
{
    static const QRegularExpression pattern(QStringLiteral(R"(https?://[^\s<>"']+)"));
    return pattern;
}

QLabel *note(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                             .arg(QLatin1String(Theme::TextFaint)));
    return label;
}

} // namespace

void AnonymousPlugin::onLoad(PluginContext *context)
{
    Plugin::onLoad(context);
    selfCheck();
    applyPresence();
}

// Runs the link cleaner over a handful of known cases as the plugin loads.
//
// This is the one hook in Singularity that rewrites what you say before it is sent,
// so a quiet mistake here does not show up as an error: it shows up as a
// broken link in somebody else's conversation. A line in the log is cheap, and
// silence means all of these passed.
void AnonymousPlugin::selfCheck() const
{
    struct Case
    {
        const char *input;
        const char *expected;
    };

    static const Case cases[] = {
        // A tracker on its own leaves no query behind.
        {"watch https://youtu.be/abc?si=XYZ now", "watch https://youtu.be/abc now"},
        // The full stop belongs to the sentence, not the link.
        {"see https://x.com/a?utm_source=b.", "see https://x.com/a."},
        // A link with nothing to remove must come back byte for byte.
        {"https://x.com/a?id=5", "https://x.com/a?id=5"},
        // Only the tracker goes; the parameter that does something stays.
        {"https://a.com/?utm_source=x&id=5", "https://a.com/?id=5"},
        // Two links, and only the second needs anything done to it.
        {"https://a.com/?id=1 and https://b.com/?fbclid=z",
         "https://a.com/?id=1 and https://b.com/"},
        // Text with no link at all is never touched.
        {"nothing to do here", "nothing to do here"},
    };

    QStringList failures;
    for (const Case &test : cases) {
        const QString got = cleanLinks(QString::fromUtf8(test.input));
        const QString want = QString::fromUtf8(test.expected);
        if (got != want)
            failures << QStringLiteral("\"%1\" became \"%2\"").arg(QString::fromUtf8(test.input), got);
    }

    // Written straight to the log file rather than through the plugin's own
    // logger. That one raises a signal for the window to show, and at load
    // time the window does not exist yet, so the line would go nowhere.
    if (failures.isEmpty()) {
        wlog(QStringLiteral("anonymous"),
             QStringLiteral("link cleaner passed its own check, %1 cases")
                 .arg(static_cast<int>(std::size(cases))));
        return;
    }

    // Refuses to touch a message rather than risk mangling one. A tracking
    // code getting through is a small harm; a broken link that the sender
    // cannot see is a worse one.
    m_linkCleanerTrusted = false;

    wlog(QStringLiteral("anonymous"),
         QStringLiteral("link cleaner failed its own check, so it will not edit messages: %1")
             .arg(failures.join(QStringLiteral("; "))));
}

void AnonymousPlugin::onUnload()
{
    // Switching the plugin off must put the status back, or turning it off
    // would leave you invisible with nothing left on screen to explain why.
    //
    // Back to what you chose, not to "online". Hard-coding online here meant
    // this quietly cancelled a Do Not Disturb you had set yourself.
    if (context() && context()->gateway()) {
        context()->gateway()->setPresenceStatus(
            AppConfig::instance()
                .value(QStringLiteral("presence/status"), QStringLiteral("online"))
                .toString());
    }

    Plugin::onUnload();
}

bool AnonymousPlugin::stripTracking() const
{
    if (!m_linkCleanerTrusted)
        return false;
    return !context() || context()->setting(QStringLiteral("stripTracking"), true).toBool();
}

bool AnonymousPlugin::hideTyping() const
{
    return context() && context()->setting(QStringLiteral("hideTyping"), false).toBool();
}

bool AnonymousPlugin::appearOffline() const
{
    return context() && context()->setting(QStringLiteral("appearOffline"), false).toBool();
}

void AnonymousPlugin::applyPresence()
{
    if (!context() || !context()->gateway())
        return;

    context()->gateway()->setPresenceStatus(
        appearOffline() ? QStringLiteral("invisible")
                        : AppConfig::instance()
                              .value(QStringLiteral("presence/status"), QStringLiteral("online"))
                              .toString());
}

QString AnonymousPlugin::cleanLinks(const QString &text, int *removedCount)
{
    int removed = 0;
    QString result;
    result.reserve(text.size());

    int copiedTo = 0;
    auto matches = linkPattern().globalMatch(text);

    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString found = match.captured();

        // Trailing punctuation usually belongs to the sentence, not the link.
        QString candidate = found;
        while (!candidate.isEmpty()
               && QStringLiteral(".,;:!?)]}").contains(candidate.back())) {
            candidate.chop(1);
        }

        QUrl url(candidate);
        if (!url.isValid() || !url.hasQuery()) {
            continue;
        }

        // Parameters are taken out of the query rather than a new query being
        // built from the ones worth keeping. Rebuilding re-encodes everything
        // it touches, which can quietly change a link that was already
        // correct; removing leaves every other parameter byte for byte.
        QUrlQuery query(url);
        int removedHere = 0;

        // queryItems hands back a copy, so removing from `query` while walking
        // this list is safe.
        const auto items = query.queryItems(QUrl::FullyEncoded);
        for (const auto &item : items) {
            if (!trackingParameters().contains(item.first.toLower()))
                continue;
            query.removeAllQueryItems(item.first);
            ++removedHere;
        }

        if (removedHere == 0)
            continue;   // nothing here was a tracker, so this link is untouched

        removed += removedHere;

        if (query.isEmpty())
            url.setQuery(QString());
        else
            url.setQuery(query);

        // Copy everything before this link unchanged, then the cleaned link,
        // then whatever punctuation was trimmed off the end of it.
        result += text.mid(copiedTo, match.capturedStart() - copiedTo);
        result += url.toString(QUrl::FullyEncoded);
        result += found.mid(candidate.size());
        copiedTo = match.capturedEnd();
    }

    result += text.mid(copiedTo);

    if (removedCount)
        *removedCount = removed;

    return removed > 0 ? result : text;
}

bool AnonymousPlugin::onOutgoingMessage(QString &content, const QString &channelId)
{
    Q_UNUSED(channelId)

    if (!stripTracking() || content.isEmpty())
        return true;

    int removed = 0;
    const QString cleaned = cleanLinks(content, &removed);

    if (removed > 0) {
        content = cleaned;
        wlog(QStringLiteral("anonymous"),
             QStringLiteral("removed %1 tracking parameter%2 from a link before sending")
                 .arg(removed)
                 .arg(removed == 1 ? QString() : QStringLiteral("s")));
    }

    return true;
}

bool AnonymousPlugin::onBeforeTyping(const QString &channelId)
{
    Q_UNUSED(channelId)
    return !hideTyping();
}

QWidget *AnonymousPlugin::createSettingsWidget(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // ---- what this plugin switches off ---------------------------------

    auto *strip = new QCheckBox(QStringLiteral("Remove tracking codes from links I send"), page);
    strip->setChecked(stripTracking());
    layout->addWidget(strip);
    layout->addWidget(note(QStringLiteral(
        "Share links often carry a code naming who sent them, so opening one tells the "
        "site that you and the sender know each other. This takes those out before the "
        "message leaves. The link still works, and it protects whoever you send it to as "
        "much as it protects you."), page));

    auto *typing = new QCheckBox(QStringLiteral("Never tell anyone I am typing"), page);
    typing->setChecked(hideTyping());
    layout->addWidget(typing);
    layout->addWidget(note(QStringLiteral(
        "The Silent typing plugin does exactly this on its own, if it is the only part "
        "you want."), page));

    auto *offline = new QCheckBox(QStringLiteral("Always appear offline"), page);
    offline->setChecked(appearOffline());
    layout->addWidget(offline);
    layout->addWidget(note(QStringLiteral(
        "Hides you from other people. It does not hide you from Discord, whose servers "
        "still know you are connected, because you are."), page));

    // ---- what Singularity never did in the first place ------------------------

    auto *heading = new QLabel(QStringLiteral("Things Singularity never sends"), page);
    heading->setStyleSheet(QStringLiteral("color: %1; font-weight: 600; margin-top: 12px;")
                               .arg(QLatin1String(Theme::TextMuted)));
    layout->addWidget(heading);

    layout->addWidget(note(QStringLiteral(
        "These are not settings, and no switch turns them on. They are missing because "
        "the code to do them was never written.\n\n"
        "•  No analytics events. The official client posts to an endpoint called "
        "/science as you click around. Singularity never calls it.\n"
        "•  No read receipts. Nothing tells Discord which messages you have looked "
        "at, or when.\n"
        "•  No real fingerprint. The client details sent on sign-in are fixed "
        "numbers written into the source, not your actual Windows version, locale or "
        "hardware, and they are identical for everyone running Singularity.\n"
        "•  No session correlation. The fields the official client fills with "
        "identifiers that link your sessions together are sent empty.\n"
        "•  No game detection. Nothing looks at what programs you have open.\n"
        "•  No third party image loading. Pictures are only ever fetched from "
        "Discord's own hosts, so a stranger's message cannot make your client contact "
        "an address of their choosing and learn where you are."), page));

    layout->addStretch(1);

    QObject::connect(strip, &QCheckBox::toggled, page, [this](bool on) {
        if (context())
            context()->setSetting(QStringLiteral("stripTracking"), on);
    });

    QObject::connect(typing, &QCheckBox::toggled, page, [this](bool on) {
        if (context())
            context()->setSetting(QStringLiteral("hideTyping"), on);
    });

    QObject::connect(offline, &QCheckBox::toggled, page, [this](bool on) {
        if (context())
            context()->setSetting(QStringLiteral("appearOffline"), on);
        applyPresence();
    });

    return page;
}
