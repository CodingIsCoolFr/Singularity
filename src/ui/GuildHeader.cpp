#include "ui/GuildHeader.h"

#include "core/MessageStore.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QLinearGradient>
#include <QMouseEvent>
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
    setMouseTracking(true);

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

bool GuildHeader::overTitle(const QPoint &pos) const
{
    return m_isGuild && pos.y() >= 0 && pos.y() < TitleRow;
}

void GuildHeader::mouseMoveEvent(QMouseEvent *event)
{
    const bool over = overTitle(event->position().toPoint());
    if (over != m_hoverTitle) {
        m_hoverTitle = over;
        setCursor(over ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update(0, 0, width(), TitleRow);
    }
    QWidget::mouseMoveEvent(event);
}

void GuildHeader::leaveEvent(QEvent *event)
{
    if (m_hoverTitle) {
        m_hoverTitle = false;
        unsetCursor();
        update(0, 0, width(), TitleRow);
    }
    QWidget::leaveEvent(event);
}

void GuildHeader::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && overTitle(event->position().toPoint())) {
        emit menuRequested(mapToGlobal(QPoint(Side / 2, TitleRow - 4)));
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void GuildHeader::setDirectMessages()
{
    m_isGuild = false;
    m_title = QStringLiteral("Direct messages");
    m_bannerUrl.clear();
    m_banner = QImage();
    m_showBoosts = false;
    updateGeometry();
    update();
}

void GuildHeader::setGuild(const GuildInfo &guild)
{
    m_isGuild = true;
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
        // Dark at the top so the name reads on any picture. The bottom is
        // clear, so it meets the sidebar whether that sidebar is solid or glass.
        QLinearGradient shade(0, 0, 0, bannerH);
        shade.setColorAt(0.0, QColor(0, 0, 0, 150));
        shade.setColorAt(0.35, QColor(0, 0, 0, 0));
        shade.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.fillRect(area, shade);
    }

    // The server's name, over the banner or on its own row. On a server it is
    // a button, as in Discord: a faint wash on hover and a chevron at the end.
    if (m_isGuild && m_hoverTitle)
        p.fillRect(QRect(0, 0, w, TitleRow), QColor(255, 255, 255, 18));

    QFont title = font();
    title.setPixelSize(15);
    title.setWeight(QFont::DemiBold);
    p.setFont(title);
    p.setPen(QColor(Theme::TextPrimary));
    constexpr int Chevron = 10;
    const QRect titleBox(Side, 0, w - 2 * Side - (m_isGuild ? Chevron + 8 : 0), TitleRow);
    p.drawText(titleBox, Qt::AlignLeft | Qt::AlignVCenter,
               p.fontMetrics().elidedText(m_title, Qt::ElideRight, titleBox.width()));

    if (m_isGuild) {
        const QPointF mid(w - Side - Chevron / 2.0, TitleRow / 2.0 + 1);
        QPainterPath chevron;
        chevron.moveTo(mid.x() - Chevron / 2.0, mid.y() - 2.5);
        chevron.lineTo(mid.x(), mid.y() + 2.5);
        chevron.lineTo(mid.x() + Chevron / 2.0, mid.y() - 2.5);
        p.setPen(QPen(QColor(Theme::TextPrimary), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(chevron);
    }

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
