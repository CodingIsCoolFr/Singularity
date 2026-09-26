#pragma once

#include "core/VideoDecoder.h"

#include <QAudioFormat>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>
#include <QWebSocket>

#include <atomic>
#include <functional>
#include <utility>

class DaveSession;
class JitterBuffer;
class QAudioSource;
class QAudioSink;
class QIODevice;

struct OpusEncoder;
struct OpusDecoder;

// A real voice connection: the second socket Discord uses for calls.
//
// The flow is fixed and every step depends on the one before it:
//   1. the main gateway gives us a session id and a voice server
//   2. we open a websocket to that voice server and identify
//   3. the server answers with our ssrc and where to send sound
//   4. we ask the internet what our own address looks like from outside
//   5. we tell the server that address, and it sends back the secret key
//   6. from then on, sound travels over UDP, encrypted, as Opus frames
class VoiceConnection : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        Connecting,
        Handshaking,
        Connected,
        Failed,
    };
    Q_ENUM(State)

    explicit VoiceConnection(QObject *parent = nullptr);
    ~VoiceConnection() override;

    // Every connection lives on one thread of its own - the media thread -
    // and never on the one that draws the window.
    //
    // This is how Discord's own engine is built. Its voice module names its
    // threads discord_audio_playout and discord_audio_capture, and asks
    // Windows for "Pro Audio" scheduling on them. Here everything ran on the
    // window's thread instead, so a busy window made the 20 ms audio beat
    // late: in a twenty person call the five second tally took ten, which is
    // the beat running at half speed, heard as lag and chop.
    //
    // Call this once, from the media thread itself, when it starts.
    static void prepareMediaThread();

    // Called once both halves have arrived from the main gateway.
    //
    // `daveGroupId` is the MLS group the call uses. Voice channels use the
    // channel id; a Go Live stream uses the stream's media-session id, which
    // is one less than its rtc server id. Zero means "use the channel id".
    void connectToVoice(const QString &guildId, const QString &channelId, const QString &userId,
                        const QString &sessionId, const QString &token, const QString &endpoint,
                        quint64 daveGroupId = 0);
    void disconnectFromVoice();

    // Watching rather than taking part.
    //
    // A Go Live stream is a second connection of exactly this shape, so the
    // same class carries it. The differences are that `guildId` is the stream
    // server's own id rather than a server, and that a viewer has no
    // microphone: opening one here would capture the sound of the call twice.
    void setViewerOnly(bool viewerOnly) { m_viewerOnly = viewerOnly; }
    bool isViewerOnly() const { return m_viewerOnly; }

    // A Go Live viewer has no speakers of its own. Its sound is mixed into
    // the call underneath, so two devices are not fighting over the same
    // headset — which is what made shares blast and the voice sound late.
    void setAudioHost(VoiceConnection *host) { m_audioHost = host; }
    void offerExternalPcm(const QByteArray &pcm);

    // Which cameras the window is showing, and how many pixels each tile has.
    // Anyone missing is not downloaded; a small tile gets the small copy.
    // Until this is first called, every camera is wanted at its best.
    void setVideoViews(const QHash<QString, int> &pixelsByUser);

    // Safe to ask from any thread. These read copies the connection keeps up
    // to date for the window, because the real fields belong to the media
    // thread and change while it works.
    State state() const { return static_cast<State>(m_publicState.load()); }
    QString channelId() const
    {
        QMutexLocker lock(&m_publicMutex);
        return m_publicChannelId;
    }

    // Every setter below may be called from the window. Each one posts itself
    // to the media thread and returns at once, so the order of calls is kept
    // and nothing is ever changed underneath the audio while it runs.
    void setMuted(bool muted);
    void setDeafened(bool deafened);
    bool isMuted() const { return m_muted; }
    bool isDeafened() const { return m_deafened; }

    // 0 to 200, matching the sliders in settings.
    void setInputVolume(int percent);
    void setOutputVolume(int percent);

    // How loud each person is, on top of the call-wide slider.
    //
    // Keys are user ids. A missing id is 100 and not muted. The same numbers
    // Discord stores, so a change there and a change here are one setting.
    void setUserVolumes(const QHash<QString, int> &percent, const QSet<QString> &muted);
    void setInputDevice(const QByteArray &deviceId);
    void setOutputDevice(const QByteArray &deviceId);
    void setSensitivity(int percent);

    // Sending a picture of our own.
    //
    // Used on the second connection that a Go Live stream runs over. Calling
    // this tells the server we have pictures to send and what size they are;
    // after it, every call to sendPicture() puts one on the wire.
    // `fps` and `bitrate` are what the encoder really runs at; the Video
    // payload tells the server so, as Discord's client does.
    void startSendingVideo(int width, int height, int fps = 30, int bitrate = 2500000);
    void stopSendingVideo();
    bool isSendingVideo() const { return m_publicSending.load(); }

    // One complete picture, already split into NAL units by the encoder.
    //
    // Chopping it into packets, sealing each one for the other people, and
    // wrapping that for the transport all happen here, because this is the
    // only object that holds the keys and the socket.
    void sendPicture(const QList<QByteArray> &units);

    // Sound that goes out with a shared screen: 48 kHz, stereo, 16 bit, in
    // any size of piece. Safe to call from any thread - it comes straight from
    // the capture thread - and only the connection carrying a share sends it.
    // It goes out marked as SOUNDSHARE, the flag Discord gives context audio,
    // so nobody gets a green speaking ring for your game.
    void offerSharedSound(const QByteArray &pcm);

    // Handed each slice of microphone sound just before it is encoded, so a
    // plugin can change how you sound to other people.
    //
    // A plain function rather than a signal: this is called fifty times a
    // second from the middle of the audio path, and it has to finish before
    // the next slice arrives. It is also why core does not include the plugin
    // headers — the window hands this in, so the layering stays one way.
    using MicrophoneProcessor = std::function<void(qint16 *samples, int frames, int channels, int sampleRate)>;
    void setMicrophoneProcessor(MicrophoneProcessor processor);

signals:
    void stateChanged(VoiceConnection::State state);
    void speakingChanged(const QString &userId, bool speaking);
    void failed(const QString &reason);

    // One decoded picture from somebody's camera or shared screen.
    void videoFrame(const QString &userId, const QImage &image);

    // Somebody turned their camera or screen on, or off.
    void videoAvailable(const QString &userId, bool available);

    // A viewer told us it cannot draw anything from what we have sent. The
    // encoder has to produce a keyframe; everything before it describes
    // changes from a picture that viewer never saw.
    void keyframeWanted();

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onTextMessage(const QString &message);

    // The key exchange travels as binary, not JSON, so these messages have to
    // be taken apart by hand.
    void onBinaryMessage(const QByteArray &message);
    void onUdpReadyRead();
    void onSendTick();

private:
    // True when called from anywhere but the media thread, in which case the
    // work has been posted there and the caller should return. Posting keeps
    // calls in the order they were made, which a setter followed by a connect
    // depends on.
    template <typename Work>
    bool postToOwnThread(Work &&work)
    {
        if (QThread::currentThread() == thread())
            return false;
        QMetaObject::invokeMethod(this, std::forward<Work>(work), Qt::QueuedConnection);
        return true;
    }

    // The copies the window reads. Written only on the media thread.
    std::atomic<int> m_publicState{static_cast<int>(State::Idle)};
    std::atomic<bool> m_publicSending{false};
    mutable QMutex m_publicMutex;
    QString m_publicChannelId;
    void publishChannel(const QString &channelId);

    // How late the 20 ms playback beat has been since the last tally. This is
    // the number that says whether a stutter is ours, and it is the one that
    // was missing: the tally counted ticks, so when ticks ran late the tally
    // itself stretched from five seconds to ten and nothing else looked wrong.
    QElapsedTimer m_beatClock;
    qint64 m_lastBeatMs = -1;
    qint64 m_worstBeatLateMs = 0;
    int m_beatsLate = 0;

    void setState(State state);
    void sendJson(const QJsonObject &payload);
    void sendIdentify();
    void sendHeartbeat();
    void sendSelectProtocol(const QString &address, quint16 port);
    void sendSpeaking(bool speaking);

    // What we send, and what we want sent to us. The second is the one that
    // makes other people's cameras appear at all.
    void sendVideoState();
    void sendVideoWants();

    void handleReady(const QJsonObject &data);
    void handleSessionDescription(const QJsonObject &data);
    void handleSpeaking(const QJsonObject &data);

    // The green ring, driven by sound rather than by announcements.
    void noteSpeaking(quint32 ssrc);
    void sweepSpeaking();

    QSet<QString> m_speakingNow;
    QHash<QString, qint64> m_lastHeardMs;

    // End-to-end encryption. Discord requires it on every call since March
    // 2026, so a client that cannot do this cannot join one at all.
    void sendBinary(int opcode, const QByteArray &payload);
    void handleDaveMessage(int opcode, const QByteArray &payload, qint64 sequence);
    QSet<QString> recognisedUsers() const;
    void sendInvalidCommitWelcome(qint64 transitionId);
    void announceReadyForTransition(qint64 transitionId);
    void refreshDaveKeys();

    DaveSession *m_dave = nullptr;

    // Everyone currently in the call, which the key group needs by name.
    QSet<QString> m_roster;
    int m_daveVersion = 0;

    void beginIpDiscovery();
    bool readIpDiscoveryReply(const QByteArray &packet, QString &address, quint16 &port);

    void startAudio();
    void stopAudio();
    void teardown();

    // Encryption. Both are the "rtpsize" variants Discord offers today.
    QByteArray encryptFrame(const QByteArray &rtpHeader, const QByteArray &opusFrame);
    bool decryptFrame(const QByteArray &packet, QByteArray &opusFrame, quint32 &ssrc,
                      bool *marker = nullptr, quint16 *sequence = nullptr,
                      int *rotationDegrees = nullptr);

    // One person's pictures on their way to the screen.
    //
    // The decoder itself lives on the video thread. What stays here is the
    // half-built picture and the few packets that arrived out of order.
    struct HeldPacket {
        QByteArray payload;
        bool endOfFrame = false;
        int rotation = 0;
    };

    struct VideoStream
    {
        QByteArray assembling;
        QHash<quint16, HeldPacket> held;
        quint16 nextSeq = 0;
        bool haveSeq = false;
        bool gap = false;
        bool loggedFirst = false;
        qint64 lastPliMs = 0;
        int packets = 0;
        int frames = 0;
        int dropped = 0;
        int hungry = 0;
        int rotation = 0;

        // Lost packets we have asked to have sent again, and when we last
        // asked. A hole is waited on for a while before the picture is given
        // up, because the copy is usually on its way.
        QHash<quint16, qint64> nacked;
        QHash<quint16, int> nackTries;
        qint64 holeSinceMs = 0;
        int asked = 0;
        int resent = 0;
    };

    // Resending. A picture is dozens of packets; lose one and the whole
    // picture is useless, and every picture after it until the next keyframe.
    // Discord's clients ask for just the missing packet (RTCP NACK, RFC 4585)
    // and get it back on the stream's "rtx" partner (RFC 4588). Without this,
    // a stream through a far or busy server showed nothing at all.
    void requestMissingVideo(VideoStream &stream, quint32 ssrc, quint16 from, quint16 to);
    void sendNack(quint32 mediaSsrc, const QList<quint16> &sequences);
    void sweepVideoHoles();
    QHash<quint32, quint32> m_rtxToMedia;
    int m_statVideoRecovered = 0;
    int m_statNacksSent = 0;

    // Our own pictures, kept for a moment so a viewer who lost one can have
    // it again. Indexed by sequence number modulo the size.
    struct SentVideoPacket {
        quint16 sequence = 0;
        quint32 timestamp = 0;
        bool marker = false;
        bool valid = false;
        QByteArray payload;
    };
    static constexpr int SentVideoHistory = 1024;
    QList<SentVideoPacket> m_sentVideo;
    quint16 m_rtxSequence = 0;
    quint16 m_transportSequence = 0;
    int m_statVideoResentOut = 0;
    bool m_saidFeedback = false;
    int m_statRtcpIn = 0;
    void resendVideo(const QList<quint16> &sequences);

    // Pacing. A keyframe is a hundred packets; sent all at once they queue in
    // the home router in front of the voice packets, and the voice arrives
    // late in a lump every two seconds. Spread over a few milliseconds each,
    // at a bit over twice the stream's own rate, they do not.
    void queueVideoDatagram(const QByteArray &packet);
    void drainPacer();
    QList<QByteArray> m_paceQueue;
    qint64 m_paceQueuedBytes = 0;
    QTimer m_paceTimer;
    QElapsedTimer m_paceClock;
    qint64 m_paceLastMs = 0;
    double m_paceBudget = 0.0;
    qint64 m_rateWindowStartMs = 0;
    qint64 m_rateWindowBytes = 0;
    double m_videoBytesPerMs = 0.0;

    void handleVideoPacket(quint32 ssrc, const QByteArray &payload, bool endOfFrame,
                           quint16 sequence, int rotation);
    void ingestVideoPayload(VideoStream &stream, quint32 ssrc, const QString &userId,
                            const QByteArray &payload, bool endOfFrame, int rotation);
    void finishVideoPicture(VideoStream &stream, quint32 ssrc, const QString &userId);
    void flushHeldVideo(VideoStream &stream, quint32 ssrc, const QString &userId);
    void skipLostVideo(VideoStream &stream, quint32 ssrc, const QString &userId);

    // Asks the sender for a keyframe so a stream we joined in the middle can
    // actually produce a picture, rather than waiting for the next one.
    void sendPictureLossIndication(quint32 mediaSsrc);

    // The other direction: somebody asked us for one.
    void handleIncomingRtcp(const QByteArray &packet);

    // One NAL unit, as one packet or as several. RFC 6184 calls the split
    // form FU-A.
    void packetiseNalUnit(const QByteArray &nal, bool lastOfPicture);
    void sendVideoPacket(const QByteArray &payload, bool endOfPicture);

    // Picks the highest-quality layer we were offered and asks only for that.
    void refreshVideoWants();

    // The one layer of this person's camera to receive, or 0 for none.
    quint32 chosenVideoSsrc(const QString &userId) const;
    QHash<QString, int> m_videoViews;
    bool m_videoViewsKnown = false;
    QSet<quint32> m_lastWantedVideo;

    // Held by pointer because a decoder cannot be copied, and a QHash copies
    // what it stores.
    VideoStream *videoStreamFor(quint32 ssrc);
    void clearVideoStreams();

    QHash<quint32, QString> m_videoSsrcToUser;
    QHash<quint32, int> m_videoRid;
    QHash<QString, quint32> m_bestVideoSsrc;
    QHash<quint32, VideoStream *> m_videoStreams;
    QSet<quint32> m_rtxSsrcs;
    QThread m_videoThread;
    VideoDecodeWorker *m_videoWorker = nullptr;
    QStringList m_experiments;
    quint64 m_daveGroupId = 0;
    quint8 m_videoPayloadType = 101;
    quint8 m_rtxPayloadType = 102;

    // Our own outgoing picture.
    //
    // Video runs on its own synchronisation source with its own sequence
    // numbering, because it is a separate stream from the sound even though
    // both go down the same socket.
    // What the server set aside for our pictures, out of its own READY. Zero
    // until it says, and guessing instead is what made a share invisible.
    quint32 m_assignedVideoSsrc = 0;
    quint32 m_assignedRtxSsrc = 0;

    quint32 m_videoSsrc = 0;
    quint32 m_rtxSsrc = 0;
    quint16 m_videoSequence = 0;
    quint32 m_videoTimestamp = 0;
    bool m_sendingVideo = false;
    QByteArray m_lastSinkWants;   // the last op 15 logged while sending
    int m_sendFps = 30;
    int m_sendBitrate = 2500000;
    int m_sendWidth = 0;
    int m_sendHeight = 0;
    int m_statVideoSent = 0;
    int m_statVideoSealFailed = 0;
    qint64 m_lastKeyframeRequestMs = 0;

    // One packet of somebody's sound, opened and handed to their buffer.
    void receiveAudio(quint32 ssrc, quint16 sequence, quint32 timestamp, const QByteArray &frame);

    // One person's sound on its way to the speakers: a NetEq-style buffer
    // that orders packets, rebuilds lost ones, and picks its own cushion.
    // Held by pointer because a buffer owns a decoder and cannot be copied.
    JitterBuffer *bufferFor(quint32 ssrc);
    void onPlayTick();
    QByteArray mixWaitingStreams();

    // Encodes and sends whatever shared sound has arrived since the last beat.
    void sendSharedSound();
    QMutex m_sharedSoundMutex;
    QByteArray m_sharedSound;
    bool m_saidSharedSound = false;

    QHash<quint32, JitterBuffer *> m_buffers;
    QTimer m_playTimer;

    // The one clock packets are stamped and pulled against.
    QElapsedTimer m_mediaClock;

    QWebSocket m_socket;
    QUdpSocket m_udp;
    QTimer m_heartbeatTimer;
    QTimer m_sendTimer;

    // Names the last handshake step that completed, so a stall reports where
    // it stopped instead of a bare "failed".
    QTimer m_handshakeWatchdog;
    QString m_stage;

    // The last message put on the wire, with the token blanked. Discord says
    // only "malformed request", never which one, so we keep it ourselves.
    QString m_lastSent;

    State m_state = State::Idle;

    // Who and where.
    QString m_guildId;
    QString m_channelId;
    QString m_userId;
    QString m_sessionId;
    QString m_token;
    QString m_endpoint;

    // Given to us by the voice server.
    quint32 m_ssrc = 0;
    QString m_serverAddress;
    quint16 m_serverPort = 0;
    QByteArray m_secretKey;
    QString m_mode;
    QStringList m_offeredModes;
    qint64 m_lastSequence = -1;

    // Picking a dropped call back up without leaving it (voice opcode 7).
    bool m_resuming = false;
    int m_resumeAttempts = 0;
    void sendResume();

    // Outgoing packet counters.
    quint16 m_rtpSequence = 0;
    quint32 m_rtpTimestamp = 0;
    quint32 m_nonceCounter = 0;

    // Sound.
    QAudioSource *m_input = nullptr;
    QAudioSink *m_output = nullptr;
    QIODevice *m_inputStream = nullptr;
    QIODevice *m_outputStream = nullptr;
    QByteArray m_captureBuffer;
    OpusEncoder *m_encoder = nullptr;
    QHash<quint32, QString> m_ssrcToUser;

    // A running tally of what happened to each frame of sound.
    //
    // Every way sound can fail here is a quiet "skip this frame", which leaves
    // a silent call looking exactly like a working one in the log. These count
    // the skips by reason and print a line every few seconds, so "I cannot hear
    // anyone" arrives already explained.
    void reportAudioStats();

    int m_statTicks = 0;
    int m_statSent = 0;          // frames we put on the wire
    int m_statPlayed = 0;        // frames we turned back into sound
    int m_statSealFailed = 0;    // our own words, no group key yet
    int m_statOpenFailed = 0;    // someone else's words we could not open
    int m_statNoOwner = 0;       // sound from an ssrc we cannot name
    int m_statUndecryptable = 0; // transport layer refused the packet
    int m_statVideoPackets = 0;  // UDP packets that looked like video
    int m_statVideoDaveFailed = 0;
    int m_statVideoUnknownSsrc = 0;

    bool m_viewerOnly = false;
    VoiceConnection *m_audioHost = nullptr;
    QByteArray m_externalPcm;
    bool m_muted = false;
    bool m_deafened = false;
    bool m_speaking = false;
    int m_silentFrames = 0;
    MicrophoneProcessor m_micProcessor;

    int m_inputVolume = 100;
    int m_outputVolume = 100;
    QHash<QString, int> m_userVolume;
    QSet<QString> m_userMuted;
    int m_sensitivity = 15;
    QByteArray m_inputDeviceId;
    QByteArray m_outputDeviceId;
};
