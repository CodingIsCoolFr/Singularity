#include "ui/BadgesDialog.h"

#include "core/Logger.h"
#include "core/RestClient.h"
#include "ui/FlowLayout.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QListWidget>
#include <QLocale>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <utility>

namespace {

constexpr int GridIconPixels = 56;
constexpr int BigIconPixels = 120;
constexpr int ProgressIconPixels = 44;
constexpr int TierIconPixels = 40;

constexpr int BadgeIdRole = Qt::UserRole;
constexpr int IconUrlRole = Qt::UserRole + 1;
constexpr int DimRole = Qt::UserRole + 2;

QJsonObject tierByKey(const QJsonObject &badge, const QString &key)
{
    if (key.isEmpty())
        return {};
    for (const QJsonValue &value : badge.value(QStringLiteral("tiers")).toArray()) {
        if (value.toObject().value(QStringLiteral("key")).toString() == key)
            return value.toObject();
    }
    return {};
}

// The tier a badge is shown at: the one you have, or for a badge you do not
// have yet, the one you would get first. Discord's own rule.
QJsonObject shownTier(const QJsonObject &badge)
{
    const QJsonArray tiers = badge.value(QStringLiteral("tiers")).toArray();
    if (tiers.isEmpty())
        return {};
    const bool owned = badge.value(QStringLiteral("owned")).toBool();
    const QJsonObject tier = tierByKey(badge, badge.value(owned ? QStringLiteral("current_tier")
                                                                 : QStringLiteral("next_tier")).toString());
    return tier.isEmpty() ? tiers.first().toObject() : tier;
}

// The fuller picture where Discord has one, the flat one otherwise; the
// tier's own before the badge's.
QUrl iconUrl(const QJsonObject &badge, const QJsonObject &tier)
{
    static const char *const keys[] = {"complex_icon_static_url", "simple_icon_raster_url", "simple_icon_url"};
    for (const QJsonObject &source : {tier, badge}) {
        for (const char *key : keys) {
            const QString url = source.value(QLatin1String(key)).toString();
            if (!url.isEmpty())
                return QUrl(url);
        }
    }
    const QString hash = badge.value(QStringLiteral("icon")).toString();
    return hash.isEmpty() ? QUrl() : BadgesDialog::profileBadgeIconUrl(hash);
}

QString rarityName(int rarity)
{
    switch (rarity) {
    case 2: return QStringLiteral("Rare");
    case 3: return QStringLiteral("Epic");
    case 5: return QStringLiteral("Mythic");
    default: return {};
    }
}

// Discord sends some times as ISO text and some as milliseconds.
QDateTime readTime(const QJsonValue &value)
{
    if (value.isDouble())
        return QDateTime::fromMSecsSinceEpoch(qint64(value.toDouble()));
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}

QDateTime obtainedAt(const QJsonObject &badge)
{
    const QString current = badge.value(QStringLiteral("current_tier")).toString();
    if (!current.isEmpty()) {
        const QDateTime tierTime =
            readTime(badge.value(QStringLiteral("tier_obtained_at")).toObject().value(current));
        if (tierTime.isValid())
            return tierTime;
    }
    return readTime(badge.value(QStringLiteral("obtained_at")));
}

bool isNewTiered(int badgeId)
{
    return badgeId == BadgesDialog::AccountAge || badgeId == BadgesDialog::Streaming
           || badgeId == BadgesDialog::GameTime || badgeId == BadgesDialog::GameVariety;
}

QPixmap fitted(const QImage &image, int size, bool dim)
{
    QPixmap out(size, size);
    out.fill(Qt::transparent);
    if (image.isNull())
        return out;
    const QImage scaled = image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter painter(&out);
    painter.setOpacity(dim ? 0.35 : 1.0);
    painter.drawImage((size - scaled.width()) / 2, (size - scaled.height()) / 2, scaled);
    return out;
}

QLabel *styledLabel(const QString &style, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setWordWrap(true);
    label->setStyleSheet(style);
    return label;
}

} // namespace

// ---------------------------------------------------------------------------
// Profile badges
// ---------------------------------------------------------------------------

int BadgesDialog::badgeIdFromProfile(const QString &profileBadgeId)
{
    // Discord's own table, from its web client.
    static const QHash<QString, int> known = {
        {QStringLiteral("staff"), Staff},
        {QStringLiteral("partner"), 2},
        {QStringLiteral("certified_moderator"), 3},
        {QStringLiteral("hypesquad"), 4},
        {QStringLiteral("hypesquad_house_1"), 5},
        {QStringLiteral("hypesquad_house_2"), 6},
        {QStringLiteral("hypesquad_house_3"), 7},
        {QStringLiteral("bug_hunter_level_1"), 8},
        {QStringLiteral("bug_hunter_level_2"), 9},
        {QStringLiteral("verified_developer"), 10},
        {QStringLiteral("early_supporter"), 11},
        {QStringLiteral("legacy_username"), 13},
        {QStringLiteral("quest_completed"), 14},
        {QStringLiteral("april_fools_2026"), AprilFools2026},
        {QStringLiteral("orb_profile_badge"), 16},
        {QStringLiteral("gifting"), 17},
        {QStringLiteral("account_age"), AccountAge},
        {QStringLiteral("streaming"), Streaming},
        {QStringLiteral("game_time"), GameTime},
        {QStringLiteral("game_variety"), GameVariety},
    };
    const QString id = profileBadgeId.toLower();
    if (known.contains(id))
        return known.value(id);
    // Every Nitro length and every boost level is one badge each.
    if (id.startsWith(QLatin1String("staff")))
        return Staff;
    if (id == QLatin1String("premium") || id.startsWith(QLatin1String("premium_tenure")))
        return PremiumTenure;
    if (id.startsWith(QLatin1String("guild_booster")))
        return 12;
    // The new ones are named after their kind, sometimes with the tier on the end.
    for (auto it = known.constBegin(); it != known.constEnd(); ++it) {
        if (id.startsWith(it.key() + QLatin1Char('_')))
            return it.value();
    }
    return 0;
}

QUrl BadgesDialog::profileBadgeIconUrl(const QString &iconHash)
{
    return QUrl(QStringLiteral("https://cdn.discordapp.com/badge-icons/%1.png").arg(iconHash));
}

QUrl BadgesDialog::directoryIconUrl(const QJsonObject &badge)
{
    return iconUrl(badge, shownTier(badge));
}

QString BadgesDialog::directoryName(const QJsonObject &badge)
{
    QString text = badge.value(QStringLiteral("name")).toString();
    const QString tierName = shownTier(badge).value(QStringLiteral("name")).toString();
    if (!tierName.isEmpty())
        text += QStringLiteral(" - ") + tierName;
    return text;
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

void BadgesDialog::open(RestClient *rest, const QString &userId, const QString &name, bool isSelf,
                        int initialBadgeId, QWidget *parent)
{
    auto *dialog = new BadgesDialog(rest, userId, name, isSelf, initialBadgeId, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

BadgesDialog::BadgesDialog(RestClient *rest, const QString &userId, const QString &name, bool isSelf,
                           int initialBadgeId, QWidget *parent)
    : QDialog(parent)
    , m_rest(rest)
    , m_userId(userId)
    , m_name(name)
    , m_isSelf(isSelf)
    , m_selected(initialBadgeId)
{
    setWindowTitle(isSelf            ? QStringLiteral("Your badges")
                   : name.isEmpty() ? QStringLiteral("Badges")
                                    : QStringLiteral("%1's badges").arg(name));
    resize(900, 620);

    const QString heading = QStringLiteral("color: %1; font-size: 15px; font-weight: 600;")
                                .arg(QLatin1String(Theme::TextPrimary));

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(14);

    // --- left: the badges ---------------------------------------------------
    auto *left = new QVBoxLayout;
    left->setSpacing(8);

    m_status = styledLabel(QStringLiteral("color: %1; font-size: 11px;").arg(QLatin1String(Theme::TextFaint)), this);
    m_status->setText(QStringLiteral("Loading badges..."));
    left->addWidget(m_status);

    m_ownedTitle = new QLabel(isSelf ? QStringLiteral("Your badges") : windowTitle(), this);
    m_ownedTitle->setStyleSheet(heading);
    left->addWidget(m_ownedTitle);
    m_ownedGrid = makeGrid();
    left->addWidget(m_ownedGrid, 3);

    m_earnableTitle = new QLabel(QStringLiteral("Available to unlock"), this);
    m_earnableTitle->setStyleSheet(heading);
    left->addWidget(m_earnableTitle);
    m_earnableGrid = makeGrid();
    left->addWidget(m_earnableGrid, 1);

    m_customize = new QPushButton(QStringLiteral("Customize your badges"), this);
    m_customize->setMinimumHeight(36);
    m_customize->setVisible(isSelf);
    m_customize->setEnabled(false);
    connect(m_customize, &QPushButton::clicked, this, &BadgesDialog::openCustomize);
    left->addWidget(m_customize);

    auto *leftHost = new QWidget(this);
    leftHost->setLayout(left);
    leftHost->setFixedWidth(340);
    root->addWidget(leftHost);

    // --- right: the one picked ---------------------------------------------
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("BadgeCard"));
    card->setStyleSheet(QStringLiteral("QFrame#BadgeCard { background-color: %1; border: 1px solid %2; "
                                       "border-radius: 12px; }")
                            .arg(QLatin1String(Theme::SurfaceRail), QLatin1String(Theme::Border)));
    m_detail = card;
    auto *detail = new QVBoxLayout(card);
    detail->setContentsMargins(22, 18, 22, 22);
    detail->setSpacing(6);

    m_beta = new QLabel(QStringLiteral("BETA"), card);
    m_beta->setStyleSheet(QStringLiteral("color: %1; background-color: %2; border-radius: 8px; padding: 2px 9px; "
                                         "font-size: 11px; font-weight: 700;")
                              .arg(QLatin1String(Theme::TextPrimary), QLatin1String(Theme::SurfaceHover)));
    detail->addWidget(m_beta, 0, Qt::AlignLeft);

    m_bigIcon = new QLabel(card);
    m_bigIcon->setFixedSize(BigIconPixels, BigIconPixels);
    m_bigIcon->setStyleSheet(QStringLiteral("background: transparent;"));
    detail->addWidget(m_bigIcon, 0, Qt::AlignHCenter);

    m_eyebrow = styledLabel(QStringLiteral("color: %1; font-size: 15px; background: transparent;")
                                .arg(QLatin1String(Theme::TextMuted)),
                            card);
    m_eyebrow->setAlignment(Qt::AlignHCenter);
    detail->addWidget(m_eyebrow);

    m_title = styledLabel(QStringLiteral("color: %1; font-size: 26px; font-weight: 800; background: transparent;")
                              .arg(QLatin1String(Theme::TextPrimary)),
                          card);
    m_title->setAlignment(Qt::AlignHCenter);
    detail->addWidget(m_title);

    m_meta = styledLabel(QStringLiteral("color: %1; font-size: 13px; background: transparent;")
                             .arg(QLatin1String(Theme::TextMuted)),
                         card);
    m_meta->setAlignment(Qt::AlignHCenter);
    detail->addWidget(m_meta);

    m_description = styledLabel(QStringLiteral("color: %1; font-size: 13px; background: transparent;")
                                    .arg(QLatin1String(Theme::LightGray)),
                                card);
    m_description->setAlignment(Qt::AlignHCenter);
    detail->addWidget(m_description);

    // Your progress: this tier, a bar, the next tier.
    auto *progress = new QFrame(card);
    progress->setObjectName(QStringLiteral("BadgeProgress"));
    progress->setStyleSheet(QStringLiteral("QFrame#BadgeProgress { background-color: %1; border-radius: 12px; }")
                                .arg(QLatin1String(Theme::SurfaceInput)));
    m_progressCard = progress;
    auto *progressLayout = new QVBoxLayout(progress);
    progressLayout->setContentsMargins(16, 14, 16, 14);
    progressLayout->setSpacing(8);
    m_progressTitle = new QLabel(QStringLiteral("Your progress"), progress);
    m_progressTitle->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; background: transparent;")
                                       .arg(QLatin1String(Theme::TextPrimary)));
    progressLayout->addWidget(m_progressTitle);
    auto *progressRow = new QHBoxLayout;
    progressRow->setSpacing(12);
    m_progressFrom = new QLabel(progress);
    m_progressFrom->setFixedSize(ProgressIconPixels, ProgressIconPixels);
    progressRow->addWidget(m_progressFrom);
    auto *middle = new QVBoxLayout;
    middle->setSpacing(4);
    m_progressText = styledLabel(QStringLiteral("color: %1; font-size: 14px; background: transparent;")
                                     .arg(QLatin1String(Theme::TextPrimary)),
                                 progress);
    middle->addWidget(m_progressText);
    m_progressBar = new QProgressBar(progress);
    m_progressBar->setTextVisible(false);
    m_progressBar->setFixedHeight(8);
    m_progressBar->setStyleSheet(QStringLiteral("QProgressBar { background-color: %1; border: none; border-radius: 4px; }"
                                                "QProgressBar::chunk { background-color: %2; border-radius: 4px; }")
                                     .arg(QLatin1String(Theme::SurfaceHover), QLatin1String(Theme::Accent)));
    middle->addWidget(m_progressBar);
    m_progressNext = styledLabel(QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                                     .arg(QLatin1String(Theme::TextFaint)),
                                 progress);
    middle->addWidget(m_progressNext);
    progressRow->addLayout(middle, 1);
    m_progressTo = new QLabel(progress);
    m_progressTo->setFixedSize(ProgressIconPixels, ProgressIconPixels);
    progressRow->addWidget(m_progressTo);
    progressLayout->addLayout(progressRow);
    m_instructions = styledLabel(QStringLiteral("color: %1; font-size: 13px; background: transparent; "
                                                "padding-top: 6px;")
                                     .arg(QLatin1String(Theme::LightGray)),
                                 progress);
    progressLayout->addWidget(m_instructions);
    detail->addSpacing(8);
    detail->addWidget(progress);

    // Every tier, the ones not reached yet faded.
    m_tierHost = new QWidget(card);
    m_tierHost->setStyleSheet(QStringLiteral("background: transparent;"));
    m_tierRow = new FlowLayout(m_tierHost, 0, 10);
    m_tierRow->setContentsMargins(0, 10, 0, 0);
    detail->addWidget(m_tierHost);
    detail->addStretch(1);

    scroll->setWidget(card);
    root->addWidget(scroll, 1);
    m_detail->setVisible(false);

    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &) { refreshIcons(); });

    load();
}

QListWidget *BadgesDialog::makeGrid()
{
    auto *grid = new QListWidget(this);
    grid->setViewMode(QListView::IconMode);
    grid->setIconSize(QSize(GridIconPixels, GridIconPixels));
    grid->setGridSize(QSize(GridIconPixels + 22, GridIconPixels + 22));
    grid->setMovement(QListView::Static);
    grid->setResizeMode(QListView::Adjust);
    grid->setWrapping(true);
    grid->setUniformItemSizes(true);
    grid->setStyleSheet(QStringLiteral("QListWidget { background-color: %1; border: 1px solid %2; border-radius: 12px; "
                                       "padding: 6px; }"
                                       "QListWidget::item { border-radius: 10px; }"
                                       "QListWidget::item:selected { background-color: %3; }"
                                       "QListWidget::item:hover { background-color: %3; }")
                            .arg(QLatin1String(Theme::SurfaceRail), QLatin1String(Theme::Border),
                                 QLatin1String(Theme::SurfaceHover)));
    connect(grid, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        select(item->data(BadgeIdRole).toInt());
    });
    return grid;
}

void BadgesDialog::load(const QString &notice)
{
    m_status->setText(QStringLiteral("Loading badges..."));
    m_status->setVisible(true);
    const QString who = m_isSelf ? QStringLiteral("@me") : m_userId;
    QPointer<BadgesDialog> guard(this);
    m_rest->fetchUserBadges(
        who,
        [this, guard, notice](const QJsonObject &payload) {
            if (!guard)
                return;
            m_order.clear();
            QHash<int, QJsonObject> fresh;
            for (const QJsonValue &value : payload.value(QStringLiteral("badges")).toArray()) {
                const QJsonObject badge = value.toObject();
                const int id = badge.value(QStringLiteral("badge_id")).toInt();
                if (id == 0)
                    continue;
                // Progress fetched earlier stays until it is fetched again.
                QJsonObject merged = badge;
                const QJsonObject before = m_badges.value(id);
                if (!merged.contains(QStringLiteral("progress")) && before.contains(QStringLiteral("progress")))
                    merged.insert(QStringLiteral("progress"), before.value(QStringLiteral("progress")));
                fresh.insert(id, merged);
                m_order.append(id);
            }
            m_badges = fresh;
            m_loaded = true;
            wlog(QStringLiteral("badges"), QStringLiteral("badge directory for %1: %2 badges")
                                               .arg(m_isSelf ? QStringLiteral("you") : m_userId)
                                               .arg(m_order.size()));
            m_status->setText(notice);
            m_status->setVisible(!notice.isEmpty());
            m_customize->setEnabled(true);
            rebuildGrids();
        },
        [this, guard](const RestClient::Error &error) {
            if (!guard)
                return;
            wlog(QStringLiteral("badges"), QStringLiteral("badge directory refused: HTTP %1 %2 %3")
                                               .arg(error.httpStatus)
                                               .arg(error.message.left(160),
                                                    QString::fromUtf8(QJsonDocument(error.body).toJson(
                                                                          QJsonDocument::Compact))
                                                        .left(300)));
            loadFromProfile(error.httpStatus);
        });
}

void BadgesDialog::loadFromProfile(int directoryStatus)
{
    // The directory is still being rolled out, client by client. The badges
    // on the profile itself have been there for years, so they are shown
    // instead: every badge with its picture and Discord's words for it, just
    // no tiers, progress or customizing.
    QPointer<BadgesDialog> guard(this);
    m_rest->fetchUserProfile(
        m_userId, QString(),
        [this, guard, directoryStatus](const QJsonObject &payload) {
            if (!guard)
                return;
            m_order.clear();
            m_badges.clear();
            int spare = 1000;
            for (const QJsonValue &value : payload.value(QStringLiteral("badges")).toArray()) {
                const QJsonObject source = value.toObject();
                int id = badgeIdFromProfile(source.value(QStringLiteral("id")).toString());
                if (id == 0 || m_badges.contains(id))
                    id = spare++;
                QJsonObject badge{
                    {QStringLiteral("badge_id"), id},
                    {QStringLiteral("name"), source.value(QStringLiteral("description"))},
                    {QStringLiteral("icon"), source.value(QStringLiteral("icon"))},
                    {QStringLiteral("owned"), true},
                };
                m_badges.insert(id, badge);
                m_order.append(id);
            }
            m_loaded = false;   // nothing here to customize
            m_customize->setEnabled(false);
            m_status->setText(QStringLiteral("Discord's badge page did not open for this client (HTTP %1), so these "
                                             "are the badges on the profile, without tiers or progress.")
                                  .arg(directoryStatus));
            m_status->setVisible(true);
            wlog(QStringLiteral("badges"), QStringLiteral("showing %1 badges from the profile instead").arg(m_order.size()));
            rebuildGrids();
        },
        [this, guard, directoryStatus](const RestClient::Error &error) {
            if (!guard)
                return;
            m_status->setText(QStringLiteral("Discord did not send the badges (HTTP %1, then %2 for the profile).")
                                  .arg(directoryStatus)
                                  .arg(error.httpStatus));
            m_status->setVisible(true);
        });
}

void BadgesDialog::rebuildGrids()
{
    QList<int> owned;
    QList<int> earnable;
    for (int id : std::as_const(m_order)) {
        const QJsonObject badge = m_badges.value(id);
        if (badge.value(QStringLiteral("owned")).toBool()) {
            // Hidden ones are still yours, so they are listed, last.
            if (!badge.value(QStringLiteral("hidden")).toBool())
                owned.append(id);
        } else if (m_isSelf && badge.value(QStringLiteral("is_earnable")).toBool()) {
            earnable.append(id);
        }
    }
    for (int id : std::as_const(m_order)) {
        const QJsonObject badge = m_badges.value(id);
        if (badge.value(QStringLiteral("owned")).toBool() && badge.value(QStringLiteral("hidden")).toBool())
            owned.append(id);
    }

    fillGrid(m_ownedGrid, owned);
    fillGrid(m_earnableGrid, earnable);
    m_earnableTitle->setVisible(!earnable.isEmpty());
    m_earnableGrid->setVisible(!earnable.isEmpty());
    if (owned.isEmpty())
        m_ownedTitle->setText(m_isSelf ? QStringLiteral("You have no badges yet")
                                       : QStringLiteral("No badges to show"));
    else
        m_ownedTitle->setText(m_isSelf ? QStringLiteral("Your badges") : windowTitle());

    int pick = m_selected;
    if (!m_badges.contains(pick))
        pick = !owned.isEmpty() ? owned.first() : (!earnable.isEmpty() ? earnable.first() : 0);
    select(pick);
}

void BadgesDialog::fillGrid(QListWidget *grid, const QList<int> &badgeIds)
{
    grid->clear();
    for (int id : badgeIds) {
        const QJsonObject badge = m_badges.value(id);
        const bool owned = badge.value(QStringLiteral("owned")).toBool();
        const bool hidden = badge.value(QStringLiteral("hidden")).toBool();
        const QJsonObject tier = shownTier(badge);

        auto *item = new QListWidgetItem;
        item->setData(BadgeIdRole, id);
        const QUrl url = iconUrl(badge, tier);
        item->setData(IconUrlRole, url);
        item->setData(DimRole, !owned || hidden);
        QString tip = badge.value(QStringLiteral("name")).toString();
        const QString tierName = tier.value(QStringLiteral("name")).toString();
        if (owned && !tierName.isEmpty())
            tip += QStringLiteral(": ") + tierName;
        if (hidden)
            tip += QStringLiteral(" (hidden from your profile)");
        item->setToolTip(tip);
        item->setSizeHint(QSize(GridIconPixels + 18, GridIconPixels + 18));
        grid->addItem(item);
    }
    refreshIcons();
}

void BadgesDialog::select(int badgeId)
{
    m_selected = badgeId;
    for (QListWidget *grid : {m_ownedGrid, m_earnableGrid}) {
        grid->blockSignals(true);
        grid->clearSelection();
        for (int row = 0; row < grid->count(); ++row) {
            if (grid->item(row)->data(BadgeIdRole).toInt() == badgeId)
                grid->item(row)->setSelected(true);
        }
        grid->blockSignals(false);
    }
    showDetail();
    if (m_isSelf && m_loaded && badgeId != 0)
        fetchProgress(badgeId);
}

void BadgesDialog::fetchProgress(int badgeId)
{
    // Once per window; the directory itself is fetched again after a change.
    if (m_progressAsked.contains(badgeId))
        return;
    m_progressAsked.append(badgeId);
    QPointer<BadgesDialog> guard(this);
    m_rest->fetchUserBadge(
        QStringLiteral("@me"), badgeId,
        [this, guard, badgeId](const QJsonObject &payload) {
            if (!guard || payload.isEmpty())
                return;
            // Discord's own client keeps what the directory said and lays the
            // fuller answer over it.
            QJsonObject merged = m_badges.value(badgeId);
            for (auto it = payload.constBegin(); it != payload.constEnd(); ++it)
                merged.insert(it.key(), it.value());
            m_badges.insert(badgeId, merged);
            if (m_selected == badgeId)
                showDetail();
        },
        [badgeId](const RestClient::Error &error) {
            wlog(QStringLiteral("badges"), QStringLiteral("progress for badge %1 refused: HTTP %2 %3")
                                               .arg(badgeId)
                                               .arg(error.httpStatus)
                                               .arg(error.message.left(120)));
        });
}

void BadgesDialog::showDetail()
{
    const QJsonObject badge = m_badges.value(m_selected);
    m_detail->setVisible(!badge.isEmpty());
    if (badge.isEmpty())
        return;

    const bool owned = badge.value(QStringLiteral("owned")).toBool();
    const QString name = badge.value(QStringLiteral("name")).toString();
    const QJsonObject current = tierByKey(badge, badge.value(QStringLiteral("current_tier")).toString());
    const QJsonObject next = tierByKey(badge, badge.value(QStringLiteral("next_tier")).toString());
    const QJsonObject shown = shownTier(badge);

    m_beta->setVisible(isNewTiered(m_selected));
    setIcon(m_bigIcon, iconUrl(badge, shown), BigIconPixels, !owned);

    // "Account Age" over "SAPLING" once you have a tier; Nitro is one line,
    // "Nitro Gold"; anything else is just its name.
    const QString tierName = current.value(QStringLiteral("name")).toString();
    if (m_selected == PremiumTenure) {
        m_eyebrow->setVisible(false);
        m_title->setText(tierName.isEmpty() ? QStringLiteral("Nitro") : QStringLiteral("Nitro %1").arg(tierName));
    } else if (owned && !tierName.isEmpty()) {
        m_eyebrow->setText(name);
        m_eyebrow->setVisible(true);
        m_title->setText(tierName.toUpper());
    } else {
        m_eyebrow->setVisible(false);
        m_title->setText(name);
    }

    QStringList meta;
    const QString info = badge.value(QStringLiteral("info_label")).toString().trimmed();
    if (!info.isEmpty())
        meta << info;
    if (!owned) {
        meta << QStringLiteral("Not unlocked yet");
    } else if (m_selected == AprilFools2026) {
        meta << QStringLiteral("Played The Last Meadow Online");
    } else {
        const QDateTime when = obtainedAt(badge);
        // Written the way the profile card writes its dates.
        meta << (when.isValid() && m_selected != Staff
                     ? QStringLiteral("Unlocked %1").arg(when.toLocalTime().toString(QStringLiteral("MMM d, yyyy")))
                     : QStringLiteral("Unlocked"));
    }
    const int rarity = shown.value(QStringLiteral("rarity")).toInt(badge.value(QStringLiteral("rarity")).toInt());
    if (owned && !rarityName(rarity).isEmpty())
        meta << rarityName(rarity);
    if (owned && badge.value(QStringLiteral("hidden")).toBool())
        meta << QStringLiteral("Hidden from your profile");
    m_meta->setText(meta.join(QStringLiteral("  ·  ")));

    const QString description = badge.value(QStringLiteral("description")).toString().trimmed();
    m_description->setText(description);
    m_description->setVisible(!description.isEmpty());

    // Progress is only ever your own; Discord does not hand out anybody
    // else's. Shown while there is a next tier to reach.
    const QJsonObject progress = badge.value(QStringLiteral("progress")).toArray().at(0).toObject();
    const bool showProgress = m_isSelf && !next.isEmpty();
    m_progressCard->setVisible(showProgress || !badge.value(QStringLiteral("leveling_instructions")).toString().isEmpty());
    for (QWidget *piece : std::initializer_list<QWidget *>{m_progressTitle, m_progressFrom, m_progressTo,
                                                           m_progressBar, m_progressText, m_progressNext})
        piece->setVisible(showProgress);
    if (showProgress) {
        setIcon(m_progressFrom, iconUrl(badge, owned ? current : QJsonObject()), ProgressIconPixels, !owned);
        setIcon(m_progressTo, iconUrl(badge, next), ProgressIconPixels, false);

        double threshold = progress.value(QStringLiteral("threshold")).toDouble(-1);
        if (threshold < 0) {
            const QJsonArray requirements = next.value(QStringLiteral("requirements")).toArray();
            threshold = requirements.isEmpty() ? -1 : requirements.first().toObject().value(QStringLiteral("threshold")).toDouble(-1);
        }
        const double floor = progress.value(QStringLiteral("floor")).toDouble(0);
        const double now = progress.value(QStringLiteral("current")).toDouble(0);
        // A bar of whole thousandths, whatever unit the badge counts in.
        if (threshold > floor) {
            m_progressBar->setRange(0, 1000);
            m_progressBar->setValue(int(qBound(0.0, (now - floor) / (threshold - floor), 1.0) * 1000));
            m_progressBar->setVisible(true);
        } else {
            m_progressBar->setVisible(false);
        }

        QString helper = progress.value(QStringLiteral("progress_helper_text")).toString().trimmed();
        if (helper.isEmpty() && progress.isEmpty())
            helper = QStringLiteral("Loading progress...");
        else if (helper.isEmpty() && threshold > 0)
            helper = QStringLiteral("%1 of %2").arg(QLocale().toString(now, 'f', 0), QLocale().toString(threshold, 'f', 0));
        m_progressText->setText(helper);
        const QString nextName = next.value(QStringLiteral("name")).toString();
        m_progressNext->setText(nextName.isEmpty() ? QString() : QStringLiteral("Next: %1").arg(nextName));
        m_progressNext->setVisible(!nextName.isEmpty());
    }
    const QString instructions = badge.value(QStringLiteral("leveling_instructions")).toString().trimmed();
    m_instructions->setText(instructions);
    m_instructions->setVisible(!instructions.isEmpty());

    // The ladder: every tier, faded until it is reached.
    while (QLayoutItem *item = m_tierRow->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const QJsonArray tiers = badge.value(QStringLiteral("tiers")).toArray();
    m_tierHost->setVisible(tiers.size() > 1);
    if (tiers.size() > 1) {
        int reached = -1;
        for (int i = 0; i < tiers.size(); ++i) {
            if (owned && tiers.at(i).toObject().value(QStringLiteral("key")) == current.value(QStringLiteral("key")))
                reached = i;
        }
        for (int i = 0; i < tiers.size(); ++i) {
            const QJsonObject tier = tiers.at(i).toObject();
            auto *column = new QWidget(m_tierHost);
            auto *columnLayout = new QVBoxLayout(column);
            columnLayout->setContentsMargins(0, 0, 0, 0);
            columnLayout->setSpacing(3);
            auto *icon = new QLabel(column);
            icon->setFixedSize(TierIconPixels, TierIconPixels);
            setIcon(icon, iconUrl(badge, tier), TierIconPixels, i > reached);
            columnLayout->addWidget(icon, 0, Qt::AlignHCenter);
            auto *label = new QLabel(tier.value(QStringLiteral("name")).toString(), column);
            label->setAlignment(Qt::AlignHCenter);
            label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; %2")
                                     .arg(QLatin1String(i <= reached ? Theme::TextPrimary : Theme::TextFaint),
                                          i == reached ? QStringLiteral("font-weight: 700;") : QString()));
            columnLayout->addWidget(label);
            const QString milestone = tier.value(QStringLiteral("milestone_text")).toString();
            column->setToolTip(milestone.isEmpty() ? label->text() : QStringLiteral("%1: %2").arg(label->text(), milestone));
            column->setFixedWidth(84);
            m_tierRow->addWidget(column);
        }
    }
}

void BadgesDialog::setIcon(QLabel *label, const QUrl &url, int size, bool dim)
{
    for (qsizetype i = m_pendingIcons.size() - 1; i >= 0; --i) {
        if (!m_pendingIcons.at(i).label || m_pendingIcons.at(i).label == label)
            m_pendingIcons.removeAt(i);
    }
    if (url.isEmpty()) {
        label->clear();
        return;
    }
    const QImage image = MediaCache::instance().image(url);
    label->setPixmap(fitted(image, size, dim));
    if (image.isNull())
        m_pendingIcons.append({label, url, size, dim});
}

void BadgesDialog::refreshIcons()
{
    for (QListWidget *grid : {m_ownedGrid, m_earnableGrid}) {
        for (int row = 0; row < grid->count(); ++row) {
            QListWidgetItem *item = grid->item(row);
            const QUrl url = item->data(IconUrlRole).toUrl();
            if (url.isEmpty())
                continue;
            const QImage image = MediaCache::instance().image(url);
            if (!image.isNull())
                item->setIcon(QIcon(fitted(image, GridIconPixels, item->data(DimRole).toBool())));
        }
    }

    const QList<PendingIcon> pending = std::exchange(m_pendingIcons, {});
    for (const PendingIcon &icon : pending) {
        if (icon.label)
            setIcon(icon.label, icon.url, icon.size, icon.dim);
    }
}

// ---------------------------------------------------------------------------
// Customize your badges
// ---------------------------------------------------------------------------

void BadgesDialog::openCustomize()
{
    if (!m_loaded)
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Customize your badges"));
    dialog.resize(420, 560);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("Customize your badges"), &dialog);
    title->setStyleSheet(QStringLiteral("color: %1; font-size: 18px; font-weight: 700;")
                             .arg(QLatin1String(Theme::TextPrimary)));
    layout->addWidget(title);
    auto *hint = new QLabel(QStringLiteral("Drag to reorder. Untick a badge to hide it from your profile. "
                                           "Staff always comes first, and Discord does not let Nitro be hidden."),
                            &dialog);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));
    layout->addWidget(hint);

    auto *list = new QListWidget(&dialog);
    list->setDragDropMode(QAbstractItemView::InternalMove);
    list->setDefaultDropAction(Qt::MoveAction);
    list->setIconSize(QSize(32, 32));
    list->setSpacing(2);
    layout->addWidget(list, 1);

    // Shown ones in profile order, then the hidden ones, as Discord lists them.
    QList<int> shownIds;
    QList<int> hiddenIds;
    for (int id : std::as_const(m_order)) {
        const QJsonObject badge = m_badges.value(id);
        if (!badge.value(QStringLiteral("owned")).toBool() || id == Staff)
            continue;
        (badge.value(QStringLiteral("hidden")).toBool() ? hiddenIds : shownIds).append(id);
    }

    const auto addRow = [&](int id, bool shown) {
        const QJsonObject badge = m_badges.value(id);
        const QJsonObject tier = shownTier(badge);
        QString text = badge.value(QStringLiteral("name")).toString();
        const QString tierName = tier.value(QStringLiteral("name")).toString();
        if (!tierName.isEmpty())
            text += QStringLiteral(" - ") + tierName;
        auto *item = new QListWidgetItem(text, list);
        item->setData(BadgeIdRole, id);
        const QImage image = MediaCache::instance().image(iconUrl(badge, tier));
        if (!image.isNull())
            item->setIcon(QIcon(fitted(image, 32, false)));
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
        if (id != PremiumTenure)
            flags |= Qt::ItemIsUserCheckable;
        else
            item->setToolTip(QStringLiteral("Discord does not let the Nitro badge be hidden."));
        item->setFlags(flags);
        item->setCheckState(shown || id == PremiumTenure ? Qt::Checked : Qt::Unchecked);
    };
    for (int id : std::as_const(shownIds))
        addRow(id, true);
    for (int id : std::as_const(hiddenIds))
        addRow(id, false);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    QList<int> order;
    QList<int> hidden;
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem *item = list->item(row);
        const int id = item->data(BadgeIdRole).toInt();
        (item->checkState() == Qt::Checked ? order : hidden).append(id);
    }
    saveCustomization(order, hidden);
}

void BadgesDialog::saveCustomization(const QList<int> &order, const QList<int> &hidden)
{
    m_status->setText(QStringLiteral("Saving..."));
    m_status->setVisible(true);
    QPointer<BadgesDialog> guard(this);
    m_rest->updateBadgeSettings(
        order, hidden,
        [this, guard, order, hidden](const QJsonObject &) {
            wlog(QStringLiteral("badges"), QStringLiteral("badge settings saved: %1 shown, %2 hidden")
                                               .arg(order.size())
                                               .arg(hidden.size()));
            if (!guard)
                return;
            m_progressAsked.clear();
            load(QStringLiteral("Saved. Your profile shows your badges this way now, everywhere."));
        },
        [this, guard](const RestClient::Error &error) {
            wlog(QStringLiteral("badges"), QStringLiteral("badge settings refused: HTTP %1 %2")
                                               .arg(error.httpStatus)
                                               .arg(error.message.left(200)));
            if (!guard)
                return;
            m_status->setText(QStringLiteral("Discord did not save that (%1).")
                                  .arg(error.message.isEmpty() ? QString::number(error.httpStatus)
                                                               : error.message.left(160)));
            m_status->setVisible(true);
        });
}
