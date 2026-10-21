#include "ui/BadgeCustomizer.h"

#include "core/Logger.h"
#include "core/RestClient.h"
#include "ui/BadgesDialog.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QEvent>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int TilePixels = 52;
constexpr int TileStep = 60;
constexpr int IconPixels = 32;

constexpr int BadgeIdRole = Qt::UserRole;
constexpr int KindRole = Qt::UserRole + 1;
enum Kind { Fixed = 0, Shown = 1, Hidden = 2 };

// The eye with a line through it that Discord puts over a hidden badge.
void drawHiddenMark(QPainter &painter, const QPointF &centre)
{
    QPen pen{QColor(Theme::TextPrimary)};
    pen.setWidthF(2.0);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const qreal half = 11.0;
    QPainterPath eye;
    eye.moveTo(centre.x() - half, centre.y());
    eye.quadTo(centre.x(), centre.y() - 13.0, centre.x() + half, centre.y());
    eye.quadTo(centre.x(), centre.y() + 13.0, centre.x() - half, centre.y());
    painter.drawPath(eye);
    painter.drawEllipse(centre, 3.2, 3.2);
    painter.drawLine(QPointF(centre.x() - 9.0, centre.y() - 9.0), QPointF(centre.x() + 9.0, centre.y() + 9.0));
}

} // namespace

BadgeCustomizer::BadgeCustomizer(RestClient *rest, QWidget *parent)
    : QFrame(parent)
    , m_rest(rest)
{
    setObjectName(QStringLiteral("BadgeCard"));
    setStyleSheet(QStringLiteral("#BadgeCard { background-color: %1; border: 1px solid %2; border-radius: 12px; }")
                      .arg(QLatin1String(Theme::SurfaceSidebar), QLatin1String(Theme::Border)));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(6);

    auto *title = new QLabel(QStringLiteral("Customize your badges"), this);
    title->setStyleSheet(QStringLiteral("color: %1; font-size: 15px; font-weight: 700;")
                             .arg(QLatin1String(Theme::TextPrimary)));
    layout->addWidget(title);

    auto *subtitle = new QLabel(QStringLiteral("Drag to reorder, and hide the ones you don't want others to see."),
                                this);
    subtitle->setWordWrap(true);
    subtitle->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(QLatin1String(Theme::TextMuted)));
    layout->addWidget(subtitle);

    m_status = new QLabel(QStringLiteral("Loading your badges..."), this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(QLatin1String(Theme::TextFaint)));
    layout->addWidget(m_status);

    m_grid = new QListWidget(this);
    m_grid->setViewMode(QListView::ListMode);
    m_grid->setFlow(QListView::LeftToRight);
    m_grid->setWrapping(true);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setGridSize(QSize(TileStep, TileStep));
    m_grid->setIconSize(QSize(TilePixels, TilePixels));
    m_grid->setUniformItemSizes(true);
    m_grid->setDragDropMode(QAbstractItemView::InternalMove);
    m_grid->setDefaultDropAction(Qt::MoveAction);
    m_grid->setDragDropOverwriteMode(false);
    m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_grid->setFocusPolicy(Qt::NoFocus);
    m_grid->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_grid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_grid->setFrameShape(QFrame::NoFrame);
    m_grid->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; }"
                                         "QListWidget::item { background: transparent; border: none; }"
                                         "QListWidget::item:selected { background: transparent; }"));
    m_grid->setCursor(Qt::PointingHandCursor);
    m_grid->installEventFilter(this);
    m_grid->hide();
    layout->addWidget(m_grid);

    auto *view = new QPushButton(QStringLiteral("View Badges"), this);
    view->setCursor(Qt::PointingHandCursor);
    connect(view, &QPushButton::clicked, this, &BadgeCustomizer::viewBadgesRequested);
    layout->addSpacing(4);
    layout->addWidget(view);

    // A click hides or shows. Later, not inside the click: the grid is
    // rebuilt, and the clicked tile with it.
    connect(m_grid, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const int id = item->data(BadgeIdRole).toInt();
        QTimer::singleShot(0, this, [this, id]() { toggleHidden(id); });
    });

    // A drag moved a tile: read the new order back.
    connect(m_grid->model(), &QAbstractItemModel::rowsMoved, this, [this]() {
        if (!m_rebuilding)
            QTimer::singleShot(0, this, &BadgeCustomizer::readOrderFromGrid);
    });

    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &url) {
        if (!m_iconUrls.contains(url.toString()))
            return;
        for (int row = 0; row < m_grid->count(); ++row) {
            QListWidgetItem *item = m_grid->item(row);
            item->setIcon(QIcon(tilePicture(item->data(BadgeIdRole).toInt(), item->data(KindRole).toInt() == Hidden)));
        }
        emit changed();
    });
}

void BadgeCustomizer::showEvent(QShowEvent *event)
{
    QFrame::showEvent(event);
    if (!m_loaded && !m_loading)
        load();
}

bool BadgeCustomizer::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_grid && event->type() == QEvent::Resize)
        fitGridHeight();
    return QFrame::eventFilter(watched, event);
}

void BadgeCustomizer::load()
{
    if (!m_rest)
        return;
    m_loading = true;
    const int request = ++m_loadRequest;
    QPointer<BadgeCustomizer> guard(this);
    m_rest->fetchUserBadges(
        QStringLiteral("@me"),
        [this, guard, request](const QJsonObject &payload) {
            if (!guard || request != m_loadRequest)
                return;
            m_loading = false;
            applyDirectory(payload);
        },
        [this, guard, request](const RestClient::Error &error) {
            if (!guard || request != m_loadRequest)
                return;
            m_loading = false;
            wlog(QStringLiteral("badges"), QStringLiteral("your badges for My Account: HTTP %1 %2")
                                               .arg(error.httpStatus)
                                               .arg(error.message.left(160)));
            m_status->setText(QStringLiteral("Discord did not send your badges (%1).")
                                  .arg(error.message.isEmpty() ? QString::number(error.httpStatus)
                                                               : error.message.left(120)));
            m_status->show();
        });
}

void BadgeCustomizer::applyDirectory(const QJsonObject &payload)
{
    m_badges.clear();
    m_fixed.clear();
    m_savedShown.clear();
    m_savedHidden.clear();
    QList<int> hiddenInOrder;

    // Discord lists them in your profile's order.
    for (const QJsonValue &value : payload.value(QStringLiteral("badges")).toArray()) {
        const QJsonObject badge = value.toObject();
        const int id = badge.value(QStringLiteral("badge_id")).toInt();
        if (id == 0 || !badge.value(QStringLiteral("owned")).toBool())
            continue;
        m_badges.insert(id, badge);
        if (id == BadgesDialog::Staff) {
            m_fixed.append(id);
        } else if (badge.value(QStringLiteral("hidden")).toBool()) {
            m_savedHidden.insert(id);
            hiddenInOrder.append(id);
        } else {
            m_savedShown.append(id);
        }
    }
    m_shown = m_savedShown;
    m_hidden = hiddenInOrder;
    m_loaded = true;

    if (m_badges.isEmpty()) {
        m_status->setText(QStringLiteral("You have no badges yet. View Badges shows the ones you can earn."));
        m_status->show();
    } else {
        m_status->hide();
    }
    rebuild();
    emit changed();
}

void BadgeCustomizer::reset()
{
    if (!m_loaded)
        return;
    m_shown = m_savedShown;
    QList<int> hidden;
    for (int id : std::as_const(m_hidden)) {
        if (m_savedHidden.contains(id))
            hidden.append(id);
    }
    for (int id : std::as_const(m_savedHidden)) {
        if (!hidden.contains(id))
            hidden.append(id);
    }
    m_hidden = hidden;
    rebuild();
    emit changed();
}

bool BadgeCustomizer::isDirty() const
{
    if (!m_loaded)
        return false;
    return m_shown != m_savedShown || QSet<int>(m_hidden.cbegin(), m_hidden.cend()) != m_savedHidden;
}

void BadgeCustomizer::save(std::function<void(bool ok, const QString &problem)> done)
{
    if (!isDirty() || !m_rest) {
        done(true, QString());
        return;
    }
    const QList<int> order = m_shown;
    const QList<int> hidden = m_hidden;
    QPointer<BadgeCustomizer> guard(this);
    m_rest->updateBadgeSettings(
        order, hidden,
        [this, guard, order, hidden, done](const QJsonObject &) {
            wlog(QStringLiteral("badges"), QStringLiteral("badge settings saved: %1 shown, %2 hidden")
                                               .arg(order.size())
                                               .arg(hidden.size()));
            if (guard) {
                m_savedShown = order;
                m_savedHidden = QSet<int>(hidden.cbegin(), hidden.cend());
            }
            done(true, QString());
        },
        [done](const RestClient::Error &error) {
            wlog(QStringLiteral("badges"), QStringLiteral("badge settings refused: HTTP %1 %2")
                                               .arg(error.httpStatus)
                                               .arg(error.message.left(200)));
            done(false, error.message.isEmpty() ? QStringLiteral("HTTP %1").arg(error.httpStatus)
                                                : error.message.left(160));
        });
}

QList<QUrl> BadgeCustomizer::shownIcons() const
{
    QList<QUrl> icons;
    for (const QList<int> &part : {m_fixed, m_shown}) {
        for (int id : part)
            icons.append(BadgesDialog::directoryIconUrl(m_badges.value(id)));
    }
    return icons;
}

QStringList BadgeCustomizer::shownNames() const
{
    QStringList names;
    for (const QList<int> &part : {m_fixed, m_shown}) {
        for (int id : part)
            names.append(BadgesDialog::directoryName(m_badges.value(id)));
    }
    return names;
}

void BadgeCustomizer::toggleHidden(int badgeId)
{
    if (!m_badges.contains(badgeId) || m_fixed.contains(badgeId))
        return;
    if (m_hidden.contains(badgeId)) {
        // Shown again, at the end of the shown ones, as in Discord.
        m_hidden.removeAll(badgeId);
        m_shown.append(badgeId);
    } else {
        if (badgeId == BadgesDialog::PremiumTenure) {
            m_status->setText(QStringLiteral("Discord does not let the Nitro badge be hidden."));
            m_status->show();
            QTimer::singleShot(4000, m_status, [label = m_status]() { label->hide(); });
            return;
        }
        m_shown.removeAll(badgeId);
        m_hidden.append(badgeId);
    }
    rebuild();
    emit changed();
}

void BadgeCustomizer::readOrderFromGrid()
{
    QList<int> shown;
    for (int row = 0; row < m_grid->count(); ++row) {
        const QListWidgetItem *item = m_grid->item(row);
        if (item->data(KindRole).toInt() == Shown)
            shown.append(item->data(BadgeIdRole).toInt());
    }
    // Wherever it was dropped - before Staff, among the hidden ones - it is
    // the order among the shown ones that counts, and the tiles go back into
    // their groups.
    const bool moved = shown != m_shown;
    m_shown = shown;
    rebuild();
    if (moved)
        emit changed();
}

void BadgeCustomizer::rebuild()
{
    m_rebuilding = true;
    m_grid->clear();
    m_iconUrls.clear();

    const auto add = [this](int id, Kind kind) {
        const QJsonObject badge = m_badges.value(id);
        auto *item = new QListWidgetItem(m_grid);
        item->setData(BadgeIdRole, id);
        item->setData(KindRole, int(kind));
        item->setSizeHint(QSize(TileStep, TileStep));

        const QUrl url = BadgesDialog::directoryIconUrl(badge);
        if (!url.isEmpty())
            m_iconUrls.insert(url.toString());
        item->setIcon(QIcon(tilePicture(id, kind == Hidden)));

        QString tip = BadgesDialog::directoryName(badge);
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (kind == Fixed) {
            tip += QStringLiteral("\nStaff always comes first.");
        } else if (kind == Hidden) {
            tip += QStringLiteral("\nHidden. Click to show it again.");
        } else {
            flags |= Qt::ItemIsDragEnabled;
            tip += id == BadgesDialog::PremiumTenure
                ? QStringLiteral("\nDrag to move it. Discord does not let Nitro be hidden.")
                : QStringLiteral("\nDrag to move it, click to hide it.");
        }
        item->setFlags(flags);
        item->setToolTip(tip);
    };
    for (int id : std::as_const(m_fixed))
        add(id, Fixed);
    for (int id : std::as_const(m_shown))
        add(id, Shown);
    for (int id : std::as_const(m_hidden))
        add(id, Hidden);

    m_rebuilding = false;
    m_grid->setVisible(m_grid->count() > 0);
    fitGridHeight();
}

void BadgeCustomizer::fitGridHeight()
{
    // Every tile on show, no scrolling: as many rows as the width needs.
    const int perRow = qMax(1, m_grid->viewport()->width() / TileStep);
    const int rows = qMax(1, (m_grid->count() + perRow - 1) / perRow);
    const int wanted = rows * TileStep + 4;
    if (m_grid->height() != wanted)
        m_grid->setFixedHeight(wanted);
}

QPixmap BadgeCustomizer::tilePicture(int badgeId, bool hidden) const
{
    const qreal ratio = qMax<qreal>(1.0, devicePixelRatioF());
    QPixmap out(QSize(TilePixels, TilePixels) * ratio);
    out.setDevicePixelRatio(ratio);
    out.fill(Qt::transparent);

    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF box(0.5, 0.5, TilePixels - 1.0, TilePixels - 1.0);
    painter.setPen(QColor(Theme::Border));
    painter.setBrush(QColor(Theme::SurfaceInput));
    painter.drawRoundedRect(box, 10, 10);

    const QUrl url = BadgesDialog::directoryIconUrl(m_badges.value(badgeId));
    const QImage icon = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
    if (!icon.isNull()) {
        QImage scaled = icon.scaled(QSize(IconPixels, IconPixels) * ratio, Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(ratio);
        const QSizeF shown = QSizeF(scaled.size()) / ratio;
        painter.setOpacity(hidden ? 0.3 : 1.0);
        painter.drawImage(QPointF((TilePixels - shown.width()) / 2.0, (TilePixels - shown.height()) / 2.0), scaled);
        painter.setOpacity(1.0);
    }

    if (hidden)
        drawHiddenMark(painter, QPointF(TilePixels / 2.0, TilePixels / 2.0));
    return out;
}
