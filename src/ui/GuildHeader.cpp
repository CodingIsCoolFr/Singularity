#include "ui/GuildHeader.h"

#include "core/MessageStore.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace {

constexpr int TitleRow = 48;      // the plain name row, as before
constexpr int BannerHeight = 138; // Discord's sidebar banner is about this tall
constexpr int BoostRow = 40;
constexpr int Side = 16;

// Discord's boost pink. Colour in this client only stays where it means
// something, and this one means "boosts" everywhere Discord shows them.
const QColor BoostPink(0xff, 0x73, 0xfa);
const QColor BoostViolet(0xb4, 0x73, 0xf5);

} // namespace

GuildHeader::GuildHeader(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("SidebarHeader"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // The banner arrives after the header is drawn; draw again when it does.
    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &url) {
        if (url != m_bannerUrl)
            return;
        m_banner = MediaCache::instance().image(url);
        updateGeometry();
        update();
    });
}

// Discord's own rule, read from its web client (the `to()` hook beside the
// progress bar, and module 568065, September 2026):
//
//   goal = boosts for Level 3 (14; 0 if the server has PREMIUM_TIER_3_OVERRIDE)
//        + the boost price of every perk bought with boosts rather than
//          unlocked by a level.
//
// Six of those perks always count: server tags 3, enhanced role colours 3,
// and the pets 3, flex 5, plant 3 and creepy-crawlies 2 badge packs - 19.
// Three more count only where a Discord experiment has switched them on for
// that server: 250 MB uploads 4, server themes 3, game servers 3. Singularity
// cannot see experiments, so those are counted only once the server owns the
// perk. That is why a server in one of those tests can read, say, 20/33 here
// and 20/36 in Discord. The vanity URL (5) is part of Level 3 and not extra.
int GuildHeader::boostGoal(const GuildInfo &guild)
{
    int goal = guild.features.contains(QStringLiteral("PREMIUM_TIER_3_OVERRIDE")) ? 0 : 14;
    goal += 3 + 3 + 3 + 5 + 3 + 2;

    struct Gated { const char *feature; int price; };
    static const Gated gated[] = {
        {"MAX_FILE_SIZE_250_MB", 4},
        {"GUILD_THEME", 3},
        {"GAME_SERVERS", 3},
    };
    for (const Gated &perk : gated) {
        if (guild.features.contains(QLatin1String(perk.feature)))
            goal += perk.price;
    }
    return goal;
}

void GuildHeader::setDirectMessages()
{
    m_title = QStringLiteral("Direct messages");
    m_bannerUrl.clear();
    m_banner = QImage();
    m_showBoosts = false;
    updateGeometry();
    update();
}

void GuildHeader::setGuild(const GuildInfo &guild)
{
    m_title = guild.name;
    m_bannerUrl = MediaCache::bannerUrl(guild.id, guild.bannerHash, 480);
    m_banner = m_bannerUrl.isEmpty() ? QImage() : MediaCache::instance().image(m_bannerUrl);
    m_boosts = guild.boostCount;
    m_goal = boostGoal(guild);
    m_showBoosts = guild.boostBarEnabled && m_goal > 0;
    updateGeometry();
    update();
}

int GuildHeader::bannerHeight() const
{
    return m_bannerUrl.isEmpty() ? 0 : BannerHeight;
}

int GuildHeader::boostRowHeight() const
{
    return m_showBoosts ? BoostRow : 0;
}

QSize GuildHeader::sizeHint() const
{
    const int top = bannerHeight() > 0 ? bannerHeight() : TitleRow;
    return {200, top + boostRowHeight()};
}

void GuildHeader::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const int w = width();
    const int bannerH = bannerHeight();

    if (bannerH > 0) {
        const QRect area(0, 0, w, bannerH);
        if (!m_banner.isNull()) {
            // Cover the strip, cropping the middle, as Discord does.
            const QImage scaled = m_banner.scaled(area.size(), Qt::KeepAspectRatioByExpanding,
                                                  Qt::SmoothTransformation);
            const QPoint from((scaled.width() - w) / 2, (scaled.height() - bannerH) / 2);
            p.drawImage(area, scaled, QRect(from, area.size()));
        } else {
            p.fillRect(area, QColor(Theme::SurfaceInput));   // until it loads
        }

        // Dark at the top so the name reads on any picture, and fading into
        // the sidebar at the bottom so the banner has no hard edge.
        QLinearGradient shade(0, 0, 0, bannerH);
        shade.setColorAt(0.0, QColor(0, 0, 0, 150));
        shade.setColorAt(0.35, QColor(0, 0, 0, 0));
        QColor sidebar(Theme::SurfaceSidebar);
        sidebar.setAlpha(0);
        shade.setColorAt(0.75, sidebar);
        sidebar.setAlpha(255);
        shade.setColorAt(1.0, sidebar);
        p.fillRect(area, shade);
    }

    // The server's name, over the banner or on its own row.
    QFont title = font();
    title.setPixelSize(15);
    title.setWeight(QFont::DemiBold);
    p.setFont(title);
    p.setPen(QColor(Theme::TextPrimary));
    const QRect titleBox(Side, 0, w - 2 * Side, TitleRow);
    p.drawText(titleBox, Qt::AlignLeft | Qt::AlignVCenter,
               p.fontMetrics().elidedText(m_title, Qt::ElideRight, titleBox.width()));

    if (!m_showBoosts)
        return;

    // Boost goal: a pill that fills, and the count beside it.
    const int goal = m_goal;
    const int top = (bannerH > 0 ? bannerH : TitleRow);
    const int rowMid = top + BoostRow / 2 - 4;

    QFont small = font();
    small.setPixelSize(12);
    small.setWeight(QFont::DemiBold);
    p.setFont(small);
    const QString count = QStringLiteral("%1/%2 Boosts ›").arg(m_boosts).arg(goal);
    const int countWidth = p.fontMetrics().horizontalAdvance(count);

    const QRectF pill(Side, rowMid - 11, qMax(60, w - 2 * Side - countWidth - 12), 22);
    QPainterPath shape;
    shape.addRoundedRect(pill, 11, 11);
    p.fillPath(shape, QColor(Theme::SurfaceInput));

    const double share = goal > 0 ? qBound(0.0, double(m_boosts) / goal, 1.0) : 0.0;
    if (share > 0) {
        QLinearGradient fill(pill.topLeft(), pill.topRight());
        fill.setColorAt(0.0, BoostViolet);
        fill.setColorAt(1.0, BoostPink);
        p.save();
        p.setClipPath(shape);
        p.fillRect(QRectF(pill.left(), pill.top(), pill.width() * share, pill.height()), fill);
        p.restore();
    }

    QFont label = small;
    label.setPixelSize(11);
    p.setFont(label);
    p.setPen(Qt::white);
    p.drawText(pill.adjusted(10, 0, -6, 0), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("Boost Goal"));

    p.setFont(small);
    p.setPen(QColor(Theme::TextMuted));
    p.drawText(QRectF(pill.right() + 12, pill.top(), countWidth + 2, pill.height()),
               Qt::AlignLeft | Qt::AlignVCenter, count);
}
