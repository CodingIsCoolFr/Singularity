#include "core/VoiceConnection.h"

#include "core/DaveSession.h"
#include "core/Logger.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMediaDevices>
#include <QNetworkDatagram>
#include <QRandomGenerator>

#include <opus/opus.h>
#include <sodium.h>

#ifdef WISP_HAVE_DAVE
#include <dave/dave.h>
#endif

#include <cmath>
#include <cstring>

namespace {

// Voice gateway opcodes.
constexpr int OpIdentify = 0;
constexpr int OpSelectProtocol = 1;
constexpr int OpReady = 2;
constexpr int OpHeartbeat = 3;
constexpr int OpSessionDescription = 4;
constexpr int OpSpeaking = 5;
constexpr int OpHeartbeatAck = 6;
constexpr int OpHello = 8;
constexpr int OpClientsConnect = 11;
constexpr int OpVideo = 12;
constexpr int OpClientDisconnect = 13;
constexpr int OpMediaSinkWants = 15;

// Payload types, carried in the low seven bits of the second RTP byte.
constexpr int VideoPayloadType = 101;   // H.264

// End-to-end encryption, straight from Discord's opcode table. The ones marked
// binary do not arrive as JSON.
constexpr int OpDavePrepareTransition = 21;          // server
constexpr int OpDaveExecuteTransition = 22;          // server
constexpr int OpDaveTransitionReady = 23;            // client
constexpr int OpDavePrepareEpoch = 24;               // server
constexpr int OpDaveMlsExternalSender = 25;          // server, binary
constexpr int OpDaveMlsKeyPackage = 26;              // client, binary
constexpr int OpDaveMlsProposals = 27;               // server, binary
constexpr int OpDaveMlsCommitWelcome = 28;           // client, binary
constexpr int OpDaveMlsAnnounceCommitTransition = 29;// server, binary
constexpr int OpDaveMlsWelcome = 30;                 // server, binary
constexpr int OpDaveMlsInvalidCommitWelcome = 31;    // client

// The highest end-to-end encryption version this client can honour.
//
// Asked of the library itself rather than written down, so it can never claim
// more than it can deliver. Zero means the library is absent and calls cannot
// be joined at all.
int maxDaveProtocolVersion()
{
    // Asked of the library rather than written down, so this can never claim
    // more than it can deliver. Zero means calls cannot be joined at all.
    return DaveSession::maxSupportedVersion();
}

// Discord's voice is always 48 kHz stereo, in 20 millisecond frames.
constexpr int SampleRate = 48000;
constexpr int Channels = 2;
constexpr int FrameSamples = 960;                                  // per channel
constexpr int FrameBytes = FrameSamples * Channels * 2;            // 16 bit
constexpr int FrameMs = 20;

constexpr int RtpHeaderSize = 12;
constexpr int NonceTailSize = 4;

// How long to keep sending after you stop talking, so words are not clipped.
constexpr int SilenceFramesBeforeStop = 10;   // 200 ms

// How much of someone's sound to hold back before starting to play it.
//
// The internet does not deliver packets evenly. Playing each one the moment it
// lands means every late arrival is a gap. Three frames is 60 milliseconds of
// cushion: too small to hear as delay, big enough to smooth out normal jitter.
constexpr int JitterFrames = 3;

// The most that may ever pile up for one person, half a second. Beyond this
// the connection is behind in a way that waiting will not fix.
constexpr int MaxQueuedFrames = 25;

QByteArray buildRtpHeader(quint16 sequence, quint32 timestamp, quint32 ssrc)
{
    QByteArray header(RtpHeaderSize, '\0');
    auto *bytes = reinterpret_cast<quint8 *>(header.data());

    bytes[0] = 0x80;   // version 2, no padding, no extension
    bytes[1] = 0x78;   // payload type 120, which is Opus

    bytes[2] = static_cast<quint8>(sequence >> 8);
    bytes[3] = static_cast<quint8>(sequence & 0xFF);

    bytes[4] = static_cast<quint8>(timestamp >> 24);
    bytes[5] = static_cast<quint8>(timestamp >> 16);
    bytes[6] = static_cast<quint8>(timestamp >> 8);
    bytes[7] = static_cast<quint8>(timestamp & 0xFF);

    bytes[8] = static_cast<quint8>(ssrc >> 24);
    bytes[9] = static_cast<quint8>(ssrc >> 16);
    bytes[10] = static_cast<quint8>(ssrc >> 8);
    bytes[11] = static_cast<quint8>(ssrc & 0xFF);

    return header;
}

// How much of a packet is left in the clear, and how much of what follows is
// extension rather than sound.
//
// Discord: "the clear authenticated data is only the fixed RTP header, any
// CSRCs, and the 4 byte RTP extension preamble. The individual RTP extension
// elements are encrypted with the RTP payload."
//
// So the preamble counts as header, but the elements it describes do NOT.
// They sit at the front of the decrypted text and have to be stepped over
// before the rest can be treated as sound. Counting them as header instead
// makes every packet carrying an extension fail to open, which is nearly all
// of them.
int clearHeaderLength(const QByteArray &packet, int *extensionBodyBytes = nullptr)
{
    if (extensionBodyBytes)
        *extensionBodyBytes = 0;

    if (packet.size() < RtpHeaderSize)
        return -1;

    const auto *bytes = reinterpret_cast<const quint8 *>(packet.constData());
    const int csrcCount = bytes[0] & 0x0F;
    int length = RtpHeaderSize + csrcCount * 4;

    const bool hasExtension = (bytes[0] & 0x10) != 0;
    if (hasExtension) {
        if (packet.size() < length + 4)
            return -1;

        // Two bytes name the profile, two give the length in 32 bit words.
        const int words = (bytes[length + 2] << 8) | bytes[length + 3];
        length += 4;
        if (extensionBodyBytes)
            *extensionBodyBytes = words * 4;
    }

    return length <= packet.size() ? length : -1;
}

QAudioFormat voiceFormat()
{
    QAudioFormat format;
    format.setSampleRate(SampleRate);
    format.setChannelCount(Channels);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

QAudioDevice findInput(const QByteArray &id)
{
    if (!id.isEmpty()) {
        const QList<QAudioDevice> devices = QMediaDevices::audioInputs();
        for (const QAudioDevice &device : devices) {
            if (device.id() == id)
                return device;
        }
    }
    return QMediaDevices::defaultAudioInput();
}

QAudioDevice findOutput(const QByteArray &id)
{
    if (!id.isEmpty()) {
        const QList<QAudioDevice> devices = QMediaDevices::audioOutputs();
        for (const QAudioDevice &device : devices) {
            if (device.id() == id)
                return device;
        }
    }
    return QMediaDevices::defaultAudioOutput();
}

} // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

VoiceConnection::VoiceConnection(QObject *parent)
    : QObject(parent)
    , m_socket(QStringLiteral("https://discord.com"))
{
    if (sodium_init() < 0)
        wlog(QStringLiteral("voice"), QStringLiteral("libsodium refused to start"));

    m_heartbeatTimer.setSingleShot(false);
    m_sendTimer.setSingleShot(false);
    m_sendTimer.setTimerType(Qt::PreciseTimer);
    m_sendTimer.setInterval(FrameMs);

    // Playback runs on its own steady beat rather than on packet arrival.
    m_playTimer.setSingleShot(false);
    m_playTimer.setTimerType(Qt::PreciseTimer);
    m_playTimer.setInterval(FrameMs);
    connect(&m_playTimer, &QTimer::timeout, this, &VoiceConnection::onPlayTick);

    connect(&m_socket, &QWebSocket::connected, this, &VoiceConnection::onSocketConnected);
    connect(&m_socket, &QWebSocket::disconnected, this, &VoiceConnection::onSocketDisconnected);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &VoiceConnection::onTextMessage);
    connect(&m_socket, &QWebSocket::binaryMessageReceived, this, &VoiceConnection::onBinaryMessage);
    connect(&m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        wlog(QStringLiteral("voice"), QStringLiteral("socket error: %1").arg(m_socket.errorString()));
    });

    m_dave = new DaveSession(this);
    connect(m_dave, &DaveSession::groupFailed, this, [this](const QString &reason) {
        // Only written down here. The caller that noticed the failure knows
        // which change it belongs to, and sends the complaint itself; two
        // complaints for one failure is one too many.
        wlog(QStringLiteral("voice"), QStringLiteral("encryption group failed: %1").arg(reason));
    });

    connect(&m_udp, &QUdpSocket::readyRead, this, &VoiceConnection::onUdpReadyRead);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &VoiceConnection::sendHeartbeat);
    connect(&m_sendTimer, &QTimer::timeout, this, &VoiceConnection::onSendTick);

    m_handshakeWatchdog.setSingleShot(true);
    connect(&m_handshakeWatchdog, &QTimer::timeout, this, [this]() {
        if (m_state == State::Connected || m_state == State::Idle)
            return;
        wlog(QStringLiteral("voice"), QStringLiteral("gave up waiting, stuck after: %1").arg(m_stage));
        emit failed(QStringLiteral("stuck after %1").arg(m_stage));
        setState(State::Failed);
        teardown();
    });
}

VoiceConnection::~VoiceConnection()
{
    teardown();
}

void VoiceConnection::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

void VoiceConnection::connectToVoice(const QString &guildId, const QString &channelId,
                                     const QString &userId, const QString &sessionId,
                                     const QString &token, const QString &endpoint)
{
    disconnectFromVoice();

    m_guildId = guildId;
    m_channelId = channelId;
    m_userId = userId;
    m_sessionId = sessionId;
    m_token = token;
    m_endpoint = endpoint;

    // Fresh connection, fresh counters. A stale sequence number from the last
    // call is rejected as a bad payload.
    m_lastSequence = -1;
    m_offeredModes.clear();
    m_secretKey.clear();

    // The endpoint arrives as "host:port" with no scheme. Discord's own guide
    // says to prepend "wss://" and nothing else, so the port is kept.
    //
    // Stripping it was a mistake that cost a lot of time: the connection then
    // lands on a different machine, which answers and then refuses the ticket
    // with "session no longer valid", hiding the real reply.
    QString host = endpoint;
    host.remove(QStringLiteral("wss://"));

    // Version 8, the one current clients use.
    //
    // Version 4 was tried first as the simpler option, but only ever against
    // the wrong server, because the port was being discarded. With the right
    // server it is refused. Version 8 needs two extra things and both are
    // handled below: an explicit opt out of the end-to-end encryption layer,
    // and a sequence number on the heartbeat.
    const QString url = QStringLiteral("wss://%1/?v=8").arg(host);

    wlog(QStringLiteral("voice"), QStringLiteral("opening %1").arg(url));

    m_stage = QStringLiteral("opening the voice socket");
    m_handshakeWatchdog.start(15000);

    setState(State::Connecting);
    m_socket.open(QUrl(url));
}

void VoiceConnection::disconnectFromVoice()
{
    if (m_state == State::Idle)
        return;
    teardown();
    setState(State::Idle);
}

void VoiceConnection::teardown()
{
    m_heartbeatTimer.stop();
    m_handshakeWatchdog.stop();
    stopAudio();

    if (m_socket.state() != QAbstractSocket::UnconnectedState)
        m_socket.close();

    m_udp.close();
    m_secretKey.clear();
    m_ssrc = 0;
    m_ssrcToUser.clear();
    m_speaking = false;
    m_roster.clear();
    m_daveVersion = 0;
    clearVideoStreams();

    m_statTicks = 0;
    m_statSent = 0;
    m_statPlayed = 0;
    m_statSealFailed = 0;
    m_statOpenFailed = 0;
    m_statNoOwner = 0;
    m_statUndecryptable = 0;

    if (m_dave)
        m_dave->end();
}

// ---------------------------------------------------------------------------
// The websocket half
// ---------------------------------------------------------------------------

void VoiceConnection::onSocketConnected()
{
    wlog(QStringLiteral("voice"), QStringLiteral("voice socket open"));
    m_stage = QStringLiteral("waiting for the voice server greeting");
    setState(State::Handshaking);

    // Signing in waits for the greeting rather than racing it. Sending first
    // worked, but it left the two messages crossing on the wire, which makes
    // any failure harder to read than it needs to be.
}

void VoiceConnection::onSocketDisconnected()
{
    const int code = static_cast<int>(m_socket.closeCode());
    const QString reason = m_socket.closeReason();

    // Discord's voice close codes, spelled out so a failure is diagnosable
    // without reading the log.
    QString meaning;
    switch (code) {
    case 4001: meaning = QStringLiteral("unknown opcode"); break;
    case 4002: meaning = QStringLiteral("bad payload"); break;
    case 4003: meaning = QStringLiteral("not signed in"); break;
    case 4004: meaning = QStringLiteral("token rejected"); break;
    case 4006: meaning = QStringLiteral("session no longer valid"); break;
    case 4009: meaning = QStringLiteral("session timed out"); break;
    case 4011: meaning = QStringLiteral("server not found"); break;
    case 4012: meaning = QStringLiteral("unknown protocol version"); break;
    case 4014: meaning = QStringLiteral("disconnected from the channel"); break;
    case 4015: meaning = QStringLiteral("voice server crashed"); break;
    case 4016: meaning = QStringLiteral("unknown encryption mode"); break;
    // Taken from Discord's own table, not guessed.
    case 4017: meaning = QStringLiteral("this channel requires end-to-end encryption, which "
                                        "Wisp does not have yet"); break;
    case 4020: meaning = QStringLiteral("malformed request"); break;
    case 4021: meaning = QStringLiteral("rate limited"); break;
    case 4022: meaning = QStringLiteral("the call ended"); break;
    default:   meaning = QStringLiteral("closed"); break;
    }

    wlog(QStringLiteral("voice"),
         QStringLiteral("voice socket closed: code=%1 (%2) reason=\"%3\" stage=\"%4\"")
             .arg(code)
             .arg(meaning, reason.isEmpty() ? QStringLiteral("none") : reason, m_stage));

    // For a refusal about the content of a message, the message itself is the
    // whole story, so it is printed rather than described.
    if (code == 4001 || code == 4002 || code == 4012 || code == 4016 || code == 4020) {
        wlog(QStringLiteral("voice"),
             QStringLiteral("the last thing we sent was: %1")
                 .arg(m_lastSent.isEmpty() ? QStringLiteral("(nothing)") : m_lastSent));
    }

    m_heartbeatTimer.stop();
    stopAudio();

    if (m_state == State::Idle)
        return;

    setState(State::Failed);

    // Some refusals are about the channel or the account, not about this
    // attempt. Trying again just repeats them, so they are marked so the
    // window can stop rather than loop.
    // 4004 bad token, 4014 kicked, 4017 needs end-to-end encryption,
    // 4020 we sent something wrong, 4021 rate limited, 4022 the call ended.
    //
    // 4020 is on this list because sending the same wrong message again cannot
    // turn it into a right one. Retrying only kicks the person out repeatedly.
    const bool worthRetrying = !(code == 4004 || code == 4014 || code == 4017 || code == 4020
                                 || code == 4021 || code == 4022);

    emit failed(QStringLiteral("%1 (code %2)%3")
                    .arg(meaning)
                    .arg(code)
                    .arg(worthRetrying ? QString() : QStringLiteral(" [final]")));
}

void VoiceConnection::sendJson(const QJsonObject &payload)
{
    if (m_socket.state() != QAbstractSocket::ConnectedState)
        return;

    const QByteArray text = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    // Kept so that a "malformed request" close can name the message Discord
    // objected to. The token is replaced, never written down.
    QJsonObject shown = payload;
    QJsonObject inner = shown.value(QStringLiteral("d")).toObject();
    if (inner.contains(QStringLiteral("token"))) {
        inner[QStringLiteral("token")] = QStringLiteral("(%1 characters)")
                                             .arg(inner.value(QStringLiteral("token")).toString().size());
        shown[QStringLiteral("d")] = inner;
    }
    m_lastSent = QString::fromUtf8(QJsonDocument(shown).toJson(QJsonDocument::Compact));

    m_socket.sendTextMessage(QString::fromUtf8(text));
}

void VoiceConnection::sendIdentify()
{
    // The whole sign-in, with only the token held back. Every failure so far
    // has come down to one of these four values, so print all of them rather
    // than reason about which one is suspect.
    wlog(QStringLiteral("voice"),
         QStringLiteral("identifying: server=%1 user=%2 session=%3 tokenLength=%4")
             .arg(m_guildId, m_userId, m_sessionId)
             .arg(m_token.size()));

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpIdentify},
        {QStringLiteral("d"),
         QJsonObject{
             {QStringLiteral("server_id"), m_guildId},
             {QStringLiteral("user_id"), m_userId},
             {QStringLiteral("session_id"), m_sessionId},
             {QStringLiteral("token"), m_token},
             {QStringLiteral("video"), false},
             {QStringLiteral("max_dave_protocol_version"), maxDaveProtocolVersion()},
         }},
    });
}

void VoiceConnection::sendHeartbeat()
{
    // Version 8 wants an object carrying a nonce and the number of the last
    // numbered message the server sent.
    //
    // Before any numbered message has arrived, Discord's own documentation
    // says to leave the field out or send -1. Sending 0 claims we already saw
    // message zero, which is not true, and is refused as a malformed request.
    QJsonObject beat{
        {QStringLiteral("t"), static_cast<qint64>(QRandomGenerator::global()->generate())},
    };
    if (m_lastSequence >= 0)
        beat.insert(QStringLiteral("seq_ack"), m_lastSequence);

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpHeartbeat},
        {QStringLiteral("d"), beat},
    });
}

void VoiceConnection::onTextMessage(const QString &message)
{
    const QJsonObject packet = QJsonDocument::fromJson(message.toUtf8()).object();
    const int op = packet.value(QStringLiteral("op")).toInt(-1);
    const QJsonObject data = packet.value(QStringLiteral("d")).toObject();

    // Version 8 numbers its messages, and the heartbeat has to say which one
    // it last saw.
    if (packet.contains(QStringLiteral("seq")))
        m_lastSequence = static_cast<qint64>(packet.value(QStringLiteral("seq")).toDouble());

    // Every opcode is logged except the heartbeat reply, so a stalled
    // handshake shows exactly what the server did and did not send.
    if (op != OpHeartbeatAck)
        wlog(QStringLiteral("voice"), QStringLiteral("received op %1").arg(op));

    switch (op) {
    case OpHello: {
        const int interval = data.value(QStringLiteral("heartbeat_interval")).toInt(13750);

        m_stage = QStringLiteral("signing in to the voice server");
        sendIdentify();

        m_heartbeatTimer.start(interval);
        sendHeartbeat();
        break;
    }
    case OpReady:
        handleReady(data);
        break;
    case OpSessionDescription:
        handleSessionDescription(data);
        break;
    case OpSpeaking:
        handleSpeaking(data);
        break;
    case OpHeartbeatAck:
        break;
    case OpClientDisconnect: {
        const QString userId = data.value(QStringLiteral("user_id")).toString();
        if (!userId.isEmpty()) {
            emit speakingChanged(userId, false);
            m_roster.remove(userId);
        }
        break;
    }

    case OpVideo: {
        // Says which stream numbers belong to whom. Without this a picture
        // arrives with no name on it and there is nowhere to put it.
        const QString userId = data.value(QStringLiteral("user_id")).toString();
        const auto videoSsrc = static_cast<quint32>(data.value(QStringLiteral("video_ssrc")).toDouble());
        const auto audioSsrc = static_cast<quint32>(data.value(QStringLiteral("audio_ssrc")).toDouble());

        if (userId.isEmpty())
            break;

        if (audioSsrc != 0)
            m_ssrcToUser.insert(audioSsrc, userId);

        if (videoSsrc != 0) {
            m_videoSsrcToUser.insert(videoSsrc, userId);
            wlog(QStringLiteral("video"), QStringLiteral("%1 is sending pictures on stream %2")
                                              .arg(userId)
                                              .arg(videoSsrc));

            // Name it explicitly now that we know it, rather than leaving it
            // to the catch-all.
            sendVideoWants();
            emit videoAvailable(userId, true);
        } else {
            // Zero means they turned it off.
            const auto stale = m_videoSsrcToUser.key(userId, 0);
            if (stale != 0) {
                m_videoSsrcToUser.remove(stale);
                delete m_videoStreams.take(stale);
            }
            emit videoAvailable(userId, false);
        }
        break;
    }

    case OpClientsConnect: {
        // The key group needs everyone in the call by name.
        const QJsonArray ids = data.value(QStringLiteral("user_ids")).toArray();
        for (const QJsonValue &value : ids) {
            const QString userId = value.toString();
            if (!userId.isEmpty())
                m_roster.insert(userId);
        }
        break;
    }

    // --- the text side of an encryption change ---------------------------
    case OpDavePrepareTransition: {
        // A change is coming. Say we are ready for it.
        const qint64 transitionId =
            static_cast<qint64>(data.value(QStringLiteral("transition_id")).toDouble());
        wlog(QStringLiteral("voice"), QStringLiteral("encryption change %1 announced")
                                          .arg(transitionId));
        announceReadyForTransition(transitionId);
        break;
    }

    case OpDaveExecuteTransition:
        // The change is now in force, so the new keys apply from here.
        wlog(QStringLiteral("voice"), QStringLiteral("encryption change now in force"));
        refreshDaveKeys();
        break;

    case OpDavePrepareEpoch: {
        // A brand new group. Publish our key package again so we are included.
        const int version = data.value(QStringLiteral("protocol_version")).toInt();
        wlog(QStringLiteral("voice"), QStringLiteral("new key group, version %1").arg(version));

        if (m_dave && version > 0) {
            m_dave->begin(version, m_channelId.toULongLong(), m_userId);
            const QByteArray package = m_dave->keyPackage();
            if (!package.isEmpty())
                sendBinary(OpDaveMlsKeyPackage, package);
        }
        break;
    }
    default:
        break;
    }
}

void VoiceConnection::onBinaryMessage(const QByteArray &message)
{
    // Discord's shape for these, from its own guide:
    //   2 bytes  sequence number, big endian, server to client only
    //   1 byte   opcode
    //   rest     payload
    if (message.size() < 3) {
        wlog(QStringLiteral("voice"), QStringLiteral("binary message too short (%1 bytes)")
                                          .arg(message.size()));
        return;
    }

    const auto *bytes = reinterpret_cast<const quint8 *>(message.constData());
    const qint64 sequence = (static_cast<qint64>(bytes[0]) << 8) | bytes[1];
    const int opcode = bytes[2];

    // Every server message carries a sequence number, and the heartbeat has to
    // acknowledge the last one seen.
    m_lastSequence = sequence;

    handleDaveMessage(opcode, message.mid(3), sequence);
}

void VoiceConnection::sendBinary(int opcode, const QByteArray &payload)
{
    if (m_socket.state() != QAbstractSocket::ConnectedState)
        return;

    // Client to server messages carry no sequence number, only the opcode.
    QByteArray frame;
    frame.append(static_cast<char>(opcode));
    frame.append(payload);

    m_socket.sendBinaryMessage(frame);
}

// Everyone libdave is allowed to see named in a group message.
//
// This has to include us. A Welcome names every member of the group, and we
// are one of them, so leaving ourselves out is refused with "Welcome message
// lists unrecognized user ID" and the whole call dies.
QSet<QString> VoiceConnection::recognisedUsers() const
{
    QSet<QString> everyone = m_roster;
    if (!m_userId.isEmpty())
        everyone.insert(m_userId);
    return everyone;
}

// Tells the server the commit or welcome it sent could not be used.
//
// The transition it belongs to is required. Sending an empty object is
// refused as a malformed request, which closes the call with code 4020.
void VoiceConnection::sendInvalidCommitWelcome(qint64 transitionId)
{
    wlog(QStringLiteral("voice"),
         QStringLiteral("telling the server change %1 could not be used").arg(transitionId));

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpDaveMlsInvalidCommitWelcome},
        {QStringLiteral("d"), QJsonObject{{QStringLiteral("transition_id"), transitionId}}},
    });
}

void VoiceConnection::announceReadyForTransition(qint64 transitionId)
{
    // Tells the server we have the keys for the upcoming change and it may go
    // ahead. Without this the call stays silent.
    sendJson(QJsonObject{
        {QStringLiteral("op"), OpDaveTransitionReady},
        {QStringLiteral("d"), QJsonObject{{QStringLiteral("transition_id"), transitionId}}},
    });
}

void VoiceConnection::refreshDaveKeys()
{
    if (!m_dave || !m_dave->isActive())
        return;

    if (!m_dave->applyKeys(m_userId, m_roster))
        wlog(QStringLiteral("voice"), QStringLiteral("keys are not ready yet"));
}

void VoiceConnection::handleDaveMessage(int opcode, const QByteArray &payload, qint64 sequence)
{
    Q_UNUSED(sequence)

    if (!m_dave || !m_dave->isActive()) {
        wlog(QStringLiteral("voice"), QStringLiteral("ignoring encryption op %1, no group open")
                                          .arg(opcode));
        return;
    }

    switch (opcode) {
    case OpDaveMlsExternalSender: {
        // Who may speak for the group. Once that is known we can publish our
        // own key package and ask to be let in.
        m_dave->setExternalSender(payload);

        const QByteArray package = m_dave->keyPackage();
        if (!package.isEmpty())
            sendBinary(OpDaveMlsKeyPackage, package);
        break;
    }

    case OpDaveMlsProposals: {
        // People joining or leaving. Our answer carries a commit, and a
        // welcome for anyone we are letting in.
        const QByteArray reply = m_dave->processProposals(payload, recognisedUsers());
        if (!reply.isEmpty())
            sendBinary(OpDaveMlsCommitWelcome, reply);
        break;
    }

    case OpDaveMlsAnnounceCommitTransition: {
        // The first two bytes name the change this commit belongs to.
        if (payload.size() < 2)
            break;

        const auto *bytes = reinterpret_cast<const quint8 *>(payload.constData());
        const qint64 transitionId = (static_cast<qint64>(bytes[0]) << 8) | bytes[1];

        if (m_dave->processCommit(payload.mid(2))) {
            refreshDaveKeys();
            announceReadyForTransition(transitionId);
        } else {
            // Rejected. Asking to be added again is the documented way out.
            sendInvalidCommitWelcome(transitionId);
        }
        break;
    }

    case OpDaveMlsWelcome: {
        if (payload.size() < 2)
            break;

        const auto *bytes = reinterpret_cast<const quint8 *>(payload.constData());
        const qint64 transitionId = (static_cast<qint64>(bytes[0]) << 8) | bytes[1];

        if (m_dave->processWelcome(payload.mid(2), recognisedUsers())) {
            refreshDaveKeys();
            announceReadyForTransition(transitionId);
        } else {
            sendInvalidCommitWelcome(transitionId);
        }
        break;
    }

    default:
        wlog(QStringLiteral("voice"), QStringLiteral("unhandled encryption op %1").arg(opcode));
        break;
    }
}

void VoiceConnection::handleReady(const QJsonObject &data)
{
    m_ssrc = static_cast<quint32>(data.value(QStringLiteral("ssrc")).toDouble());
    m_serverAddress = data.value(QStringLiteral("ip")).toString();
    m_serverPort = static_cast<quint16>(data.value(QStringLiteral("port")).toInt());

    m_offeredModes.clear();
    const QJsonArray modes = data.value(QStringLiteral("modes")).toArray();
    for (const QJsonValue &value : modes)
        m_offeredModes.append(value.toString());

    wlog(QStringLiteral("voice"), QStringLiteral("ready: ssrc=%1 server=%2:%3 modes=[%4]")
                                      .arg(m_ssrc)
                                      .arg(m_serverAddress)
                                      .arg(m_serverPort)
                                      .arg(m_offeredModes.join(QStringLiteral(", "))));

    m_stage = QStringLiteral("asking what our own address looks like");
    beginIpDiscovery();
}

void VoiceConnection::handleSessionDescription(const QJsonObject &data)
{
    m_mode = data.value(QStringLiteral("mode")).toString();

    m_secretKey.clear();
    const QJsonArray key = data.value(QStringLiteral("secret_key")).toArray();
    for (const QJsonValue &value : key)
        m_secretKey.append(static_cast<char>(value.toInt()));

    wlog(QStringLiteral("voice"), QStringLiteral("session ready: mode=%1 key=%2 bytes")
                                      .arg(m_mode).arg(m_secretKey.size()));

    if (m_secretKey.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
        emit failed(QStringLiteral("Discord sent a key of an unexpected size."));
        setState(State::Failed);
        return;
    }

    // Discord names the encryption version for this call here. Anything above
    // zero means every frame must be end-to-end encrypted.
    m_daveVersion = data.value(QStringLiteral("dave_protocol_version")).toInt();
    if (m_daveVersion > 0) {
        wlog(QStringLiteral("voice"), QStringLiteral("this call is end-to-end encrypted, version %1")
                                          .arg(m_daveVersion));

        if (!m_dave->begin(m_daveVersion, m_channelId.toULongLong(), m_userId)) {
            emit failed(QStringLiteral("could not start the encryption group"));
            setState(State::Failed);
            return;
        }

        // The group's authority arrives next, as opcode 25, and our key
        // package goes back once we have it.
    }

    m_handshakeWatchdog.stop();
    m_stage = QStringLiteral("connected");
    setState(State::Connected);
    startAudio();

    // Say what we send, then ask for what we want. Without the second of
    // these the server sends no video at all, and every camera stays blank.
    sendVideoState();
    sendVideoWants();
}

void VoiceConnection::handleSpeaking(const QJsonObject &data)
{
    const QString userId = data.value(QStringLiteral("user_id")).toString();
    const auto ssrc = static_cast<quint32>(data.value(QStringLiteral("ssrc")).toDouble());
    const int flags = data.value(QStringLiteral("speaking")).toInt();

    if (!userId.isEmpty() && ssrc != 0)
        m_ssrcToUser.insert(ssrc, userId);

    if (!userId.isEmpty() && userId != m_userId)
        emit speakingChanged(userId, flags != 0);
}

// ---------------------------------------------------------------------------
// Finding our own address
// ---------------------------------------------------------------------------

void VoiceConnection::beginIpDiscovery()
{
    if (!m_udp.bind(QHostAddress::AnyIPv4, 0)) {
        emit failed(QStringLiteral("Could not open a UDP socket for voice."));
        setState(State::Failed);
        return;
    }

    // Discord answers this with the address and port it sees us on, which is
    // what has to go in select protocol.
    QByteArray packet(74, '\0');
    auto *bytes = reinterpret_cast<quint8 *>(packet.data());
    bytes[0] = 0x00;
    bytes[1] = 0x01;   // type 1: request
    bytes[2] = 0x00;
    bytes[3] = 70;     // length of everything after these four bytes
    bytes[4] = static_cast<quint8>(m_ssrc >> 24);
    bytes[5] = static_cast<quint8>(m_ssrc >> 16);
    bytes[6] = static_cast<quint8>(m_ssrc >> 8);
    bytes[7] = static_cast<quint8>(m_ssrc & 0xFF);

    m_udp.writeDatagram(packet, QHostAddress(m_serverAddress), m_serverPort);
}

bool VoiceConnection::readIpDiscoveryReply(const QByteArray &packet, QString &address, quint16 &port)
{
    if (packet.size() < 74)
        return false;

    const auto *bytes = reinterpret_cast<const quint8 *>(packet.constData());
    if (bytes[0] != 0x00 || bytes[1] != 0x02)
        return false;

    // Address is null padded text starting at byte 8.
    address = QString::fromLatin1(packet.constData() + 8, qstrnlen(packet.constData() + 8, 64));
    port = static_cast<quint16>((bytes[72] << 8) | bytes[73]);
    return true;
}

void VoiceConnection::sendSelectProtocol(const QString &address, quint16 port)
{
    // The only scheme implemented here. Discord has offered it everywhere for
    // years, but ask rather than assume: picking a mode the server did not
    // offer is closed with code 4016.
    const QString wanted = QStringLiteral("aead_xchacha20_poly1305_rtpsize");

    if (!m_offeredModes.isEmpty() && !m_offeredModes.contains(wanted)) {
        wlog(QStringLiteral("voice"), QStringLiteral("server does not offer %1").arg(wanted));
        emit failed(QStringLiteral("This voice server wants an encryption scheme Wisp does not have."));
        setState(State::Failed);
        return;
    }

    wlog(QStringLiteral("voice"), QStringLiteral("our address looks like %1:%2, choosing %3")
                                      .arg(address).arg(port).arg(wanted));
    m_stage = QStringLiteral("waiting for the encryption key");

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpSelectProtocol},
        {QStringLiteral("d"),
         QJsonObject{
             {QStringLiteral("protocol"), QStringLiteral("udp")},
             {QStringLiteral("data"),
              QJsonObject{
                  {QStringLiteral("address"), address},
                  {QStringLiteral("port"), port},
                  {QStringLiteral("mode"), wanted},
              }},
         }},
    });
}

// ---------------------------------------------------------------------------
// Encryption
// ---------------------------------------------------------------------------

QByteArray VoiceConnection::encryptFrame(const QByteArray &rtpHeader, const QByteArray &opusFrame)
{
    // The nonce is 24 bytes, but only the first four travel with the packet.
    // The rest are zeros that both sides agree on.
    unsigned char nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
    std::memset(nonce, 0, sizeof(nonce));

    const quint32 counter = ++m_nonceCounter;
    nonce[0] = static_cast<unsigned char>(counter >> 24);
    nonce[1] = static_cast<unsigned char>(counter >> 16);
    nonce[2] = static_cast<unsigned char>(counter >> 8);
    nonce[3] = static_cast<unsigned char>(counter & 0xFF);

    QByteArray cipher(opusFrame.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long cipherLength = 0;

    const int result = crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char *>(cipher.data()), &cipherLength,
        reinterpret_cast<const unsigned char *>(opusFrame.constData()), opusFrame.size(),
        reinterpret_cast<const unsigned char *>(rtpHeader.constData()), rtpHeader.size(),
        nullptr, nonce, reinterpret_cast<const unsigned char *>(m_secretKey.constData()));

    if (result != 0)
        return {};

    cipher.resize(static_cast<int>(cipherLength));

    QByteArray packet = rtpHeader;
    packet.append(cipher);
    packet.append(reinterpret_cast<const char *>(nonce), NonceTailSize);
    return packet;
}

bool VoiceConnection::decryptFrame(const QByteArray &packet, QByteArray &opusFrame, quint32 &ssrc,
                                   bool *marker)
{
    // The top bit of the second byte marks the last packet of a video frame.
    // Sound does not use it, and video cannot be rebuilt without it.
    if (marker)
        *marker = packet.size() > 1 && (static_cast<quint8>(packet[1]) & 0x80) != 0;

    int extensionBodyBytes = 0;
    const int headerLength = clearHeaderLength(packet, &extensionBodyBytes);
    if (headerLength < RtpHeaderSize)
        return false;

    const int minimum = headerLength + crypto_aead_xchacha20poly1305_ietf_ABYTES + NonceTailSize;
    if (packet.size() < minimum)
        return false;

    const auto *bytes = reinterpret_cast<const quint8 *>(packet.constData());
    ssrc = (static_cast<quint32>(bytes[8]) << 24) | (static_cast<quint32>(bytes[9]) << 16)
        | (static_cast<quint32>(bytes[10]) << 8) | static_cast<quint32>(bytes[11]);

    unsigned char nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
    std::memset(nonce, 0, sizeof(nonce));
    std::memcpy(nonce, packet.constData() + packet.size() - NonceTailSize, NonceTailSize);

    const int cipherLength = packet.size() - headerLength - NonceTailSize;

    opusFrame.resize(cipherLength);
    unsigned long long plainLength = 0;

    const int result = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char *>(opusFrame.data()), &plainLength, nullptr,
        reinterpret_cast<const unsigned char *>(packet.constData() + headerLength), cipherLength,
        reinterpret_cast<const unsigned char *>(packet.constData()), headerLength,
        nonce, reinterpret_cast<const unsigned char *>(m_secretKey.constData()));

    if (result != 0)
        return false;

    opusFrame.resize(static_cast<int>(plainLength));

    // The extension elements were encrypted along with the sound, so they are
    // sitting at the front of what just came out. Step over them.
    if (extensionBodyBytes > 0) {
        if (opusFrame.size() < extensionBodyBytes)
            return false;
        opusFrame.remove(0, extensionBodyBytes);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

void VoiceConnection::startAudio()
{
    stopAudio();

    int error = 0;
    m_encoder = opus_encoder_create(SampleRate, Channels, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !m_encoder) {
        emit failed(QStringLiteral("Could not start the Opus encoder."));
        return;
    }
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(64000));
    opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(m_encoder, OPUS_SET_PACKET_LOSS_PERC(10));

    const QAudioFormat format = voiceFormat();

    m_input = new QAudioSource(findInput(m_inputDeviceId), format, this);
    m_inputStream = m_input->start();

    m_output = new QAudioSink(findOutput(m_outputDeviceId), format, this);

    // Room for eight frames. Too small and the sound card runs dry between
    // ticks, which is heard as clicking; this must be set before starting.
    m_output->setBufferSize(FrameBytes * 8);
    m_outputStream = m_output->start();

    // The speaker exists now, so whatever the slider was set to can finally
    // be applied to it.
    m_output->setVolume(m_outputVolume / 100.0);

    m_captureBuffer.clear();
    m_rtpSequence = static_cast<quint16>(QRandomGenerator::global()->bounded(65535));
    m_rtpTimestamp = QRandomGenerator::global()->generate();
    m_nonceCounter = 0;

    m_sendTimer.start();
    m_playTimer.start();

    wlog(QStringLiteral("voice"), QStringLiteral("audio running"));
}

void VoiceConnection::stopAudio()
{
    m_sendTimer.stop();
    m_playTimer.stop();

    if (m_input) {
        m_input->stop();
        delete m_input;
        m_input = nullptr;
        m_inputStream = nullptr;
    }
    if (m_output) {
        m_output->stop();
        delete m_output;
        m_output = nullptr;
        m_outputStream = nullptr;
    }
    if (m_encoder) {
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
    }
    for (IncomingStream &stream : m_streams) {
        if (stream.decoder)
            opus_decoder_destroy(stream.decoder);
    }
    m_streams.clear();
    m_captureBuffer.clear();
}

void VoiceConnection::onSendTick()
{
    // Counted first, so the report below still happens when the microphone
    // never opened. That case is silence with a cause worth naming.
    const bool canCapture = m_inputStream && m_encoder && !m_secretKey.isEmpty();

    if (!canCapture) {
        if (++m_statTicks >= 250) {
            m_statTicks = 0;
            wlog(QStringLiteral("voice"),
                 QStringLiteral("no sound is being captured: microphone %1, encoder %2, key %3")
                     .arg(m_inputStream ? QStringLiteral("open") : QStringLiteral("missing"))
                     .arg(m_encoder ? QStringLiteral("ready") : QStringLiteral("missing"))
                     .arg(m_secretKey.isEmpty() ? QStringLiteral("missing") : QStringLiteral("held")));
            reportAudioStats();
        }
        return;
    }

    m_captureBuffer.append(m_inputStream->readAll());

    while (m_captureBuffer.size() >= FrameBytes) {
        QByteArray frame = m_captureBuffer.left(FrameBytes);
        m_captureBuffer.remove(0, FrameBytes);

        auto *samples = reinterpret_cast<qint16 *>(frame.data());
        const int count = FrameBytes / 2;

        // The slider first, so a plugin is handed sound at the level you set
        // rather than whatever the microphone happened to produce.
        for (int i = 0; i < count; ++i) {
            const double value = qBound(-32768.0, samples[i] * (m_inputVolume / 100.0), 32767.0);
            samples[i] = static_cast<qint16>(value);
        }

        // Plugins get the sound next, and may rewrite it entirely.
        if (m_micProcessor)
            m_micProcessor(samples, FrameSamples, Channels, SampleRate);

        // Loudness is measured afterwards, on what will actually be sent. A
        // plugin holding back quiet sound should make you count as silent, and
        // one boosting it should make you count as talking; measuring first
        // would ignore both.
        double sum = 0.0;
        for (int i = 0; i < count; ++i) {
            const double normalised = samples[i] / 32768.0;
            sum += normalised * normalised;
        }
        const double level = std::sqrt(sum / count);
        const bool loudEnough = level * 3.0 >= (m_sensitivity / 100.0);

        if (m_muted) {
            if (m_speaking) {
                m_speaking = false;
                sendSpeaking(false);
            }
            continue;
        }

        if (loudEnough) {
            m_silentFrames = 0;
            if (!m_speaking) {
                m_speaking = true;
                sendSpeaking(true);
            }
        } else if (m_speaking) {
            // Keep going briefly so the end of a word is not cut off.
            if (++m_silentFrames > SilenceFramesBeforeStop) {
                m_speaking = false;
                sendSpeaking(false);
            }
        }

        m_rtpTimestamp += FrameSamples;
        m_rtpSequence++;

        if (!m_speaking)
            continue;

        unsigned char encoded[4000];
        const int encodedBytes = opus_encode(m_encoder, samples, FrameSamples, encoded, sizeof(encoded));
        if (encodedBytes <= 0)
            continue;

        QByteArray voice(reinterpret_cast<const char *>(encoded), encodedBytes);

        // On an encrypted call the sound is sealed for the other people first,
        // so Discord's servers only ever carry something they cannot read.
        // The transport encryption below is applied on top of that.
        if (m_daveVersion > 0) {
            voice = m_dave->encrypt(voice, m_ssrc);
            if (voice.isEmpty()) {
                ++m_statSealFailed;   // keys not ready, stay quiet rather than leak
                continue;
            }
        }

        const QByteArray header = buildRtpHeader(m_rtpSequence, m_rtpTimestamp, m_ssrc);
        const QByteArray packet = encryptFrame(header, voice);
        if (packet.isEmpty())
            continue;

        m_udp.writeDatagram(packet, QHostAddress(m_serverAddress), m_serverPort);
        ++m_statSent;
    }

    // Every 250 ticks is five seconds.
    if (++m_statTicks >= 250) {
        m_statTicks = 0;
        reportAudioStats();
    }
}

// Says, in one line, what became of the sound in the last five seconds.
//
// Silence is only written down when something was actually thrown away, so a
// quiet call does not fill the log.
void VoiceConnection::reportAudioStats()
{
    const int lost = m_statSealFailed + m_statOpenFailed + m_statNoOwner + m_statUndecryptable;
    if (m_statSent == 0 && m_statPlayed == 0 && lost == 0)
        return;

    QString line = QStringLiteral("sent %1, played %2").arg(m_statSent).arg(m_statPlayed);
    if (m_statSealFailed > 0)
        line += QStringLiteral(", %1 of ours unsealed (no group key)").arg(m_statSealFailed);
    if (m_statNoOwner > 0)
        line += QStringLiteral(", %1 from an unknown speaker").arg(m_statNoOwner);
    if (m_statOpenFailed > 0)
        line += QStringLiteral(", %1 we could not open").arg(m_statOpenFailed);
    if (m_statUndecryptable > 0)
        line += QStringLiteral(", %1 rejected by the transport").arg(m_statUndecryptable);

    wlog(QStringLiteral("voice"), line);

    // Pictures, counted the same way and for the same reason: a black tile
    // needs to say whether nothing arrived, or whether it arrived and could
    // not be turned into a picture.
    for (auto it = m_videoStreams.begin(); it != m_videoStreams.end(); ++it) {
        VideoStream *stream = it.value();
        if (stream->packets == 0)
            continue;

        wlog(QStringLiteral("video"),
             QStringLiteral("%1: %2 packets, %3 pictures, %4 incomplete, %5 waiting on a keyframe")
                 .arg(m_videoSsrcToUser.value(it.key(), QStringLiteral("unknown sender")))
                 .arg(stream->packets)
                 .arg(stream->frames)
                 .arg(stream->dropped)
                 .arg(stream->decoder.hungryFrames()));

        stream->packets = 0;
        stream->frames = 0;
        stream->dropped = 0;
    }

    m_statSent = 0;
    m_statPlayed = 0;
    m_statSealFailed = 0;
    m_statOpenFailed = 0;
    m_statNoOwner = 0;
    m_statUndecryptable = 0;
}

void VoiceConnection::onUdpReadyRead()
{
    while (m_udp.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_udp.receiveDatagram();
        const QByteArray packet = datagram.data();

        // Before the key arrives, the only thing expected is the reply telling
        // us what our own address looks like.
        if (m_secretKey.isEmpty()) {
            QString address;
            quint16 port = 0;
            if (readIpDiscoveryReply(packet, address, port))
                sendSelectProtocol(address, port);
            continue;
        }

        // Sound and pictures share this socket and are told apart by the
        // payload type in the second byte. Deafening silences the sound and
        // leaves the pictures alone, the same as the real client.
        const bool isVideo = packet.size() > 1
            && (static_cast<quint8>(packet[1]) & 0x7F) == VideoPayloadType;

        if (m_deafened && !isVideo)
            continue;

        QByteArray payload;
        quint32 ssrc = 0;
        bool marker = false;
        if (!decryptFrame(packet, payload, ssrc, &marker)) {
            ++m_statUndecryptable;
            continue;
        }

        if (isVideo) {
            handleVideoPacket(ssrc, payload, marker);
            continue;
        }

        // Discord sends tiny keep alive frames that decode to nothing useful.
        if (payload.size() < 3)
            continue;

        playDecoded(ssrc, payload);
    }
}

// Rebuilds one picture out of the packets carrying it, then decodes it.
//
// A video frame is far too big for one packet, so H.264 is chopped up on the
// way out and has to be glued back together here. Three shapes arrive:
//
//   - a whole small part on its own
//   - several small parts bundled into one packet
//   - one large part split across many packets
//
// The last packet of a picture is flagged, which is how we know the picture is
// complete and can be handed to the decoder.
VoiceConnection::VideoStream *VoiceConnection::videoStreamFor(quint32 ssrc)
{
    const auto it = m_videoStreams.constFind(ssrc);
    if (it != m_videoStreams.constEnd())
        return it.value();

    auto *stream = new VideoStream;
    m_videoStreams.insert(ssrc, stream);
    return stream;
}

void VoiceConnection::clearVideoStreams()
{
    qDeleteAll(m_videoStreams);
    m_videoStreams.clear();
    m_videoSsrcToUser.clear();
}

void VoiceConnection::handleVideoPacket(quint32 ssrc, const QByteArray &payload, bool endOfFrame)
{
    if (payload.isEmpty())
        return;

    const QString userId = m_videoSsrcToUser.value(ssrc);

    VideoStream &stream = *videoStreamFor(ssrc);
    ++stream.packets;

    const auto *bytes = reinterpret_cast<const quint8 *>(payload.constData());
    const int kind = bytes[0] & 0x1F;

    // Every part is written with a four byte marker in front, which is how the
    // decoder finds where each one begins.
    static const QByteArray startCode = QByteArray::fromHex("00000001");

    if (kind >= 1 && kind <= 23) {
        // A whole part, on its own.
        stream.assembling += startCode;
        stream.assembling += payload;
    } else if (kind == 24) {
        // Several small parts bundled together, each behind its own length.
        int offset = 1;
        while (offset + 2 <= payload.size()) {
            const int size = (static_cast<quint8>(payload[offset]) << 8)
                | static_cast<quint8>(payload[offset + 1]);
            offset += 2;
            if (size <= 0 || offset + size > payload.size())
                break;
            stream.assembling += startCode;
            stream.assembling += payload.mid(offset, size);
            offset += size;
        }
    } else if (kind == 28) {
        // One large part, split. The first packet carries a start flag and
        // the real type; the header has to be rebuilt from the two bytes.
        if (payload.size() < 3)
            return;

        const quint8 indicator = bytes[0];
        const quint8 fragment = bytes[1];
        const bool first = (fragment & 0x80) != 0;
        const int realKind = fragment & 0x1F;

        if (first) {
            stream.assembling += startCode;
            stream.assembling += static_cast<char>((indicator & 0xE0) | realKind);
        } else if (stream.assembling.isEmpty()) {
            // Joined a stream part way through a part. Nothing useful can be
            // built from the middle, so wait for the next beginning.
            return;
        }

        stream.assembling += payload.mid(2);
    } else {
        // Types 25 to 27 and 29 exist and Discord does not send them.
        return;
    }

    if (!endOfFrame || stream.assembling.isEmpty())
        return;

    const QByteArray picture = stream.assembling;
    stream.assembling.clear();

    if (!stream.decoder.isOpen() && !stream.decoder.open())
        return;

    QImage image;
    if (!stream.decoder.decode(picture, image)) {
        ++stream.dropped;
        return;
    }

    ++stream.frames;
    if (!userId.isEmpty())
        emit videoFrame(userId, image);
}

VoiceConnection::IncomingStream *VoiceConnection::streamFor(quint32 ssrc)
{
    const auto it = m_streams.find(ssrc);
    if (it != m_streams.end())
        return &it.value();

    int error = 0;
    OpusDecoder *decoder = opus_decoder_create(SampleRate, Channels, &error);
    if (error != OPUS_OK || !decoder)
        return nullptr;

    IncomingStream stream;
    stream.decoder = decoder;
    return &m_streams.insert(ssrc, stream).value();
}

// Hands one 20 millisecond slice of sound to the speakers, every 20
// milliseconds, no matter how unevenly the network delivered it.
//
// Two things happen here that cannot happen packet by packet:
//
//   - Several people talking at once are added together. Writing two streams
//     to the sound card one after the other does not mix them, it queues them,
//     which is heard as chopped up and rushed speech.
//   - A frame that has not arrived yet is replaced by Opus's own guess at what
//     it would have been, so a late packet is a soft blur instead of a click.
void VoiceConnection::onPlayTick()
{
    if (!m_outputStream || m_streams.isEmpty())
        return;

    // Never run ahead of the sound card.
    if (m_output && m_output->bytesFree() < FrameBytes)
        return;

    qint32 mixed[FrameSamples * Channels] = {0};
    bool anyone = false;

    for (auto it = m_streams.begin(); it != m_streams.end(); ++it) {
        IncomingStream &stream = it.value();

        // Wait for a small cushion before starting, so the first moments are
        // not immediately starved.
        if (!stream.started) {
            if (stream.waiting.size() < JitterFrames)
                continue;
            stream.started = true;
        }

        QByteArray frame;
        if (!stream.waiting.isEmpty()) {
            frame = stream.waiting.takeFirst();
        } else {
            // Nothing arrived in time. Let Opus invent a plausible frame
            // rather than leaving a hole.
            frame = QByteArray(FrameBytes, '\0');
            const int samples = opus_decode(stream.decoder, nullptr, 0,
                                            reinterpret_cast<qint16 *>(frame.data()), FrameSamples, 0);
            if (samples <= 0) {
                stream.started = false;   // rebuild the cushion before resuming
                continue;
            }
            frame.resize(samples * Channels * 2);
        }

        const auto *samples = reinterpret_cast<const qint16 *>(frame.constData());
        const int count = qMin<int>(frame.size() / 2, FrameSamples * Channels);
        for (int i = 0; i < count; ++i)
            mixed[i] += samples[i];

        anyone = true;
    }

    if (!anyone)
        return;

    QByteArray out(FrameBytes, '\0');
    auto *target = reinterpret_cast<qint16 *>(out.data());
    for (int i = 0; i < FrameSamples * Channels; ++i)
        target[i] = static_cast<qint16>(qBound(-32768, mixed[i], 32767));

    m_outputStream->write(out);
    ++m_statPlayed;
}

void VoiceConnection::playDecoded(quint32 ssrc, const QByteArray &frame)
{
    IncomingStream *stream = streamFor(ssrc);
    if (!stream || !m_outputStream)
        return;

    QByteArray opusFrame = frame;

    // On an encrypted call the sender sealed this for the group, so it has to
    // be opened with their own key before it can be decoded.
    if (m_daveVersion > 0) {
        const QString userId = m_ssrcToUser.value(ssrc);
        if (userId.isEmpty()) {
            ++m_statNoOwner;   // we do not know whose stream this is yet
            return;
        }

        opusFrame = m_dave->decrypt(userId, opusFrame);
        if (opusFrame.isEmpty()) {
            ++m_statOpenFailed;
            return;
        }
    }

    QByteArray pcm(FrameBytes, '\0');
    const int samples = opus_decode(stream->decoder,
                                    reinterpret_cast<const unsigned char *>(opusFrame.constData()),
                                    opusFrame.size(), reinterpret_cast<qint16 *>(pcm.data()),
                                    FrameSamples, 0);
    if (samples <= 0)
        return;

    pcm.resize(samples * Channels * 2);

    // Queued, not played. The steady tick takes it from here.
    //
    // A queue that keeps growing means sound arriving faster than it is
    // played, which would turn into a delay that never recovers. Past a
    // half second the oldest is dropped: a small gap now beats talking to
    // someone who hears you late for the rest of the call.
    stream->waiting.append(pcm);
    while (stream->waiting.size() > MaxQueuedFrames)
        stream->waiting.removeFirst();
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

// Tells the server what we are sending, which for us is sound and nothing
// else. It is also how a client says it understands video at all.
void VoiceConnection::sendVideoState()
{
    sendJson(QJsonObject{
        {QStringLiteral("op"), OpVideo},
        {QStringLiteral("d"),
         QJsonObject{
             {QStringLiteral("audio_ssrc"), static_cast<qint64>(m_ssrc)},
             {QStringLiteral("video_ssrc"), 0},   // zero means we send no pictures
             {QStringLiteral("rtx_ssrc"), 0},
             {QStringLiteral("streams"), QJsonArray{}},
         }},
    });
}

// Asks for other people's video.
//
// This is the piece whose absence made every camera invisible. Discord does
// not push video at a client that has not asked for it: the server waits to be
// told which streams are wanted and how good they should be. A client that
// never sends this is treated as wanting none, which is exactly what we got.
//
// Values run from 0, meaning do not send this at all, to 100, the best
// available. "any" covers anyone whose stream we have not named yet, so a
// camera switched on later is not missed while we wait to hear about it.
void VoiceConnection::sendVideoWants()
{
    QJsonObject wants;
    wants.insert(QStringLiteral("any"), 100);

    for (auto it = m_videoSsrcToUser.constBegin(); it != m_videoSsrcToUser.constEnd(); ++it)
        wants.insert(QString::number(it.key()), 100);

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpMediaSinkWants},
        {QStringLiteral("d"), wants},
    });

    wlog(QStringLiteral("video"),
         QStringLiteral("asked for everyone's pictures (%1 named)").arg(m_videoSsrcToUser.size()));
}

void VoiceConnection::sendSpeaking(bool speaking)
{
    sendJson(QJsonObject{
        {QStringLiteral("op"), OpSpeaking},
        {QStringLiteral("d"),
         QJsonObject{
             {QStringLiteral("speaking"), speaking ? 1 : 0},
             {QStringLiteral("delay"), 0},
             {QStringLiteral("ssrc"), static_cast<qint64>(m_ssrc)},
         }},
    });

    if (!m_userId.isEmpty())
        emit speakingChanged(m_userId, speaking);
}

void VoiceConnection::setMuted(bool muted)
{
    m_muted = muted;
    if (muted && m_speaking) {
        m_speaking = false;
        sendSpeaking(false);
    }
}

void VoiceConnection::setDeafened(bool deafened)
{
    m_deafened = deafened;
}

void VoiceConnection::setOutputVolume(int percent)
{
    // Remembered, not just handed to the speaker.
    //
    // This used to set the volume and keep nothing. Settings are applied just
    // before a call is built, when there is no speaker yet, so the number went
    // to a null pointer check and was lost. Moving the slider did nothing at
    // any point in the call, which is exactly what it looked like.
    m_outputVolume = qBound(0, percent, 200);

    if (m_output)
        m_output->setVolume(m_outputVolume / 100.0);
}
