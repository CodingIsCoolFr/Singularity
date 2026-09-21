#include "core/VideoEncoder.h"

#include "core/Logger.h"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {

// Tried in order, best first.
//
// The hardware three come before the software one on purpose: see the note in
// the header. Media Foundation is Windows' own and sits between them, because
// it will usually reach whatever hardware is present even when the vendor's
// own encoder is not the one installed.
const char *const kCandidates[] = {
    "h264_nvenc",     // NVIDIA
    "h264_amf",       // AMD
    "h264_qsv",       // Intel
    "h264_mf",        // Windows Media Foundation, whatever is underneath
    "libopenh264",    // Cisco's software encoder, the floor
};

bool isHardwareName(const QString &name)
{
    return name != QLatin1String("libopenh264");
}

} // namespace

VideoEncoder::~VideoEncoder()
{
    close();
}

bool VideoEncoder::open(int width, int height, int fps, int bitrate)
{
    close();

    // H.264 counts chroma in pairs of pixels, so both sides must be even. A
    // window dragged to an odd width is ordinary and would otherwise fail
    // deep inside the encoder with a message about nothing recognisable.
    width &= ~1;
    height &= ~1;
    if (width < 16 || height < 16)
        return false;

    for (const char *candidate : kCandidates) {
        if (tryCodec(candidate, width, height, fps, bitrate)) {
            m_width = width;
            m_height = height;
            m_name = QString::fromLatin1(candidate);
            m_hardware = isHardwareName(m_name);

            wlog(QStringLiteral("encode"),
                 QStringLiteral("%1 encoder ready, %2x%3 at %4 fps, %5 kbit")
                     .arg(m_name)
                     .arg(width)
                     .arg(height)
                     .arg(fps)
                     .arg(bitrate / 1000));
            return true;
        }
    }

    wlog(QStringLiteral("encode"),
         QStringLiteral("no usable H.264 encoder: tried %1")
             .arg(QString::fromLatin1(
                 "h264_nvenc, h264_amf, h264_qsv, h264_mf, libopenh264")));
    return false;
}

bool VideoEncoder::tryCodec(const char *name, int width, int height, int fps, int bitrate)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (!codec)
        return false;

    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (!context)
        return false;

    context->width = width;
    context->height = height;
    context->time_base = AVRational{1, fps};
    context->framerate = AVRational{fps, 1};
    context->pix_fmt = AV_PIX_FMT_YUV420P;

    context->bit_rate = bitrate;
    context->rc_max_rate = bitrate;
    // One second of slack. A screen share alternates between long still
    // stretches and sudden whole-screen changes, and a buffer this size lets
    // a scene change spend ahead rather than come out as a smear.
    context->rc_buffer_size = bitrate;

    // No B-frames, and one reference. A B-frame is described partly by a
    // picture that has not been sent yet, which means holding frames back.
    // That is free in a file and is delay in a conversation.
    context->max_b_frames = 0;
    context->has_b_frames = 0;
    context->refs = 1;

    // Keyframes every two seconds. A viewer who joins between them asks, and
    // sending them more often than that just spends upstream.
    context->gop_size = fps * 2;

    context->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // Parameter sets in the stream rather than only in the header. RTP has no
    // header to put them in, and a viewer arriving mid-stream needs them.
    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    context->thread_count = 1;

    // Discord's clients decode constrained baseline for screen shares, and it
    // is the profile every device agrees on. The extra tools in high profile
    // buy little on text and window edges, which is most of a shared screen.
    av_opt_set(context->priv_data, "profile", "baseline", 0);

    if (qstrcmp(name, "h264_nvenc") == 0) {
        av_opt_set(context->priv_data, "preset", "p4", 0);
        av_opt_set(context->priv_data, "tune", "ull", 0);       // ultra low latency
        av_opt_set(context->priv_data, "rc", "cbr", 0);
        av_opt_set_int(context->priv_data, "delay", 0, 0);
        av_opt_set_int(context->priv_data, "zerolatency", 1, 0);
        av_opt_set_int(context->priv_data, "b_ref_mode", 0, 0);
    } else if (qstrcmp(name, "h264_amf") == 0) {
        av_opt_set(context->priv_data, "usage", "ultralowlatency", 0);
        av_opt_set(context->priv_data, "rc", "cbr", 0);
        av_opt_set_int(context->priv_data, "bf_delta_qp", 0, 0);
    } else if (qstrcmp(name, "h264_qsv") == 0) {
        av_opt_set(context->priv_data, "preset", "veryfast", 0);
        av_opt_set_int(context->priv_data, "async_depth", 1, 0);
        av_opt_set_int(context->priv_data, "low_delay_brc", 1, 0);
    } else if (qstrcmp(name, "h264_mf") == 0) {
        av_opt_set_int(context->priv_data, "hw_encoding", 1, 0);
        av_opt_set(context->priv_data, "rate_control", "cbr", 0);
    } else if (qstrcmp(name, "libopenh264") == 0) {
        // Software, so it gets the cheapest settings and more threads: this
        // is the path where the machine has nothing better, and the cost
        // lands on the same CPU running whatever is being shared.
        av_opt_set(context->priv_data, "rc_mode", "bitrate", 0);
        av_opt_set(context->priv_data, "allow_skip_frames", "1", 0);
        context->thread_count = 0;   // let FFmpeg choose
    }

    if (avcodec_open2(context, codec, nullptr) < 0) {
        avcodec_free_context(&context);
        return false;
    }

    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    if (!frame || !packet) {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
        return false;
    }

    frame->format = context->pix_fmt;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
        return false;
    }

    m_context = context;
    m_frame = frame;
    m_packet = packet;
    m_frameIndex = 0;
    m_forceKeyframe = true;

    // Whatever the encoder put in its extradata is the parameter sets, and
    // some encoders never repeat them afterwards.
    m_parameterSets.clear();
    if (context->extradata && context->extradata_size > 0) {
        m_parameterSets = QByteArray(reinterpret_cast<const char *>(context->extradata),
                                     context->extradata_size);
    }
    return true;
}

void VideoEncoder::close()
{
    if (m_scaler) {
        sws_freeContext(m_scaler);
        m_scaler = nullptr;
    }
    av_frame_free(&m_frame);
    av_packet_free(&m_packet);
    if (m_context)
        avcodec_free_context(&m_context);

    m_name.clear();
    m_parameterSets.clear();
    m_width = m_height = 0;
    m_sourceWidth = m_sourceHeight = m_sourceStride = 0;
    m_hardware = false;
    m_frameIndex = 0;
}

bool VideoEncoder::convert(const uchar *bgra, int srcWidth, int srcHeight, int stride)
{
    // The captured picture may be a different size from what is being sent -
    // a 4K monitor shared at 1080p is the normal case - so this both scales
    // and changes the colour layout.
    if (!m_scaler || m_sourceWidth != srcWidth || m_sourceHeight != srcHeight ||
        m_sourceStride != stride) {
        if (m_scaler)
            sws_freeContext(m_scaler);

        // Bicubic rather than bilinear when shrinking a lot. A 4K desktop
        // reduced to 1080p with bilinear turns small text into mush, and text
        // is most of what anybody shares a screen to show.
        const bool bigReduction = srcWidth >= m_width * 2;
        m_scaler = sws_getContext(srcWidth, srcHeight, AV_PIX_FMT_BGRA,
                                  m_width, m_height, AV_PIX_FMT_YUV420P,
                                  bigReduction ? SWS_BICUBIC : SWS_BILINEAR,
                                  nullptr, nullptr, nullptr);
        m_sourceWidth = srcWidth;
        m_sourceHeight = srcHeight;
        m_sourceStride = stride;
        if (!m_scaler)
            return false;
    }

    const uint8_t *src[4] = {bgra, nullptr, nullptr, nullptr};
    const int srcStride[4] = {stride, 0, 0, 0};

    sws_scale(m_scaler, src, srcStride, 0, srcHeight, m_frame->data, m_frame->linesize);
    return true;
}

void VideoEncoder::splitAnnexB(const uchar *data, int size, QList<QByteArray> &out)
{
    // Walks the byte stream and hands back each NAL on its own, without the
    // start code. RTP carries one NAL per payload (or a fragment of one), so
    // the start codes have no place on the wire and would only be overhead.
    int i = 0;
    int start = -1;

    while (i + 2 < size) {
        const bool three = data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1;
        const bool four = i + 3 < size && data[i] == 0 && data[i + 1] == 0 &&
                          data[i + 2] == 0 && data[i + 3] == 1;

        if (three || four) {
            if (start >= 0 && i > start)
                out.append(QByteArray(reinterpret_cast<const char *>(data + start), i - start));
            i += four ? 4 : 3;
            start = i;
            continue;
        }
        ++i;
    }

    if (start >= 0 && size > start)
        out.append(QByteArray(reinterpret_cast<const char *>(data + start), size - start));
}

bool VideoEncoder::encode(const uchar *bgra, int srcWidth, int srcHeight, int stride,
                          QList<QByteArray> &out, bool *keyframe)
{
    if (!m_context || !m_frame || !m_packet)
        return false;

    if (av_frame_make_writable(m_frame) < 0)
        return false;

    if (!convert(bgra, srcWidth, srcHeight, stride))
        return false;

    m_frame->pts = m_frameIndex++;

    // Asking by picture type is the portable way. Some encoders also take a
    // flag, and several quietly ignore it.
    m_frame->pict_type = m_forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    m_forceKeyframe = false;

    if (avcodec_send_frame(m_context, m_frame) < 0)
        return false;

    bool sawKeyframe = false;
    while (true) {
        const int got = avcodec_receive_packet(m_context, m_packet);
        if (got == AVERROR(EAGAIN) || got == AVERROR_EOF)
            break;
        if (got < 0)
            return false;

        const bool key = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
        sawKeyframe = sawKeyframe || key;

        // In front of every keyframe, the parameter sets. A viewer who
        // started watching a moment ago has not seen them, and without them a
        // keyframe describes a picture of unknown size in unknown colours.
        if (key && !m_parameterSets.isEmpty()) {
            splitAnnexB(reinterpret_cast<const uchar *>(m_parameterSets.constData()),
                        m_parameterSets.size(), out);
        }

        splitAnnexB(m_packet->data, m_packet->size, out);
        av_packet_unref(m_packet);
    }

    if (keyframe)
        *keyframe = sawKeyframe;
    return true;
}
