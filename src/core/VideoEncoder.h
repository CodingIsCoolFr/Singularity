#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// Turns pictures into H.264, which is the other half of VideoDecoder.
//
// Which encoder does the work is decided at runtime, not at build time. The
// FFmpeg we ship is the LGPL build, so it has no libx264 in it - that is GPL
// and linking it would impose the GPL on all of this. What it does have is
// every hardware encoder (NVIDIA, AMD, Intel, and Windows' own Media
// Foundation) and OpenH264, which is Cisco's and BSD licensed.
//
// Hardware is tried first and not because of frame rate. Encoding 1080p in
// software costs a core or more, on the same machine that is already running
// the game or the editor being shared, and a share that makes the thing you
// are sharing stutter is worse than no share. OpenH264 is the floor, so a
// machine with no usable hardware encoder still works.
//
// Everything here is shaped for a live call rather than a file:
//
//   No B-frames.     A B-frame is described partly by a picture that has not
//                    been sent yet, so the encoder must hold frames back and
//                    reorder them. That is free on disk and is latency in a
//                    conversation.
//   No look-ahead.   Same reason.
//   Keyframes on     A viewer joining halfway can draw nothing until a
//   request.         keyframe arrives, and asking is much cheaper than sending
//                    them constantly.
class VideoEncoder
{
public:
    VideoEncoder() = default;
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder &) = delete;
    VideoEncoder &operator=(const VideoEncoder &) = delete;

    // `bitrate` is in bits per second. Discord's own Go Live tops out around
    // 2.5 Mbit for a free account at 1080p30, and going far above it wastes
    // upstream on a stream the server will throttle anyway.
    bool open(int width, int height, int fps, int bitrate);
    void close();
    bool isOpen() const { return m_context != nullptr; }

    int width() const { return m_width; }
    int height() const { return m_height; }

    // Which encoder was actually chosen, for the log and the settings page.
    QString name() const { return m_name; }
    bool isHardware() const { return m_hardware; }

    // Feeds one picture, as BGRA - which is what both the desktop duplication
    // API and QImage::Format_ARGB32 hand over on a little endian machine.
    //
    // The source size is passed rather than worked out from the stride,
    // because a captured row is usually padded and dividing by four then gives
    // a width slightly too large. It also need not match the encoded size: a
    // 4K monitor shared at 1080p is the ordinary case, and the scaling happens
    // here.
    //
    // `out` receives whole NAL units with their start codes stripped, because
    // that is the form RTP wants. A single picture is usually several.
    bool encode(const uchar *bgra, int srcWidth, int srcHeight, int stride, QList<QByteArray> &out,
                bool *keyframe = nullptr);

    // Makes the next picture a keyframe. Sent when a viewer says it cannot
    // draw anything, or when somebody new starts watching.
    void requestKeyframe() { m_forceKeyframe = true; }

    // The parameter sets, kept aside from the stream.
    //
    // A viewer that arrives mid-stream needs these before any picture means
    // anything, and some encoders only emit them once, at the very start. So
    // they are remembered and re-sent in front of every keyframe.
    QByteArray parameterSets() const { return m_parameterSets; }

private:
    bool tryCodec(const char *name, int width, int height, int fps, int bitrate);
    bool convert(const uchar *bgra, int srcWidth, int srcHeight, int stride);
    void splitAnnexB(const uchar *data, int size, QList<QByteArray> &out);

    AVCodecContext *m_context = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_packet = nullptr;
    SwsContext *m_scaler = nullptr;

    QString m_name;
    QByteArray m_parameterSets;

    int m_width = 0;
    int m_height = 0;

    // What the scaler was last built for. A shared window changes size while
    // it is being shared, and the scaler has to be rebuilt when it does.
    int m_sourceWidth = 0;
    int m_sourceHeight = 0;
    int m_sourceStride = 0;
    bool m_hardware = false;
    bool m_forceKeyframe = false;
    qint64 m_frameIndex = 0;
};
