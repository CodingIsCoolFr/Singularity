#include "core/GatewayClient.h"

#include "core/DiscordIdentity.h"
#include "core/Logger.h"

#include <QJsonArray>
#include <QSslError>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QUrl>

namespace {

// Gateway opcodes we care about.
constexpr int OpDispatch = 0;
constexpr int OpHeartbeat = 1;
constexpr int OpIdentify = 2;
constexpr int OpResume = 6;
constexpr int OpReconnect = 7;
constexpr int OpInvalidSession = 9;
constexpr int OpHello = 10;
constexpr int OpHeartbeatAck = 11;
constexpr int OpPresenceUpdate = 3;
constexpr int OpVoiceStateUpdate = 4;
constexpr int OpGuildSubscribe = 14;

// Go Live. Watching somebody's shared screen is a request on this socket, and
// the answer is a whole second voice server to connect to.
constexpr int OpStreamCreate = 18;
constexpr int OpStreamDelete = 19;
constexpr int OpStreamWatch = 20;
constexpr int OpStreamSetPaused = 22;

// Capability bitfield the desktop client sends. It tells Discord which
// optimised payload shapes this client understands.
constexpr int ClientCapabilities = 161789;

constexpr int MaxReconnectDelayMs = 60000;

// Give up after this many sign-ins that never reach READY. Anything above a
// couple means Discord is refusing us, not that the network is flaky.
constexpr int MaxIdentifiesWithoutReady = 4;

} // namespace

GatewayClient::GatewayClient(QObject *parent)
    : QObject(parent)
    // The origin belongs to the socket itself. Setting it as a raw header too
    // would put "Origin" in the handshake twice, which some proxies reject.
    , m_socket(QStringLiteral("https://discord.com"))
{
    m_heartbeatTimer.setSingleShot(false);
    m_reconnectTimer.setSingleShot(true);

    connect(&m_socket, &QWebSocket::connected, this, &GatewayClient::onConnected);
    connect(&m_socket, &QWebSocket::disconnected, this, &GatewayClient::onDisconnected);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &GatewayClient::onTextMessage);
    connect(&m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        emit logLine(QStringLiteral("socket error: %1").arg(m_socket.errorString()));
        wlog(QStringLiteral("gateway"), QStringLiteral("socket error %1: %2")
                                            .arg(static_cast<int>(error))
                                            .arg(m_socket.errorString()));
    });

    // TLS trouble is silent otherwise, and it looks exactly like a rejected
    // login from the outside.
    connect(&m_socket, &QWebSocket::sslErrors, this, [](const QList<QSslError> &errors) {
        for (const QSslError &error : errors)
            wlog(QStringLiteral("gateway"), QStringLiteral("ssl error: %1").arg(error.errorString()));
    });

    connect(&m_heartbeatTimer, &QTimer::timeout, this, &GatewayClient::sendHeartbeat);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &GatewayClient::openSocket);

    m_firstHeartbeatTimer.setSingleShot(true);
    connect(&m_firstHeartbeatTimer, &QTimer::timeout, this, [this]() {
        if (m_socket.state() != QAbstractSocket::ConnectedState)
            return;
        sendHeartbeat();
        m_heartbeatTimer.start(m_heartbeatInterval);
    });
}

void GatewayClient::start(const QString &token)
{
    // The token itself never goes in the log. Its shape is enough to tell a
    // missing token apart from a rejected one.
    wlog(QStringLiteral("gateway"), QStringLiteral("start: token length=%1, dots=%2")
                                        .arg(token.length())
                                        .arg(token.count(QLatin1Char('.'))));
    m_token = token;
    m_wantConnection = true;
    m_reconnectAttempts = 0;
    m_canResume = false;
    m_sessionId.clear();
    m_lastSequence = -1;
    openSocket();
}

void GatewayClient::stop()
{
    m_wantConnection = false;
    m_reconnectTimer.stop();
    m_heartbeatTimer.stop();
    m_firstHeartbeatTimer.stop();
    m_subscribedGuilds.clear();
    m_subscribedChannels.clear();
    m_recentSends.clear();
    if (m_socket.state() != QAbstractSocket::UnconnectedState)
        m_socket.close();
    setState(State::Disconnected);
}

void GatewayClient::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

void GatewayClient::openSocket()
{
    if (!m_wantConnection)
        return;

    const QString url = (m_canResume && !m_resumeGatewayUrl.isEmpty())
        ? QStringLiteral("%1/?v=%2&encoding=json").arg(m_resumeGatewayUrl).arg(DiscordIdentity::ApiVersion)
        : DiscordIdentity::gatewayUrl();

    emit logLine(QStringLiteral("connecting to %1").arg(url));
    wlog(QStringLiteral("gateway"), QStringLiteral("opening %1 (resume=%2)").arg(url).arg(m_canResume));
    setState(m_reconnectAttempts > 0 ? State::Reconnecting : State::Connecting);

    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, DiscordIdentity::userAgent());
    m_socket.open(request);
}

void GatewayClient::scheduleReconnect()
{
    if (!m_wantConnection)
        return;

    // Exponential backoff with jitter, capped at one minute.
    m_reconnectAttempts++;
    int delay = qMin(MaxReconnectDelayMs, 1000 * (1 << qMin(m_reconnectAttempts, 6)));
    delay += static_cast<int>(QRandomGenerator::global()->bounded(1000));

    emit logLine(QStringLiteral("reconnecting in %1 ms (attempt %2)").arg(delay).arg(m_reconnectAttempts));
    setState(State::Reconnecting);
    m_reconnectTimer.start(delay);
}

void GatewayClient::sendJson(const QJsonObject &payload)
{
    if (m_socket.state() != QAbstractSocket::ConnectedState)
        return;

    const int op = payload.value(QStringLiteral("op")).toInt(-1);

    // Discord allows about 120 commands a minute on this socket and closes it
    // without much explanation when that is passed.
    //
    // Heartbeat, identify and resume are never held back. Dropping one of
    // those does not save the connection, it kills it: an identify that never
    // goes out means the socket sits silent until Discord times it out, and
    // the client reconnects into the same trap for ever.
    const bool mustGoOut = op == OpHeartbeat || op == OpIdentify || op == OpResume;
    if (!mustGoOut) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        while (!m_recentSends.isEmpty() && now - m_recentSends.first() > 60000)
            m_recentSends.removeFirst();
        m_recentSends.append(now);

        if (m_recentSends.size() > 100) {
            wlog(QStringLiteral("gateway"),
                 QStringLiteral("holding back op %1: %2 commands in the last minute, near the limit")
                     .arg(op)
                     .arg(m_recentSends.size()));
            return;
        }
    }

    m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
}

void GatewayClient::onConnected()
{
    emit logLine(QStringLiteral("socket open"));
    wlog(QStringLiteral("gateway"), QStringLiteral("socket open"));
    m_awaitingHeartbeatAck = false;
}

void GatewayClient::onDisconnected()
{
    // Both, or a pending first beat fires into the next connection.
    m_heartbeatTimer.stop();
    m_firstHeartbeatTimer.stop();

    const int code = static_cast<int>(m_socket.closeCode());
    const QString reason = m_socket.closeReason();

    emit logLine(QStringLiteral("socket closed (code %1)").arg(code));

    // These codes never fix themselves, so stop instead of looping forever.
    // 4004 rejected token, 4010-4014 are a malformed or unsupported identify.
    bool fatal = (code == 4004) || (code >= 4010 && code <= 4014);

    QString meaning;
    switch (code) {
    case 4000: meaning = QStringLiteral("unknown error"); break;
    case 4001: meaning = QStringLiteral("unknown opcode"); break;
    case 4002: meaning = QStringLiteral("Discord could not read what we sent"); break;
    case 4003: meaning = QStringLiteral("we sent something before signing in"); break;
    case 4004: meaning = QStringLiteral("the token was rejected"); break;
    case 4005: meaning = QStringLiteral("we signed in twice"); break;
    case 4007: meaning = QStringLiteral("the resume point was wrong"); break;
    case 4008: meaning = QStringLiteral("too many commands, rate limited"); break;
    case 4009: meaning = QStringLiteral("the session timed out"); break;
    case 4010: meaning = QStringLiteral("bad shard"); break;
    case 4011: meaning = QStringLiteral("sharding required"); break;
    case 4012: meaning = QStringLiteral("unknown gateway version"); break;
    case 4013: meaning = QStringLiteral("bad intents"); break;
    case 4014: meaning = QStringLiteral("intents not allowed"); break;
    case 1006: meaning = QStringLiteral("the connection dropped with no goodbye"); break;
    default:   meaning = QStringLiteral("closed"); break;
    }

    wlog(QStringLiteral("gateway"),
         QStringLiteral("closed: code=%1 (%2) reason=\"%3\" socketError=\"%4\" identifiesWithoutReady=%5")
             .arg(code)
             .arg(meaning, reason.isEmpty() ? QStringLiteral("(none)") : reason, m_socket.errorString())
             .arg(m_identifiesWithoutReady));

    // Discord does not always send a proper goodbye when it refuses a
    // session; it can simply drop the connection, which arrives here as an
    // ordinary lost connection. Repeated sign-ins that never reach READY mean
    // the same thing, so treat that as fatal too rather than looping for ever.
    if (!fatal && m_identifiesWithoutReady >= MaxIdentifiesWithoutReady) {
        fatal = true;
        meaning = QStringLiteral("signed in %1 times without ever connecting, so the session is "
                                 "being refused")
                      .arg(m_identifiesWithoutReady);
    }

    if (fatal) {
        m_wantConnection = false;
        setState(State::Disconnected);
        wlog(QStringLiteral("gateway"), QStringLiteral("giving up: %1 (close code %2)").arg(meaning).arg(code));
        emit fatalAuthError();
        return;
    }

    if (!m_wantConnection) {
        setState(State::Disconnected);
        return;
    }

    // The next join must actually go out. Deduping against the pre-drop
    // channel would swallow it, and Discord no longer has us in the call.
    m_voiceChannelId.clear();
    m_voiceGuildId.clear();

    if (m_reconnectImmediately) {
        m_reconnectImmediately = false;
        m_reconnectAttempts = 0;
        setState(State::Reconnecting);
        wlog(QStringLiteral("gateway"), QStringLiteral("opening immediately (Discord asked)"));
        QTimer::singleShot(0, this, [this]() {
            if (m_wantConnection)
                openSocket();
        });
        return;
    }
    scheduleReconnect();
}

void GatewayClient::onTextMessage(const QString &message)
{
    const QJsonObject packet = QJsonDocument::fromJson(message.toUtf8()).object();
    const int op = packet.value(QStringLiteral("op")).toInt(-1);
    const QJsonObject data = packet.value(QStringLiteral("d")).toObject();

    if (op == OpDispatch) {
        wlog(QStringLiteral("gateway"), QStringLiteral("event %1")
                                            .arg(packet.value(QStringLiteral("t")).toString()));
    } else if (op != OpHeartbeatAck) {
        wlog(QStringLiteral("gateway"), QStringLiteral("op %1").arg(op));
    }

    switch (op) {
    case OpHello:
        handleHello(data);
        break;

    case OpHeartbeatAck: {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_awaitingHeartbeatAck = false;
        m_missedHeartbeatAcks = 0;
        m_lastAckAt = now;

        // A slow reply is worth knowing about: it is the early warning for the
        // connection being dropped later.
        const qint64 roundTrip = m_lastBeatSentAt > 0 ? now - m_lastBeatSentAt : 0;
        if (roundTrip > 3000) {
            wlog(QStringLiteral("gateway"), QStringLiteral("slow heartbeat reply: %1 ms")
                                                .arg(roundTrip));
        }
        break;
    }

    case OpHeartbeat:
        // Discord can ask for an immediate beat.
        sendHeartbeat();
        break;

    case OpDispatch: {
        const qint64 sequence = static_cast<qint64>(packet.value(QStringLiteral("s")).toDouble(-1));
        if (sequence >= 0)
            m_lastSequence = sequence;
        handleDispatch(packet.value(QStringLiteral("t")).toString(), data, sequence);
        break;
    }

    case OpReconnect:
        emit logLine(QStringLiteral("gateway asked for a reconnect"));
        wlog(QStringLiteral("gateway"),
             QStringLiteral("opcode 7: reconnecting immediately and keeping the voice channel"));
        m_canResume = true;
        m_reconnectImmediately = true;
        m_socket.close();
        break;

    case OpInvalidSession: {
        // The boolean payload says whether the session can be resumed.
        const bool resumable = packet.value(QStringLiteral("d")).toBool(false);
        emit logLine(QStringLiteral("session invalidated (resumable: %1)").arg(resumable));
        m_canResume = resumable;
        if (!resumable) {
            m_sessionId.clear();
            m_lastSequence = -1;
        }
        m_socket.close();
        break;
    }

    default:
        break;
    }
}

void GatewayClient::handleHello(const QJsonObject &data)
{
    m_heartbeatInterval = data.value(QStringLiteral("heartbeat_interval")).toInt(41250);

    // A fresh connection starts with a clean slate. Carrying a stale "still
    // waiting for an acknowledgement" across a reconnect makes the very first
    // beat look unanswered, and the socket gets torn down for no reason.
    m_awaitingHeartbeatAck = false;
    m_missedHeartbeatAcks = 0;
    m_lastAckAt = 0;
    m_lastBeatSentAt = 0;
    m_heartbeatTimer.stop();
    m_firstHeartbeatTimer.stop();

    // The first beat is offset by a random fraction of the interval so a fleet
    // of clients does not beat in lockstep. Discord documents this explicitly.
    const int firstDelay =
        static_cast<int>(m_heartbeatInterval * QRandomGenerator::global()->generateDouble());
    m_firstHeartbeatTimer.start(firstDelay);

    if (m_canResume && !m_sessionId.isEmpty())
        sendResume();
    else
        sendIdentify();
}

void GatewayClient::sendHeartbeat()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (m_awaitingHeartbeatAck) {
        ++m_missedHeartbeatAcks;

        wlog(QStringLiteral("gateway"),
             QStringLiteral("heartbeat %1 unanswered (%2 ms since the last reply)")
                 .arg(m_missedHeartbeatAcks)
                 .arg(m_lastAckAt > 0 ? now - m_lastAckAt : -1));

        // One late reply is a slow moment, not a dead connection. Tearing the
        // socket down on the first one was causing needless reconnects.
        if (m_missedHeartbeatAcks < 3) {
            m_awaitingHeartbeatAck = false;   // let this beat go out anyway
        } else {
            emit logLine(QStringLiteral("connection went quiet, reconnecting"));
            wlog(QStringLiteral("gateway"), QStringLiteral("three unanswered heartbeats, dropping"));
            m_canResume = true;
            m_heartbeatTimer.stop();
            m_socket.abort();
            return;
        }
    }

    m_lastBeatSentAt = now;
    m_awaitingHeartbeatAck = true;
    QJsonObject packet{{QStringLiteral("op"), OpHeartbeat}};
    if (m_lastSequence >= 0)
        packet.insert(QStringLiteral("d"), m_lastSequence);
    else
        packet.insert(QStringLiteral("d"), QJsonValue::Null);
    sendJson(packet);
}

void GatewayClient::sendIdentify()
{
    setState(State::Identifying);

    const QJsonObject clientState{
        {QStringLiteral("guild_versions"), QJsonObject{}},
        {QStringLiteral("highest_last_message_id"), QStringLiteral("0")},
        {QStringLiteral("read_state_version"), 0},
        {QStringLiteral("user_guild_settings_version"), -1},
        {QStringLiteral("private_channels_version"), QStringLiteral("0")},
        {QStringLiteral("api_code_version"), 0},
    };

    // Activities stay empty on purpose. The official client fills this with
    // whatever game it caught you running, which nobody asked it to look for.
    const QJsonObject presence{
        {QStringLiteral("status"), m_presenceStatus},
        {QStringLiteral("since"), 0},
        {QStringLiteral("activities"), QJsonArray{}},
        {QStringLiteral("afk"), false},
    };

    const QJsonObject payload{
        {QStringLiteral("op"), OpIdentify},
        {QStringLiteral("d"),
         QJsonObject{
             {QStringLiteral("token"), m_token},
             {QStringLiteral("capabilities"), ClientCapabilities},
             {QStringLiteral("properties"), DiscordIdentity::superProperties()},
             {QStringLiteral("presence"), presence},
             {QStringLiteral("compress"), false},
             {QStringLiteral("client_state"), clientState},
         }},
    };

    ++m_identifiesWithoutReady;

    emit logLine(QStringLiteral("sending IDENTIFY"));
    wlog(QStringLiteral("gateway"), QStringLiteral("sending IDENTIFY (%1 bytes, attempt %2)")
                                        .arg(QJsonDocument(payload).toJson(QJsonDocument::Compact).size())
                                        .arg(m_identifiesWithoutReady));
    sendJson(payload);
}

void GatewayClient::sendResume()
{
    emit logLine(QStringLiteral("sending RESUME for session %1").arg(m_sessionId));
    sendJson(QJsonObject{
        {QStringLiteral("op"), OpResume},
        {QStringLiteral("d"),
         QJsonObject{
             {QStringLiteral("token"), m_token},
             {QStringLiteral("session_id"), m_sessionId},
             {QStringLiteral("seq"), m_lastSequence},
         }},
    });
}

void GatewayClient::subscribeToGuild(const QString &guildId, const QString &channelId)
{
    if (guildId.isEmpty())
        return;

    // Nothing to gain from asking the same question twice, and every repeat
    // counts against the command limit that closes the socket.
    if (m_subscribedGuilds.contains(guildId) && m_subscribedChannels.value(guildId) == channelId)
        return;

    m_subscribedGuilds.insert(guildId);
    m_subscribedChannels.insert(guildId, channelId);

    // Asking for rows 0 to 99 of a channel's member list is what makes Discord
    // send everyone's status. With an empty channel map it answers with
    // nothing, which is why every member looked offline.
    QJsonObject channels;
    if (!channelId.isEmpty()) {
        // Discord wants a list OF ranges: [[0, 99]].
        //
        // This must be built a step at a time. Writing QJsonArray{QJsonArray{0, 99}}
        // looks right and is not: a braced list holding one item of the same
        // type is a copy, not a nesting, so it quietly produced [0, 99]. That
        // one wrong pair of brackets made Discord answer "Error while decoding
        // payload" and close the connection, over and over.
        QJsonArray firstRange;
        firstRange.append(0);
        firstRange.append(99);

        QJsonArray ranges;
        ranges.append(firstRange);

        channels.insert(channelId, ranges);
    }

    wlog(QStringLiteral("gateway"), QStringLiteral("subscribing to guild %1%2")
                                        .arg(guildId,
                                             channelId.isEmpty()
                                                 ? QStringLiteral(" (no member list)")
                                                 : QStringLiteral(" via channel %1").arg(channelId)));

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpGuildSubscribe},
        {QStringLiteral("d"),
         QJsonObject{
             {QStringLiteral("guild_id"), guildId},
             {QStringLiteral("typing"), true},
             {QStringLiteral("threads"), true},
             {QStringLiteral("activities"), true},
             {QStringLiteral("members"), QJsonArray{}},
             {QStringLiteral("channels"), channels},
             {QStringLiteral("thread_member_lists"), QJsonArray{}},
         }},
    });
}

void GatewayClient::setPresenceStatus(const QString &status)
{
    if (status != QLatin1String("online") && status != QLatin1String("idle")
        && status != QLatin1String("dnd") && status != QLatin1String("invisible"))
        return;

    m_presenceStatus = status;
    wlog(QStringLiteral("gateway"), QStringLiteral("presence is now \"%1\"").arg(status));
    publishPresence();
}

void GatewayClient::publishPresence()
{
    // A sign-in that has not finished yet cannot carry this. The ready
    // handler sends it the moment the session exists.
    if (m_state != State::Ready || m_socket.state() != QAbstractSocket::ConnectedState)
        return;

    // Idle is the only status that carries a time. Everyone else sends 0,
    // which means "not idle". A missing or wrong `since` is dropped whole,
    // and the status on screen never leaves this machine.
    const bool idle = m_presenceStatus == QLatin1String("idle");
    const QJsonObject body{
        {QStringLiteral("since"), idle ? QJsonValue(QDateTime::currentMSecsSinceEpoch()) : QJsonValue(0)},
        {QStringLiteral("activities"), QJsonArray{}},
        {QStringLiteral("status"), m_presenceStatus},
        {QStringLiteral("afk"), idle},
    };

    wlog(QStringLiteral("gateway"),
         QStringLiteral("telling Discord %1")
             .arg(QString::fromUtf8(QJsonDocument(body).toJson(QJsonDocument::Compact))));

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpPresenceUpdate},
        {QStringLiteral("d"), body},
    });
}

void GatewayClient::joinVoice(const QString &guildId, const QString &channelId, bool selfMute, bool selfDeaf,
                              bool force)
{
    if (channelId.isEmpty())
        return;

    // Repeating an identical request achieves nothing and spends one of the
    // limited commands, so the mute and deafen buttons cannot rattle the
    // connection by toggling quickly. A rejoin after a drop must still send.
    if (!force && m_voiceChannelId == channelId && m_voiceGuildId == guildId && m_voiceMuted == selfMute
        && m_voiceDeafened == selfDeaf) {
        return;
    }
    m_voiceMuted = selfMute;
    m_voiceDeafened = selfDeaf;

    m_voiceGuildId = guildId;
    m_voiceChannelId = channelId;

    const QJsonObject request{
        {QStringLiteral("op"), OpVoiceStateUpdate},
        {QStringLiteral("d"),
         QJsonObject{
             {QStringLiteral("guild_id"), guildId.isEmpty() ? QJsonValue::Null : QJsonValue(guildId)},
             {QStringLiteral("channel_id"), channelId},
             {QStringLiteral("self_mute"), selfMute},
             {QStringLiteral("self_deaf"), selfDeaf},
             {QStringLiteral("self_video"), m_selfVideo},
         }},
    };

    // Written out in full. A join that Discord answers with silence gives no
    // other way to tell a wrong field from a message that never left.
    wlog(QStringLiteral("voice"),
         QStringLiteral("joining channel %1 in guild %2, sending %3")
             .arg(channelId, guildId,
                  QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact))));

    sendJson(request);
}

QString GatewayClient::streamKeyFor(const QString &guildId, const QString &channelId,
                                    const QString &userId)
{
    // A private call has no server, and its key is shorter for that reason.
    if (guildId.isEmpty())
        return QStringLiteral("call:%1:%2").arg(channelId, userId);
    return QStringLiteral("guild:%1:%2:%3").arg(guildId, channelId, userId);
}

void GatewayClient::watchStream(const QString &streamKey)
{
    if (streamKey.isEmpty())
        return;

    wlog(QStringLiteral("stream"), QStringLiteral("asking to watch %1").arg(streamKey));

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpStreamWatch},
        {QStringLiteral("d"), QJsonObject{{QStringLiteral("stream_key"), streamKey}}},
    });
}

void GatewayClient::stopWatchingStream(const QString &streamKey)
{
    if (streamKey.isEmpty())
        return;

    wlog(QStringLiteral("stream"), QStringLiteral("no longer watching %1").arg(streamKey));

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpStreamDelete},
        {QStringLiteral("d"), QJsonObject{{QStringLiteral("stream_key"), streamKey}}},
    });
}

void GatewayClient::startStream(const QString &guildId, const QString &channelId)
{
    if (channelId.isEmpty())
        return;

    wlog(QStringLiteral("stream"),
         QStringLiteral("going live in channel %1").arg(channelId));

    // "guild" and "call" are the two kinds of stream, and they are told apart
    // by whether a server is involved - the same distinction the stream key
    // makes. A private call has no guild_id at all rather than an empty one.
    QJsonObject data{
        {QStringLiteral("type"), guildId.isEmpty() ? QStringLiteral("call")
                                                   : QStringLiteral("guild")},
        {QStringLiteral("channel_id"), channelId},

        // Which region to put the stream server in. Discord picks a sensible
        // one when this is absent, and picking badly ourselves would add a
        // detour to every packet.
        {QStringLiteral("preferred_region"), QJsonValue::Null},
    };
    if (!guildId.isEmpty())
        data.insert(QStringLiteral("guild_id"), guildId);

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpStreamCreate},
        {QStringLiteral("d"), data},
    });
}

void GatewayClient::stopStream(const QString &streamKey)
{
    if (streamKey.isEmpty())
        return;

    wlog(QStringLiteral("stream"), QStringLiteral("ending our stream %1").arg(streamKey));

    // The same opcode that stops watching somebody else's. Which one it means
    // is decided by whose key it carries.
    sendJson(QJsonObject{
        {QStringLiteral("op"), OpStreamDelete},
        {QStringLiteral("d"), QJsonObject{{QStringLiteral("stream_key"), streamKey}}},
    });
}

void GatewayClient::setStreamPaused(const QString &streamKey, bool paused)
{
    if (streamKey.isEmpty())
        return;

    sendJson(QJsonObject{
        {QStringLiteral("op"), OpStreamSetPaused},
        {QStringLiteral("d"),
         QJsonObject{{QStringLiteral("stream_key"), streamKey},
                     {QStringLiteral("paused"), paused}}},
    });
}

void GatewayClient::setSelfVideo(bool on)
{
    if (m_selfVideo == on)
        return;
    m_selfVideo = on;
    if (m_voiceChannelId.isEmpty())
        return;
    joinVoice(m_voiceGuildId, m_voiceChannelId, m_voiceMuted, m_voiceDeafened, true);
}

void GatewayClient::leaveVoice(const QString &guildId)
{
    wlog(QStringLiteral("voice"), QStringLiteral("leaving voice in guild %1").arg(guildId));
    m_selfVideo = false;

    // A null channel is how Discord is told you left.
    sendJson(QJsonObject{
        {QStringLiteral("op"), OpVoiceStateUpdate},
        {QStringLiteral("d"),
         QJsonObject{
             {QStringLiteral("guild_id"), guildId.isEmpty() ? QJsonValue::Null : QJsonValue(guildId)},
             {QStringLiteral("channel_id"), QJsonValue::Null},
             {QStringLiteral("self_mute"), false},
             {QStringLiteral("self_deaf"), false},
             {QStringLiteral("self_video"), false},
         }},
    });

    m_voiceGuildId.clear();
    m_voiceChannelId.clear();
}

void GatewayClient::handleDispatch(const QString &type, const QJsonObject &data, qint64 sequence)
{
    Q_UNUSED(sequence)

    if (type == QLatin1String("READY")) {
        m_sessionId = data.value(QStringLiteral("session_id")).toString();
        m_resumeGatewayUrl = data.value(QStringLiteral("resume_gateway_url")).toString();
        m_currentUser = data.value(QStringLiteral("user")).toObject();
        m_reconnectAttempts = 0;
        m_identifiesWithoutReady = 0;
        m_canResume = true;
        // A fresh session knows nothing about what we asked for before.
        m_subscribedGuilds.clear();
        m_subscribedChannels.clear();
        setState(State::Ready);
        publishPresence();
        emit logLine(QStringLiteral("ready as %1").arg(m_currentUser.value(QStringLiteral("username")).toString()));
        wlog(QStringLiteral("gateway"),
             QStringLiteral("READY: user=%1 guilds=%2 dms=%3")
                 .arg(m_currentUser.value(QStringLiteral("username")).toString())
                 .arg(data.value(QStringLiteral("guilds")).toArray().size())
                 .arg(data.value(QStringLiteral("private_channels")).toArray().size()));
        emit ready(data);
    } else if (type == QLatin1String("RESUMED")) {
        m_reconnectAttempts = 0;
        setState(State::Ready);
        publishPresence();
        emit logLine(QStringLiteral("session resumed"));
    }

    emit dispatch(type, data);
}
