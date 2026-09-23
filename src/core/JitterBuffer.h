#pragma once

#include <QByteArray>
#include <QMap>
#include <QVector>
#include <QtGlobal>

struct OpusDecoder;

// One person's incoming sound, held just long enough to play it smoothly.
//
// Modelled on WebRTC's NetEq, which is what Discord's own voice engine uses
// (webrtc::NetEqImpl is in its discord_voice.node). The queue it replaces did
// the simple thing: decode each packet the moment it arrived, keep two or
// three of them, and throw the oldest away when more came. That has five
// faults, and each of them is heard:
//
//   - Packets were played in the order they arrived, not the order they were
//     sent. The internet reorders them now and then, and that was a jumble.
//   - A lost packet was never noticed. The queue was simply one frame short,
//     so the loss came out later as an underrun somewhere else.
//   - Opus sends a rough copy of each packet inside the next one (in-band
//     FEC). Nothing ever used it.
//   - The cushion was fixed. Too small for a bad connection, so it chopped;
//     no smaller on a good one.
//   - Catching up threw away whole 20 ms frames, which clicks.
//
// This keeps packets by sequence number, rebuilds a lost one from the next
// packet's FEC when it can and conceals it when it cannot, measures how
// unevenly packets arrive and sets its own cushion from that, and reaches the
// cushion by removing or repeating single pitch periods - a few milliseconds
// of a voice, which cannot be heard - rather than whole frames.
class JitterBuffer
{
public:
    static constexpr int SampleRate = 48000;
    static constexpr int Channels = 2;
    static constexpr int FrameSamples = 960; // per channel: 20 ms
    static constexpr int FrameMs = 20;

    JitterBuffer();
    ~JitterBuffer();
    JitterBuffer(const JitterBuffer &) = delete;
    JitterBuffer &operator=(const JitterBuffer &) = delete;

    bool isValid() const { return m_decoder != nullptr; }

    // One packet as it came off the wire, already opened by both layers of
    // encryption. `arrivalMs` is any steady clock, the same one pull() uses.
    void insert(quint16 sequence, quint32 timestamp, const QByteArray &opus, qint64 arrivalMs);

    // Exactly one 20 ms frame, interleaved stereo, 16 bit. Empty when this
    // person is not talking, which is most of the time in a big call - and
    // an empty buffer costs nothing, where the old queue kept guessing at
    // sound for everyone who had ever spoken.
    QByteArray pull(qint64 nowMs);

    bool isIdle() const { return m_mode == Mode::Idle; }

    struct Stats
    {
        int targetMs = 0;     // the cushion it is aiming for right now
        int bufferedMs = 0;   // what it actually holds
        int concealed = 0;    // frames invented because nothing arrived
        int recovered = 0;    // lost frames rebuilt from the next packet
        int shortened = 0;    // pitch periods removed to catch up
        int lengthened = 0;   // pitch periods repeated to avoid running dry
        int late = 0;         // arrived after their moment had passed
        int reordered = 0;    // arrived out of order but in time
    };

    // Counts since the last call, plus the current cushion.
    Stats takeStats();

private:
    enum class Mode {
        Idle,      // nothing to play; the person is quiet
        Buffering, // a talkspurt has begun; filling the cushion first
        Playing,
    };

    struct Packet
    {
        quint32 timestamp = 0;
        QByteArray opus;
    };

    qint64 unwrap(quint16 sequence) const;
    void noteArrival(quint32 timestamp, qint64 arrivalMs);
    bool decodeNext(qint64 nowMs);
    bool decodeInto(const QByteArray &opus, bool fec, int frameSize);
    void conceal();
    int bufferedMs() const;
    void adjustTiming();
    int bestLag(int *lag, double *correlation) const;
    bool shorten();
    bool lengthen();
    void goIdle();

    OpusDecoder *m_decoder = nullptr;
    Mode m_mode = Mode::Idle;

    QMap<qint64, Packet> m_packets; // by unwrapped sequence number
    qint64 m_highestSeq = -1;
    qint64 m_nextSeq = -1;          // the one to decode next
    qint64 m_bufferingSince = 0;
    qint64 m_lastArrivalMs = -1;
    int m_lastPacketSamples = FrameSamples;

    // Decoded sound waiting to go out, interleaved.
    QVector<qint16> m_pcm;

    // Arrival spread. For each packet, how long after the fastest recent one
    // it came, relative to when it was sent. The 95th percentile of that is
    // how much cushion keeps nineteen packets in twenty on time.
    QVector<double> m_relativeDelay;
    int m_delayWrite = 0;
    bool m_haveTimeBase = false;
    quint32 m_timeBase = 0;
    double m_targetMs = 60.0;

    // Consecutive frames concealed while waiting for a packet that may be late.
    int m_waitingRun = 0;

    // Those same frames, not yet counted. If the packet turns up they were a
    // real gap and are added to `concealed`; if the person had simply stopped
    // talking they were the fade after the last word, and counting them would
    // make every sentence look like a glitch in the log.
    int m_pendingConceals = 0;

    Stats m_stats;
};
