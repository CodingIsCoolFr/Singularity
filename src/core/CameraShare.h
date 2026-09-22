#pragma once

#include "core/VideoEncoder.h"

#include <QImage>
#include <QObject>
#include <QThread>

// Turns webcam frames into the same H.264 pieces a screen share sends.
//
// The camera itself stays on the window's thread, because that is where Qt
// will deliver frames. Only the encoding moves, for the same reason a screen
// share does: doing it on the window thread is a stutter you can see.
class CameraWorker : public QObject
{
    Q_OBJECT

public:
    explicit CameraWorker(QObject *parent = nullptr);

public slots:
    void begin(int width, int height, int fps);
    void end();
    void submit(const QImage &frame);
    void requestKeyframe();

signals:
    void picture(const QList<QByteArray> &units, bool keyframe);
    void started(int width, int height, const QString &encoder);
    void stopped();
    void failed(const QString &reason);

private:
    void tick();

    VideoEncoder m_encoder;
    QTimer *m_timer = nullptr;
    QImage m_frame;
    bool m_running = false;
    int m_failures = 0;
};

class CameraShare : public QObject
{
    Q_OBJECT

public:
    explicit CameraShare(QObject *parent = nullptr);
    ~CameraShare() override;

    void start(int width, int height, int fps);
    void stop();
    void submit(const QImage &frame);
    void requestKeyframe();
    bool isRunning() const { return m_running; }

signals:
    void picture(const QList<QByteArray> &units, bool keyframe);
    void started(int width, int height, const QString &encoder);
    void stopped();
    void failed(const QString &reason);

private:
    QThread m_thread;
    CameraWorker *m_worker = nullptr;
    bool m_running = false;
};
