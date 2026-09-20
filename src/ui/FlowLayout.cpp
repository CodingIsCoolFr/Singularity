#include "ui/FlowLayout.h"

#include <QWidget>

FlowLayout::FlowLayout(QWidget *parent, int margin, int spacing)
    : QLayout(parent)
    , m_spacing(spacing)
{
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem *item = takeAt(0))
        delete item;
}

void FlowLayout::addItem(QLayoutItem *item)
{
    m_items.append(item);
}

int FlowLayout::count() const
{
    return static_cast<int>(m_items.size());
}

QLayoutItem *FlowLayout::itemAt(int index) const
{
    return m_items.value(index);
}

QLayoutItem *FlowLayout::takeAt(int index)
{
    if (index < 0 || index >= m_items.size())
        return nullptr;
    return m_items.takeAt(index);
}

Qt::Orientations FlowLayout::expandingDirections() const
{
    return {};
}

bool FlowLayout::hasHeightForWidth() const
{
    return true;
}

int FlowLayout::heightForWidth(int width) const
{
    return layoutRows(QRect(0, 0, width, 0), false);
}

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    layoutRows(rect, true);
}

QSize FlowLayout::sizeHint() const
{
    return minimumSize();
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (QLayoutItem *item : m_items)
        size = size.expandedTo(item->minimumSize());

    const QMargins margins = contentsMargins();
    return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
}

int FlowLayout::layoutRows(const QRect &rect, bool apply) const
{
    const QMargins margins = contentsMargins();
    const QRect area = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());

    int x = area.x();
    int y = area.y();
    int rowHeight = 0;

    for (QLayoutItem *item : m_items) {
        const QSize hint = item->sizeHint();

        // Wrap when this one would run past the right edge.
        if (rowHeight > 0 && x + hint.width() > area.right() + 1) {
            x = area.x();
            y += rowHeight + m_spacing;
            rowHeight = 0;
        }

        if (apply)
            item->setGeometry(QRect(QPoint(x, y), hint));

        x += hint.width() + m_spacing;
        rowHeight = qMax(rowHeight, hint.height());
    }

    return y + rowHeight - rect.y() + margins.bottom();
}
