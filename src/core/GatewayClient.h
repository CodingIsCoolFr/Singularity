#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QWebSocket>

// Live connection to the Discord gateway.
//
// Handles the parts that must be right or the socket dies: the HELLO/heartbeat
// contract, IDENTIFY, session resume after a drop, and backoff on reconnect.
// Everything else is handed upward as a raw dispatch event.
class GatewayClient : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Disconnected,
        Connecting,
        Identifying,
        Ready,
        Reconnecting,
    };
    Q_ENUM(State)

    explicit GatewayClient(QObject *parent = nullptr);

    void start(const QString &token);
    void stop();

    State state() const { return m_state; }
    QJsonObject currentUser() const { return m_currentUser; }

    // The live session. Voice needs this exact value, and it changes on every
    // fresh sign-in, so it must be read at the moment voice starts rather than
    // remembered from earlier.
    QString sessionId() const { return m_sessionId; }

    // Tells Discord which guild the user is looking at. Without this the
    // gateway never sends member or presence data for that guild.
    // `channelId` should be a text channel you can read. Without one Discord
    // sends no member list and therefore no statuses, so everybody in the
    // server looks offline.
    void subscribeToGuild(const QString &guildId, const QString &channelId = QString());

    // The status Discord broadcasts on your behalf.
    //
    // "online" normally. "invisible" makes everyone else see you as offline.
    // Be clear about what that does and does not do: Discord's own servers
    // still know you are connected, because you are. It hides you from other
    // people, not from Discord.
    //
    // Takes effect at once when already signed in, and is carried by the next
    // sign-in otherwise.
    void setPresenceStatus(const QString &status);
    QString presenceStatus() const { return m_presenceStatus; }

    // Sends the status we are holding, even when it has not changed. Needed
    // after sign-in: Discord does not take the status inside the first
    // identify as the one other people should see.
    void publishPresence();

    // Joins or leaves a voice channel.
    //
    // This is the first half of a voice connection: it puts you in the channel
    // so everyone sees you there, and makes Discord send back the voice server
    // details. VoiceConnection does the rest, and carries the sound.
    // `force` sends even when we already think we are in that channel. Needed
    // after a gateway drop: Discord has forgotten the call, but the ids match.
    void joinVoice(const QString &guildId, const QString &channelId, bool selfMute, bool selfDeaf,
                   bool force = false);
    void setSelfVideo(bool on);
    void leaveVoice(const QString &guildId);

    // Asks to watch somebody's shared screen.
    //
    // A Go Live stream is not carried by the voice connection. It has its own
    // server, its own websocket and its own packets, and the voice connection
    // has to stay up underneath it. This asks the main gateway for that second
    // server; the answer arrives as STREAM_CREATE and STREAM_SERVER_UPDATE.
    //
    // The key names the stream: "guild:<guild>:<channel>:<user>" in a server,
    // or "call:<channel>:<user>" in a private call.
    void watchStream(const QString &streamKey);
    void stopWatchingStream(const QString &streamKey);

    // Going live ourselves.
    //
    // The mirror of watchStream: it asks Discord to make a stream rather than
    // to join one, and the answer comes back as the same two events. The key
    // Discord then uses is the one naming us, so from that point on the code
    // that handles somebody else's stream handles ours as well.
    void startStream(const QString &guildId, const QString &channelId);
    void stopStream(const QString &streamKey);

    // Tells Discord nobody can see anything at the moment - a full screen
    // program took the display, or the window being shared was minimised.
    // Viewers are shown "paused" rather than a frozen picture.
    void setStreamPaused(const QString &streamKey, bool paused);

    static QString streamKeyFor(const QString &guildId, const QString &channelId,
                                const QString &userId);

    QString voiceChannelId() const { return m_voiceChannelId; }
    QString voiceGuildId() const { return m_voiceGuildId; }

signals:
    void stateChanged(GatewayClient::State state);
    void ready(const QJsonObject &readyPayload);
    void dispatch(const QString &eventType, const QJsonObject &data);
    void logLine(const QString &line);
    void fatalAuthError();

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString &message);

private:
    void setState(State state);
    void openSocket();
    void scheduleReconnect();
    void sendJson(const QJsonObject &payload);
    void sendIdentify();
    void sendResume();
    void sendHeartbeat();
    void handleHello(const QJsonObject &data);
    void handleDispatch(const QString &type, const QJsonObject &data, qint64 sequence);

    QWebSocket m_socket;
    QTimer m_heartbeatTimer;
    QTimer m_reconnectTimer;

    // The first beat is deliberately delayed by a random slice of the
    // interval. It needs to be a real timer we own, so that a reconnect can
    // cancel a pending one. A loose single shot left over from an old
    // connection will fire into the new one and knock it over.
    QTimer m_firstHeartbeatTimer;
    int m_heartbeatInterval = 41250;

    QString m_token;
    QString m_sessionId;
    QString m_resumeGatewayUrl;
    QJsonObject m_currentUser;

    State m_state = State::Disconnected;
    qint64 m_lastSequence = -1;
    int m_reconnectAttempts = 0;

    // How many times we have signed in without ever reaching READY. Discord
    // sometimes drops a refused session without a proper goodbye, which Qt
    // reports as an ordinary lost connection. Counting is the only reliable
    // way to tell "the network blipped" from "this session is dead".
    int m_identifiesWithoutReady = 0;
    // Heartbeat health. One missed reply is usually a slow moment, not a dead
    // connection, so the socket is only torn down after several.
    int m_missedHeartbeatAcks = 0;
    qint64 m_lastBeatSentAt = 0;
    qint64 m_lastAckAt = 0;
    bool m_awaitingHeartbeatAck = false;
    bool m_wantConnection = false;
    bool m_canResume = false;
    // Opcode 7: Discord asked us to resume now, not after the usual backoff.
    bool m_reconnectImmediately = false;
    QSet<QString> m_subscribedGuilds;

    // Which channel each guild was subscribed with, so clicking the same
    // server twice does not send the same request again. Discord counts every
    // command against a limit and hangs up when it is passed.
    QHash<QString, QString> m_subscribedChannels;

    // A rolling count of commands sent, purely so the log shows it before a
    // close rather than leaving us guessing.
    QList<qint64> m_recentSends;
    QString m_voiceGuildId;
    QString m_voiceChannelId;
    bool m_voiceMuted = false;
    bool m_voiceDeafened = false;
    bool m_selfVideo = false;

    // What Discord tells everybody else about you. The Anonymous plugin is
    // the only thing that changes this.
    QString m_presenceStatus{QStringLiteral("online")};
};
