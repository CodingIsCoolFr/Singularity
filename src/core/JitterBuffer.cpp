#include "core/JitterBuffer.h"

#include <opus/opus.h>

#include <algorithm>
#include <cmath>

namespace {

// The cushion never goes below this. Packets are 20 ms apart and the beat
// that takes them out is 20 ms apart, with no fixed relation between the
// two, so a packet can land anywhere in the gap. Twenty is the least that
// always covers that, and the rest is room for the beat itself to wobble.
constexpr double MinTargetMs = 40.0;

// Past this, waiting longer is worse than the gap it saves.
constexpr double MaxTargetMs = 400.0;

// Added on top of the measured spread, to cover the phase between packets
// arriving and the beat taking them.
constexpr double TargetMarginMs = 30.0;

// How far back the spread is measured: 250 packets, five seconds of speech.
constexpr int DelayWindow = 250;

// The cushion rises at once when the network gets worse, and falls slowly
// when it improves - a quarter of a millisecond per frame, twelve and a half
// a second. Rising slowly would chop through every spike; falling fast would
// chase noise.
constexpr double DecayPerFrameMs = 0.25;

// How far from the target the buffer may drift before it is nudged back.
constexpr double StretchBandMs = 20.0;

// Pitch periods of human voices: 2.5 ms (400 Hz) to 15 ms (67 Hz).
constexpr int MinLag = 120;
constexpr int MaxLag = 720;
constexpr int CorrLength = 480;

// Below this correlation a stretch would be heard, so it waits for a better
// moment. Quiet passages are always safe to stretch.
constexpr double MinCorrelation = 0.75;
constexpr double QuietRms = 200.0;

// A hole longer than this is not concealed frame by frame: after that long,
// the guesses have faded to silence anyway.
constexpr qint64 LongGapPackets = 15;

// Past the target by this much, packets are dropped rather than squeezed.
// Only ever reached after the network stalls and then delivers a second's
// worth of sound at once.
constexpr double HardCatchUpMs = 250.0;

constexpr int MaxPacketSamples = 5760; // 120 ms, the largest Opus frame

} // namespace

JitterBuffer::JitterBuffer()
{
    int error = 0;
    m_decoder = opus_decoder_create(SampleRate, Channels, &error);
    if (error != OPUS_OK)
        m_decoder = nullptr;
    m_relativeDelay.reserve(DelayWindow);
}

JitterBuffer::~JitterBuffer()
{
    if (m_decoder)
        opus_decoder_destroy(m_decoder);
}

qint64 JitterBuffer::unwrap(quint16 sequence) const
{
    if (m_highestSeq < 0)
        return sequence;

    // The nearest number to the highest seen that ends in these sixteen bits.
    qint64 candidate = (m_highestSeq & ~qint64(0xFFFF)) | sequence;
    if (candidate - m_highestSeq > 0x8000)
        candidate -= 0x10000;
    else if (m_highestSeq - candidate > 0x8000)
        candidate += 0x10000;
    return candidate;
}

void JitterBuffer::noteArrival(quint32 timestamp, qint64 arrivalMs)
{
    if (!m_haveTimeBase) {
        m_timeBase = timestamp;
        m_haveTimeBase = true;
    }

    // When it arrived, less when it was sent, in one clock. The sender's clock
    // and ours are unrelated, so the number itself means nothing; only the
    // spread between packets does.
    const double sentMs = double(qint32(timestamp - m_timeBase)) / (SampleRate / 1000.0);
    const double relative = double(arrivalMs) - sentMs;

    if (m_relativeDelay.size() < DelayWindow) {
        m_relativeDelay.append(relative);
    } else {
        m_relativeDelay[m_delayWrite] = relative;
        m_delayWrite = (m_delayWrite + 1) % DelayWindow;
    }

    if (m_relativeDelay.size() < 8)
        return;

    // How much later than the fastest packet each one came.
    QVector<double> spread = m_relativeDelay;
    const double fastest = *std::min_element(spread.cbegin(), spread.cend());
    for (double &value : spread)
        value -= fastest;

    const int rank = qMin(int(spread.size()) - 1, int(spread.size() * 0.95));
    std::nth_element(spread.begin(), spread.begin() + rank, spread.end());
    const double wanted = qBound(MinTargetMs, spread[rank] + TargetMarginMs, MaxTargetMs);

    // Up at once, down slowly - see DecayPerFrameMs.
    if (wanted > m_targetMs)
        m_targetMs = wanted;
    else
        m_targetMs = qMax(wanted, m_targetMs - DecayPerFrameMs);
}

void JitterBuffer::insert(quint16 sequence, quint32 timestamp, const QByteArray &opus,
                          qint64 arrivalMs)
{
    if (!m_decoder || opus.isEmpty())
        return;

    // A new talkspurt starts from a clean slate, so a sequence number that has
    // moved a long way while this person was quiet is not mistaken for a loss.
    if (m_mode == Mode::Idle)
        m_highestSeq = -1;

    const qint64 seq = unwrap(sequence);

    if (m_mode == Mode::Playing && seq < m_nextSeq) {
        // Its moment has passed. Something was already played in its place.
        // It still says two things: the network is slower than the cushion
        // allowed for, and this person is still talking.
        ++m_stats.late;
        m_lastArrivalMs = arrivalMs;
        noteArrival(timestamp, arrivalMs);
        return;
    }
    if (m_packets.contains(seq))
        return; // the network sent it twice

    if (m_highestSeq >= 0 && seq < m_highestSeq)
        ++m_stats.reordered;
    if (seq > m_highestSeq)
        m_highestSeq = seq;

    m_packets.insert(seq, Packet{timestamp, opus});
    m_lastArrivalMs = arrivalMs;
    noteArrival(timestamp, arrivalMs);

    switch (m_mode) {
    case Mode::Idle:
        m_mode = Mode::Buffering;
        m_bufferingSince = arrivalMs;
        m_nextSeq = seq;
        break;
    case Mode::Buffering:
        // One that was sent earlier overtook the first to arrive.
        m_nextSeq = qMin(m_nextSeq, seq);
        break;
    case Mode::Playing:
        break;
    }

    // A second of sound is the most that is ever worth holding.
    while (m_packets.size() > 50) {
        m_packets.erase(m_packets.begin());
        if (!m_packets.isEmpty() && m_nextSeq < m_packets.firstKey())
            m_nextSeq = m_packets.firstKey();
    }
}

bool JitterBuffer::decodeInto(const QByteArray &opus, bool fec, int frameSize)
{
    qint16 pcm[MaxPacketSamples * Channels];
    const int samples = opus_decode(m_decoder, reinterpret_cast<const unsigned char *>(opus.constData()),
                                    int(opus.size()), pcm, fec ? frameSize : MaxPacketSamples,
                                    fec ? 1 : 0);
    if (samples <= 0)
        return false;

    if (!fec)
        m_lastPacketSamples = samples;

    const int count = samples * Channels;
    const int at = int(m_pcm.size());
    m_pcm.resize(at + count);
    std::copy(pcm, pcm + count, m_pcm.begin() + at);
    return true;
}

void JitterBuffer::conceal()
{
    // Opus's own guess at what the missing sound was: it continues the voice
    // it was hearing and fades it out over a few frames.
    qint16 pcm[MaxPacketSamples * Channels];
    const int samples = opus_decode(m_decoder, nullptr, 0, pcm, m_lastPacketSamples, 0);
    if (samples <= 0)
        return;
    const int count = samples * Channels;
    const int at = int(m_pcm.size());
    m_pcm.resize(at + count);
    std::copy(pcm, pcm + count, m_pcm.begin() + at);
}

bool JitterBuffer::decodeNext(qint64 nowMs)
{
    // The one that is due.
    auto due = m_packets.find(m_nextSeq);
    if (due != m_packets.end()) {
        decodeInto(due->opus, false, 0);
        m_packets.erase(due);
        ++m_nextSeq;
        m_waitingRun = 0;
        // It was late, not the end of a sentence: the wait was a real gap.
        m_stats.concealed += m_pendingConceals;
        m_pendingConceals = 0;
        return true;
    }

    const bool anythingLater = !m_packets.isEmpty() && m_packets.lastKey() > m_nextSeq;

    if (!anythingLater) {
        // Nothing after it either. Either it is late, or this person stopped
        // talking - and there is no telling which until time passes, because
        // Discord does not skip a sequence number across a pause.
        //
        // So this frame is guessed, but the packet is still expected: if it
        // turns up it plays next, and the cushion has quietly grown by one
        // frame, which is exactly what a late packet says it needs. Moving on
        // instead would throw away the real sound when it came.
        if (m_lastArrivalMs >= 0 && nowMs - m_lastArrivalMs > qint64(m_targetMs) + 60) {
            goIdle();
            return false;
        }
        conceal();
        ++m_waitingRun;
        ++m_pendingConceals;
        return true;
    }

    // A real hole: later packets have arrived, so this one was lost - and any
    // frames guessed while waiting for it were a gap, not a fade.
    m_stats.concealed += m_pendingConceals;
    m_pendingConceals = 0;
    const qint64 firstHeld = m_packets.firstKey();
    if (firstHeld - m_nextSeq > LongGapPackets) {
        m_nextSeq = firstHeld;
        conceal();
        ++m_stats.concealed;
        m_waitingRun = 0;
        return true;
    }

    // Opus puts a rough copy of each packet inside the next one. If the next
    // one is here, the lost one can be rebuilt from it rather than guessed.
    auto following = m_packets.find(m_nextSeq + 1);
    if (following != m_packets.end() && decodeInto(following->opus, true, m_lastPacketSamples)) {
        ++m_stats.recovered;
        ++m_nextSeq;
        m_waitingRun = 0;
        return true;
    }

    conceal();
    ++m_stats.concealed;
    ++m_nextSeq;
    m_waitingRun = 0;
    return true;
}

int JitterBuffer::bufferedMs() const
{
    const double decoded = double(m_pcm.size() / Channels) / (SampleRate / 1000.0);
    double waiting = 0.0;
    if (m_nextSeq >= 0 && m_highestSeq >= m_nextSeq) {
        const double packetMs = double(m_lastPacketSamples) / (SampleRate / 1000.0);
        waiting = double(m_highestSeq - m_nextSeq + 1) * packetMs;
    }
    return int(decoded + waiting);
}

int JitterBuffer::bestLag(int *lag, double *correlation) const
{
    // Mono, for the search only. The cut is then made in both channels at
    // the same place, so the stereo image is untouched.
    const int needed = MaxLag + CorrLength;
    if (m_pcm.size() / Channels < qMax(needed, 2 * MaxLag))
        return -1;

    QVector<double> mono(needed);
    for (int i = 0; i < needed; ++i)
        mono[i] = (double(m_pcm[i * Channels]) + double(m_pcm[i * Channels + 1])) * 0.5;

    double energy = 0.0;
    for (int i = 0; i < CorrLength; ++i)
        energy += mono[i] * mono[i];
    const double rms = std::sqrt(energy / CorrLength);

    // Quiet: any cut is inaudible, so take the biggest and be done.
    if (rms < QuietRms) {
        *lag = MaxLag;
        *correlation = 1.0;
        return 0;
    }

    double best = -1.0;
    int bestAt = MinLag;
    for (int candidate = MinLag; candidate <= MaxLag; ++candidate) {
        double cross = 0.0;
        double later = 0.0;
        for (int i = 0; i < CorrLength; ++i) {
            const double b = mono[candidate + i];
            cross += mono[i] * b;
            later += b * b;
        }
        const double denom = std::sqrt(energy * later);
        const double value = denom > 0.0 ? cross / denom : 0.0;
        if (value > best) {
            best = value;
            bestAt = candidate;
        }
    }

    *lag = bestAt;
    *correlation = best;
    return 1;
}

bool JitterBuffer::shorten()
{
    // One pitch period out. The period before it and the period after it are
    // nearly the same shape - that is what a pitch period is - so blending
    // the first into the second and dropping the join removes a few
    // milliseconds of voice without a click and without changing its pitch.
    int lag = 0;
    double correlation = 0.0;
    if (bestLag(&lag, &correlation) < 0 || correlation < MinCorrelation)
        return false;

    QVector<qint16> out;
    out.reserve(m_pcm.size() - lag * Channels);
    for (int i = 0; i < lag; ++i) {
        const double w = (i + 0.5) / lag;
        for (int c = 0; c < Channels; ++c) {
            const double a = m_pcm[i * Channels + c];
            const double b = m_pcm[(i + lag) * Channels + c];
            out.append(qint16(qBound(-32768.0, a * (1.0 - w) + b * w, 32767.0)));
        }
    }
    for (int i = 2 * lag * Channels; i < m_pcm.size(); ++i)
        out.append(m_pcm[i]);

    m_pcm = out;
    return true;
}

bool JitterBuffer::lengthen()
{
    // The same in reverse: one pitch period repeated, blended in so both
    // joins are smooth.
    int lag = 0;
    double correlation = 0.0;
    if (bestLag(&lag, &correlation) < 0 || correlation < MinCorrelation)
        return false;

    QVector<qint16> out;
    out.reserve(m_pcm.size() + lag * Channels);
    for (int i = 0; i < lag * Channels; ++i)
        out.append(m_pcm[i]);
    for (int i = 0; i < lag; ++i) {
        const double w = (i + 0.5) / lag;
        for (int c = 0; c < Channels; ++c) {
            const double a = m_pcm[(i + lag) * Channels + c];
            const double b = m_pcm[i * Channels + c];
            out.append(qint16(qBound(-32768.0, a * (1.0 - w) + b * w, 32767.0)));
        }
    }
    for (int i = lag * Channels; i < m_pcm.size(); ++i)
        out.append(m_pcm[i]);

    m_pcm = out;
    return true;
}

void JitterBuffer::adjustTiming()
{
    const double buffered = bufferedMs();

    // Far too much: the network stalled and then delivered it all at once.
    // Squeezing a second of speech a pitch period at a time would take
    // seconds, so whole packets go instead. This is the only place anything
    // is dropped, and it is only reached after a stall.
    if (buffered > m_targetMs + HardCatchUpMs) {
        while (!m_packets.isEmpty() && bufferedMs() > m_targetMs + StretchBandMs) {
            m_packets.erase(m_packets.begin());
            m_nextSeq = m_packets.isEmpty() ? m_highestSeq + 1 : m_packets.firstKey();
        }
        return;
    }

    if (buffered > m_targetMs + StretchBandMs) {
        // Enough decoded to search for a period, taken only from packets that
        // are actually here. Guessing sound in order to throw sound away
        // would be backwards.
        const int wanted = (2 * MaxLag + CorrLength + FrameSamples) * Channels;
        while (m_pcm.size() < wanted && m_packets.contains(m_nextSeq)) {
            auto due = m_packets.find(m_nextSeq);
            decodeInto(due->opus, false, 0);
            m_packets.erase(due);
            ++m_nextSeq;
        }
        if (shorten())
            ++m_stats.shortened;
        return;
    }

    // Running low, but sound is still arriving: stretch a little now rather
    // than run dry and have to guess later. Not while waiting on a late
    // packet - that is concealment's job, and stretching the guess would only
    // stretch the fade.
    if (buffered < m_targetMs - StretchBandMs && m_waitingRun == 0 && !m_packets.isEmpty()) {
        if (lengthen())
            ++m_stats.lengthened;
    }
}

void JitterBuffer::goIdle()
{
    m_mode = Mode::Idle;
    m_pcm.clear();
    m_packets.clear();
    m_nextSeq = -1;
    m_waitingRun = 0;
    m_pendingConceals = 0; // the fade after the last word, not a glitch
}

QByteArray JitterBuffer::pull(qint64 nowMs)
{
    if (!m_decoder || m_mode == Mode::Idle)
        return {};

    if (m_mode == Mode::Buffering) {
        // Play once the cushion is full, or once the first packet has waited
        // as long as the cushion would have held it - whichever comes first.
        // A short "yeah" never fills a cushion, and must not wait forever.
        if (bufferedMs() < m_targetMs && nowMs - m_bufferingSince < qint64(m_targetMs))
            return {};
        m_mode = Mode::Playing;
    }

    const int needed = FrameSamples * Channels;
    while (m_pcm.size() < needed) {
        if (!decodeNext(nowMs))
            break;
    }

    if (m_mode == Mode::Playing)
        adjustTiming();

    if (m_pcm.isEmpty())
        return {};

    QByteArray out(needed * int(sizeof(qint16)), '\0');
    auto *target = reinterpret_cast<qint16 *>(out.data());
    const int take = qMin(needed, int(m_pcm.size()));
    std::copy(m_pcm.cbegin(), m_pcm.cbegin() + take, target);
    m_pcm.remove(0, take);
    return out;
}

JitterBuffer::Stats JitterBuffer::takeStats()
{
    Stats out = m_stats;
    out.targetMs = int(m_targetMs);
    out.bufferedMs = bufferedMs();
    m_stats = Stats{};
    return out;
}
