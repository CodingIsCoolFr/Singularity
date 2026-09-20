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

    // Joins or leaves a voice channel.
    //
    // This is only the first half of a voice connection: it puts you in the
    // channel so everyone sees you there, and makes Discord send back the
    // voice server details. Carrying actual sound needs an Opus encoder and
    // libsodium, which this client does not have yet.
    void joinVoice(const QString &guildId, const QString &channelId, bool selfMute, bool selfDeaf);
    void leaveVoice(const QString &guildId);

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
};
