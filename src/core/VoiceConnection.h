#pragma once

#include "core/VideoDecoder.h"

#include <QAudioFormat>
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUdpSocket>
#include <QWebSocket>

#include <functional>

class DaveSession;
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

    // Called once both halves have arrived from the main gateway.
    void connectToVoice(const QString &guildId, const QString &channelId, const QString &userId,
                        const QString &sessionId, const QString &token, const QString &endpoint);
    void disconnectFromVoice();

    State state() const { return m_state; }
    QString channelId() const { return m_channelId; }

    void setMuted(bool muted);
    void setDeafened(bool deafened);
    bool isMuted() const { return m_muted; }
    bool isDeafened() const { return m_deafened; }

    // 0 to 200, matching the sliders in settings.
    void setInputVolume(int percent) { m_inputVolume = percent; }
    void setOutputVolume(int percent);
    void setInputDevice(const QByteArray &deviceId) { m_inputDeviceId = deviceId; }
    void setOutputDevice(const QByteArray &deviceId) { m_outputDeviceId = deviceId; }
    void setSensitivity(int percent) { m_sensitivity = percent; }

    // Handed each slice of microphone sound just before it is encoded, so a
    // plugin can change how you sound to other people.
    //
    // A plain function rather than a signal: this is called fifty times a
    // second from the middle of the audio path, and it has to finish before
    // the next slice arrives. It is also why core does not include the plugin
    // headers — the window hands this in, so the layering stays one way.
    using MicrophoneProcessor = std::function<void(qint16 *samples, int frames, int channels, int sampleRate)>;
    void setMicrophoneProcessor(MicrophoneProcessor processor) { m_micProcessor = std::move(processor); }

signals:
    void stateChanged(VoiceConnection::State state);
    void speakingChanged(const QString &userId, bool speaking);
    void failed(const QString &reason);

    // One decoded picture from somebody's camera or shared screen.
    void videoFrame(const QString &userId, const QImage &image);

    // Somebody turned their camera or screen on, or off.
    void videoAvailable(const QString &userId, bool available);

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
    void setState(State state);
    void sendJson(const QJsonObject &payload);
    void sendIdentify();
    void sendHeartbeat();
    void sendSelectProtocol(const QString &address, quint16 port);
    void sendSpeaking(bool speaking);

    void handleReady(const QJsonObject &data);
    void handleSessionDescription(const QJsonObject &data);
    void handleSpeaking(const QJsonObject &data);

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
                      bool *marker = nullptr);

    // One person's pictures on their way to the screen.
    //
    // Each needs its own decoder, because a decoder holds the earlier frames
    // that later ones are described as changes from, and its own half built
    // picture, because one picture spans many packets.
    struct VideoStream
    {
        VideoDecoder decoder;
        QByteArray assembling;
        int packets = 0;
        int frames = 0;
        int dropped = 0;
    };

    void handleVideoPacket(quint32 ssrc, const QByteArray &payload, bool endOfFrame);

    // Held by pointer because a decoder cannot be copied, and a QHash copies
    // what it stores.
    VideoStream *videoStreamFor(quint32 ssrc);
    void clearVideoStreams();

    QHash<quint32, QString> m_videoSsrcToUser;
    QHash<quint32, VideoStream *> m_videoStreams;

    void playDecoded(quint32 ssrc, const QByteArray &frame);

    // One person's sound on its way to the speakers.
    //
    // Packets do not arrive evenly spaced, so each person's frames wait in a
    // short queue and are taken out at a steady rate. Playing them the instant
    // they land is what makes a call sound broken up.
    struct IncomingStream {
        OpusDecoder *decoder = nullptr;
        QList<QByteArray> waiting;   // decoded sound, one 20 ms frame each
        bool started = false;        // false until enough has built up to begin
    };

    IncomingStream *streamFor(quint32 ssrc);
    void onPlayTick();

    QHash<quint32, IncomingStream> m_streams;
    QTimer m_playTimer;

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

    bool m_muted = false;
    bool m_deafened = false;
    bool m_speaking = false;
    int m_silentFrames = 0;
    MicrophoneProcessor m_micProcessor;

    int m_inputVolume = 100;
    int m_outputVolume = 100;
    int m_sensitivity = 15;
    QByteArray m_inputDeviceId;
    QByteArray m_outputDeviceId;
};
