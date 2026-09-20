#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// Turns H.264 into pictures.
//
// One of these per person sending video, because a decoder carries the state
// of the stream it is reading: earlier frames that later ones are described as
// changes from. Feeding two people through one decoder produces garbage.
//
// Frames arrive as Annex B, which is the byte-stream form with 00 00 00 01
// between the parts. VideoStream below assembles that from the packets.
class VideoDecoder
{
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    bool open();
    void close();
    bool isOpen() const { return m_context != nullptr; }

    // Feeds one complete access unit. Returns true and fills `out` when a
    // picture came of it.
    //
    // Not every unit produces one: the decoder holds frames back while it
    // waits for the parts it needs, and the first picture of a stream cannot
    // appear until a keyframe has arrived.
    bool decode(const QByteArray &annexB, QImage &out);

    // How many units went in without a picture coming out, which is the useful
    // number when a stream stays black.
    int hungryFrames() const { return m_hungry; }

private:
    bool toImage(QImage &out);

    AVCodecContext *m_context = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_packet = nullptr;
    SwsContext *m_scaler = nullptr;

    int m_scalerWidth = 0;
    int m_scalerHeight = 0;
    int m_hungry = 0;
};

// Owns one H.264 decoder per sender and runs on its own thread.
//
// Decoding 1080p on the thread that reads UDP and mixes sound is what made
// a busy share hitch: the socket buffer overflowed, pictures arrived with
// holes, and voices waited on swscale. This object never touches those.
class VideoDecodeWorker : public QObject
{
    Q_OBJECT

public:
    explicit VideoDecodeWorker(QObject *parent = nullptr);
    ~VideoDecodeWorker() override;

    // Live: only the newest picture per sender is kept. Catching up a
    // backlog is how a share falls a second behind and never recovers.
    void submit(quint32 ssrc, const QString &userId, const QByteArray &annexB);
    void drop(quint32 ssrc);
    void reset();

signals:
    void frameReady(quint32 ssrc, const QString &userId, const QImage &image);
    void decodeFailed(quint32 ssrc, int hungry);

private:
    struct Job {
        QString userId;
        QByteArray annexB;
    };

    void pump();

    QHash<quint32, VideoDecoder *> m_decoders;
    QHash<quint32, Job> m_latest;
    bool m_busy = false;
};
