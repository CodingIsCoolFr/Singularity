#include "core/ScreenShare.h"

#include "core/Logger.h"

namespace {

// How long a still screen may go before the same picture is sent again.
constexpr qint64 StillResendMs = 1500;

// The encoder is allowed to fail a few times before the share is given up.
// A resolution change tears down the capture and rebuilds it, and a frame or
// two across that boundary can be the wrong size.
constexpr int MaxEncodeFailures = 30;

// How often your own tile is refreshed. Twice a second is enough to tell you
// the right screen is going out, which is all it is for.
constexpr qint64 PreviewEveryMs = 500;

} // namespace

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

ScreenShareWorker::ScreenShareWorker(QObject *parent)
    : QObject(parent)
{
    // The timer has to belong to this object, and that is not a tidiness
    // point - it is the whole reason screen sharing never sent a single
    // packet.
    //
    // This object is built on the main thread and then moved to its own.
    // moveToThread carries an object's *children* with it, and a plain member
    // is not a child, so the timer stayed behind. Qt refuses to start a timer
    // from a thread it does not live on: it prints a warning and does
    // nothing. Every other sign was healthy - the encoder opened, the capture
    // opened, the connection was up - because the one thing that never
    // happened was the tick that does the work.
    //
    // Making it a child costs nothing and is safe: a member is destroyed
    // before the QObject base runs, so it removes itself from the child list
    // before anything tries to delete it a second time.
    m_timer.setParent(this);
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
        emit failed(ScreenCapture::isWindowId(monitorId)
                        ? QStringLiteral("Windows would not let that window be captured. "
                                         "It may have closed, or be minimised.")
                        : QStringLiteral("Windows would not let this screen be captured. "
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
    m_saidFirst = false;
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
        const bool window = m_capture.isWindow();
        end();
        emit failed(window ? QStringLiteral("The shared window was closed, so the share ended.")
                           : QStringLiteral("The screen capture stopped working."));
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

    // Your own tile, a couple of times a second.
    //
    // Taken from the same pixels the encoder just read, so it costs a shrink
    // and nothing else. Done after the encode rather than before it, because
    // the encode is the job and this is the courtesy.
    if (now - m_lastPreviewMs >= PreviewEveryMs) {
        m_lastPreviewMs = now;

        // Wraps the mapped pixels without copying; scaled() then makes the
        // copy that is safe to send across the thread boundary.
        const QImage whole(pixels, m_capture.width(), m_capture.height(), stride,
                           QImage::Format_ARGB32);
        emit preview(whole.scaled(QSize(480, 270), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    if (units.isEmpty())
        return;

    // Said once, the first time a picture actually leaves here.
    //
    // Everything up to this point could be healthy while nothing was being
    // produced at all, which is exactly what happened: the encoder and the
    // capture both reported themselves ready and the timer driving them had
    // never ticked. One line closes that gap.
    if (!m_saidFirst) {
        m_saidFirst = true;
        wlog(QStringLiteral("share"),
             QStringLiteral("first picture encoded: %1 pieces, %2")
                 .arg(units.size())
                 .arg(keyframe ? QStringLiteral("keyframe") : QStringLiteral("not a keyframe")));
    }

    m_lastSentMs = now;
    emit picture(units, keyframe);
}

// ---------------------------------------------------------------------------
// The handle
// ---------------------------------------------------------------------------

ScreenShare::ScreenShare(QObject *parent)
    : QObject(parent)
{
    // Pictures cross a thread boundary, and a queued signal carrying a type
    // Qt has not been told about is dropped with a warning rather than
    // delivered. Registering it costs nothing and removes one more way for
    // this to fail without saying so.
    qRegisterMetaType<QList<QByteArray>>("QList<QByteArray>");

    m_worker = new ScreenShareWorker;
    m_worker->moveToThread(&m_thread);

    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &ScreenShareWorker::picture, this, &ScreenShare::picture);
    connect(m_worker, &ScreenShareWorker::preview, this, &ScreenShare::preview);
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
