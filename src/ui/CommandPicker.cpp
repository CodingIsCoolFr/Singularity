#include "ui/CommandPicker.h"

#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>

namespace {

constexpr int HeaderHeight = 26;
constexpr int NoteHeight = 32;
constexpr int RowHeight = 40;
constexpr int IconSize = 24;

class PickerDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const int kind = index.data(CommandPicker::RowKind).toInt();
        const int height = kind == 1 ? HeaderHeight : kind == 2 ? NoteHeight : RowHeight;
        return {option.rect.width(), height};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        const QRect rect = option.rect.adjusted(4, 1, -4, -1);
        const int kind = index.data(CommandPicker::RowKind).toInt();

        QFont font = option.font;
        font.setFamily(QStringLiteral("Segoe UI"));

        if (kind == 1) {
            font.setPixelSize(11);
            font.setWeight(QFont::DemiBold);
            font.setCapitalization(QFont::AllUppercase);
            painter->setFont(font);
            painter->setPen(QColor(QLatin1String(Theme::TextMuted)));
            painter->drawText(rect.adjusted(10, 6, -10, 0), Qt::AlignLeft | Qt::AlignVCenter,
                              index.data(CommandPicker::Title).toString());
            painter->restore();
            return;
        }

        if (kind == 2) {
            font.setPixelSize(13);
            font.setItalic(true);
            painter->setFont(font);
            painter->setPen(QColor(QLatin1String(Theme::TextMuted)));
            painter->drawText(rect.adjusted(12, 0, -12, 0), Qt::AlignLeft | Qt::AlignVCenter,
                              index.data(CommandPicker::Title).toString());
            painter->restore();
            return;
        }

        if (option.state & (QStyle::State_Selected | QStyle::State_MouseOver)) {
            QPainterPath path;
            path.addRoundedRect(rect, 6, 6);
            QColor fill{QLatin1String(Theme::SurfaceHover)};
            if (!(option.state & QStyle::State_Selected))
                fill.setAlphaF(fill.alphaF() * 0.6);
            painter->fillPath(path, fill);
        }

        int x = rect.left() + 10;
        const QUrl iconUrl = index.data(CommandPicker::IconUrl).toUrl();
        const QString iconName = index.data(CommandPicker::IconName).toString();
        if (!iconUrl.isEmpty() || !iconName.isEmpty()) {
            QPixmap icon;
            if (!iconUrl.isEmpty()) {
                const QImage image = MediaCache::instance().image(iconUrl);
                if (!image.isNull())
                    icon = MediaCache::circular(image, IconSize * 2);
            }
            if (icon.isNull())
                icon = MediaCache::initialsAvatar(iconName, IconSize * 2);
            const QRect iconRect(x, rect.center().y() - IconSize / 2, IconSize, IconSize);
            painter->drawPixmap(iconRect, icon);
            x += IconSize + 10;
        }

        const QString right = index.data(CommandPicker::Right).toString();
        int rightWidth = 0;
        if (!right.isEmpty()) {
            QFont small = font;
            small.setPixelSize(12);
            const QFontMetrics metrics(small);
            const int most = rect.width() * 35 / 100;
            const QString shownRight = metrics.elidedText(right, Qt::ElideRight, most);
            rightWidth = metrics.horizontalAdvance(shownRight) + 12;
            painter->setFont(small);
            painter->setPen(QColor(QLatin1String(Theme::TextFaint)));
            painter->drawText(QRect(rect.right() - rightWidth, rect.top(), rightWidth - 10, rect.height()),
                              Qt::AlignRight | Qt::AlignVCenter, shownRight);
        }

        const int textRight = rect.right() - rightWidth - 8;
        QFont titleFont = font;
        titleFont.setPixelSize(14);
        titleFont.setWeight(QFont::DemiBold);
        const QFontMetrics titleMetrics(titleFont);
        const QString title = titleMetrics.elidedText(index.data(CommandPicker::Title).toString(),
                                                      Qt::ElideRight, qMax(40, textRight - x));
        painter->setFont(titleFont);
        painter->setPen(QColor(QLatin1String(Theme::TextPrimary)));
        painter->drawText(QRect(x, rect.top(), textRight - x, rect.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          title);
        x += titleMetrics.horizontalAdvance(title) + 10;

        const QString detail = index.data(CommandPicker::Detail).toString();
        if (!detail.isEmpty() && x < textRight - 20) {
            QFont detailFont = font;
            detailFont.setPixelSize(13);
            const QFontMetrics metrics(detailFont);
            painter->setFont(detailFont);
            painter->setPen(QColor(QLatin1String(Theme::TextMuted)));
            painter->drawText(QRect(x, rect.top(), textRight - x, rect.height()), Qt::AlignLeft | Qt::AlignVCenter,
                              metrics.elidedText(detail, Qt::ElideRight, textRight - x));
        }
        painter->restore();
    }
};

} // namespace

CommandPicker::CommandPicker(QWidget *parent)
    : QListWidget(parent)
{
    setObjectName(QStringLiteral("CommandPicker"));
    setFocusPolicy(Qt::NoFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setMouseTracking(true);
    setItemDelegate(new PickerDelegate(this));
    setStyleSheet(QStringLiteral("QListWidget#CommandPicker { background-color: %1; border: 1px solid %2; "
                                 "border-radius: 10px; padding: 4px; outline: none; }"
                                 "QListWidget#CommandPicker::item { background: transparent; border: none; }")
                      .arg(QLatin1String(Theme::SurfaceSidebar), QLatin1String(Theme::Border)));

    connect(this, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (item && item->data(RowKind).toInt() == 0)
            emit picked(item->data(Key).toString());
    });
    // An app's picture arrives after the list is drawn.
    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &) {
        if (isVisible())
            viewport()->update();
    });
    hide();
}

void CommandPicker::clearRows()
{
    clear();
}

void CommandPicker::addHeader(const QString &text)
{
    auto *item = new QListWidgetItem(this);
    item->setData(RowKind, 1);
    item->setData(Title, text);
    item->setFlags(Qt::NoItemFlags);
}

void CommandPicker::addNote(const QString &text)
{
    auto *item = new QListWidgetItem(this);
    item->setData(RowKind, 2);
    item->setData(Title, text);
    item->setFlags(Qt::NoItemFlags);
}

void CommandPicker::addRow(const QString &title, const QString &detail, const QString &right, const QString &key,
                           const QUrl &icon, const QString &iconName)
{
    auto *item = new QListWidgetItem(this);
    item->setData(RowKind, 0);
    item->setData(Title, title);
    item->setData(Detail, detail);
    item->setData(Right, right);
    item->setData(Key, key);
    item->setData(IconUrl, icon);
    item->setData(IconName, iconName);
    item->setToolTip(detail);
}

void CommandPicker::step(int delta)
{
    if (!hasRows())
        return;
    int row = currentRow();
    for (int tries = 0; tries < count(); ++tries) {
        row += delta;
        if (row < 0 || row >= count())
            return;
        if (item(row)->data(RowKind).toInt() == 0) {
            setCurrentRow(row);
            scrollToItem(item(row));
            return;
        }
    }
}

void CommandPicker::selectKey(const QString &key)
{
    int first = -1;
    for (int i = 0; i < count(); ++i) {
        if (item(i)->data(RowKind).toInt() != 0)
            continue;
        if (first < 0)
            first = i;
        if (!key.isEmpty() && item(i)->data(Key).toString() == key) {
            setCurrentRow(i);
            return;
        }
    }
    if (first >= 0) {
        setCurrentRow(first);
        scrollToItem(item(first));
    }
}

QString CommandPicker::currentKey() const
{
    const QListWidgetItem *item = currentItem();
    if (!item || item->data(RowKind).toInt() != 0)
        return {};
    return item->data(Key).toString();
}

bool CommandPicker::hasRows() const
{
    for (int i = 0; i < count(); ++i) {
        if (item(i)->data(RowKind).toInt() == 0)
            return true;
    }
    return false;
}

void CommandPicker::place(QWidget *anchor)
{
    QWidget *window = parentWidget();
    if (!anchor || !window)
        return;
    int height = 10;
    for (int i = 0; i < count(); ++i) {
        const int kind = item(i)->data(RowKind).toInt();
        height += kind == 1 ? HeaderHeight : kind == 2 ? NoteHeight : RowHeight;
    }
    const QPoint topLeft = anchor->mapTo(window, QPoint(0, 0));
    height = qMin(height, qMax(120, qMin(380, topLeft.y() - 60)));
    const int width = qMax(320, anchor->width());
    setFixedSize(width, height);
    move(topLeft.x(), topLeft.y() - height - 6);
    raise();
    show();
}
