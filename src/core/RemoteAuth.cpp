#include "core/RemoteAuth.h"

#include "core/DiscordIdentity.h"
#include "core/Logger.h"
#include "core/QrCode.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalBlocker>
#include <QUrl>

namespace {

// v2 of the remote-auth gateway. The Origin header is checked, exactly as it is
// for the main gateway, so a page cannot open this connection on its own.
constexpr char GatewayUrl[] = "wss://remote-auth-gateway.discord.gg/?v=2";

// The phone app reads this, matches the fingerprint against the one the gateway
// showed it, and only then offers to sign in. discord.com and discordapp.com
// both resolve here; the app accepts either.
QString qrAddress(const QString &fingerprint)
{
    return QStringLiteral("https://discordapp.com/ra/") + fingerprint;
}

} // namespace

RemoteAuth::RemoteAuth(QObject *parent)
    : QObject(parent)
    // QWebSocket writes its own Origin header from this, and ignores one set
    // on the request. Left empty, the handshake went out with no Origin at
    // all and the gateway closed the connection straight away.
    , m_socket(QStringLiteral("https://discord.com"))
{
    connect(&m_socket, &QWebSocket::connected, this,
            []() { wlog(QStringLiteral("qr"), QStringLiteral("connected to the QR sign-in gateway")); });

    connect(&m_socket, &QWebSocket::textMessageReceived, this,
            [this](const QString &text) { handleMessage(text.toUtf8()); });

    connect(&m_socket, &QWebSocket::disconnected, this, [this]() {
        m_heartbeat.stop();
        wlog(QStringLiteral("qr"), QStringLiteral("gateway closed the connection: code %1 %2")
                                       .arg(int(m_socket.closeCode()))
                                       .arg(m_socket.closeReason()));
        if (m_started) {
            // 4003 is the gateway's own "the code was never used" timeout.
            if (m_socket.closeCode() == 4003) {
                emit expired();
            } else {
                emit failed(QStringLiteral("The QR sign-in connection dropped (code %1).")
                                .arg(int(m_socket.closeCode())));
            }
            m_started = false;
        }
    });

    connect(&m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        wlog(QStringLiteral("qr"), QStringLiteral("socket error: %1").arg(m_socket.errorString()));
        if (m_started)
            emit failed(QStringLiteral("Could not reach Discord for QR sign-in: %1")
                            .arg(m_socket.errorString()));
    });

    connect(&m_heartbeat, &QTimer::timeout, this,
            [this]() { sendJson(QJsonObject{{QStringLiteral("op"), QStringLiteral("heartbeat")}}); });
}

RemoteAuth::~RemoteAuth()
{
    stop();
}

void RemoteAuth::start()
{
    stop();

    if (!m_crypto.generate()) {
        emit failed(QStringLiteral("This computer could not create the sign-in key."));
        return;
    }

    m_started = true;

    QNetworkRequest request((QUrl(QLatin1String(GatewayUrl))));
    request.setHeader(QNetworkRequest::UserAgentHeader, DiscordIdentity::userAgent());
    m_socket.open(request);
    wlog(QStringLiteral("qr"), QStringLiteral("opening the QR sign-in connection"));
}

void RemoteAuth::stop()
{
    m_started = false;
    m_heartbeat.stop();
    m_fingerprint.clear();
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        // Quietly: this is us hanging up, not the gateway. Without the blocker
        // the old connection's "disconnected" could land after a restart and
        // report the new one as dropped.
        const QSignalBlocker quiet(&m_socket);
        m_socket.abort();
    }
}

void RemoteAuth::sendJson(const QJsonObject &object)
{
    sendText(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void RemoteAuth::sendText(const QString &text)
{
    m_socket.sendTextMessage(text);
}

void RemoteAuth::handleMessage(const QByteArray &json)
{
    const QJsonObject message = QJsonDocument::fromJson(json).object();
    const QString op = message.value(QStringLiteral("op")).toString();

    // The op names only. The payloads carry sealed data and the ticket,
    // none of which belongs in a log.
    if (op != QLatin1String("heartbeat_ack"))
        wlog(QStringLiteral("qr"), QStringLiteral("gateway says: %1").arg(op));

    if (op == QLatin1String("hello")) {
        const int interval = message.value(QStringLiteral("heartbeat_interval")).toInt(41250);
        m_heartbeat.start(interval);
        // Our public key, as the gateway wants it: plain base64 of the DER.
        sendJson(QJsonObject{
            {QStringLiteral("op"), QStringLiteral("init")},
            {QStringLiteral("encoded_public_key"), QString::fromLatin1(m_crypto.publicKeyDer().toBase64())},
        });
        return;
    }

    if (op == QLatin1String("heartbeat_ack"))
        return;

    if (op == QLatin1String("nonce_proof")) {
        // Prove we hold the private key: the gateway sealed a random nonce to
        // our public key; we open it and hand it straight back, base64url with
        // no padding, in a field confusingly also called "nonce".
        const QByteArray encrypted =
            QByteArray::fromBase64(message.value(QStringLiteral("encrypted_nonce")).toString().toLatin1());
        const QByteArray nonce = m_crypto.decrypt(encrypted);
        if (nonce.isEmpty()) {
            emit failed(QStringLiteral("The QR sign-in handshake could not be completed."));
            stop();
            return;
        }
        sendJson(QJsonObject{
            {QStringLiteral("op"), QStringLiteral("nonce_proof")},
            {QStringLiteral("nonce"), QString::fromLatin1(RemoteAuthCrypto::base64Url(nonce))},
        });
        return;
    }

    if (op == QLatin1String("pending_remote_init")) {
        // The gateway accepted the key. Show its fingerprint as the code.
        m_fingerprint = message.value(QStringLiteral("fingerprint")).toString();
        const QString url = qrAddress(m_fingerprint);
        const QImage code = QrCode::render(QrCode::encode(url.toUtf8()), 8);
        wlog(QStringLiteral("qr"), QStringLiteral("QR ready, waiting for a phone to scan it"));
        emit qrCodeReady(code, url);
        return;
    }

    if (op == QLatin1String("pending_ticket")) {
        // The phone scanned the code. The gateway sends who it is about to sign
        // in, sealed to our key, so the desktop can name the account.
        const QByteArray payload = m_crypto.decrypt(QByteArray::fromBase64(
            message.value(QStringLiteral("encrypted_user_payload")).toString().toLatin1()));
        // "id:discriminator:avatar:username".
        const QList<QByteArray> parts = payload.split(':');
        const QString username = parts.size() >= 4
            ? QString::fromUtf8(parts.mid(3).join(':'))
            : QStringLiteral("your account");
        emit scanned(username);
        return;
    }

    if (op == QLatin1String("pending_login")) {
        // Approved. Trade the ticket for the token.
        const QString ticket = message.value(QStringLiteral("ticket")).toString();
        wlog(QStringLiteral("qr"), QStringLiteral("approved on the phone, fetching the token"));
        exchangeTicket(ticket);
        return;
    }

    if (op == QLatin1String("cancel")) {
        emit declined();
        stop();
        return;
    }
}

void RemoteAuth::exchangeTicket(const QString &ticket)
{
    QNetworkRequest request(QUrl(DiscordIdentity::restBase() + QStringLiteral("/users/@me/remote-auth/login")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, DiscordIdentity::userAgent());
    request.setRawHeader("Origin", "https://discord.com");

    const QByteArray body =
        QJsonDocument(QJsonObject{{QStringLiteral("ticket"), ticket}}).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_network.post(request, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        onEncryptedToken(body.value(QStringLiteral("encrypted_token")).toString().toLatin1());
    });
}

void RemoteAuth::onEncryptedToken(const QByteArray &encryptedTokenBase64)
{
    const QByteArray token = m_crypto.decrypt(QByteArray::fromBase64(encryptedTokenBase64));
    if (token.isEmpty()) {
        emit failed(QStringLiteral("Signed in on the phone, but the token could not be read back."));
        stop();
        return;
    }
    emit succeeded(QString::fromUtf8(token));
    stop();
}
