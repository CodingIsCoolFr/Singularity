#include "core/VoiceConnection.h"

#include "core/DaveSession.h"
#include "core/JitterBuffer.h"
#include "core/Logger.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMediaDevices>
#include <QMetaObject>
#include <QNetworkDatagram>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QSet>
#include <QThread>
#include <QTransform>
#include <QUuid>

#include <opus/opus.h>
#include <sodium.h>

#ifdef SINGULARITY_HAVE_DAVE
#include <dave/dave.h>
#endif

#include <cmath>
#include <cstring>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#endif

namespace {

// Voice gateway opcodes.
constexpr int OpIdentify = 0;
constexpr int OpSelectProtocol = 1;
constexpr int OpReady = 2;
constexpr int OpHeartbeat = 3;
constexpr int OpSessionDescription = 4;
constexpr int OpSpeaking = 5;
constexpr int OpHeartbeatAck = 6;
constexpr int OpResume = 7;
constexpr int OpHello = 8;
constexpr int OpResumed = 9;
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

// How much of someone's sound is held back before it plays is no longer a
// constant here. It used to be two frames to start and three at most, with
// whole frames thrown away past that - fixed, so it chopped on a bad line and
// clicked every time it caught up. Each person now has a JitterBuffer that
// measures their line and picks its own cushion; see JitterBuffer.h.

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

// The same header, for a picture.
//
// Three things differ from sound. The payload type says H.264 rather than
// Opus, the marker bit is set on the last packet of each picture, which is
// the only way the far end can know a picture is complete, because one picture
// is almost always several packets, and the extension bit is set, because a
// picture packet carries a short header extension the far end reads before the
// picture itself.
QByteArray buildVideoRtpHeader(quint16 sequence, quint32 timestamp, quint32 ssrc, bool marker)
{
    QByteArray header = buildRtpHeader(sequence, timestamp, ssrc);
    auto *bytes = reinterpret_cast<quint8 *>(header.data());

    bytes[0] = 0x90;   // version 2, extension present
    bytes[1] = static_cast<quint8>(VideoPayloadType);
    if (marker)
        bytes[1] |= 0x80;

    return header;
}

// What rides in front of a picture, in the one-byte extension form Discord
// uses on UDP. The four-byte preamble (the 0xBEDE marker and the length)
// stays in the clear with the RTP header. These elements do not: they are
// sealed with the picture and the far end steps over them after opening the
// packet.
//
//   5  a transport sequence, two bytes, counted per packet
//   6  how long the receiver may wait before drawing. Discord expects this
//      on every picture packet. Zero and zero means "do not hold it".
//   7  whether this is a camera or a screen. One means a screen. A client
//      that only has this byte to go on will otherwise not show the share.
//   11 which quality layer, the text "100", the only layer we send, and the
//      same name used when the stream was offered
//
// Thirteen bytes of elements, padded with zeros out to four 32-bit words,
// which is what the length in the preamble counts.
QByteArray videoExtensionBody(quint16 sequence, bool screen)
{
    QByteArray body;
    body.reserve(16);

    body.append(static_cast<char>(0x51));   // id 5, two bytes
    body.append(static_cast<char>(sequence >> 8));
    body.append(static_cast<char>(sequence & 0xFF));

    body.append(static_cast<char>(0x62));   // id 6, three bytes, min 0 max 0
    body.append(static_cast<char>(0));
    body.append(static_cast<char>(0));
    body.append(static_cast<char>(0));

    body.append(static_cast<char>(0x70));   // id 7, one byte
    body.append(static_cast<char>(screen ? 1 : 0));

    body.append(static_cast<char>(0xB2));   // id 11, three bytes
    body.append('1');
    body.append('0');
    body.append('0');

    body.append(static_cast<char>(0));      // pad to 16 bytes, four words
    body.append(static_cast<char>(0));
    body.append(static_cast<char>(0));

    return body;
}

QByteArray videoExtensionPreamble()
{
    QByteArray preamble(4, '\0');
    auto *bytes = reinterpret_cast<quint8 *>(preamble.data());
    bytes[0] = 0xBE;
    bytes[1] = 0xDE;
    bytes[2] = 0;
    bytes[3] = 4;   // four 32-bit words
    return preamble;
}

// One sealed picture comes back as Annex B: a start code, a piece, a start
// code, a piece, and the group seal hanging off the end of the last piece
// with no start code of its own. RTP wants each piece without its start code,
// and the seal has to stay on the last piece or the far end never finds it.
QList<QByteArray> splitAnnexB(const QByteArray &frame)
{
    const auto *bytes = reinterpret_cast<const quint8 *>(frame.constData());
    const int size = frame.size();

    QList<int> codeAt;
    QList<int> payloadAt;
    for (int i = 0; i + 2 < size; ++i) {
        if (bytes[i] != 0 || bytes[i + 1] != 0 || bytes[i + 2] != 1)
            continue;

        int start = i;
        if (i > 0 && bytes[i - 1] == 0)
            start = i - 1;
        codeAt.append(start);
        payloadAt.append(i + 3);
        i += 2;
    }

    QList<QByteArray> units;
    if (payloadAt.isEmpty()) {
        if (!frame.isEmpty())
            units.append(frame);
        return units;
    }

    for (int i = 0; i < payloadAt.size(); ++i) {
        const int from = payloadAt.at(i);
        const int to = (i + 1 < codeAt.size()) ? codeAt.at(i + 1) : size;
        if (from < to)
            units.append(frame.mid(from, to - from));
    }
    return units;
}

// How big one video packet's payload may be.
//
// The path to Discord is an ordinary internet one, so the whole datagram has
// to fit in about 1200 bytes to survive without being fragmented - and an IP
// fragment lost in transit takes the whole packet with it, which for video
// means a visible tear rather than a hiccup.
//
// What is left for actual picture data is that, less the RTP header, less
// what the transport encryption adds, less what the end-to-end layer adds on
// top of that, less the two byte fragmentation header. The number is
// deliberately conservative: a packet a little smaller than it could be costs
// a fraction of a percent, and one a little too large is dropped.
constexpr int MaxVideoPayload = 1100;

// The 90 kHz clock every H.264 stream is timed on.
constexpr quint32 VideoClockRate = 90000;

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

// Discord puts the camera's orientation in the encrypted one-byte extension,
// the same layout WebRTC uses: the low two bits are quarter-turns clockwise.
// Leaving them unread draws a phone or a sideways webcam on its side.
int videoOrientationDegrees(const QByteArray &extension)
{
    const auto *bytes = reinterpret_cast<const quint8 *>(extension.constData());
    int degrees = 0;
    int index = 0;
    while (index < extension.size()) {
        if (bytes[index] == 0) {
            ++index;
            continue;
        }
        const int id = bytes[index] >> 4;
        const int length = (bytes[index] & 0x0F) + 1;
        if (index + 1 + length > extension.size())
            break;
        if ((id == 4 || id == 13) && length >= 1)
            degrees = (bytes[index + 1] & 0x03) * 90;
        index += 1 + length;
    }
    return degrees;
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

    // Parented, so they follow this object onto the media thread.
    //
    // moveToThread carries an object's children and nothing else. These are
    // plain members, and left alone they would stay on the window's thread:
    // the sockets would deliver there and the timers would refuse to start
    // from here, silently. The same trap once stopped a screen share from
    // sending a single frame. Each member unregisters itself from the parent
    // when it is destroyed, which happens before the parent's own cleanup, so
    // nothing is deleted twice.
    m_socket.setParent(this);
    m_udp.setParent(this);
    m_heartbeatTimer.setParent(this);
    m_sendTimer.setParent(this);
    m_playTimer.setParent(this);
    m_handshakeWatchdog.setParent(this);

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

    m_videoWorker = new VideoDecodeWorker;
    m_videoWorker->moveToThread(&m_videoThread);
    connect(m_videoWorker, &VideoDecodeWorker::frameReady, this,
            [this](quint32 ssrc, const QString &userId, const QImage &image) {
                int rotation = 0;
                if (VideoStream *stream = m_videoStreams.value(ssrc)) {
                    ++stream->frames;
                    stream->hungry = 0;
                    rotation = stream->rotation;
                }
                QImage upright = image;
                if (rotation != 0 && !upright.isNull())
                    upright = upright.transformed(QTransform().rotate(rotation), Qt::SmoothTransformation);
                if (!userId.isEmpty())
                    emit videoFrame(userId, upright);
            });
    connect(m_videoWorker, &VideoDecodeWorker::decodeFailed, this,
            [this](quint32 ssrc, int hungry) {
                VideoStream *stream = m_videoStreams.value(ssrc);
                if (!stream)
                    return;
                ++stream->dropped;
                stream->hungry = hungry;
                if (hungry == 8 || hungry == 40)
                    sendPictureLossIndication(ssrc);
            });
    m_videoThread.start();

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
    if (m_videoThread.isRunning()) {
        m_videoThread.quit();
        m_videoThread.wait(3000);
    }
    delete m_videoWorker;
    m_videoWorker = nullptr;
}

void VoiceConnection::prepareMediaThread()
{
    QThread::currentThread()->setObjectName(QStringLiteral("singularity_media"));

#ifdef Q_OS_WIN
    // The same request Discord's voice engine makes of Windows.
    //
    // "Pro Audio" puts this thread in the Multimedia Class Scheduler, which
    // gives it priority over ordinary work for the few milliseconds each beat
    // needs. Without it a game or a browser tab can push the beat late even
    // when this program is idle. Failing is not fatal - the thread still runs,
    // just without the guarantee - so it is written down and ignored.
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (task) {
        AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
        wlog(QStringLiteral("voice"), QStringLiteral("media thread running with Pro Audio priority"));
    } else {
        wlog(QStringLiteral("voice"),
             QStringLiteral("media thread could not get Pro Audio priority (error %1)")
                 .arg(GetLastError()));
    }
#endif
}

void VoiceConnection::publishChannel(const QString &channelId)
{
    QMutexLocker lock(&m_publicMutex);
    m_publicChannelId = channelId;
}

void VoiceConnection::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    m_publicState.store(static_cast<int>(state));
    emit stateChanged(state);
}

void VoiceConnection::connectToVoice(const QString &guildId, const QString &channelId,
                                     const QString &userId, const QString &sessionId,
                                     const QString &token, const QString &endpoint,
                                     quint64 daveGroupId)
{
    if (postToOwnThread([=, this]() {
            connectToVoice(guildId, channelId, userId, sessionId, token, endpoint, daveGroupId);
        }))
        return;

    disconnectFromVoice();

    m_guildId = guildId;
    m_channelId = channelId;
    publishChannel(channelId);
    m_userId = userId;
    m_sessionId = sessionId;
    m_token = token;
    m_endpoint = endpoint;
    m_daveGroupId = daveGroupId;

    // Fresh connection, fresh counters. A stale sequence number from the last
    // call is rejected as a bad payload.
    m_lastSequence = -1;
    m_resuming = false;
    m_resumeAttempts = 0;
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
    if (postToOwnThread([this]() { disconnectFromVoice(); }))
        return;

    if (m_state == State::Idle)
        return;
    teardown();
    publishChannel(QString());
    setState(State::Idle);
}

void VoiceConnection::teardown()
{
    m_resuming = false;
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
    m_daveGroupId = 0;
    m_experiments.clear();

    if (m_videoWorker && m_videoThread.isRunning()) {
        QMetaObject::invokeMethod(m_videoWorker, [w = m_videoWorker]() { w->reset(); },
                                  Qt::BlockingQueuedConnection);
    }
    clearVideoStreams();

    m_statTicks = 0;
    m_statSent = 0;
    m_statPlayed = 0;
    m_statSealFailed = 0;
    m_statOpenFailed = 0;
    m_statNoOwner = 0;
    m_statUndecryptable = 0;
    m_statVideoPackets = 0;
    m_statVideoDaveFailed = 0;
    m_statVideoUnknownSsrc = 0;

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
                                        "Singularity does not have yet"); break;
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

    // A live call whose voice server closed on us - a restart, a move, a blip
    // on the network - is picked back up with a resume, the way the official
    // client does it. Same server, same ticket, same encryption group, and we
    // never leave the channel, so nobody sees "left" and a "join to create"
    // room is not emptied. Leaving and rejoining (what the window falls back
    // to) took over ten seconds and looked exactly like being kicked.
    //
    // Not for closes that say the session itself is over or was refused:
    // resuming those only gets the same answer again.
    const bool callWasLive = (m_state == State::Connected || m_resuming) && !m_secretKey.isEmpty();
    const bool sessionOver = code == 4004 || code == 4006 || code == 4009 || code == 4011
                             || code == 4012 || code == 4014 || code == 4016 || code == 4017
                             || code == 4020 || code == 4021 || code == 4022;
    if (callWasLive && !sessionOver && m_resumeAttempts < 3) {
        ++m_resumeAttempts;
        m_resuming = true;
        m_stage = QStringLiteral("resuming the voice connection");
        setState(State::Connecting);

        // A quarter of a second, then a half, then one: quick enough to be a
        // hiccup rather than a drop, spaced enough not to hammer a server
        // that is restarting.
        const int delay = 250 << (m_resumeAttempts - 1);
        wlog(QStringLiteral("voice"),
             QStringLiteral("resuming the call (try %1 of 3) in %2 ms, without leaving the channel")
                 .arg(m_resumeAttempts)
                 .arg(delay));

        // If the resume never completes, this gives up and hands the problem
        // to the window, which leaves and rejoins.
        m_handshakeWatchdog.start(12000);

        QTimer::singleShot(delay, this, [this]() {
            if (!m_resuming)
                return;
            QString host = m_endpoint;
            host.remove(QStringLiteral("wss://"));
            m_socket.open(QUrl(QStringLiteral("wss://%1/?v=8").arg(host)));
        });
        return;
    }
    m_resuming = false;

    setState(State::Failed);

    // Some refusals are about the channel or the account, not about this
    // attempt. Trying again just repeats them, so they are marked so the
    // window can stop rather than loop.
    // 4004 bad token, 4017 needs end-to-end encryption, 4020 we sent something
    // wrong, 4021 rate limited, 4022 the call ended.
    //
    // 4014 means this voice socket is dead. Discord sends it for a real kick
    // and also whenever the gateway reconnects. Same token will not come
    // back; MainWindow rejoins the channel once the gateway can talk again.
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

    // The layers we are willing to be sent. Naming a full quality one and a
    // half quality one lets the server drop us to the smaller picture when the
    // connection cannot carry the larger, rather than sending nothing.
    //
    // A Go Live viewer offers `screen` rather than `video`; the server still
    // answers with `video` as the actual media type.
    const QString streamType = m_viewerOnly ? QStringLiteral("screen") : QStringLiteral("video");
    QJsonArray streams;
    streams.append(QJsonObject{{QStringLiteral("type"), streamType},
                               {QStringLiteral("rid"), QStringLiteral("100")},
                               {QStringLiteral("quality"), 100}});
    streams.append(QJsonObject{{QStringLiteral("type"), streamType},
                               {QStringLiteral("rid"), QStringLiteral("50")},
                               {QStringLiteral("quality"), 50}});

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpIdentify},
        {QStringLiteral("d"),
         QJsonObject{
             {QStringLiteral("server_id"), m_guildId},
             {QStringLiteral("user_id"), m_userId},
             {QStringLiteral("session_id"), m_sessionId},
             {QStringLiteral("token"), m_token},

             // "Whether this connection supports video", and it defaults to
             // false. Sending false is a promise never to be sent a picture,
             // which is what we were doing: every camera stayed blank because
             // we had said at sign-in that we could not handle one. It does
             // not mean we intend to send video, only that we understand it.
             {QStringLiteral("video"), true},
             {QStringLiteral("streams"), streams},

             {QStringLiteral("max_dave_protocol_version"), maxDaveProtocolVersion()},
         }},
    });
}

void VoiceConnection::sendResume()
{
    // The same four facts as signing in, minus the rest: which server, which
    // session, the ticket, and the last numbered message we saw, so the server
    // can send anything we missed while the socket was down.
    wlog(QStringLiteral("voice"),
         QStringLiteral("resuming: server=%1 session=%2 seq_ack=%3")
             .arg(m_guildId, m_sessionId)
             .arg(m_lastSequence));

    QJsonObject d{
        {QStringLiteral("server_id"), m_guildId},
        {QStringLiteral("session_id"), m_sessionId},
        {QStringLiteral("token"), m_token},
    };
    if (m_lastSequence >= 0)
        d.insert(QStringLiteral("seq_ack"), m_lastSequence);

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpResume},
        {QStringLiteral("d"), d},
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

        if (m_resuming) {
            m_stage = QStringLiteral("asking the voice server to resume");
            sendResume();
        } else {
            m_stage = QStringLiteral("signing in to the voice server");
            sendIdentify();
        }

        m_heartbeatTimer.start(interval);
        sendHeartbeat();
        break;
    }
    case OpResumed: {
        // Back on the same call. The UDP link, the key and the encryption
        // group were never dropped; only the sound devices were stopped when
        // the socket closed, so they start again.
        //
        // The packet counters carry on from where they were. Starting the
        // encryption nonce again from zero would reuse nonces under the same
        // key, which must never happen.
        const quint16 sequence = m_rtpSequence;
        const quint32 timestamp = m_rtpTimestamp;
        const quint32 nonce = m_nonceCounter;

        m_resuming = false;
        m_resumeAttempts = 0;
        m_handshakeWatchdog.stop();
        m_stage = QStringLiteral("connected");
        setState(State::Connected);
        startAudio();

        m_rtpSequence = sequence;
        m_rtpTimestamp = timestamp;
        m_nonceCounter = nonce;

        wlog(QStringLiteral("voice"), QStringLiteral("call resumed; never left the channel"));
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
        //
        // Simulcast puts each quality on its own SSRC, listed under
        // `streams`. The top-level `video_ssrc` is only one of them.
        const QString userId = data.value(QStringLiteral("user_id")).toString();
        const auto videoSsrc = static_cast<quint32>(data.value(QStringLiteral("video_ssrc")).toDouble());
        const auto audioSsrc = static_cast<quint32>(data.value(QStringLiteral("audio_ssrc")).toDouble());

        if (userId.isEmpty())
            break;

        if (audioSsrc != 0)
            m_ssrcToUser.insert(audioSsrc, userId);

        QSet<quint32> keep;
        if (videoSsrc != 0) {
            keep.insert(videoSsrc);
            if (!m_videoRid.contains(videoSsrc))
                m_videoRid.insert(videoSsrc, 100);
        }

        const QJsonArray streams = data.value(QStringLiteral("streams")).toArray();
        for (const QJsonValue &value : streams) {
            const QJsonObject stream = value.toObject();
            const auto ssrc = static_cast<quint32>(stream.value(QStringLiteral("ssrc")).toDouble());
            const auto rtx = static_cast<quint32>(stream.value(QStringLiteral("rtx_ssrc")).toDouble());
            const bool active = stream.value(QStringLiteral("active")).toBool(true);
            if (rtx != 0)
                m_rtxSsrcs.insert(rtx);
            if (ssrc == 0 || !active)
                continue;
            keep.insert(ssrc);
            const int rid = stream.value(QStringLiteral("rid")).toString().toInt();
            m_videoRid.insert(ssrc, rid > 0 ? rid : 100);
            wlog(QStringLiteral("video"),
                 QStringLiteral("%1 is sending pictures on stream %2 (rid %3)")
                     .arg(userId)
                     .arg(ssrc)
                     .arg(stream.value(QStringLiteral("rid")).toString()));
        }

        // Keep the decoder for any stream that is still live. Throwing it away
        // on every quality update was why a share sat on "waiting for a
        // keyframe" and then drew garbage once one finally arrived.
        const QList<quint32> old = m_videoSsrcToUser.keys(userId);
        for (quint32 ssrc : old) {
            if (keep.contains(ssrc))
                continue;
            m_videoSsrcToUser.remove(ssrc);
            m_videoRid.remove(ssrc);
            delete m_videoStreams.take(ssrc);
        }

        bool sending = false;
        quint32 bestSsrc = 0;
        int bestRid = -1;
        for (quint32 ssrc : keep) {
            m_videoSsrcToUser.insert(ssrc, userId);
            sending = true;
            const int rid = m_videoRid.value(ssrc, 0);
            if (rid >= bestRid) {
                bestRid = rid;
                bestSsrc = ssrc;
            }
        }

        if (bestSsrc != 0)
            m_bestVideoSsrc.insert(userId, bestSsrc);
        else
            m_bestVideoSsrc.remove(userId);

        if (sending && streams.isEmpty()) {
            wlog(QStringLiteral("video"), QStringLiteral("%1 is sending pictures on stream %2")
                                              .arg(userId)
                                              .arg(videoSsrc));
        }

        if (sending) {
            refreshVideoWants();
            emit videoAvailable(userId, true);
        } else {
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
        //
        // Restarted in place, not begun again: see DaveSession::restart. And
        // the same group id the connection started with - a Go Live stream's
        // group is its media session, not the channel.
        const int version = data.value(QStringLiteral("protocol_version")).toInt();
        wlog(QStringLiteral("voice"), QStringLiteral("new key group, version %1").arg(version));

        if (m_dave && version > 0) {
            const quint64 groupId = m_daveGroupId != 0 ? m_daveGroupId : m_channelId.toULongLong();
            m_dave->restart(version, groupId, m_userId);
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

    m_experiments.clear();
    const QJsonArray experiments = data.value(QStringLiteral("experiments")).toArray();
    for (const QJsonValue &value : experiments)
        m_experiments.append(value.toString());

    // The synchronisation sources the server has set aside for our pictures.
    //
    // We used to invent these, on the reasonable-looking guess that Discord
    // numbers them consecutively from the audio one. It does, usually - and
    // "usually" is why a share went out and nobody saw it: packets arriving
    // on a source the server did not allocate are simply dropped, silently,
    // because from its side they belong to nobody.
    //
    // The identify asked for two layers, so the answer describes both. The
    // full quality one is the only one we send.
    m_assignedVideoSsrc = 0;
    m_assignedRtxSsrc = 0;

    const QJsonArray streams = data.value(QStringLiteral("streams")).toArray();
    for (const QJsonValue &value : streams) {
        const QJsonObject stream = value.toObject();
        if (stream.value(QStringLiteral("rid")).toString() != QLatin1String("100"))
            continue;

        m_assignedVideoSsrc = static_cast<quint32>(stream.value(QStringLiteral("ssrc")).toDouble());
        m_assignedRtxSsrc =
            static_cast<quint32>(stream.value(QStringLiteral("rtx_ssrc")).toDouble());
        break;
    }

    if (!streams.isEmpty()) {
        wlog(QStringLiteral("voice"),
             QStringLiteral("the server gave us video ssrc %1 (rtx %2) across %3 layers")
                 .arg(m_assignedVideoSsrc)
                 .arg(m_assignedRtxSsrc)
                 .arg(streams.size()));
    }

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

    const QString videoCodec = data.value(QStringLiteral("video_codec")).toString();
    wlog(QStringLiteral("voice"), QStringLiteral("session ready: mode=%1 key=%2 bytes video=%3")
                                      .arg(m_mode)
                                      .arg(m_secretKey.size())
                                      .arg(videoCodec.isEmpty() ? QStringLiteral("none") : videoCodec));

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

        const quint64 groupId = m_daveGroupId != 0 ? m_daveGroupId : m_channelId.toULongLong();
        if (!m_dave->begin(m_daveVersion, groupId, m_userId)) {
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

    // Three things, in this order, before anything will be sent to us.
    //
    // Discord is explicit that a connection has to announce itself before it
    // may send or receive: at least one speaking payload before any data at
    // all, and at least one video payload before any video. We only ever sent
    // a speaking payload when somebody actually talked, so a listener who sat
    // quietly, or who joined muted, had never announced anything.
    sendSpeaking(false);
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

    if (userId.isEmpty() || userId == m_userId)
        return;

    // A "stopped" is believed at once. A "started" only lights the ring; what
    // keeps it lit is sound actually arriving, so somebody who announces
    // themselves and then says nothing does not glow indefinitely.
    if (flags == 0) {
        if (m_speakingNow.remove(userId))
            emit speakingChanged(userId, false);
        m_lastHeardMs.remove(userId);
        return;
    }

    if (!m_speakingNow.contains(userId)) {
        m_speakingNow.insert(userId);
        emit speakingChanged(userId, true);
    }
    m_lastHeardMs.insert(userId, QDateTime::currentMSecsSinceEpoch());
}

void VoiceConnection::noteSpeaking(quint32 ssrc)
{
    const QString userId = m_ssrcToUser.value(ssrc);
    if (userId.isEmpty() || userId == m_userId)
        return;

    m_lastHeardMs.insert(userId, QDateTime::currentMSecsSinceEpoch());

    if (!m_speakingNow.contains(userId)) {
        m_speakingNow.insert(userId);
        emit speakingChanged(userId, true);
    }
}

void VoiceConnection::sweepSpeaking()
{
    if (m_speakingNow.isEmpty())
        return;

    // Long enough to ride over the gaps between packets on a poor connection,
    // short enough that the ring goes out when somebody stops rather than a
    // beat later.
    constexpr qint64 QuietForMs = 400;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    const QStringList talking = m_speakingNow.values();
    for (const QString &userId : talking) {
        if (now - m_lastHeardMs.value(userId, 0) < QuietForMs)
            continue;

        m_speakingNow.remove(userId);
        m_lastHeardMs.remove(userId);
        emit speakingChanged(userId, false);
    }
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
        emit failed(QStringLiteral("This voice server wants an encryption scheme Singularity does not have."));
        setState(State::Failed);
        return;
    }

    wlog(QStringLiteral("voice"), QStringLiteral("our address looks like %1:%2, choosing %3")
                                      .arg(address).arg(port).arg(wanted));
    m_stage = QStringLiteral("waiting for the encryption key");

    // Without this list the server assumes we cannot decode video, and either
    // sends nothing or sends a codec we never open. H.264 on 101 is the
    // fallback every official client still understands.
    //
    // `encode` false means this client cannot send video in that format, and
    // the server will not build a path for a codec we said we cannot send.
    // True is necessary. It is not sufficient: the picture still has to be
    // sealed as one frame and only then split, which sendPicture does.
    QJsonArray codecs;
    codecs.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("opus")},
        {QStringLiteral("type"), QStringLiteral("audio")},
        {QStringLiteral("priority"), 1000},
        {QStringLiteral("payload_type"), 120},
    });
    codecs.append(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("H264")},
        {QStringLiteral("type"), QStringLiteral("video")},
        {QStringLiteral("priority"), 1000},
        {QStringLiteral("payload_type"), static_cast<int>(m_videoPayloadType)},
        {QStringLiteral("rtx_payload_type"), static_cast<int>(m_rtxPayloadType)},
        {QStringLiteral("encode"), true},
        {QStringLiteral("decode"), true},
    });

    wlog(QStringLiteral("voice"),
         QStringLiteral("offering H264 on %1 (rtx %2), encode and decode")
             .arg(m_videoPayloadType)
             .arg(m_rtxPayloadType));

    QJsonArray experiments;
    for (const QString &name : m_experiments)
        experiments.append(name);

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
             {QStringLiteral("codecs"), codecs},
             {QStringLiteral("rtc_connection_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
             {QStringLiteral("experiments"), experiments},
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
                                   bool *marker, quint16 *sequence, int *rotationDegrees)
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
    if (sequence)
        *sequence = static_cast<quint16>((bytes[2] << 8) | bytes[3]);

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
        if (rotationDegrees)
            *rotationDegrees = videoOrientationDegrees(opusFrame.left(extensionBodyBytes));
        opusFrame.remove(0, extensionBodyBytes);
    } else if (rotationDegrees) {
        *rotationDegrees = 0;
    }

    // RTP padding is counted in the last byte and sits after the real payload.
    // Leaving it on a picture makes the group seal look missing.
    if ((bytes[0] & 0x20) != 0) {
        if (opusFrame.isEmpty())
            return false;
        const int pad = static_cast<quint8>(opusFrame.at(opusFrame.size() - 1));
        if (pad == 0 || pad > opusFrame.size())
            return false;
        opusFrame.chop(pad);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

void VoiceConnection::startAudio()
{
    stopAudio();

    // A viewer-only connection only ever encodes a share's sound - music,
    // games, video - which Opus's speech mode would thin out.
    int error = 0;
    m_encoder = opus_encoder_create(SampleRate, Channels,
                                    m_viewerOnly ? OPUS_APPLICATION_AUDIO : OPUS_APPLICATION_VOIP,
                                    &error);
    if (error != OPUS_OK || !m_encoder) {
        emit failed(QStringLiteral("Could not start the Opus encoder."));
        return;
    }
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(m_viewerOnly ? 96000 : 64000));
    opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(m_encoder, OPUS_SET_PACKET_LOSS_PERC(10));

    const QAudioFormat format = voiceFormat();

    // A viewer opens no microphone. The call underneath is already carrying
    // one, and a second would capture the same room twice.
    if (!m_viewerOnly) {
        m_input = new QAudioSource(findInput(m_inputDeviceId), format, this);
        m_inputStream = m_input->start();
    }

    // Speakers only belong on the call itself. A share's sound is mixed into
    // that sink so the headset is driven by one clock, not two.
    if (!m_audioHost) {
        m_output = new QAudioSink(findOutput(m_outputDeviceId), format, this);

        // Four frames of hardware cushion. Eight used to sit under a growing
        // jitter queue and the call sounded a beat behind.
        m_output->setBufferSize(FrameBytes * 4);
        m_outputStream = m_output->start();
        m_output->setVolume(1.0);
    }

    m_captureBuffer.clear();
    m_rtpSequence = static_cast<quint16>(QRandomGenerator::global()->bounded(65535));
    m_rtpTimestamp = QRandomGenerator::global()->generate();
    m_nonceCounter = 0;

    m_lastBeatMs = -1;
    m_worstBeatLateMs = 0;
    m_beatsLate = 0;
    m_sendTimer.start();
    m_playTimer.start();

    wlog(QStringLiteral("voice"),
         m_audioHost ? QStringLiteral("share audio will mix into the call")
                     : QStringLiteral("audio running"));
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
    qDeleteAll(m_buffers);
    m_buffers.clear();
    m_captureBuffer.clear();
    m_externalPcm.clear();

    QMutexLocker lock(&m_sharedSoundMutex);
    m_sharedSound.clear();
    m_saidSharedSound = false;
}

void VoiceConnection::onSendTick()
{
    // A share carries no microphone, only the sound of what is shared.
    if (m_viewerOnly) {
        sendSharedSound();
        if (++m_statTicks >= 250) {
            m_statTicks = 0;
            reportAudioStats();
        }
        return;
    }

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
        //
        // 100 is the microphone as it arrives. The rest of the slider is
        // boost. At the far end a quiet headset is twelve times louder, which
        // is what it takes to sit with everyone else when you are close to the
        // mic and the bar still barely moved. Peaks are eased off so that
        // boost does not turn into a buzz.
        const double extra = qMax(0, m_inputVolume - 100) / 100.0;
        const double gain = (m_inputVolume / 100.0) * std::pow(6.0, extra);
        for (int i = 0; i < count; ++i) {
            double value = samples[i] * gain;
            const double absValue = std::abs(value);
            if (absValue > 28000.0) {
                const double sign = value < 0.0 ? -1.0 : 1.0;
                value = sign * (28000.0 + 4767.0 * std::tanh((absValue - 28000.0) / 8000.0));
            }
            samples[i] = static_cast<qint16>(qBound(-32767.0, value, 32767.0));
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

    // Pictures we sent count as something happening.
    //
    // They did not, and that is why a broken share left no trace: the
    // connection carrying a Go Live stream opens no microphone and plays no
    // sound, so every one of the counts below stayed at zero and this
    // returned without printing. The one line that would have said what was
    // happening was suppressed by the one case it was needed for.
    const int video = m_statVideoSent + m_statVideoSealFailed;

    if (m_statSent == 0 && m_statPlayed == 0 && lost == 0 && video == 0)
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
    if (m_statVideoPackets > 0 || m_statVideoDaveFailed > 0 || m_statVideoUnknownSsrc > 0)
        line += QStringLiteral(", video packets %1 (unreadable %2, unnamed %3)")
                    .arg(m_statVideoPackets)
                    .arg(m_statVideoDaveFailed)
                    .arg(m_statVideoUnknownSsrc);

    // Our own share, counted for the same reason as everything else here: a
    // viewer seeing nothing needs this line to say whether we sent anything.
    if (m_sendingVideo || m_statVideoSent > 0 || m_statVideoSealFailed > 0) {
        line += QStringLiteral(", sent %1 video packets on ssrc %2")
                    .arg(m_statVideoSent)
                    .arg(m_videoSsrc);
        if (m_statVideoSealFailed > 0)
            line += QStringLiteral(" (%1 refused by the encryption layer, no group key yet)")
                        .arg(m_statVideoSealFailed);
        if (m_daveVersion > 0)
            line += QStringLiteral(" [end-to-end v%1]").arg(m_daveVersion);
        m_statVideoSent = 0;
        m_statVideoSealFailed = 0;
    }

    // What the buffers did, summed over everyone. "rebuilt" is a lost packet
    // recovered from the next one's FEC copy; "guessed" is one there was no
    // copy of. A clean line reads zero for both.
    {
        JitterBuffer::Stats sum;
        int cushionTotal = 0;
        int talking = 0;
        for (JitterBuffer *buffer : std::as_const(m_buffers)) {
            const JitterBuffer::Stats s = buffer->takeStats();
            sum.concealed += s.concealed;
            sum.recovered += s.recovered;
            sum.late += s.late;
            sum.reordered += s.reordered;
            sum.shortened += s.shortened;
            sum.lengthened += s.lengthened;
            if (!buffer->isIdle()) {
                cushionTotal += s.targetMs;
                ++talking;
            }
        }
        if (!m_buffers.isEmpty()) {
            line += QStringLiteral(", buffer: cushion %1 ms, rebuilt %2, guessed %3, late %4, "
                                   "reordered %5, stretched -%6/+%7")
                        .arg(talking ? cushionTotal / talking : 0)
                        .arg(sum.recovered)
                        .arg(sum.concealed)
                        .arg(sum.late)
                        .arg(sum.reordered)
                        .arg(sum.shortened)
                        .arg(sum.lengthened);
        }
    }

    // The beat, always printed. A clean call should read "worst 1 ms"; a call
    // that stutters will say by how much, and whether it is us.
    line += QStringLiteral(", beat late worst %1 ms (%2 over 10 ms)")
                .arg(m_worstBeatLateMs)
                .arg(m_beatsLate);
    m_worstBeatLateMs = 0;
    m_beatsLate = 0;

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
                 .arg(stream->hungry));

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
    m_statVideoPackets = 0;
    m_statVideoDaveFailed = 0;
    m_statVideoUnknownSsrc = 0;
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
        //
        // RTCP (sender reports, NACKs) uses 200-207 and is not media. Counting
        // those as failed decrypts made a working call look broken.
        const quint8 payloadType = packet.size() > 1
            ? static_cast<quint8>(packet[1]) & 0x7F
            : 0;
        if (payloadType >= 200 && payloadType <= 207) {
            // Not media, but not nothing: this is where a viewer says it
            // cannot draw anything from what we have sent.
            handleIncomingRtcp(packet);
            continue;
        }
        if (payloadType == m_rtxPayloadType)
            continue;

        const bool isVideo = payloadType == m_videoPayloadType
            || payloadType == VideoPayloadType;

        if (m_deafened && !isVideo)
            continue;

        QByteArray payload;
        quint32 ssrc = 0;
        bool marker = false;
        quint16 sequence = 0;
        int rotation = 0;
        if (!decryptFrame(packet, payload, ssrc, &marker, &sequence, isVideo ? &rotation : nullptr)) {
            ++m_statUndecryptable;
            continue;
        }

        if (isVideo) {
            ++m_statVideoPackets;
            if (m_rtxSsrcs.contains(ssrc))
                continue;
            handleVideoPacket(ssrc, payload, marker, sequence, rotation);
            continue;
        }

        // Discord sends tiny keep alive frames that decode to nothing useful.
        if (payload.size() < 3)
            continue;

        // The RTP timestamp, bytes 4 to 7, which is in the clear. The buffer
        // needs it to tell a slow network from a person who paused.
        const auto *raw = reinterpret_cast<const quint8 *>(packet.constData());
        const quint32 timestamp = (quint32(raw[4]) << 24) | (quint32(raw[5]) << 16)
            | (quint32(raw[6]) << 8) | quint32(raw[7]);

        receiveAudio(ssrc, sequence, timestamp, payload);
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
    m_videoRid.clear();
    m_bestVideoSsrc.clear();
    m_rtxSsrcs.clear();
    m_lastWantedVideo.clear();
}

void VoiceConnection::handleVideoPacket(quint32 ssrc, const QByteArray &payload, bool endOfFrame,
                                        quint16 sequence, int rotation)
{
    if (payload.isEmpty())
        return;

    const QString userId = m_videoSsrcToUser.value(ssrc);

    // Simulcast sends the same picture several times. Drawing every layer is
    // wasted work and two decoders fighting over one tile. And a camera that
    // is off screen is not decoded at all, even if a few packets still arrive
    // after the server was told to stop.
    if (!userId.isEmpty() && m_bestVideoSsrc.contains(userId)) {
        const quint32 chosen = chosenVideoSsrc(userId);
        if (chosen != ssrc)
            return;
    }

    VideoStream &stream = *videoStreamFor(ssrc);
    ++stream.packets;

    if (!stream.haveSeq) {
        stream.nextSeq = sequence;
        stream.haveSeq = true;
        ingestVideoPayload(stream, ssrc, userId, payload, endOfFrame, rotation);
        stream.nextSeq = static_cast<quint16>(sequence + 1);
        return;
    }

    const quint16 dist = static_cast<quint16>(sequence - stream.nextSeq);
    if (dist == 0) {
        ingestVideoPayload(stream, ssrc, userId, payload, endOfFrame, rotation);
        stream.nextSeq = static_cast<quint16>(stream.nextSeq + 1);
        flushHeldVideo(stream, ssrc, userId);
        return;
    }

    if (dist < 0x8000 && dist <= 64) {
        // Arrived early. Hold it and wait for the hole — treating this as
        // loss is what dropped a whole busy 1080p picture when two packets
        // swapped places.
        stream.held.insert(sequence, {payload, endOfFrame, rotation});
        bool markerHeld = false;
        for (const HeldPacket &held : stream.held) {
            if (held.endOfFrame) {
                markerHeld = true;
                break;
            }
        }
        if ((markerHeld && !stream.held.contains(stream.nextSeq)) || stream.held.size() >= 64)
            skipLostVideo(stream, ssrc, userId);
        return;
    }

    if (dist < 0x8000) {
        // Jumped far ahead: a burst was lost, not reordered.
        stream.gap = true;
        stream.held.clear();
        stream.assembling.clear();
        ingestVideoPayload(stream, ssrc, userId, payload, endOfFrame, rotation);
        stream.nextSeq = static_cast<quint16>(sequence + 1);
        return;
    }

    // Duplicate or late. Already assembled past it.
}

void VoiceConnection::flushHeldVideo(VideoStream &stream, quint32 ssrc, const QString &userId)
{
    while (true) {
        const auto it = stream.held.constFind(stream.nextSeq);
        if (it == stream.held.cend())
            break;
        const HeldPacket pkt = it.value();
        stream.held.remove(stream.nextSeq);
        ingestVideoPayload(stream, ssrc, userId, pkt.payload, pkt.endOfFrame, pkt.rotation);
        stream.nextSeq = static_cast<quint16>(stream.nextSeq + 1);
    }
}

void VoiceConnection::skipLostVideo(VideoStream &stream, quint32 ssrc, const QString &userId)
{
    if (stream.held.isEmpty())
        return;
    if (!stream.assembling.isEmpty())
        stream.gap = true;

    quint16 bestDist = 0xFFFF;
    quint16 bestSeq = stream.nextSeq;
    for (auto it = stream.held.cbegin(); it != stream.held.cend(); ++it) {
        const quint16 dist = static_cast<quint16>(it.key() - stream.nextSeq);
        if (dist < 0x8000 && dist < bestDist) {
            bestDist = dist;
            bestSeq = it.key();
        }
    }
    if (bestDist == 0xFFFF)
        return;
    stream.nextSeq = bestSeq;
    flushHeldVideo(stream, ssrc, userId);
}

void VoiceConnection::ingestVideoPayload(VideoStream &stream, quint32 ssrc, const QString &userId,
                                         const QByteArray &payload, bool endOfFrame, int rotation)
{
    const auto *bytes = reinterpret_cast<const quint8 *>(payload.constData());
    const int kind = bytes[0] & 0x1F;

    if (!stream.loggedFirst) {
        stream.loggedFirst = true;
        wlog(QStringLiteral("video"),
             QStringLiteral("first packet from %1: %2 bytes, nal %3")
                 .arg(userId.isEmpty() ? QString::number(ssrc) : userId)
                 .arg(payload.size())
                 .arg(kind));
    }

    if (stream.assembling.isEmpty())
        stream.assembling.reserve(65536);

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
        // Leftover on the last packet is the group seal. Leftover on an
        // earlier STAP is an incomplete length, and stuffing it between
        // SPS/PPS and the IDR is how Relic's share came out as invalid data.
        if (endOfFrame && offset < payload.size())
            stream.assembling += payload.mid(offset);
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
            stream.gap = true;
            return;
        }

        stream.assembling += payload.mid(2);
    } else {
        // Types 25 to 27 and 29 exist and Discord does not send them.
        return;
    }

    if (endOfFrame) {
        stream.rotation = rotation;
        finishVideoPicture(stream, ssrc, userId);
    }
}

void VoiceConnection::finishVideoPicture(VideoStream &stream, quint32 ssrc, const QString &userId)
{
    if (stream.assembling.isEmpty())
        return;

    const bool gapped = stream.gap;
    QByteArray picture = stream.assembling;
    stream.assembling.clear();
    stream.gap = false;

    if (gapped) {
        ++stream.dropped;
        sendPictureLossIndication(ssrc);
        return;
    }

    if (stream.frames == 0 && stream.dropped == 0) {
        const bool marked = picture.size() >= 2
            && static_cast<quint8>(picture[picture.size() - 2]) == 0xFA
            && static_cast<quint8>(picture[picture.size() - 1]) == 0xFA;
        wlog(QStringLiteral("video"),
             QStringLiteral("assembled a picture from %1: %2 bytes, group seal %3")
                 .arg(userId.isEmpty() ? QString::number(ssrc) : userId)
                 .arg(picture.size())
                 .arg(marked ? QStringLiteral("present") : QStringLiteral("missing")));
    }

    // Transport decrypt only unwraps the packet. The picture itself is still
    // sealed for the group, the same way the sound is, and has to be opened
    // before the decoder will see H.264 rather than ciphertext.
    if (m_daveVersion > 0) {
        if (userId.isEmpty()) {
            ++m_statVideoUnknownSsrc;
            ++stream.dropped;
            return;
        }
        picture = m_dave->decrypt(userId, picture, true);
        if (picture.isEmpty()) {
            ++m_statVideoDaveFailed;
            ++stream.dropped;
            if (stream.dropped == 1 || (stream.dropped % 60) == 0) {
                wlog(QStringLiteral("video"),
                     QStringLiteral("could not open a picture from %1 (%2 so far)")
                         .arg(userId)
                         .arg(stream.dropped));
            }
            sendPictureLossIndication(ssrc);
            return;
        }
    }

    if (!m_videoWorker)
        return;

    VideoDecodeWorker *worker = m_videoWorker;
    QMetaObject::invokeMethod(worker, [worker, ssrc, userId, picture]() {
        worker->submit(ssrc, userId, picture);
    }, Qt::QueuedConnection);
}

JitterBuffer *VoiceConnection::bufferFor(quint32 ssrc)
{
    if (JitterBuffer *existing = m_buffers.value(ssrc))
        return existing;

    auto *buffer = new JitterBuffer;
    if (!buffer->isValid()) {
        delete buffer;
        return nullptr;
    }
    m_buffers.insert(ssrc, buffer);
    return buffer;
}

void VoiceConnection::offerExternalPcm(const QByteArray &pcm)
{
    // Always called by the stream connection, which lives on the same thread,
    // so this is normally direct. Posted anyway if that ever stops being true.
    if (postToOwnThread([this, pcm]() { offerExternalPcm(pcm); }))
        return;

    if (pcm.size() == FrameBytes)
        m_externalPcm = pcm;
}

void VoiceConnection::offerSharedSound(const QByteArray &pcm)
{
    // Ten frames of backlog at most. The capture and this beat run on
    // different clocks, and a share that falls behind should drop sound and
    // stay in step with its picture rather than drift further behind it.
    constexpr int MaxBacklog = FrameBytes * 10;

    QMutexLocker lock(&m_sharedSoundMutex);
    m_sharedSound.append(pcm);
    if (m_sharedSound.size() > MaxBacklog) {
        const int excess = m_sharedSound.size() - MaxBacklog;
        m_sharedSound.remove(0, excess - excess % 4);
    }
}

void VoiceConnection::sendSharedSound()
{
    // How quiet counts as silence: about -70 dB. Real silence from Windows is
    // exact zeros; this also catches the faint hiss some programs leave on.
    constexpr double SilentLevel = 0.0003;

    // A second of quiet before saying the sound stopped. Music has pauses,
    // and each stop and start costs the listener a moment of cushion.
    constexpr int TailFrames = 50;

    QByteArray pending;
    {
        QMutexLocker lock(&m_sharedSoundMutex);
        const int whole = m_sharedSound.size() - m_sharedSound.size() % FrameBytes;
        if (whole > 0) {
            pending = m_sharedSound.left(whole);
            m_sharedSound.remove(0, whole);
        }
    }

    const bool canSend = m_encoder && !m_secretKey.isEmpty() && m_ssrc != 0;

    // Nothing arrived: the program went quiet, or closed. Count the beat as
    // silence so the flag still comes down.
    if (pending.isEmpty() || !canSend) {
        if (m_speaking && ++m_silentFrames > TailFrames) {
            m_speaking = false;
            sendSpeaking(false);
        }
        return;
    }

    for (int offset = 0; offset < pending.size(); offset += FrameBytes) {
        auto *samples = reinterpret_cast<qint16 *>(pending.data() + offset);
        const int count = FrameSamples * Channels;

        double sum = 0.0;
        for (int i = 0; i < count; ++i) {
            const double normalised = samples[i] / 32768.0;
            sum += normalised * normalised;
        }
        const bool audible = std::sqrt(sum / count) > SilentLevel;

        if (audible) {
            m_silentFrames = 0;
            if (!m_speaking) {
                m_speaking = true;
                sendSpeaking(true);
            }
        } else if (m_speaking && ++m_silentFrames > TailFrames) {
            m_speaking = false;
            sendSpeaking(false);
        }

        m_rtpTimestamp += FrameSamples;
        m_rtpSequence++;

        if (!m_speaking)
            continue;

        unsigned char encoded[4000];
        const int encodedBytes = opus_encode(m_encoder, samples, FrameSamples, encoded, sizeof(encoded));
        if (encodedBytes <= 0)
            continue;

        QByteArray sound(reinterpret_cast<const char *>(encoded), encodedBytes);
        if (m_daveVersion > 0) {
            sound = m_dave->encrypt(sound, m_ssrc);
            if (sound.isEmpty()) {
                ++m_statSealFailed;
                continue;
            }
        }

        const QByteArray packet = encryptFrame(buildRtpHeader(m_rtpSequence, m_rtpTimestamp, m_ssrc), sound);
        if (packet.isEmpty())
            continue;

        m_udp.writeDatagram(packet, QHostAddress(m_serverAddress), m_serverPort);
        ++m_statSent;

        if (!m_saidSharedSound) {
            m_saidSharedSound = true;
            wlog(QStringLiteral("share"), QStringLiteral("first piece of screen sound sent"));
        }
    }
}

QByteArray VoiceConnection::mixWaitingStreams()
{
    qint32 mixed[FrameSamples * Channels] = {0};
    bool anyone = false;

    if (!m_mediaClock.isValid())
        m_mediaClock.start();
    const qint64 now = m_mediaClock.elapsed();

    for (auto it = m_buffers.begin(); it != m_buffers.end(); ++it) {
        // Empty when this person is quiet, which in a twenty person call is
        // almost everyone almost all the time. The old queue kept inventing
        // sound for everybody who had ever spoken, every 20 ms, for the rest
        // of the call.
        const QByteArray frame = it.value()->pull(now);
        if (frame.isEmpty())
            continue;

        const QString userId = m_ssrcToUser.value(it.key());
        const bool silenced = !userId.isEmpty() && m_userMuted.contains(userId);
        const double person = silenced ? 0.0 : (m_userVolume.value(userId, 100) / 100.0);

        const auto *samples = reinterpret_cast<const qint16 *>(frame.constData());
        const int count = qMin<int>(frame.size() / 2, FrameSamples * Channels);
        if (person != 0.0) {
            for (int i = 0; i < count; ++i)
                mixed[i] += static_cast<qint32>(samples[i] * person);
        }

        anyone = true;
    }

    if (!m_externalPcm.isEmpty() && m_externalPcm.size() == FrameBytes) {
        const auto *extra = reinterpret_cast<const qint16 *>(m_externalPcm.constData());
        for (int i = 0; i < FrameSamples * Channels; ++i)
            mixed[i] += extra[i];
        anyone = true;
        m_externalPcm.clear();
    } else {
        m_externalPcm.clear();
    }

    if (!anyone)
        return {};

    const double gain = m_deafened ? 0.0 : (m_outputVolume / 100.0);

    QByteArray out(FrameBytes, '\0');
    auto *target = reinterpret_cast<qint16 *>(out.data());
    for (int i = 0; i < FrameSamples * Channels; ++i)
        target[i] = static_cast<qint16>(qBound(-32768, static_cast<int>(mixed[i] * gain), 32767));

    return out;
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
    // How late this beat is. A beat that arrives late plays late, and one
    // that is late by more than the cushion plays nothing at all.
    if (!m_beatClock.isValid())
        m_beatClock.start();
    const qint64 now = m_beatClock.elapsed();
    if (m_lastBeatMs >= 0) {
        const qint64 late = now - m_lastBeatMs - FrameMs;
        if (late > m_worstBeatLateMs)
            m_worstBeatLateMs = late;
        if (late > 10)
            ++m_beatsLate;
    }
    m_lastBeatMs = now;

    sweepSpeaking();

    if (m_audioHost) {
        const QByteArray mixed = mixWaitingStreams();
        if (!mixed.isEmpty())
            m_audioHost->offerExternalPcm(mixed);
        return;
    }

    if (!m_outputStream)
        return;

    // One frame per tick keeps the call on time. A tick that arrives late
    // finds the device empty, and writing only one frame then leaves a hole
    // where the missed ones should have been. Fill what the device has room
    // for, up to three, which is the whole cushion.
    int budget = 1;
    if (m_output) {
        const int freeFrames = m_output->bytesFree() / FrameBytes;
        budget = qBound(1, freeFrames, 3);
    }

    for (int i = 0; i < budget; ++i) {
        if (m_output && m_output->bytesFree() < FrameBytes)
            break;

        const QByteArray mixed = mixWaitingStreams();
        if (mixed.isEmpty())
            break;

        m_outputStream->write(mixed);
        ++m_statPlayed;
    }
}

void VoiceConnection::receiveAudio(quint32 ssrc, quint16 sequence, quint32 timestamp,
                                   const QByteArray &frame)
{
    // Somebody's ring goes on because sound is arriving from them, and off
    // when it stops.
    //
    // Opcode 5 is what used to drive this on its own, and it is not enough:
    // Discord sends it when somebody starts, and the matching "stopped" does
    // not always arrive - a client that crashes, drops out, or is moved never
    // sends one at all. The ring then stayed lit for the rest of the call.
    //
    // Sound itself cannot lie about this. A client that stops talking stops
    // sending, so silence is the signal.
    noteSpeaking(ssrc);

    JitterBuffer *buffer = bufferFor(ssrc);
    if (!buffer)
        return;
    if (!m_outputStream && !m_audioHost)
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

    // Held, not decoded. The buffer decodes in sequence order when each packet
    // is due, which is the only way a lost packet can be rebuilt from the one
    // after it: Opus has to see them in order.
    if (!m_mediaClock.isValid())
        m_mediaClock.start();
    buffer->insert(sequence, timestamp, opusFrame, m_mediaClock.elapsed());
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

// Tells the server what we are sending, which for us is sound and nothing
// else. It is also how a client says it understands video at all.
void VoiceConnection::sendVideoState()
{
    QJsonArray streams;
    if (m_sendingVideo) {
        // One layer, at full quality. Discord's own client offers several
        // sizes so the server can drop viewers on poor connections to a
        // smaller one; doing that means running the encoder several times
        // over, and one good picture is a better first version than three
        // mediocre ones.
        streams.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("video")},
            {QStringLiteral("rid"), QStringLiteral("100")},
            {QStringLiteral("ssrc"), static_cast<qint64>(m_videoSsrc)},
            {QStringLiteral("active"), true},
            {QStringLiteral("quality"), 100},
            {QStringLiteral("rtx_ssrc"), static_cast<qint64>(m_rtxSsrc)},
            {QStringLiteral("max_bitrate"), 2500000},
            {QStringLiteral("max_framerate"), 30},
            {QStringLiteral("max_resolution"),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("fixed")},
                         {QStringLiteral("width"), m_sendWidth},
                         {QStringLiteral("height"), m_sendHeight}}},
        });
    }

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpVideo},
        {QStringLiteral("d"),
         QJsonObject{
             {QStringLiteral("audio_ssrc"), static_cast<qint64>(m_ssrc)},
             // Zero means we send no pictures, which is true of every
             // connection except the one carrying a share.
             {QStringLiteral("video_ssrc"), static_cast<qint64>(m_videoSsrc)},
             {QStringLiteral("rtx_ssrc"), static_cast<qint64>(m_rtxSsrc)},
             {QStringLiteral("streams"), streams},
         }},
    });
}

// ---------------------------------------------------------------------------
// Sending a picture
// ---------------------------------------------------------------------------

void VoiceConnection::startSendingVideo(int width, int height)
{
    if (postToOwnThread([this, width, height]() { startSendingVideo(width, height); }))
        return;

    if (m_ssrc == 0) {
        wlog(QStringLiteral("share"),
             QStringLiteral("asked to send video before the server gave us an ssrc"));
        return;
    }

    // Whatever the server set aside for us, and only if it did not, the guess
    // that it numbers them consecutively from the audio one. Sending on a
    // source the server never allocated is not refused, it is ignored - which
    // is how a share can look perfectly healthy from this end and be invisible
    // from every other.
    if (m_assignedVideoSsrc != 0) {
        m_videoSsrc = m_assignedVideoSsrc;
        m_rtxSsrc = m_assignedRtxSsrc != 0 ? m_assignedRtxSsrc : m_assignedVideoSsrc + 1;
    } else {
        wlog(QStringLiteral("share"),
             QStringLiteral("the server named no video ssrc, falling back to audio+1"));
        m_videoSsrc = m_ssrc + 1;
        m_rtxSsrc = m_ssrc + 2;
    }

    m_sendWidth = width;
    m_sendHeight = height;
    m_videoSequence = 0;
    m_videoTimestamp = 0;
    m_sendingVideo = true;
    m_publicSending.store(true);

    wlog(QStringLiteral("share"),
         QStringLiteral("sending video: %1x%2, ssrc %3").arg(width).arg(height).arg(m_videoSsrc));

    sendVideoState();

    // Announce it the way a camera is announced. Without this the other
    // clients are told a stream exists but never that it started.
    sendSpeaking(m_speaking);
}

void VoiceConnection::stopSendingVideo()
{
    if (postToOwnThread([this]() { stopSendingVideo(); }))
        return;

    if (!m_sendingVideo)
        return;

    m_sendingVideo = false;
    m_publicSending.store(false);
    m_videoSsrc = 0;
    m_rtxSsrc = 0;
    m_sendWidth = m_sendHeight = 0;

    wlog(QStringLiteral("share"), QStringLiteral("stopped sending video"));

    // Says the stream is gone. Skipping this leaves a tile that never fills.
    sendVideoState();
}

void VoiceConnection::sendPicture(const QList<QByteArray> &units)
{
    // Normally already here: the encoder's signal is connected straight to
    // this object, so pictures go encoder thread to media thread and never
    // wait behind the window.
    if (postToOwnThread([this, units]() { sendPicture(units); }))
        return;

    if (!m_sendingVideo || units.isEmpty() || m_secretKey.isEmpty())
        return;

    // One picture, in the form the encryption library scans: a four-byte
    // start code in front of every piece. The encoder hands the pieces over
    // with those already removed, because that is what a packet wants, and
    // they have to be put back before the picture is sealed.
    QByteArray picture;
    const char startCode[4] = {0, 0, 0, 1};
    for (const QByteArray &nal : units) {
        if (nal.isEmpty())
            continue;
        picture.append(startCode, 4);
        picture.append(nal);
    }
    if (picture.isEmpty())
        return;

    // Seal the whole picture, then split it. The other way round is what left
    // every viewer on a loading screen.
    //
    // A receiver glues the packets back into one picture and only then opens
    // the seal. Sealing each packet on its own means the glued picture is a
    // row of seals, and nothing in it is a picture. The library also has to
    // be told the stream is H.264, or it seals the bytes a packetiser must
    // still be able to read, the piece type and where the piece starts, and
    // the far end cannot find the pieces.
    if (m_daveVersion > 0) {
        m_dave->useH264(m_videoSsrc);
        const QByteArray sealed = m_dave->encrypt(picture, m_videoSsrc, true);
        if (sealed.isEmpty()) {
            ++m_statVideoSealFailed;
            if (m_statVideoSealFailed == 1 || (m_statVideoSealFailed % 30) == 0) {
                wlog(QStringLiteral("share"),
                     QStringLiteral("could not seal a picture (%1 so far)")
                         .arg(m_statVideoSealFailed));
            }
            return;
        }
        picture = sealed;
    }

    const QList<QByteArray> pieces = splitAnnexB(picture);
    if (pieces.isEmpty())
        return;

    if (m_statVideoSent == 0) {
        const bool marked = picture.size() >= 2
            && static_cast<quint8>(picture.at(picture.size() - 2)) == 0xFA
            && static_cast<quint8>(picture.at(picture.size() - 1)) == 0xFA;
        wlog(QStringLiteral("share"),
             QStringLiteral("first picture: %1 bytes, %2 pieces, group seal %3")
                 .arg(picture.size())
                 .arg(pieces.size())
                 .arg(marked ? QStringLiteral("present") : QStringLiteral("not yet")));
    }

    // Every packet of one picture carries the same timestamp; that is how the
    // far end knows which packets belong together. It only advances between
    // pictures.
    //
    // Stepped by the clock rather than by a frame counter, because the
    // capture rate is whatever the screen actually managed - a still screen
    // produces nothing at all for a while - and a receiver timing playback
    // off these needs them to mean elapsed time.
    m_videoTimestamp = quint32(QDateTime::currentMSecsSinceEpoch() * VideoClockRate / 1000);

    for (int i = 0; i < pieces.size(); ++i)
        packetiseNalUnit(pieces.at(i), i == pieces.size() - 1);
}

void VoiceConnection::packetiseNalUnit(const QByteArray &nal, bool lastOfPicture)
{
    if (nal.isEmpty())
        return;

    if (nal.size() <= MaxVideoPayload) {
        // Small enough to travel whole. The NAL's own first byte is the
        // payload's first byte; there is no wrapper.
        sendVideoPacket(nal, lastOfPicture);
        return;
    }

    // Too big, so it is split. Each piece carries two bytes in front of it:
    // one repeating the original NAL's type and importance, and one saying
    // whether this is the first piece, the last, or neither. The far end
    // glues them back together by those flags.
    const quint8 header = quint8(nal.at(0));
    const quint8 nalType = header & 0x1F;
    const quint8 nalRefIdc = header & 0x60;

    const char *body = nal.constData() + 1;
    int remaining = nal.size() - 1;
    bool first = true;

    while (remaining > 0) {
        const int take = qMin(remaining, MaxVideoPayload - 2);
        const bool last = take == remaining;

        QByteArray packet;
        packet.reserve(take + 2);
        packet.append(char(nalRefIdc | 28));   // 28 is "this is a fragment"
        packet.append(char((first ? 0x80 : 0) | (last ? 0x40 : 0) | nalType));
        packet.append(body, take);

        // Only the very last piece of the very last unit ends the picture.
        sendVideoPacket(packet, lastOfPicture && last);

        body += take;
        remaining -= take;
        first = false;
    }
}

void VoiceConnection::sendVideoPacket(const QByteArray &payload, bool endOfPicture)
{
    // The picture was sealed already, as one frame, in sendPicture. What is
    // left here is the transport seal, which Discord's servers can undo and
    // the other people cannot. Doing the group seal here, on one packet, is
    // what made a share load forever: the viewer reassembled the packets and
    // then had nothing it could open.
    ++m_videoSequence;
    const QByteArray header =
        buildVideoRtpHeader(m_videoSequence, m_videoTimestamp, m_videoSsrc, endOfPicture);

    QByteArray clear = header;
    clear.append(videoExtensionPreamble());

    QByteArray body = videoExtensionBody(m_videoSequence, m_viewerOnly);
    body.append(payload);

    const QByteArray packet = encryptFrame(clear, body);
    if (packet.isEmpty())
        return;

    m_udp.writeDatagram(packet, QHostAddress(m_serverAddress), m_serverPort);
    ++m_statVideoSent;
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
    refreshVideoWants();
}

void VoiceConnection::setVideoViews(const QHash<QString, int> &pixelsByUser)
{
    if (postToOwnThread([this, pixelsByUser]() { setVideoViews(pixelsByUser); }))
        return;

    if (m_videoViewsKnown && m_videoViews == pixelsByUser)
        return;
    m_videoViews = pixelsByUser;
    m_videoViewsKnown = true;

    if (m_state != State::Idle && !m_videoSsrcToUser.isEmpty())
        refreshVideoWants();
}

// Which of a person's layers to take.
//
// A camera is usually sent two or three times at once - full size, half and
// sometimes quarter - and the server forwards whichever one each viewer asks
// for. Discord's own client asks for the big one only for the tile on the
// stage, the small one for the row underneath, and nothing at all for tiles
// scrolled out of view. A call of forty cameras is then a handful of small
// streams rather than forty full ones.
quint32 VoiceConnection::chosenVideoSsrc(const QString &userId) const
{
    const quint32 best = m_bestVideoSsrc.value(userId);
    if (!m_videoViewsKnown || m_viewerOnly || best == 0)
        return best;

    const int pixels = m_videoViews.value(userId, 0);
    if (pixels <= 0)
        return 0;
    if (pixels >= 640 * 360)
        return best;

    quint32 smallest = best;
    int smallestRid = m_videoRid.value(best, 100);
    for (auto it = m_videoSsrcToUser.constBegin(); it != m_videoSsrcToUser.constEnd(); ++it) {
        if (it.value() != userId)
            continue;
        const int rid = m_videoRid.value(it.key(), 100);
        if (rid < smallestRid) {
            smallestRid = rid;
            smallest = it.key();
        }
    }
    return smallest;
}

void VoiceConnection::refreshVideoWants()
{
    QJsonObject wants;
    wants.insert(QStringLiteral("any"), 100);

    QJsonObject pixelCounts;
    QSet<quint32> wanted;
    for (auto it = m_bestVideoSsrc.constBegin(); it != m_bestVideoSsrc.constEnd(); ++it) {
        const quint32 chosen = chosenVideoSsrc(it.key());
        if (chosen != 0)
            wanted.insert(chosen);
    }

    for (auto it = m_videoSsrcToUser.constBegin(); it != m_videoSsrcToUser.constEnd(); ++it) {
        const bool take = wanted.contains(it.key());
        wants.insert(QString::number(it.key()), take ? 100 : 0);
        if (!take)
            continue;
        int pixels = m_viewerOnly ? (1920 * 1080) : (640 * 360);
        if (m_videoViewsKnown && !m_viewerOnly)
            pixels = m_videoViews.value(it.value(), pixels);
        pixelCounts.insert(QString::number(it.key()), pixels);
    }
    if (!pixelCounts.isEmpty())
        wants.insert(QStringLiteral("pixelCounts"), pixelCounts);

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpMediaSinkWants},
        {QStringLiteral("d"), wants},
    });

    wlog(QStringLiteral("video"),
         QStringLiteral("asked for %1 of %2 cameras%3")
             .arg(wanted.size())
             .arg(m_bestVideoSsrc.size())
             .arg(m_videoViewsKnown && !m_viewerOnly ? QStringLiteral(", the ones on screen")
                                                     : QString()));

    // A layer that was not being received has no picture to build on, so ask
    // its sender for a fresh one. Layers already flowing need nothing: asking
    // every camera for a full picture on every scroll is a burst of the
    // largest packets there are.
    for (quint32 ssrc : wanted) {
        if (m_lastWantedVideo.contains(ssrc))
            continue;
        // Whatever this layer left half-built when it was last dropped
        // belongs to a picture that will never finish.
        if (VideoStream *stream = m_videoStreams.value(ssrc)) {
            stream->assembling.clear();
            stream->held.clear();
            stream->haveSeq = false;
            stream->gap = false;
            stream->lastPliMs = 0;
        }
        if (!m_secretKey.isEmpty())
            sendPictureLossIndication(ssrc);
    }
    m_lastWantedVideo = wanted;
}

void VoiceConnection::sendPictureLossIndication(quint32 mediaSsrc)
{
    if (m_secretKey.isEmpty() || m_ssrc == 0 || mediaSsrc == 0)
        return;

    VideoStream *stream = m_videoStreams.value(mediaSsrc);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (stream) {
        if (now - stream->lastPliMs < 750)
            return;
        stream->lastPliMs = now;
    }

    // RTCP PSFB PLI, RFC 4585. Twelve bytes: header, our ssrc, their ssrc.
    QByteArray rtcp(12, '\0');
    auto *bytes = reinterpret_cast<quint8 *>(rtcp.data());
    bytes[0] = 0x81;   // version 2, FMT 1 (PLI)
    bytes[1] = 206;    // payload-specific feedback
    bytes[2] = 0;
    bytes[3] = 2;      // length in 32-bit words minus one
    bytes[4] = static_cast<quint8>(m_ssrc >> 24);
    bytes[5] = static_cast<quint8>(m_ssrc >> 16);
    bytes[6] = static_cast<quint8>(m_ssrc >> 8);
    bytes[7] = static_cast<quint8>(m_ssrc & 0xFF);
    bytes[8] = static_cast<quint8>(mediaSsrc >> 24);
    bytes[9] = static_cast<quint8>(mediaSsrc >> 16);
    bytes[10] = static_cast<quint8>(mediaSsrc >> 8);
    bytes[11] = static_cast<quint8>(mediaSsrc & 0xFF);

    // Same scheme as media: the eight-byte RTCP header stays in the clear
    // and authenticates the encrypted body.
    const QByteArray packet = encryptFrame(rtcp.left(8), rtcp.mid(8));
    if (packet.isEmpty())
        return;

    m_udp.writeDatagram(packet, QHostAddress(m_serverAddress), m_serverPort);
}

// The other end of sendPictureLossIndication: somebody is asking us.
//
// A viewer who joins halfway through has been sent nothing but descriptions of
// changes from pictures it never saw, so it has nothing to draw and says so.
// Without this the person watching stares at a black tile until the encoder
// happens to produce a keyframe on its own, which is up to two seconds.
void VoiceConnection::handleIncomingRtcp(const QByteArray &packet)
{
    if (!m_sendingVideo || m_secretKey.isEmpty())
        return;

    constexpr int RtcpHeaderSize = 8;
    if (packet.size() < RtcpHeaderSize + crypto_aead_xchacha20poly1305_ietf_ABYTES + NonceTailSize)
        return;

    const quint8 type = static_cast<quint8>(packet[1]);
    const quint8 format = static_cast<quint8>(packet[0]) & 0x1F;

    // 206 is payload-specific feedback. Format 1 is "I have lost the picture",
    // format 4 is "send a full picture now". Both mean the same thing to us.
    // 205 is transport feedback, which is mostly requests to resend a
    // particular packet; we do not keep old packets to resend, and a keyframe
    // fixes the same problem more bluntly.
    if (type != 206 || (format != 1 && format != 4))
        return;

    // The body is encrypted, and the ssrc being complained about is in it.
    unsigned char nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
    std::memset(nonce, 0, sizeof(nonce));
    std::memcpy(nonce, packet.constData() + packet.size() - NonceTailSize, NonceTailSize);

    const int cipherLength = packet.size() - RtcpHeaderSize - NonceTailSize;
    QByteArray body(cipherLength, '\0');
    unsigned long long plainLength = 0;

    const int result = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char *>(body.data()), &plainLength, nullptr,
        reinterpret_cast<const unsigned char *>(packet.constData() + RtcpHeaderSize), cipherLength,
        reinterpret_cast<const unsigned char *>(packet.constData()), RtcpHeaderSize, nonce,
        reinterpret_cast<const unsigned char *>(m_secretKey.constData()));

    if (result != 0 || plainLength < 8)
        return;

    body.resize(static_cast<int>(plainLength));
    const auto *bytes = reinterpret_cast<const quint8 *>(body.constData());

    // Bytes 0-3 are whoever is asking; 4-7 are the stream they mean.
    const quint32 target = (quint32(bytes[4]) << 24) | (quint32(bytes[5]) << 16)
        | (quint32(bytes[6]) << 8) | quint32(bytes[7]);

    if (target != m_videoSsrc)
        return;

    // Several viewers joining at once ask separately, and a keyframe is the
    // most expensive thing the encoder makes. One answers all of them.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastKeyframeRequestMs < 500)
        return;
    m_lastKeyframeRequestMs = now;

    wlog(QStringLiteral("share"), QStringLiteral("a viewer asked for a keyframe"));
    emit keyframeWanted();
}

void VoiceConnection::sendSpeaking(bool speaking)
{
    sendJson(QJsonObject{
        {QStringLiteral("op"), OpSpeaking},
        {QStringLiteral("d"),
         QJsonObject{
             // 1 is a voice. 2 is SOUNDSHARE, "context audio for video, no
             // speaking indicator" - what a share's sound is marked with.
             {QStringLiteral("speaking"), speaking ? (m_viewerOnly ? 2 : 1) : 0},
             {QStringLiteral("delay"), 0},
             {QStringLiteral("ssrc"), static_cast<qint64>(m_ssrc)},
         }},
    });

    if (!m_userId.isEmpty())
        emit speakingChanged(m_userId, speaking);
}

void VoiceConnection::setMuted(bool muted)
{
    if (postToOwnThread([this, muted]() { setMuted(muted); }))
        return;

    m_muted = muted;
    if (muted && m_speaking) {
        m_speaking = false;
        sendSpeaking(false);
    }
}

void VoiceConnection::setDeafened(bool deafened)
{
    if (postToOwnThread([this, deafened]() { setDeafened(deafened); }))
        return;

    m_deafened = deafened;
}

void VoiceConnection::setUserVolumes(const QHash<QString, int> &percent, const QSet<QString> &muted)
{
    if (postToOwnThread([this, percent, muted]() { setUserVolumes(percent, muted); }))
        return;

    m_userVolume = percent;
    m_userMuted = muted;
}

void VoiceConnection::setInputVolume(int percent)
{
    if (postToOwnThread([this, percent]() { setInputVolume(percent); }))
        return;
    m_inputVolume = percent;
}

void VoiceConnection::setInputDevice(const QByteArray &deviceId)
{
    if (postToOwnThread([this, deviceId]() { setInputDevice(deviceId); }))
        return;
    m_inputDeviceId = deviceId;
}

void VoiceConnection::setOutputDevice(const QByteArray &deviceId)
{
    if (postToOwnThread([this, deviceId]() { setOutputDevice(deviceId); }))
        return;
    m_outputDeviceId = deviceId;
}

void VoiceConnection::setSensitivity(int percent)
{
    if (postToOwnThread([this, percent]() { setSensitivity(percent); }))
        return;
    m_sensitivity = percent;
}

void VoiceConnection::setMicrophoneProcessor(MicrophoneProcessor processor)
{
    if (postToOwnThread([this, processor]() { setMicrophoneProcessor(processor); }))
        return;
    m_micProcessor = std::move(processor);
}

void VoiceConnection::setOutputVolume(int percent)
{
    if (postToOwnThread([this, percent]() { setOutputVolume(percent); }))
        return;

    // Remembered, not just handed to the speaker.
    //
    // This used to set the volume and keep nothing. Settings are applied just
    // before a call is built, when there is no speaker yet, so the number went
    // to a null pointer check and was lost. Moving the slider did nothing at
    // any point in the call, which is exactly what it looked like.
    m_outputVolume = qBound(0, percent, 200);
}
