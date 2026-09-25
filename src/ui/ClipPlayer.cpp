#include "ui/ClipPlayer.h"

#include "core/DiscordIdentity.h"
#include "core/Logger.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

namespace {

// Discord's own gifv cards are a few megabytes. Anything much larger is a
// real video that nobody asked to autoplay.
constexpr qint64 MaxClipBytes = 40LL * 1024 * 1024;

// Thirty pictures a second at most; the chat does not redraw faster than that
// and a 60 fps clip would only double the work.
constexpr int TickMs = 33;

// A clip shorter than this is really a still picture; looping it would spin.
constexpr qint64 MinLoopMs = 80;

QNetworkAccessManager &network()
{
    static auto *manager = new QNetworkAccessManager;
    return *manager;
}

// One thread for every clip in the chat, below normal priority, so that
// decoding can never take time from the window or from a call.
QThread *clipThread()
{
    static QThread *thread = nullptr;
    if (!thread) {
        thread = new QThread;
        thread->setObjectName(QStringLiteral("chat-clips"));
        thread->start(QThread::LowPriority);
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, []() {
            thread->quit();
            thread->wait(2000);
        });
    }
    return thread;
}

} // namespace

// ---------------------------------------------------------------------------
// The decoder, on the clip thread
// ---------------------------------------------------------------------------

class ClipDecoder : public QObject
{
    Q_OBJECT

public:
    ClipDecoder()
    {
        // A child, so it moves to the clip thread with this object.
        m_timer.setParent(this);
        // One shot, aimed at the next picture's own time. A repeating 33 ms
        // timer is rounded by Windows to 47 ms, which played every clip at
        // about twenty pictures a second.
        m_timer.setSingleShot(true);
        m_timer.setTimerType(Qt::PreciseTimer);
        connect(&m_timer, &QTimer::timeout, this, &ClipDecoder::tick);
    }

    ~ClipDecoder() override
    {
        sws_freeContext(m_sws);
        av_frame_free(&m_frame);
        av_packet_free(&m_packet);
        avcodec_free_context(&m_codec);
        avformat_close_input(&m_format);
        if (m_io) {
            av_freep(&m_io->buffer);
            avio_context_free(&m_io);
        }
    }

    void start(const QByteArray &data, const QSize &box)
    {
        m_data = data;
        m_box = box;
        if (!open()) {
            emit failed(QStringLiteral("could not read the clip"));
            return;
        }
        m_clock.start();
        if (!m_paused)
            m_timer.start(0);
    }

    void setPaused(bool paused)
    {
        if (paused == m_paused)
            return;
        m_paused = paused;
        if (paused) {
            if (m_clock.isValid())
                m_baseMs += m_clock.elapsed();
            m_timer.stop();
        } else if (m_codec) {
            m_clock.restart();
            m_timer.start(0);
        }
    }

signals:
    void frame(const QImage &picture);
    void failed(const QString &reason);

private:
    static int readPacket(void *opaque, uint8_t *buffer, int size)
    {
        auto *self = static_cast<ClipDecoder *>(opaque);
        const qint64 left = self->m_data.size() - self->m_pos;
        if (left <= 0)
            return AVERROR_EOF;
        const int count = int(qMin<qint64>(left, size));
        std::memcpy(buffer, self->m_data.constData() + self->m_pos, size_t(count));
        self->m_pos += count;
        return count;
    }

    static int64_t seekPacket(void *opaque, int64_t offset, int whence)
    {
        auto *self = static_cast<ClipDecoder *>(opaque);
        const qint64 size = self->m_data.size();
        whence &= ~AVSEEK_FORCE;
        if (whence == AVSEEK_SIZE)
            return size;
        qint64 base = 0;
        if (whence == SEEK_CUR)
            base = self->m_pos;
        else if (whence == SEEK_END)
            base = size;
        const qint64 target = base + offset;
        if (target < 0 || target > size)
            return -1;
        self->m_pos = target;
        return target;
    }

    bool open()
    {
        constexpr int IoBufferSize = 32 * 1024;
        auto *buffer = static_cast<unsigned char *>(av_malloc(IoBufferSize));
        if (!buffer)
            return false;
        m_io = avio_alloc_context(buffer, IoBufferSize, 0, this, &ClipDecoder::readPacket, nullptr,
                                  &ClipDecoder::seekPacket);
        if (!m_io) {
            av_free(buffer);
            return false;
        }

        m_format = avformat_alloc_context();
        if (!m_format)
            return false;
        m_format->pb = m_io;
        m_format->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(&m_format, nullptr, nullptr, nullptr) < 0)
            return false;   // frees m_format and nulls it
        if (avformat_find_stream_info(m_format, nullptr) < 0)
            return false;

        const AVCodec *decoder = nullptr;
        m_stream = av_find_best_stream(m_format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (m_stream < 0 || !decoder)
            return false;

        AVStream *stream = m_format->streams[m_stream];
        m_codec = avcodec_alloc_context3(decoder);
        if (!m_codec || avcodec_parameters_to_context(m_codec, stream->codecpar) < 0)
            return false;

        // Two helpers, not one per processor core. FFmpeg's "automatic" is 32
        // threads on this machine, each holding its own frames in flight -
        // that was most of the 64 threads and 150 MB it replaced.
        m_codec->thread_count = 2;
        if (avcodec_open2(m_codec, decoder, nullptr) < 0)
            return false;

        m_msPerTick = av_q2d(stream->time_base) * 1000.0;
        m_startPts = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;

        m_frame = av_frame_alloc();
        m_packet = av_packet_alloc();
        if (!m_frame || !m_packet)
            return false;

        // The size it will be drawn at, never larger than the clip itself.
        const QSize source(m_codec->width, m_codec->height);
        if (source.isEmpty())
            return false;
        m_out = source.width() > m_box.width() || source.height() > m_box.height()
            ? source.scaled(m_box, Qt::KeepAspectRatio)
            : source;
        m_out = m_out.expandedTo(QSize(1, 1));
        return true;
    }

    qint64 clipNowMs() const { return m_baseMs + (m_clock.isValid() ? m_clock.elapsed() : 0); }

    // The next picture into m_frame. False at the end, or if the file is bad.
    bool decodeNext()
    {
        for (int guard = 0; guard < 2000; ++guard) {
            const int got = avcodec_receive_frame(m_codec, m_frame);
            if (got == 0) {
                qint64 pts = m_frame->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE)
                    pts = m_frame->pts;
                m_pendingMs = pts == AV_NOPTS_VALUE
                    ? m_pendingMs + TickMs
                    : qint64(double(pts - m_startPts) * m_msPerTick);
                m_havePending = true;
                return true;
            }
            if (got == AVERROR_EOF) {
                m_atEnd = true;
                return false;
            }
            if (got != AVERROR(EAGAIN)) {
                m_broken = true;
                return false;
            }
            if (m_draining) {
                m_atEnd = true;   // nothing left to give it
                return false;
            }

            const int read = av_read_frame(m_format, m_packet);
            if (read < 0) {
                avcodec_send_packet(m_codec, nullptr);
                m_draining = true;
                continue;
            }
            if (m_packet->stream_index == m_stream)
                avcodec_send_packet(m_codec, m_packet);   // a bad packet is skipped
            av_packet_unref(m_packet);
        }
        m_broken = true;
        return false;
    }

    void rewind()
    {
        const qint64 lengthMs = m_pendingMs;
        if (lengthMs < MinLoopMs
            || av_seek_frame(m_format, m_stream, m_startPts, AVSEEK_FLAG_BACKWARD) < 0) {
            // Too short to loop, or cannot go back: leave the last picture up.
            m_timer.stop();
            return;
        }
        avcodec_flush_buffers(m_codec);
        m_draining = false;
        m_atEnd = false;
        m_havePending = false;
        m_pendingMs = 0;
        m_lastShownMs = -TickMs;
        m_baseMs = 0;
        m_clock.restart();
        m_timer.start(0);
    }

    // Sleep until the waiting picture is due, and never less than a thirtieth
    // of a second after the last one shown.
    void scheduleNext()
    {
        const qint64 now = clipNowMs();
        const qint64 due = qMax(m_pendingMs, m_lastShownMs + TickMs);
        const qint64 wait = due - now;
        m_timer.start(int(wait < 1 ? 1 : (wait > 1000 ? 1000 : wait)));
    }

    void tick()
    {
        if (m_paused || !m_codec)
            return;

        const qint64 now = clipNowMs();

        if (!m_havePending && !decodeNext()) {
            if (m_atEnd)
                rewind();
            else if (m_broken) {
                m_timer.stop();
                emit failed(QStringLiteral("the clip stopped decoding"));
            }
            return;
        }

        // Behind: decode through the late pictures without drawing them.
        while (m_pendingMs + TickMs < now) {
            if (!decodeNext()) {
                if (m_atEnd)
                    rewind();
                return;
            }
        }

        if (m_pendingMs > now || now - m_lastShownMs < TickMs - 2) {
            scheduleNext();   // not due yet
            return;
        }

        m_sws = sws_getCachedContext(m_sws, m_frame->width, m_frame->height,
                                     static_cast<AVPixelFormat>(m_frame->format), m_out.width(),
                                     m_out.height(), AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
                                     nullptr, nullptr);
        if (m_sws) {
            QImage picture(m_out, QImage::Format_RGB32);
            uint8_t *planes[4] = {picture.bits(), nullptr, nullptr, nullptr};
            const int strides[4] = {int(picture.bytesPerLine()), 0, 0, 0};
            sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, m_frame->height, planes, strides);
            emit frame(picture);
        }
        m_havePending = false;
        m_lastShownMs = now;

        // Decode the next one now, so the wait is aimed at its real time.
        if (!decodeNext()) {
            if (m_atEnd)
                rewind();
            else if (m_broken) {
                emit failed(QStringLiteral("the clip stopped decoding"));
            }
            return;
        }
        scheduleNext();
    }

    QByteArray m_data;
    qint64 m_pos = 0;
    QSize m_box;
    QSize m_out;

    AVIOContext *m_io = nullptr;
    AVFormatContext *m_format = nullptr;
    AVCodecContext *m_codec = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_packet = nullptr;
    SwsContext *m_sws = nullptr;
    int m_stream = -1;
    double m_msPerTick = 0.0;
    qint64 m_startPts = 0;

    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_baseMs = 0;
    qint64 m_pendingMs = 0;
    qint64 m_lastShownMs = -TickMs;
    bool m_havePending = false;
    bool m_draining = false;
    bool m_atEnd = false;
    bool m_broken = false;
    bool m_paused = false;
};

// ---------------------------------------------------------------------------
// The handle, on the window thread
// ---------------------------------------------------------------------------

ClipPlayer::ClipPlayer(const QUrl &url, const QSize &box, QObject *parent)
    : QObject(parent)
    , m_url(url)
    , m_box(box)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, DiscordIdentity::userAgent());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = network().get(request);
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64) {
        if (received > MaxClipBytes && m_reply)
            m_reply->abort();
    });
    connect(m_reply, &QNetworkReply::finished, this, &ClipPlayer::onDownloaded);
}

ClipPlayer::~ClipPlayer()
{
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
    }
    if (m_decoder) {
        // Nothing it sends from now on may reach this object, which is going.
        disconnect(m_decoder, nullptr, this, nullptr);
        m_decoder->deleteLater();
    }
}

void ClipPlayer::setPaused(bool paused)
{
    if (paused == m_paused)
        return;
    m_paused = paused;
    if (ClipDecoder *decoder = m_decoder)
        QMetaObject::invokeMethod(decoder, [decoder, paused]() { decoder->setPaused(paused); },
                                  Qt::QueuedConnection);
}

void ClipPlayer::onDownloaded()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    if (!reply)
        return;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit failed(reply->errorString());
        return;
    }
    const QByteArray data = reply->readAll();
    if (data.isEmpty() || data.size() > MaxClipBytes) {
        emit failed(QStringLiteral("the clip was empty or too large"));
        return;
    }

    m_decoder = new ClipDecoder;
    m_decoder->moveToThread(clipThread());
    connect(m_decoder, &ClipDecoder::frame, this, &ClipPlayer::frameReady);
    connect(m_decoder, &ClipDecoder::failed, this, &ClipPlayer::failed);

    ClipDecoder *decoder = m_decoder;
    const QSize box = m_box;
    const bool paused = m_paused;
    QMetaObject::invokeMethod(decoder, [decoder, data, box, paused]() {
        decoder->setPaused(paused);
        decoder->start(data, box);
    }, Qt::QueuedConnection);
}

#include "ClipPlayer.moc"
