#include "core/DiscordIpcServer.h"

#include "core/Logger.h"

#include <QJsonDocument>
#include <QLocalSocket>
#include <QSet>
#include <QtEndian>

namespace {

// The four opcodes Discord's pipe uses. A game speaks the first; we answer
// with the second. Ping and pong are the keepalive.
constexpr quint32 OpcodeHandshake = 0;
constexpr quint32 OpcodeFrame = 1;
constexpr quint32 OpcodePing = 3;
constexpr quint32 OpcodePong = 4;

// A timestamp under this is seconds. The pipe speaks seconds. The gateway
// speaks milliseconds, and sending one as the other makes the clock show
// fifty years.
constexpr qint64 SecondsCeiling = 10000000000LL;

qint64 gatewayTimestamp(const QJsonValue &value)
{
    if (!value.isDouble())
        return 0;
    qint64 stamp = static_cast<qint64>(value.toDouble());
    if (stamp > 0 && stamp < SecondsCeiling)
        stamp *= 1000;
    return stamp;
}

QJsonObject gatewayActivity(const QString &applicationId, const QJsonObject &rpc)
{
    QJsonObject out;
    out.insert(QStringLiteral("type"), rpc.value(QStringLiteral("type")).toInt());
    out.insert(QStringLiteral("application_id"), applicationId);

    QString name = rpc.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty())
        name = rpc.value(QStringLiteral("details")).toString().trimmed();
    if (name.isEmpty())
        name = QStringLiteral("Game");
    out.insert(QStringLiteral("name"), name);

    const QString details = rpc.value(QStringLiteral("details")).toString();
    const QString state = rpc.value(QStringLiteral("state")).toString();
    if (!details.isEmpty())
        out.insert(QStringLiteral("details"), details);
    if (!state.isEmpty())
        out.insert(QStringLiteral("state"), state);

    const QJsonObject assets = rpc.value(QStringLiteral("assets")).toObject();
    if (!assets.isEmpty())
        out.insert(QStringLiteral("assets"), assets);

    const QJsonObject timestamps = rpc.value(QStringLiteral("timestamps")).toObject();
    if (!timestamps.isEmpty()) {
        QJsonObject scaled;
        const qint64 start = gatewayTimestamp(timestamps.value(QStringLiteral("start")));
        const qint64 end = gatewayTimestamp(timestamps.value(QStringLiteral("end")));
        if (start > 0)
            scaled.insert(QStringLiteral("start"), start);
        if (end > 0)
            scaled.insert(QStringLiteral("end"), end);
        if (!scaled.isEmpty())
            out.insert(QStringLiteral("timestamps"), scaled);
    }

    // The pipe sends buttons as objects. The gateway wants the words in one
    // list and the links in another, and it will not take more than two.
    QJsonArray labels;
    QJsonArray urls;
    const QJsonArray buttons = rpc.value(QStringLiteral("buttons")).toArray();
    for (const QJsonValue &value : buttons) {
        if (labels.size() >= 2)
            break;
        if (value.isObject()) {
            const QString label = value.toObject().value(QStringLiteral("label")).toString();
            const QString url = value.toObject().value(QStringLiteral("url")).toString();
            if (label.isEmpty())
                continue;
            labels.append(label);
            urls.append(url);
        } else if (value.isString() && !value.toString().isEmpty()) {
            labels.append(value.toString());
            urls.append(QString());
        }
    }
    if (!labels.isEmpty()) {
        out.insert(QStringLiteral("buttons"), labels);
        out.insert(QStringLiteral("metadata"),
                   QJsonObject{{QStringLiteral("button_urls"), urls}});
    }

    out.insert(QStringLiteral("instance"), false);
    return out;
}

} // namespace

DiscordIpcServer::DiscordIpcServer(QObject *parent)
    : QObject(parent)
{
    connect(&m_server, &QLocalServer::newConnection, this, &DiscordIpcServer::onNewConnection);
}

void DiscordIpcServer::start()
{
    if (m_server.isListening())
        return;

    // The first free pipe. A game tries discord-ipc-0, then 1, and so on.
    // If the official client already has one, it keeps it; we take the next
    // free slot rather than kicking it off.
    for (int i = 0; i < 10; ++i) {
        const QString name = QStringLiteral("discord-ipc-%1").arg(i);
        if (m_server.listen(name)) {
            wlog(QStringLiteral("presence"),
                 QStringLiteral("listening for a game's activity on %1").arg(name));
            return;
        }
        // A previous copy that died can leave the name taken with nobody
        // behind it. Clearing that and failing again means something live
        // has it, so try the next number.
        QLocalServer::removeServer(name);
        if (m_server.listen(name)) {
            wlog(QStringLiteral("presence"),
                 QStringLiteral("listening for a game's activity on %1").arg(name));
            return;
        }
    }

    wlog(QStringLiteral("presence"),
         QStringLiteral("no free pipe for a game's activity; the official client may already have them"));
}

void DiscordIpcServer::setUser(const QJsonObject &user)
{
    m_user = user;
}

QJsonArray DiscordIpcServer::activities() const
{
    QJsonArray out;
    QSet<QString> seen;
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        const Client &client = it.value();
        if (client.applicationId.isEmpty() || client.activity.isEmpty())
            continue;
        if (seen.contains(client.applicationId))
            continue;
        seen.insert(client.applicationId);
        out.append(gatewayActivity(client.applicationId, client.activity));
        if (out.size() >= 3)
            break;
    }
    return out;
}

void DiscordIpcServer::onNewConnection()
{
    while (QLocalSocket *socket = m_server.nextPendingConnection()) {
        m_clients.insert(socket, {});
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() { onDisconnected(socket); });
    }
}

void DiscordIpcServer::onReadyRead(QLocalSocket *socket)
{
    auto it = m_clients.find(socket);
    if (it == m_clients.end())
        return;

    Client &client = it.value();
    client.buffer.append(socket->readAll());

    while (client.buffer.size() >= 8) {
        const auto opcode = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(client.buffer.constData()));
        const auto size = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(client.buffer.constData() + 4));
        if (size > 1024U * 1024U) {
            socket->abort();
            return;
        }
        if (client.buffer.size() < 8 + static_cast<int>(size))
            return;
        const QByteArray payload = client.buffer.mid(8, static_cast<int>(size));
        client.buffer.remove(0, 8 + static_cast<int>(size));
        handleFrame(socket, opcode, payload);
    }
}

void DiscordIpcServer::onDisconnected(QLocalSocket *socket)
{
    const bool had = !m_clients.value(socket).activity.isEmpty();
    m_clients.remove(socket);
    socket->deleteLater();
    if (had)
        publish();
}

void DiscordIpcServer::handleFrame(QLocalSocket *socket, quint32 opcode, const QByteArray &payload)
{
    auto it = m_clients.find(socket);
    if (it == m_clients.end())
        return;

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    const QJsonObject message = error.error == QJsonParseError::NoError && document.isObject()
        ? document.object()
        : QJsonObject{};

    if (opcode == OpcodePing) {
        sendFrame(socket, OpcodePong, message);
        return;
    }

    if (opcode == OpcodeHandshake) {
        it->applicationId = message.value(QStringLiteral("client_id")).toString();
        QJsonObject user = m_user;
        if (!user.contains(QStringLiteral("discriminator")))
            user.insert(QStringLiteral("discriminator"), QStringLiteral("0"));
        sendFrame(socket, OpcodeFrame, QJsonObject{
            {QStringLiteral("cmd"), QStringLiteral("DISPATCH")},
            {QStringLiteral("evt"), QStringLiteral("READY")},
            {QStringLiteral("nonce"), QJsonValue::Null},
            {QStringLiteral("data"),
             QJsonObject{
                 {QStringLiteral("v"), 1},
                 {QStringLiteral("config"),
                  QJsonObject{
                      {QStringLiteral("cdn_host"), QStringLiteral("cdn.discordapp.com")},
                      {QStringLiteral("api_endpoint"), QStringLiteral("//discord.com/api")},
                      {QStringLiteral("environment"), QStringLiteral("production")},
                  }},
                 {QStringLiteral("user"), user},
             }},
        });
        wlog(QStringLiteral("presence"),
             QStringLiteral("a game connected (%1)").arg(it->applicationId));
        return;
    }

    if (opcode != OpcodeFrame)
        return;

    const QString command = message.value(QStringLiteral("cmd")).toString();
    const QString nonce = message.value(QStringLiteral("nonce")).toString();

    if (command == QLatin1String("SET_ACTIVITY")) {
        const QJsonValue activity = message.value(QStringLiteral("args")).toObject()
                                        .value(QStringLiteral("activity"));
        if (activity.isNull() || !activity.isObject() || activity.toObject().isEmpty()) {
            it->activity = {};
            wlog(QStringLiteral("presence"),
                 QStringLiteral("a game cleared its activity (%1)").arg(it->applicationId));
        } else {
            it->activity = activity.toObject();
            wlog(QStringLiteral("presence"),
                 QStringLiteral("a game set \"%1\" (%2)")
                     .arg(it->activity.value(QStringLiteral("details")).toString(),
                          it->applicationId));
        }

        QJsonObject echoed = it->activity;
        if (!it->applicationId.isEmpty())
            echoed.insert(QStringLiteral("application_id"), it->applicationId);
        sendFrame(socket, OpcodeFrame, QJsonObject{
            {QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY")},
            {QStringLiteral("evt"), QJsonValue::Null},
            {QStringLiteral("nonce"), nonce},
            {QStringLiteral("data"), echoed.isEmpty() ? QJsonObject{} : echoed},
        });
        publish();
        return;
    }

    // Anything else gets an answer, so a game waiting on a nonce does not
    // sit there. We do not implement invites or subscriptions.
    sendFrame(socket, OpcodeFrame, QJsonObject{
        {QStringLiteral("cmd"), command},
        {QStringLiteral("evt"), QStringLiteral("ERROR")},
        {QStringLiteral("nonce"), nonce},
        {QStringLiteral("data"),
         QJsonObject{
             {QStringLiteral("code"), 4006},
             {QStringLiteral("message"), QStringLiteral("not supported")},
         }},
    });
}

void DiscordIpcServer::sendFrame(QLocalSocket *socket, quint32 opcode, const QJsonObject &payload)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return;
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QByteArray frame(8, Qt::Uninitialized);
    qToLittleEndian<quint32>(opcode, reinterpret_cast<uchar *>(frame.data()));
    qToLittleEndian<quint32>(static_cast<quint32>(body.size()),
                            reinterpret_cast<uchar *>(frame.data() + 4));
    frame.append(body);
    socket->write(frame);
    socket->flush();
}

void DiscordIpcServer::publish()
{
    emit activityChanged();
}
