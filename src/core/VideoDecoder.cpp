#include "core/VideoDecoder.h"

#include "core/Logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

VideoDecoder::~VideoDecoder()
{
    close();
}

bool VideoDecoder::open()
{
    if (m_context)
        return true;

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        wlog(QStringLiteral("video"), QStringLiteral("no H.264 decoder in this FFmpeg build"));
        return false;
    }

    m_context = avcodec_alloc_context3(codec);
    if (!m_context)
        return false;

    // A call is live, so being a few milliseconds late matters more than
    // being perfect. These let the decoder work on several frames at once and
    // carry on through damage rather than stopping at it.
    m_context->thread_count = 0;   // as many as the machine has
    m_context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    m_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_context->flags2 |= AV_CODEC_FLAG2_FAST | AV_CODEC_FLAG2_CHUNKS;
    m_context->err_recognition = 0;

    if (avcodec_open2(m_context, codec, nullptr) < 0) {
        wlog(QStringLiteral("video"), QStringLiteral("the H.264 decoder refused to start"));
        avcodec_free_context(&m_context);
        return false;
    }

    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();

    if (!m_frame || !m_packet) {
        close();
        return false;
    }

    wlog(QStringLiteral("video"), QStringLiteral("H.264 decoder ready"));
    return true;
}

void VideoDecoder::close()
{
    if (m_scaler) {
        sws_freeContext(m_scaler);
        m_scaler = nullptr;
    }
    if (m_packet)
        av_packet_free(&m_packet);
    if (m_frame)
        av_frame_free(&m_frame);
    if (m_context)
        avcodec_free_context(&m_context);

    m_scalerWidth = 0;
    m_scalerHeight = 0;
    m_hungry = 0;
}

bool VideoDecoder::decode(const QByteArray &annexB, QImage &out)
{
    if (!m_context || annexB.isEmpty())
        return false;

    // FFmpeg reads past the end of a packet while decoding, by design, so the
    // buffer it is given has to be padded. Copying into an owned buffer is the
    // simple way to guarantee that.
    if (av_new_packet(m_packet, annexB.size()) < 0)
        return false;
    std::memcpy(m_packet->data, annexB.constData(), static_cast<size_t>(annexB.size()));

    const int sent = avcodec_send_packet(m_context, m_packet);
    av_packet_unref(m_packet);

    if (sent < 0 && sent != AVERROR(EAGAIN)) {
        ++m_hungry;
        return false;
    }

    if (avcodec_receive_frame(m_context, m_frame) < 0) {
        // Normal at the start of a stream and after loss: the decoder is
        // waiting for a keyframe before it can draw anything at all.
        ++m_hungry;
        return false;
    }

    m_hungry = 0;
    const bool ok = toImage(out);
    av_frame_unref(m_frame);
    return ok;
}

bool VideoDecoder::toImage(QImage &out)
{
    const int width = m_frame->width;
    const int height = m_frame->height;
    if (width <= 0 || height <= 0)
        return false;

    // The scaler is built for one size and kept. Rebuilding it per frame is
    // expensive, and a stream rarely changes size mid-call.
    if (!m_scaler || m_scalerWidth != width || m_scalerHeight != height) {
        if (m_scaler)
            sws_freeContext(m_scaler);

        m_scaler = sws_getContext(width, height, static_cast<AVPixelFormat>(m_frame->format),
                                  width, height, AV_PIX_FMT_RGB32, SWS_BILINEAR, nullptr, nullptr,
                                  nullptr);
        m_scalerWidth = width;
        m_scalerHeight = height;

        if (!m_scaler)
            return false;

        wlog(QStringLiteral("video"), QStringLiteral("stream is %1 by %2").arg(width).arg(height));
    }

    out = QImage(width, height, QImage::Format_RGB32);
    uint8_t *planes[4] = {out.bits(), nullptr, nullptr, nullptr};
    int strides[4] = {static_cast<int>(out.bytesPerLine()), 0, 0, 0};

    sws_scale(m_scaler, m_frame->data, m_frame->linesize, 0, height, planes, strides);
    return true;
}
