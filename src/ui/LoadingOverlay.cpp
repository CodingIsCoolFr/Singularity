#include "ui/LoadingOverlay.h"

#include "ui/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QtMath>

namespace {

// One turn of the disk, in milliseconds.
constexpr int SpinMs = 1400;

// The shortest this may be on screen. Something that appears and vanishes is
// more jarring than never appearing at all, and a warm connection can be ready
// in a few hundred milliseconds.
constexpr qint64 MinimumMs = 700;

} // namespace

LoadingOverlay::LoadingOverlay(QWidget *parent)
    : QWidget(parent)
    , m_step(QStringLiteral("Connecting..."))
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAutoFillBackground(false);

    m_age.start();

    m_spin.setInterval(1000 / 60);
    connect(&m_spin, &QTimer::timeout, this, [this]() {
        m_angle += (2.0 * M_PI) * (m_spin.interval() / qreal(SpinMs));
        if (m_angle > 2.0 * M_PI)
            m_angle -= 2.0 * M_PI;

        if (m_leaving) {
            m_fade -= 0.055;
            if (m_fade <= 0.0) {
                m_spin.stop();
                deleteLater();
                return;
            }
        }
        update();
    });
    m_spin.start();

    if (parent) {
        setGeometry(parent->rect());
        parent->installEventFilter(this);
    }
    raise();
}

bool LoadingOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parent() && event->type() == QEvent::Resize) {
        if (auto *w = qobject_cast<QWidget *>(watched))
            setGeometry(w->rect());
    }
    return QWidget::eventFilter(watched, event);
}

void LoadingOverlay::setStep(const QString &step)
{
    if (m_step == step)
        return;
    m_step = step;
    update();
}

void LoadingOverlay::finish()
{
    if (m_leaving)
        return;

    const qint64 shown = m_age.elapsed();
    if (shown < MinimumMs) {
        QTimer::singleShot(int(MinimumMs - shown), this, [this]() { finish(); });
        return;
    }

    m_leaving = true;
}

void LoadingOverlay::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update();
}

void LoadingOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setOpacity(m_fade);

    // Opaque, not translucent. The point is that the window being built
    // underneath is not seen at all until it is finished.
    painter.fillRect(rect(), QColor(QLatin1String(Theme::Dark)));

    const QPointF centre(width() / 2.0, height() / 2.0 - 26);
    const qreal radius = 26.0;

    // The same shape the program is named after, drawn simply: a dark centre,
    // a ring hard against it, and a bright spot running round the disk.
    const QColor silver(205, 214, 230);

    // The disk, seen nearly edge on.
    painter.save();
    painter.translate(centre);
    painter.scale(1.0, 0.3);

    for (int i = 0; i < 72; ++i) {
        const qreal a = (i / 72.0) * 2.0 * M_PI;
        const qreal toward = 0.5 + 0.5 * std::cos(a - m_angle);
        const qreal weight = 0.12 + 0.88 * std::pow(toward, 2.4);

        QColor c = silver;
        c.setAlphaF(qBound(0.0, weight * m_fade, 1.0));

        QPen pen(c);
        pen.setWidthF(7.0);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);

        const qreal r = radius * 1.9;
        const qreal nxt = ((i + 1) / 72.0) * 2.0 * M_PI;
        painter.drawLine(QPointF(std::cos(a) * r, std::sin(a) * r),
                         QPointF(std::cos(nxt) * r, std::sin(nxt) * r));
    }
    painter.restore();

    // The shadow, over the far half of the disk.
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QLatin1String(Theme::Dark)));
    painter.drawEllipse(centre, radius, radius);

    // The photon ring.
    QColor ring = silver;
    ring.setAlphaF(0.92 * m_fade);
    QPen ringPen(ring);
    ringPen.setWidthF(1.6);
    painter.setPen(ringPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(centre, radius, radius);

    // The name, and what is being waited for.
    QFont name = font();
    name.setPixelSize(30);
    name.setWeight(QFont::Light);
    name.setLetterSpacing(QFont::AbsoluteSpacing, -0.8);
    painter.setFont(name);
    painter.setPen(QColor(QLatin1String(Theme::TextPrimary)));
    painter.drawText(QRect(0, int(centre.y()) + 58, width(), 40), Qt::AlignHCenter | Qt::AlignVCenter,
                     QStringLiteral("Singularity"));

    QFont small = font();
    small.setPixelSize(12);
    painter.setFont(small);
    painter.setPen(QColor(QLatin1String(Theme::TextFaint)));
    painter.drawText(QRect(0, int(centre.y()) + 98, width(), 20), Qt::AlignHCenter | Qt::AlignVCenter,
                     m_step);
}
