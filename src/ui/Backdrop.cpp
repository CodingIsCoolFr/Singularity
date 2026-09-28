#include "ui/Backdrop.h"

#include "core/Logger.h"
#include "ui/AuroraWidget.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QMovie>
#include <QPaintEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {

// The window's night colour, which the old shader dimmed towards and which
// shows through the see-through parts of a GIF.
const QColor Night(7, 9, 13);

// A frame with see-through parts would show the window behind it; they are
// painted onto the night colour instead, so a GIF stays one picture.
QImage flatten(const QImage &frame)
{
    if (frame.isNull())
        return {};
    if (!frame.hasAlphaChannel())
        return frame.convertToFormat(QImage::Format_RGB32);
    QImage flat(frame.size(), QImage::Format_RGB32);
    flat.fill(Night);
    QPainter painter(&flat);
    painter.drawImage(0, 0, frame);
    return flat;
}

} // namespace

Backdrop::Backdrop(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);
}

Backdrop::~Backdrop() = default;

void Backdrop::setBackground(const QString &picturePath, int dimPercent)
{
    m_dim = std::clamp(dimPercent, 0, 95);

    if (picturePath.isEmpty()) {
        delete m_movie;
        m_movie = nullptr;
        m_frame = QImage();
        m_shaded = QImage();
        m_picturePath.clear();

        if (!m_hole) {
            wlog(QStringLiteral("theme"), QStringLiteral("creating the black hole"));
            QElapsedTimer clock;
            clock.start();
            m_hole = new AuroraWidget(this);
            m_hole->setAttribute(Qt::WA_OpaquePaintEvent, true);
            m_hole->setAttribute(Qt::WA_NoSystemBackground, true);
            // Clicks belong to the panels on top. This widget only draws.
            m_hole->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            m_hole->setAutoFillBackground(false);
            m_hole->setGeometry(rect());
            m_hole->lower();
            if (m_haveColours)
                m_hole->setHoleColors(m_accent, m_disk, m_grade);
            m_hole->setRunning(m_running);
            m_hole->setBackgroundMode(AuroraWidget::Background::Hole);
            m_hole->show();
            wlog(QStringLiteral("theme"),
                 QStringLiteral("background: the black hole (drawn by the graphics card, %1 ms)")
                     .arg(clock.elapsed()));
        }
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        update();
        return;
    }

    if (m_hole) {
        // The window stays composed by the graphics card until it is next
        // opened: Qt does not switch a window back once an OpenGL widget has
        // been in it. Taking the widget down is still the slow step if the
        // driver has to be asked, so the time is logged before anything else
        // on this path can hide it.
        wlog(QStringLiteral("theme"), QStringLiteral("taking the black hole down"));
        QElapsedTimer clock;
        clock.start();
        delete m_hole;
        wlog(QStringLiteral("theme"),
             QStringLiteral("black hole is down (%1 ms); the full saving arrives after a restart")
                 .arg(clock.elapsed()));
    }
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    if (picturePath != m_picturePath) {
        m_picturePath = picturePath;
        wlog(QStringLiteral("theme"), QStringLiteral("loading the background picture"));
        QElapsedTimer clock;
        clock.start();
        loadPicture();
        wlog(QStringLiteral("theme"),
             QStringLiteral("background picture loaded (%1 ms)").arg(clock.elapsed()));
    }
    rebuildShading();
    update();
}

void Backdrop::loadPicture()
{
    delete m_movie;
    m_movie = nullptr;
    m_frame = QImage();
    m_shaded = QImage();

    // QMovie reads one frame at a time, so a long GIF costs no more memory
    // than a short one. A still picture is a movie with one frame.
    m_movie = new QMovie(m_picturePath, QByteArray(), this);
    if (!m_movie->isValid()) {
        wlog(QStringLiteral("theme"), QStringLiteral("could not read the background picture: %1").arg(m_picturePath));
        delete m_movie;
        m_movie = nullptr;
        return;
    }
    m_movie->setCacheMode(QMovie::CacheNone);

    connect(m_movie, &QMovie::frameChanged, this, [this](int) {
        if (!m_movie)
            return;
        m_frame = flatten(m_movie->currentImage());
        shadeFrame();
        update();
    });

    m_movie->jumpToFrame(0);
    m_frame = flatten(m_movie->currentImage());
    if (m_running && m_movie->frameCount() != 1 && isVisible())
        m_movie->start();

    wlog(QStringLiteral("theme"),
         QStringLiteral("background picture %1 (%2 frames, %3x%4), drawn without the graphics card")
             .arg(QFileInfo(m_picturePath).fileName())
             .arg(m_movie->frameCount())
             .arg(m_frame.width())
             .arg(m_frame.height()));
}

void Backdrop::setRunning(bool on)
{
    m_running = on;
    if (m_hole)
        m_hole->setRunning(on);

    // The "animated background" switch stops a GIF too, not only the hole.
    if (m_movie && m_movie->frameCount() != 1) {
        if (on && isVisible()) {
            if (m_movie->state() == QMovie::Paused)
                m_movie->setPaused(false);
            else if (m_movie->state() == QMovie::NotRunning)
                m_movie->start();
        } else if (!on && m_movie->state() == QMovie::Running) {
            m_movie->setPaused(true);
        }
    }
}

void Backdrop::setHoleColors(const QVector3D &accent, const QVector3D &disk, const QVector3D &grade)
{
    m_accent = accent;
    m_disk = disk;
    m_grade = grade;
    m_haveColours = true;
    if (m_hole)
        m_hole->setHoleColors(accent, disk, grade);
}

QRect Backdrop::coverRect() const
{
    // Covering, not stretching, like a desktop wallpaper: the picture keeps
    // its shape and the overflow is cropped evenly on both sides.
    if (m_frame.isNull() || width() <= 0 || height() <= 0)
        return rect();
    const double scale = std::max(double(width()) / m_frame.width(), double(height()) / m_frame.height());
    const QSize size(int(std::ceil(m_frame.width() * scale)), int(std::ceil(m_frame.height() * scale)));
    return QRect(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
}

void Backdrop::rebuildShading()
{
    // The old shader, step for step, per picture pixel: mix towards the night
    // colour by `dim`, then darken the far edges of the window to 90 %:
    //     out = colour * (1 - dim) * f  +  night * dim * f
    // where f comes from where that pixel lands in the window.
    if (m_frame.isNull() || width() <= 0 || height() <= 0)
        return;
    if (m_shadeWindow == size() && m_shadeFrame == m_frame.size() && m_shadeDim == m_dim) {
        if (m_shaded.isNull())
            shadeFrame();
        return;
    }

    const double dim = std::clamp(m_dim / 100.0, 0.0, 0.85);
    const QRect cover = coverRect();
    const int fw = m_frame.width();
    const int fh = m_frame.height();
    m_keep.resize(qsizetype(fw) * fh);
    m_toward.resize(qsizetype(fw) * fh);

    for (int y = 0; y < fh; ++y) {
        const double wy = cover.top() + (y + 0.5) * cover.height() / fh;
        const double qy = wy / height() - 0.5;
        for (int x = 0; x < fw; ++x) {
            const double wx = cover.left() + (x + 0.5) * cover.width() / fw;
            const double qx = (wx / width() - 0.5) * 1.1;
            // smoothstep(0.78, 0.35, length), as the shader wrote it
            const double t = std::clamp((std::sqrt(qx * qx + qy * qy) - 0.78) / (0.35 - 0.78), 0.0, 1.0);
            const double f = 0.90 + 0.10 * (t * t * (3.0 - 2.0 * t));
            m_keep[qsizetype(y) * fw + x] = float((1.0 - dim) * f);
            m_toward[qsizetype(y) * fw + x] = float(dim * f);
        }
    }
    m_shadeWindow = size();
    m_shadeFrame = m_frame.size();
    m_shadeDim = m_dim;
    shadeFrame();
}

void Backdrop::shadeFrame()
{
    if (m_frame.isNull())
        return;
    if (m_shadeFrame != m_frame.size() || m_keep.size() != qsizetype(m_frame.width()) * m_frame.height()) {
        rebuildShading();
        return;
    }

    const int fw = m_frame.width();
    const int fh = m_frame.height();
    QImage shaded(m_frame.size(), QImage::Format_RGB32);
    const float nr = float(Night.red()), ng = float(Night.green()), nb = float(Night.blue());
    for (int y = 0; y < fh; ++y) {
        const auto *in = reinterpret_cast<const QRgb *>(m_frame.constScanLine(y));
        auto *out = reinterpret_cast<QRgb *>(shaded.scanLine(y));
        const float *keep = m_keep.constData() + qsizetype(y) * fw;
        const float *toward = m_toward.constData() + qsizetype(y) * fw;
        for (int x = 0; x < fw; ++x) {
            const QRgb p = in[x];
            out[x] = qRgb(int(qRed(p) * keep[x] + nr * toward[x] + 0.5f),
                          int(qGreen(p) * keep[x] + ng * toward[x] + 0.5f),
                          int(qBlue(p) * keep[x] + nb * toward[x] + 0.5f));
        }
    }
    m_shaded = shaded;
}

void Backdrop::paintEvent(QPaintEvent *event)
{
    // The black hole draws itself underneath everything.
    if (m_hole)
        return;

    QPainter painter(this);
    painter.setClipRegion(event->region());
    if (m_shaded.isNull()) {
        painter.fillRect(rect(), Night);
        return;
    }
    // Only the part being repainted is stretched: a speaking ring changing
    // costs a ring's worth of picture, not a window's.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(coverRect(), m_shaded);
}

void Backdrop::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_hole)
        m_hole->setGeometry(rect());
    rebuildShading();
}

void Backdrop::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_movie && m_running && m_movie->frameCount() != 1) {
        if (m_movie->state() == QMovie::Paused)
            m_movie->setPaused(false);
        else if (m_movie->state() == QMovie::NotRunning)
            m_movie->start();
    }
}

void Backdrop::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // Nobody sees it, so nothing is decoded.
    if (m_movie && m_movie->state() == QMovie::Running)
        m_movie->setPaused(true);
}
