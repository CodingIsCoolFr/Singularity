#include "core/ScreenShare.h"

#include "core/Logger.h"

namespace {

// How long a still screen may go before the same picture is sent again.
constexpr qint64 StillResendMs = 1500;

// The encoder is allowed to fail a few times before the share is given up.
// A resolution change tears down the capture and rebuilds it, and a frame or
// two across that boundary can be the wrong size.
constexpr int MaxEncodeFailures = 30;

} // namespace

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

ScreenShareWorker::ScreenShareWorker(QObject *parent)
    : QObject(parent)
{
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &ScreenShareWorker::tick);
}

ScreenShareWorker::~ScreenShareWorker()
{
    end();
}

void ScreenShareWorker::begin(const QString &monitorId, int width, int height, int fps,
                              int bitrate)
{
    end();

    if (!m_capture.start(monitorId)) {
        emit failed(QStringLiteral("Windows would not let this screen be captured. "
                                   "Another program may already be sharing it."));
        return;
    }

    // Keep the shape of the screen. Sending a 16:9 monitor as 4:3 would
    // squash it, and letterboxing wastes the bitrate on black.
    const int sourceWidth = m_capture.width();
    const int sourceHeight = m_capture.height();
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        m_capture.stop();
        emit failed(QStringLiteral("That screen reported no size."));
        return;
    }

    int outWidth = width;
    int outHeight = int(qRound(double(width) * sourceHeight / sourceWidth));
    if (outHeight > height) {
        outHeight = height;
        outWidth = int(qRound(double(height) * sourceWidth / sourceHeight));
    }

    if (!m_encoder.open(outWidth, outHeight, fps, bitrate)) {
        m_capture.stop();
        emit failed(QStringLiteral("This machine has no H.264 encoder that would start."));
        return;
    }

    m_fps = qBound(5, fps, 60);
    m_running = true;
    m_encodeFailures = 0;
    m_lastSentMs = 0;
    m_since.start();

    emit started(m_encoder.width(), m_encoder.height(), m_encoder.name(), m_encoder.isHardware());

    m_timer.start(1000 / m_fps);
}

void ScreenShareWorker::end()
{
    if (!m_running && !m_capture.isRunning())
        return;

    m_timer.stop();
    m_encoder.close();
    m_capture.stop();

    const bool was = m_running;
    m_running = false;
    if (was)
        emit stopped();
}

void ScreenShareWorker::requestKeyframe()
{
    m_encoder.requestKeyframe();

    // And send one now rather than at the next change. Somebody is waiting on
    // it with a blank tile.
    m_lastSentMs = 0;
}

void ScreenShareWorker::tick()
{
    if (!m_running)
        return;

    const uchar *pixels = nullptr;
    int stride = 0;

    // Zero rather than a wait. This is already on a timer at the frame rate,
    // and blocking here would just move the waiting somewhere less visible.
    const ScreenCapture::Result result = m_capture.grab(0, &pixels, &stride);

    if (result == ScreenCapture::Result::Failed) {
        wlog(QStringLiteral("share"), QStringLiteral("capture failed, stopping the share"));
        end();
        emit failed(QStringLiteral("The screen capture stopped working."));
        return;
    }

    if (result == ScreenCapture::Result::Lost) {
        // Rebuilt, perhaps at a different size. Everything the viewers have
        // describes changes from pictures that no longer apply.
        m_encoder.requestKeyframe();
        return;
    }

    if (!pixels || stride <= 0)
        return;

    const qint64 now = m_since.elapsed();

    if (result == ScreenCapture::Result::NoChange) {
        // Nothing moved. Usually there is nothing to do, which is the whole
        // saving; but a viewer arriving during a still stretch needs
        // something, so the same picture goes again as a keyframe.
        if (now - m_lastSentMs < StillResendMs)
            return;
        m_encoder.requestKeyframe();
    }

    QList<QByteArray> units;
    bool keyframe = false;

    if (!m_encoder.encode(pixels, m_capture.width(), m_capture.height(), stride, units,
                          &keyframe)) {
        if (++m_encodeFailures >= MaxEncodeFailures) {
            wlog(QStringLiteral("share"), QStringLiteral("the encoder kept failing, stopping"));
            end();
            emit failed(QStringLiteral("The encoder stopped accepting pictures."));
        }
        return;
    }

    m_encodeFailures = 0;
    if (units.isEmpty())
        return;

    m_lastSentMs = now;
    emit picture(units, keyframe);
}

// ---------------------------------------------------------------------------
// The handle
// ---------------------------------------------------------------------------

ScreenShare::ScreenShare(QObject *parent)
    : QObject(parent)
{
    m_worker = new ScreenShareWorker;
    m_worker->moveToThread(&m_thread);

    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &ScreenShareWorker::picture, this, &ScreenShare::picture);
    connect(m_worker, &ScreenShareWorker::started, this,
            [this](int w, int h, const QString &encoder, bool hardware) {
                m_running = true;
                emit started(w, h, encoder, hardware);
            });
    connect(m_worker, &ScreenShareWorker::stopped, this, [this]() {
        m_running = false;
        emit stopped();
    });
    connect(m_worker, &ScreenShareWorker::failed, this, [this](const QString &reason) {
        m_running = false;
        emit failed(reason);
    });

    m_thread.setObjectName(QStringLiteral("screen-share"));
    m_thread.start();
}

ScreenShare::~ScreenShare()
{
    stop();
    m_thread.quit();
    m_thread.wait(3000);
}

void ScreenShare::start(const QString &monitorId, int width, int height, int fps, int bitrate)
{
    QMetaObject::invokeMethod(m_worker, "begin", Qt::QueuedConnection,
                              Q_ARG(QString, monitorId), Q_ARG(int, width), Q_ARG(int, height),
                              Q_ARG(int, fps), Q_ARG(int, bitrate));
}

void ScreenShare::stop()
{
    if (!m_worker)
        return;
    QMetaObject::invokeMethod(m_worker, "end", Qt::QueuedConnection);
}

void ScreenShare::requestKeyframe()
{
    if (!m_worker)
        return;
    QMetaObject::invokeMethod(m_worker, "requestKeyframe", Qt::QueuedConnection);
}
