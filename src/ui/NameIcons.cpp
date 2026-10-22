#include "ui/NameIcons.h"

#include <QPainter>
#include <QtMath>

NameIcons &NameIcons::instance()
{
    static NameIcons icons;
    return icons;
}

QList<NameIcon> NameIcons::icons(const QString &userId, NamePlace place, int size, qreal ratio) const
{
    if (!m_provider || userId.isEmpty())
        return {};
    return m_provider(userId, place, size, ratio);
}

namespace {

QSizeF drawnSize(const NameIcon &icon)
{
    const qreal ratio = icon.image.devicePixelRatio() > 0 ? icon.image.devicePixelRatio() : 1.0;
    return QSizeF(icon.image.size()) / ratio;
}

} // namespace

int NameIcons::width(const QList<NameIcon> &icons, int gap)
{
    qreal total = 0;
    for (const NameIcon &icon : icons) {
        if (icon.image.isNull())
            continue;
        if (total > 0)
            total += gap;
        total += drawnSize(icon).width();
    }
    return qCeil(total);
}

QList<QRect> NameIcons::paint(QPainter *painter, int left, int middle, const QList<NameIcon> &icons, int gap)
{
    QList<QRect> placed;
    qreal x = left;
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (const NameIcon &icon : icons) {
        if (icon.image.isNull()) {
            placed.append(QRect());
            continue;
        }
        const QSizeF size = drawnSize(icon);
        const QRectF target(x, middle - size.height() / 2, size.width(), size.height());
        painter->drawImage(target, icon.image);
        placed.append(target.toAlignedRect());
        x += size.width() + gap;
    }
    painter->restore();
    return placed;
}
