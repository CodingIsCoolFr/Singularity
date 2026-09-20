#include "core/RestClient.h"

#include "core/DiscordIdentity.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QUrl>

RestClient::RestClient(QObject *parent)
    : QObject(parent)
{
    m_network.setAutoDeleteReplies(false);
}

void RestClient::setToken(const QString &token)
{
    m_token = token;
}

QNetworkRequest RestClient::buildRequest(const QString &path) const
{
    QNetworkRequest request(QUrl(DiscordIdentity::restBase() + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, DiscordIdentity::userAgent());
    // A user token is sent bare. Only bot tokens carry the "Bot " prefix.
    if (!m_token.isEmpty())
        request.setRawHeader("Authorization", m_token.toUtf8());
    request.setRawHeader("X-Super-Properties", DiscordIdentity::superPropertiesHeader());
    request.setRawHeader("X-Discord-Locale", "en-US");
    request.setRawHeader("X-Debug-Options", "bugReporterEnabled");
    request.setRawHeader("Accept", "*/*");
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setRawHeader("Origin", "https://discord.com");
    request.setRawHeader("Referer", "https://discord.com/channels/@me");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

void RestClient::dispatch(QNetworkReply *reply, ObjectHandler onObject, ArrayHandler onArray, ErrorHandler onError)
{
    connect(reply, &QNetworkReply::finished, this, [this, reply, onObject, onArray, onError]() {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(payload);

        if (status >= 200 && status < 300) {
            if (doc.isArray() && onArray)
                onArray(doc.array());
            else if (doc.isObject() && onObject)
                onObject(doc.object());
            else if (onObject)
                onObject(QJsonObject{});
            return;
        }

        Error error;
        error.httpStatus = status;
        error.body = doc.isObject() ? doc.object() : QJsonObject{};
        error.message = error.body.value(QStringLiteral("message")).toString();
        if (error.message.isEmpty())
            error.message = reply->errorString();

        if (status == 401)
            emit unauthorized();

        if (onError)
            onError(error);
    });
}

void RestClient::fetchCurrentUser(ObjectHandler onOk, ErrorHandler onError)
{
    QNetworkReply *reply = m_network.get(buildRequest(QStringLiteral("/users/@me")));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::fetchMessages(const QString &channelId, int limit, ArrayHandler onOk, ErrorHandler onError)
{
    const QString path = QStringLiteral("/channels/%1/messages?limit=%2").arg(channelId).arg(limit);
    QNetworkReply *reply = m_network.get(buildRequest(path));
    dispatch(reply, nullptr, std::move(onOk), std::move(onError));
}

void RestClient::sendMessage(const QString &channelId, const QString &content, ObjectHandler onOk, ErrorHandler onError)
{
    // The nonce lets the official client match its own echoes. Discord accepts
    // any unique string, so a random 64 bit number is enough.
    const QString nonce = QString::number(QRandomGenerator::global()->generate64());

    const QJsonObject body{
        {QStringLiteral("content"), content},
        {QStringLiteral("nonce"), nonce},
        {QStringLiteral("tts"), false},
        {QStringLiteral("flags"), 0},
    };

    const QString path = QStringLiteral("/channels/%1/messages").arg(channelId);
    QNetworkReply *reply = m_network.post(buildRequest(path), QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::sendTyping(const QString &channelId)
{
    const QString path = QStringLiteral("/channels/%1/typing").arg(channelId);
    QNetworkReply *reply = m_network.post(buildRequest(path), QByteArray());
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

void RestClient::openDirectMessage(const QString &userId, ObjectHandler onOk, ErrorHandler onError)
{
    const QJsonObject body{{QStringLiteral("recipients"), QJsonArray{userId}}};
    QNetworkReply *reply = m_network.post(buildRequest(QStringLiteral("/users/@me/channels")),
                                          QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::addFriend(const QString &userId, ObjectHandler onOk, ErrorHandler onError)
{
    // An empty object means "ordinary friend request".
    const QString path = QStringLiteral("/users/@me/relationships/%1").arg(userId);
    QNetworkReply *reply = m_network.put(buildRequest(path), QByteArray("{}"));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::removeRelationship(const QString &userId, ObjectHandler onOk, ErrorHandler onError)
{
    const QString path = QStringLiteral("/users/@me/relationships/%1").arg(userId);
    QNetworkReply *reply = m_network.deleteResource(buildRequest(path));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::blockUser(const QString &userId, ObjectHandler onOk, ErrorHandler onError)
{
    const QJsonObject body{{QStringLiteral("type"), 2}};
    const QString path = QStringLiteral("/users/@me/relationships/%1").arg(userId);
    QNetworkReply *reply = m_network.put(buildRequest(path), QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::fetchUser(const QString &userId, ObjectHandler onOk, ErrorHandler onError)
{
    QNetworkReply *reply = m_network.get(buildRequest(QStringLiteral("/users/%1").arg(userId)));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::fetchUserProfile(const QString &userId, const QString &guildId, ObjectHandler onOk,
                                  ErrorHandler onError)
{
    QString path = QStringLiteral("/users/%1/profile"
                                  "?with_mutual_guilds=true&with_mutual_friends=true"
                                  "&with_mutual_friends_count=true")
                       .arg(userId);
    if (!guildId.isEmpty())
        path += QStringLiteral("&guild_id=%1").arg(guildId);

    QNetworkReply *reply = m_network.get(buildRequest(path));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::createInvite(const QString &channelId, ObjectHandler onOk, ErrorHandler onError)
{
    // The same defaults the real client uses: one day, unlimited uses.
    const QJsonObject body{
        {QStringLiteral("max_age"), 86400},
        {QStringLiteral("max_uses"), 0},
        {QStringLiteral("temporary"), false},
    };

    const QString path = QStringLiteral("/channels/%1/invites").arg(channelId);
    QNetworkReply *reply = m_network.post(buildRequest(path), QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::redeemGift(const QString &code, ObjectHandler onOk, ErrorHandler onError)
{
    // An empty body is all this endpoint needs. The official client also names
    // a payment source, which only matters for gifts that cost something.
    const QString path = QStringLiteral("/entitlements/gift-codes/%1/redeem").arg(code);
    QNetworkReply *reply = m_network.post(buildRequest(path), QByteArray("{}"));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::fetchNote(const QString &userId, ObjectHandler onOk, ErrorHandler onError)
{
    QNetworkReply *reply = m_network.get(buildRequest(QStringLiteral("/users/@me/notes/%1").arg(userId)));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::saveNote(const QString &userId, const QString &note, ObjectHandler onOk, ErrorHandler onError)
{
    const QJsonObject body{{QStringLiteral("note"), note}};
    const QString path = QStringLiteral("/users/@me/notes/%1").arg(userId);
    QNetworkReply *reply = m_network.put(buildRequest(path), QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

QString RestClient::avatarUrl(const QString &userId, const QString &avatarHash, int size)
{
    if (avatarHash.isEmpty())
        return {};
    const QString ext = avatarHash.startsWith(QStringLiteral("a_")) ? QStringLiteral("gif") : QStringLiteral("png");
    return QStringLiteral("https://cdn.discordapp.com/avatars/%1/%2.%3?size=%4")
        .arg(userId, avatarHash, ext)
        .arg(size);
}
