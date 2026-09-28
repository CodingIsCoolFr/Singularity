#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QSet>

class QLocalSocket;
class RestClient;

// The pipe a game talks to when it wants a card on your profile.
//
// Discord's own client listens on discord-ipc-0 and folds whatever a game
// sends into the same presence as its own Playing card. That is how two
// cards show at once. This is that listener, so a game does not need the
// official client open to be seen.
class DiscordIpcServer : public QObject
{
    Q_OBJECT

public:
    explicit DiscordIpcServer(QObject *parent = nullptr);

    void start();
    void setUser(const QJsonObject &user);
    void setRest(RestClient *rest) { m_rest = rest; }

    // Gateway-shaped activities, one per game that has set one. Empty when
    // nothing is connected. Singularity's own card is not in here.
    // `_exe` is the program file name. The caller removes it before sending.
    QJsonArray activities() const;

signals:
    void activityChanged();

private:
    struct Client
    {
        QString applicationId;
        QString exe;            // program file name, lower case
        QByteArray buffer;
        QJsonObject activity;   // empty when the game has cleared it
    };

    void onNewConnection();
    void onReadyRead(QLocalSocket *socket);
    void onDisconnected(QLocalSocket *socket);
    void handleFrame(QLocalSocket *socket, quint32 opcode, const QByteArray &payload);
    void noteExe(QLocalSocket *socket);
    void requestProxy(const QString &applicationId, const QJsonObject &activity);
    void sendFrame(QLocalSocket *socket, quint32 opcode, const QJsonObject &payload);
    void publish();

    QLocalServer m_server;
    QHash<QLocalSocket *, Client> m_clients;
    QJsonObject m_user;
    RestClient *m_rest = nullptr;
    QHash<QString, QString> m_proxied;   // https url -> mp: key
    QSet<QString> m_proxyPending;
};
