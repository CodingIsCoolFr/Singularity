#pragma once

#include "core/ScreenCapture.h"
#include "core/VideoEncoder.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QSize>
#include <QList>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>

// Capture and encoding, together, on a thread of their own.
//
// These two belong on the same thread and not on any other. Putting capture
// on one thread and encoding on another would mean shipping a whole
// uncompressed picture between them - at 4K that is thirty three megabytes,
// sixty times a second - and the copy alone would cost more than the encoding.
// Here the encoder reads the captured pixels where they lie.
//
// It is also not the main thread, because encoding a frame takes long enough
// to be seen as a stutter in the window if it happened there, and the whole
// point of sharing a screen is that the thing on it keeps working.
//
// What crosses the thread boundary is only what has to: a handful of
// compressed pieces per picture, a few kilobytes.
class ScreenShareWorker : public QObject
{
    Q_OBJECT

public:
    explicit ScreenShareWorker(QObject *parent = nullptr);
    ~ScreenShareWorker() override;

public slots:
    void begin(const QString &monitorId, int width, int height, int fps, int bitrate);
    void end();

    // A viewer said it cannot draw anything yet. Everything it has been sent
    // so far describes changes from a picture it never saw.
    void requestKeyframe();

    // Your own share is on the big tile: send a bigger, faster preview.
    void setPreviewLarge(bool large);

signals:
    // One complete picture, already split into the pieces RTP wants.
    void picture(const QList<QByteArray> &units, bool keyframe);

    // A small copy of what is being sent, for your own tile.
    //
    // Your own pictures leave over the network and nothing comes back, so
    // without this the person sharing is the only one in the call who cannot
    // see the share - their own tile sits on "waiting for the screen" for as
    // long as it runs, which looks exactly like a share that is not working.
    //
    // Sent a couple of times a second and shrunk first. A thumbnail does not
    // need thirty frames, and the full sized picture is already busy being
    // encoded.
    void preview(const QImage &frame);

    void started(int width, int height, const QString &encoder, bool hardware);
    void stopped();
    void failed(const QString &reason);

private:
    void tick();

    ScreenCapture m_capture;
    VideoEncoder m_encoder;
    QTimer m_timer;
    QElapsedTimer m_since;

    // How long a still screen may go without anything being sent.
    //
    // Sending nothing while nothing moves is right, and is most of why this
    // is cheap. But a viewer who starts watching during a still stretch would
    // wait for the screen to change before seeing anything at all, so the
    // same unchanged picture is sent again as a keyframe now and then.
    qint64 m_lastSentMs = 0;
    qint64 m_lastPreviewMs = 0;

    int m_fps = 30;
    bool m_previewLarge = false;
    bool m_running = false;
    bool m_saidFirst = false;
    int m_encodeFailures = 0;
};

// Owns the thread and hands out a simple interface to the window.
class ScreenShare : public QObject
{
    Q_OBJECT

public:
    explicit ScreenShare(QObject *parent = nullptr);
    ~ScreenShare() override;

    static QList<ScreenCapture::Monitor> monitors() { return ScreenCapture::monitors(); }

    void start(const QString &monitorId, int width, int height, int fps, int bitrate);
    void stop();
    void requestKeyframe();
    void setPreviewLarge(bool large);
    bool isRunning() const { return m_running; }

signals:
    void picture(const QList<QByteArray> &units, bool keyframe);
    void preview(const QImage &frame);
    void started(int width, int height, const QString &encoder, bool hardware);
    void stopped();
    void failed(const QString &reason);

private:
    QThread m_thread;
    ScreenShareWorker *m_worker = nullptr;
    bool m_running = false;
};
