#include "ui/ImageViewer.h"

#include "core/Logger.h"
#include "ui/AnimatedImage.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {
constexpr qreal MinScale = 0.05;
constexpr qreal MaxScale = 20.0;
constexpr qreal WheelStep = 1.15;
}

// ---------------------------------------------------------------------------
// The part that draws, zooms and pans
// ---------------------------------------------------------------------------

class ImageViewer::Canvas : public QWidget
{
public:
    explicit Canvas(QWidget *parent = nullptr)
        : QWidget(parent)
        , m_source(new AnimatedImage(this))
    {
        setMouseTracking(true);
        setCursor(Qt::OpenHandCursor);
        connect(m_source, &AnimatedImage::frameChanged, this, [this]() { update(); });
    }

    void setImage(const QImage &still, const QByteArray &animation)
    {
        m_still = still;
        if (animation.isEmpty())
            m_source->clear();
        else
            m_source->setData(animation);

        m_fitted = false;
        fitToWindow();
    }

    bool hasImage() const { return !currentFrame().isNull(); }
    QSize imageSize() const { return currentFrame().size(); }
    qreal scale() const { return m_scale; }

    void fitToWindow()
    {
        const QImage frame = currentFrame();
        if (frame.isNull() || frame.width() == 0 || frame.height() == 0)
            return;

        // Never blow a small picture up just to fill the window.
        const qreal byWidth = qreal(width() - 40) / frame.width();
        const qreal byHeight = qreal(height() - 40) / frame.height();
        m_scale = qBound(MinScale, qMin<qreal>(1.0, qMin(byWidth, byHeight)), MaxScale);

        m_offset = QPointF(0, 0);
        m_fitted = true;
        update();
        notifyZoom();
    }

    void setScaleAround(qreal newScale, const QPointF &anchor)
    {
        newScale = qBound(MinScale, newScale, MaxScale);
        if (qFuzzyCompare(newScale, m_scale))
            return;

        // Keep whatever sits under the pointer under the pointer.
        const QPointF centre(width() / 2.0, height() / 2.0);
        const QPointF before = (anchor - centre - m_offset) / m_scale;
        m_scale = newScale;
        m_offset = anchor - centre - before * m_scale;

        m_fitted = false;
        update();
        notifyZoom();
    }

    void toggleActualSize(const QPointF &anchor)
    {
        if (m_fitted || m_scale < 0.999)
            setScaleAround(1.0, anchor);
        else
            fitToWindow();
    }

    void zoomBy(qreal factor) { setScaleAround(m_scale * factor, QPointF(width() / 2.0, height() / 2.0)); }

    std::function<void()> onZoomChanged;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(0, 0, 0, 235));

        const QImage frame = currentFrame();
        if (frame.isNull()) {
            painter.setPen(QColor(Theme::TextMuted));
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Loading..."));
            return;
        }

        painter.setRenderHint(QPainter::SmoothPixmapTransform, m_scale < 4.0);

        const QSizeF drawn(frame.width() * m_scale, frame.height() * m_scale);
        const QPointF topLeft((width() - drawn.width()) / 2.0 + m_offset.x(),
                              (height() - drawn.height()) / 2.0 + m_offset.y());
        painter.drawImage(QRectF(topLeft, drawn), frame);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (m_fitted)
            fitToWindow();
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const int ticks = event->angleDelta().y();
        if (ticks == 0)
            return;
        setScaleAround(m_scale * (ticks > 0 ? WheelStep : 1.0 / WheelStep), event->position());
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        m_dragging = true;
        m_dragFrom = event->position();
        setCursor(Qt::ClosedHandCursor);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging)
            return;
        m_offset += event->position() - m_dragFrom;
        m_dragFrom = event->position();
        m_fitted = false;
        update();
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        toggleActualSize(event->position());
    }

private:
    QImage currentFrame() const
    {
        const QImage frame = m_source->currentFrame();
        return frame.isNull() ? m_still : frame;
    }

    void notifyZoom()
    {
        if (onZoomChanged)
            onZoomChanged();
    }

    AnimatedImage *m_source = nullptr;
    QImage m_still;
    qreal m_scale = 1.0;
    QPointF m_offset;
    QPointF m_dragFrom;
    bool m_dragging = false;
    bool m_fitted = true;
};

// ---------------------------------------------------------------------------
// The window round it
// ---------------------------------------------------------------------------

ImageViewer::ImageViewer(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Picture"));
    setModal(false);
    resize(1000, 760);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_canvas = new Canvas(this);
    layout->addWidget(m_canvas, 1);

    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("ViewerBar"));
    bar->setFixedHeight(46);

    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(14, 8, 14, 8);
    barLayout->setSpacing(8);

    m_nameLabel = new QLabel(bar);
    m_nameLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                   .arg(QLatin1String(Theme::TextFaint)));
    barLayout->addWidget(m_nameLabel, 1);

    auto *zoomOut = new QPushButton(QStringLiteral("−"), bar);
    zoomOut->setFixedSize(30, 28);
    zoomOut->setToolTip(QStringLiteral("Zoom out"));
    connect(zoomOut, &QPushButton::clicked, this, [this]() { m_canvas->zoomBy(1.0 / WheelStep); });
    barLayout->addWidget(zoomOut);

    m_zoomLabel = new QLabel(bar);
    m_zoomLabel->setFixedWidth(52);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_zoomLabel->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextPrimary)));
    barLayout->addWidget(m_zoomLabel);

    auto *zoomIn = new QPushButton(QStringLiteral("+"), bar);
    zoomIn->setFixedSize(30, 28);
    zoomIn->setToolTip(QStringLiteral("Zoom in"));
    connect(zoomIn, &QPushButton::clicked, this, [this]() { m_canvas->zoomBy(WheelStep); });
    barLayout->addWidget(zoomIn);

    auto *fit = new QPushButton(QStringLiteral("Fit"), bar);
    fit->setFixedHeight(28);
    connect(fit, &QPushButton::clicked, this, [this]() { m_canvas->fitToWindow(); });
    barLayout->addWidget(fit);

    auto *actual = new QPushButton(QStringLiteral("100%"), bar);
    actual->setFixedHeight(28);
    connect(actual, &QPushButton::clicked, this, [this]() {
        m_canvas->setScaleAround(1.0, QPointF(m_canvas->width() / 2.0, m_canvas->height() / 2.0));
    });
    barLayout->addWidget(actual);

    auto *save = new QPushButton(QStringLiteral("Save"), bar);
    save->setFixedHeight(28);
    connect(save, &QPushButton::clicked, this, &ImageViewer::saveAs);
    barLayout->addWidget(save);

    auto *open = new QPushButton(QStringLiteral("Open in browser"), bar);
    open->setFixedHeight(28);
    connect(open, &QPushButton::clicked, this, [this]() { QDesktopServices::openUrl(m_url); });
    barLayout->addWidget(open);

    layout->addWidget(bar);

    m_canvas->onZoomChanged = [this]() { updateZoomLabel(); };

    setStyleSheet(QStringLiteral("QDialog { background-color: #000000; } "
                                 "#ViewerBar { background-color: %1; border-top: 1px solid %2; }")
                      .arg(QLatin1String(Theme::SurfaceRail), QLatin1String(Theme::Border)));

    // A late download replaces the placeholder.
    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &url) {
        if (url == m_url)
            showImage(url);
    });
}

void ImageViewer::showImage(const QUrl &url)
{
    if (!MediaCache::isAllowedHost(url)) {
        wlog(QStringLiteral("viewer"), QStringLiteral("refusing %1: not a Discord host").arg(url.toString()));
        return;
    }

    m_url = url;

    const QString name = QFileInfo(url.path()).fileName();
    m_nameLabel->setText(name.isEmpty() ? url.toString() : name);

    // Asking starts the download if it is not already here.
    const QImage still = MediaCache::instance().image(url);
    m_canvas->setImage(still, MediaCache::instance().animationData(url));
    updateZoomLabel();

    if (!isVisible()) {
        if (parentWidget())
            move(parentWidget()->geometry().center() - rect().center());
        show();
    }
    raise();
    activateWindow();
}

void ImageViewer::updateZoomLabel()
{
    const int percent = qRound(m_canvas->scale() * 100.0);
    m_zoomLabel->setText(QStringLiteral("%1%").arg(percent));

    if (m_canvas->hasImage()) {
        const QSize size = m_canvas->imageSize();
        m_nameLabel->setToolTip(QStringLiteral("%1 by %2").arg(size.width()).arg(size.height()));
    }
}

void ImageViewer::saveAs()
{
    const QByteArray animation = MediaCache::instance().animationData(m_url);
    const QImage still = MediaCache::instance().image(m_url);
    if (animation.isEmpty() && still.isNull())
        return;

    QString suggested = QFileInfo(m_url.path()).fileName();
    if (suggested.isEmpty())
        suggested = QStringLiteral("picture.png");

    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save picture"), suggested);
    if (path.isEmpty())
        return;

    // An animation is written back byte for byte so it keeps moving. A still
    // picture is re-encoded from the decoded frame.
    if (!animation.isEmpty()) {
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(animation);
            file.commit();
        }
        return;
    }

    still.save(path);
}

void ImageViewer::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        close();
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        m_canvas->zoomBy(WheelStep);
        return;
    case Qt::Key_Minus:
        m_canvas->zoomBy(1.0 / WheelStep);
        return;
    case Qt::Key_0:
        m_canvas->fitToWindow();
        return;
    default:
        break;
    }
    QDialog::keyPressEvent(event);
}
