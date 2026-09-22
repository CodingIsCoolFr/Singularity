#include "core/CameraShare.h"

#include "core/Logger.h"

#include <QTimer>

CameraWorker::CameraWorker(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &CameraWorker::tick);
}

void CameraWorker::begin(int width, int height, int fps)
{
    end();

    if (!m_encoder.open(width, height, fps, 1'500'000)) {
        emit failed(QStringLiteral("Could not start a camera encoder."));
        return;
    }

    m_running = true;
    m_failures = 0;
    m_timer->start(qMax(1, 1000 / qMax(1, fps)));
    emit started(m_encoder.width(), m_encoder.height(), m_encoder.name());
}

void CameraWorker::end()
{
    m_timer->stop();
    m_frame = QImage();
    if (!m_running)
        return;
    m_encoder.close();
    m_running = false;
    emit stopped();
}

void CameraWorker::submit(const QImage &frame)
{
    if (!frame.isNull())
        m_frame = frame;
}

void CameraWorker::requestKeyframe()
{
    m_encoder.requestKeyframe();
}

void CameraWorker::tick()
{
    if (!m_running || m_frame.isNull())
        return;

    const QImage frame = m_frame.convertToFormat(QImage::Format_ARGB32);
    QList<QByteArray> units;
    bool keyframe = false;
    if (!m_encoder.encode(frame.constBits(), frame.width(), frame.height(), frame.bytesPerLine(),
                          units, &keyframe)) {
        if (++m_failures >= 30) {
            end();
            emit failed(QStringLiteral("The camera encoder stopped working."));
        }
        return;
    }

    m_failures = 0;
    if (!units.isEmpty())
        emit picture(units, keyframe);
}

CameraShare::CameraShare(QObject *parent)
    : QObject(parent)
{
    m_worker = new CameraWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &CameraWorker::picture, this, &CameraShare::picture);
    connect(m_worker, &CameraWorker::started, this, &CameraShare::started);
    connect(m_worker, &CameraWorker::stopped, this, &CameraShare::stopped);
    connect(m_worker, &CameraWorker::failed, this, &CameraShare::failed);
    m_thread.start();
}

CameraShare::~CameraShare()
{
    stop();
    m_thread.quit();
    m_thread.wait(2000);
}

void CameraShare::start(int width, int height, int fps)
{
    m_running = true;
    QMetaObject::invokeMethod(m_worker, [this, width, height, fps]() {
        m_worker->begin(width, height, fps);
    });
}

void CameraShare::stop()
{
    m_running = false;
    QMetaObject::invokeMethod(m_worker, [this]() { m_worker->end(); });
}

void CameraShare::submit(const QImage &frame)
{
    QMetaObject::invokeMethod(m_worker, [this, frame]() { m_worker->submit(frame); });
}

void CameraShare::requestKeyframe()
{
    QMetaObject::invokeMethod(m_worker, [this]() { m_worker->requestKeyframe(); });
}
