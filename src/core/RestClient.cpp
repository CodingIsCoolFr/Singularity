#include "core/RestClient.h"

#include "core/DiscordIdentity.h"
#include "core/Logger.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QTimer>
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
        const QJsonObject formErrors = error.body.value(QStringLiteral("errors")).toObject();
        if (!formErrors.isEmpty()) {
            error.message += QStringLiteral(" ")
                + QString::fromUtf8(QJsonDocument(formErrors).toJson(QJsonDocument::Compact));
        }
        if (error.message.isEmpty()) {
            const QString raw = QString::fromUtf8(payload.left(180)).simplified();
            error.message = raw.isEmpty() ? reply->errorString() : raw;
        }

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

void RestClient::fetchMessages(const QString &channelId, int limit, ArrayHandler onOk,
                               ErrorHandler onError, const QString &before)
{
    QString path = QStringLiteral("/channels/%1/messages?limit=%2").arg(channelId).arg(limit);
    if (!before.isEmpty())
        path += QStringLiteral("&before=%1").arg(before);

    QNetworkReply *reply = m_network.get(buildRequest(path));
    dispatch(reply, nullptr, std::move(onOk), std::move(onError));
}

void RestClient::leaveGuild(const QString &guildId, ObjectHandler onOk, ErrorHandler onError)
{
    // The official client's Leave Server: DELETE with {"lurking": false}.
    QNetworkReply *reply = m_network.sendCustomRequest(
        buildRequest(QStringLiteral("/users/@me/guilds/%1").arg(guildId)), QByteArrayLiteral("DELETE"),
        QJsonDocument(QJsonObject{{QStringLiteral("lurking"), false}}).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::uploadStreamPreview(const QString &streamKey, const QByteArray &jpeg, ObjectHandler onOk,
                                     ErrorHandler onError)
{
    // What the official client posts: a data URL of a JPEG, under
    // "thumbnail". Phones and anyone not watching yet see this picture on
    // the stream's tile; without it the tile is black.
    const QJsonObject body{
        {QStringLiteral("thumbnail"),
         QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(jpeg.toBase64())},
    };
    QNetworkReply *reply = m_network.post(buildRequest(QStringLiteral("/streams/%1/preview").arg(streamKey)),
                                          QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::forwardMessage(const QString &toChannelId, const QString &fromChannelId,
                                const QString &fromGuildId, const QString &messageId, ObjectHandler onOk,
                                ErrorHandler onError)
{
    const qint64 nonce = QDateTime::currentMSecsSinceEpoch() * 1000
        + QRandomGenerator::global()->bounded(1000);

    // What the official client sends: no words of its own, and a reference of
    // type 1 (FORWARD). Discord copies the original into message_snapshots.
    QJsonObject reference{
        {QStringLiteral("type"), 1},
        {QStringLiteral("message_id"), messageId},
        {QStringLiteral("channel_id"), fromChannelId},
    };
    if (!fromGuildId.isEmpty())
        reference.insert(QStringLiteral("guild_id"), fromGuildId);

    const QJsonObject body{
        {QStringLiteral("content"), QString()},
        {QStringLiteral("nonce"), QString::number(nonce)},
        {QStringLiteral("tts"), false},
        {QStringLiteral("flags"), 0},
        {QStringLiteral("message_reference"), reference},
    };
    QNetworkReply *reply = m_network.post(buildRequest(QStringLiteral("/channels/%1/messages").arg(toChannelId)),
                                          QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::sendMessage(const QString &channelId, const QString &content, const QString &replyTo,
                             const QStringList &files, ObjectHandler onOk, ErrorHandler onError,
                             const QString &stickerId, const CaptchaProof &captcha)
{
    // A nonce Discord will accept: a snowflake-sized number, not a random
    // 64-bit value that does not fit in a signed integer. The oversized one
    // is a 400, and the message never leaves the machine.
    const qint64 nonce = QDateTime::currentMSecsSinceEpoch() * 1000
        + QRandomGenerator::global()->bounded(1000);

    QJsonObject body{
        {QStringLiteral("content"), content},
        {QStringLiteral("nonce"), QString::number(nonce)},
        {QStringLiteral("tts"), false},
    };

    if (!stickerId.isEmpty())
        body.insert(QStringLiteral("sticker_ids"), QJsonArray{stickerId});

    if (!replyTo.isEmpty()) {
        body.insert(QStringLiteral("message_reference"),
                    QJsonObject{{QStringLiteral("message_id"), replyTo},
                                {QStringLiteral("channel_id"), channelId}});
        body.insert(QStringLiteral("allowed_mentions"),
                    QJsonObject{{QStringLiteral("replied_user"), true}});
    }

    const QString path = QStringLiteral("/channels/%1/messages").arg(channelId);
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);

    const auto withCaptcha = [&captcha](QNetworkRequest request) {
        if (captcha.key.isEmpty())
            return request;
        request.setRawHeader("X-Captcha-Key", captcha.key.toUtf8());
        if (!captcha.rqtoken.isEmpty())
            request.setRawHeader("X-Captcha-Rqtoken", captcha.rqtoken.toUtf8());
        if (!captcha.sessionId.isEmpty())
            request.setRawHeader("X-Captcha-Session-Id", captcha.sessionId.toUtf8());
        return request;
    };

    if (files.isEmpty()) {
        QNetworkReply *reply = m_network.post(withCaptcha(buildRequest(path)), json);
        dispatch(reply, std::move(onOk), nullptr, std::move(onError));
        return;
    }

    // The content type on the request has to be cleared. buildRequest sets
    // application/json, and leaving it there strips the multipart boundary
    // Discord needs in order to find the file.
    QNetworkRequest request = withCaptcha(buildRequest(path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QVariant());

    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart payload;
    payload.setHeader(QNetworkRequest::ContentDispositionHeader,
                      QVariant(QStringLiteral("form-data; name=\"payload_json\"")));
    payload.setBody(json);
    multi->append(payload);

    QMimeDatabase mime;
    for (int i = 0; i < files.size(); ++i) {
        auto *file = new QFile(files.at(i));
        if (!file->open(QIODevice::ReadOnly)) {
            delete file;
            continue;
        }
        file->setParent(multi);

        const QString name = QFileInfo(files.at(i)).fileName();
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QVariant(QStringLiteral("form-data; name=\"files[%1]\"; filename=\"%2\"")
                                    .arg(i)
                                    .arg(name)));
        part.setHeader(QNetworkRequest::ContentTypeHeader,
                       QVariant(mime.mimeTypeForFile(name).name()));
        part.setBodyDevice(file);
        multi->append(part);
    }

    QNetworkReply *reply = m_network.post(request, multi);
    multi->setParent(reply);
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::editMessage(const QString &channelId, const QString &messageId, const QString &content,
                             ObjectHandler onOk, ErrorHandler onError)
{
    const QJsonObject body{{QStringLiteral("content"), content}};
    const QString path = QStringLiteral("/channels/%1/messages/%2").arg(channelId, messageId);
    QNetworkReply *reply = m_network.sendCustomRequest(
        buildRequest(path), QByteArrayLiteral("PATCH"),
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::deleteMessage(const QString &channelId, const QString &messageId, ObjectHandler onOk,
                               ErrorHandler onError)
{
    const QString path = QStringLiteral("/channels/%1/messages/%2").arg(channelId, messageId);
    QNetworkReply *reply = m_network.deleteResource(buildRequest(path));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::addReaction(const QString &channelId, const QString &messageId, const QString &emoji,
                             ObjectHandler onOk, ErrorHandler onError)
{
    const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(emoji));
    const QString path = QStringLiteral("/channels/%1/messages/%2/reactions/%3/@me")
                             .arg(channelId, messageId, encoded);
    QNetworkReply *reply = m_network.put(buildRequest(path), QByteArray());
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::removeReaction(const QString &channelId, const QString &messageId, const QString &emoji,
                                ObjectHandler onOk, ErrorHandler onError)
{
    const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(emoji));
    const QString path = QStringLiteral("/channels/%1/messages/%2/reactions/%3/@me")
                             .arg(channelId, messageId, encoded);
    QNetworkReply *reply = m_network.deleteResource(buildRequest(path));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::ackMessage(const QString &channelId, const QString &messageId)
{
    if (channelId.isEmpty() || messageId.isEmpty())
        return;

    const QJsonObject body{{QStringLiteral("token"), QJsonValue::Null}};
    const QString path = QStringLiteral("/channels/%1/messages/%2/ack").arg(channelId, messageId);
    QNetworkReply *reply = m_network.post(buildRequest(path),
                                          QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
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

// The GIF picker, the way the official client fills it (no provider: the
// server picks, and it is Klipy now). Asked for as "tinywebp", small moving
// WebP pictures - the format Discord's own client asks for on Linux.
// Every GIF answer is remembered for ten minutes, and a "too fast" (429) is
// waited out and asked again, up to three times, rather than shown as an
// error. The picker asked afresh on every tab click and after every pause in
// typing, and Discord's GIF endpoints are rate limited tightly: the tab
// ended up on "The resource is being rate limited." The official client
// keeps its trending page in a store for the same reason.
void RestClient::gifGet(const QString &path, ObjectHandler onObject, ArrayHandler onArray, ErrorHandler onError,
                        int attempt)
{
    constexpr qint64 KeepMs = 10 * 60 * 1000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto cached = m_gifCache.constFind(path);
    if (cached != m_gifCache.constEnd() && now - cached->first < KeepMs) {
            const QJsonDocument doc = cached->second;
        // Answered on the next pass of the event loop, like a real reply, so
        // callers never see their handler run before the call returns.
        QTimer::singleShot(0, this, [doc, onObject, onArray]() {
            if (doc.isArray() && onArray)
                onArray(doc.array());
            else if (doc.isObject() && onObject)
                onObject(doc.object());
        });
        return;
    }

    // The answer is kept on the way through to the caller.
    ObjectHandler keepObject = nullptr;
    if (onObject) {
        keepObject = [this, path, onObject](const QJsonObject &object) {
            m_gifCache.insert(path, {QDateTime::currentMSecsSinceEpoch(), QJsonDocument(object)});
            onObject(object);
        };
    }
    ArrayHandler keepArray = nullptr;
    if (onArray) {
        keepArray = [this, path, onArray](const QJsonArray &array) {
            m_gifCache.insert(path, {QDateTime::currentMSecsSinceEpoch(), QJsonDocument(array)});
            onArray(array);
        };
    }
    // "Too fast" is waited out: Discord says for how long.
    ErrorHandler waitOrFail = [this, path, onObject, onArray, onError, attempt](const Error &error) {
        if (error.httpStatus == 429 && attempt < 3) {
            const double wait = error.body.value(QStringLiteral("retry_after")).toDouble(1.0);
            const int ms = qBound(300, int(wait * 1000) + 250, 30000);
            wlog(QStringLiteral("gifs"),
                 QStringLiteral("Discord asked us to wait %1 ms before asking for GIFs again").arg(ms));
            QTimer::singleShot(ms, this, [this, path, onObject, onArray, onError, attempt]() {
                gifGet(path, onObject, onArray, onError, attempt + 1);
            });
            return;
        }
        if (onError)
            onError(error);
    };

    QNetworkReply *reply = m_network.get(buildRequest(path));
    dispatch(reply, std::move(keepObject), std::move(keepArray), std::move(waitOrFail));
}

void RestClient::gifCategories(ObjectHandler onOk, ErrorHandler onError)
{
    gifGet(QStringLiteral("/gifs/trending?locale=en-US&media_format=tinywebp"), std::move(onOk), nullptr,
           std::move(onError));
}

void RestClient::trendingGifs(ArrayHandler onOk, ErrorHandler onError)
{
    gifGet(QStringLiteral("/gifs/trending-gifs?locale=en-US&media_format=tinywebp&limit=50"), nullptr,
           std::move(onOk), std::move(onError));
}

void RestClient::searchGifs(const QString &query, ArrayHandler onOk, ErrorHandler onError)
{
    const QString path = QStringLiteral("/gifs/search?q=%1&locale=en-US&media_format=tinywebp&limit=50")
                             .arg(QString::fromUtf8(QUrl::toPercentEncoding(query.trimmed().toLower())));
    gifGet(path, nullptr, std::move(onOk), std::move(onError));
}

void RestClient::selectGif(const QString &gifId, const QString &query)
{
    const QJsonObject body{{QStringLiteral("id"), gifId}, {QStringLiteral("q"), query}};
    QNetworkReply *reply = m_network.post(buildRequest(QStringLiteral("/gifs/select")),
                                          QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, nullptr, nullptr, nullptr);
}

void RestClient::fetchStickerPacks(ObjectHandler onOk, ErrorHandler onError)
{
    QNetworkReply *reply = m_network.get(buildRequest(QStringLiteral("/sticker-packs")));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::addFriend(const QString &userId, ObjectHandler onOk, ErrorHandler onError,
                           const CaptchaProof &captcha)
{
    // An empty object means "ordinary friend request".
    const QString path = QStringLiteral("/users/@me/relationships/%1").arg(userId);
    QNetworkRequest request = buildRequest(path);
    if (!captcha.key.isEmpty()) {
        request.setRawHeader("X-Captcha-Key", captcha.key.toUtf8());
        if (!captcha.rqtoken.isEmpty())
            request.setRawHeader("X-Captcha-Rqtoken", captcha.rqtoken.toUtf8());
        if (!captcha.sessionId.isEmpty())
            request.setRawHeader("X-Captcha-Session-Id", captcha.sessionId.toUtf8());
    }
    QNetworkReply *reply = m_network.put(request, QByteArray("{}"));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::noteOwnRemoval(const QString &id)
{
    m_ownRemovals.insert(id, QDateTime::currentMSecsSinceEpoch());
}

bool RestClient::removedByUs(const QString &id, qint64 withinMs) const
{
    const auto it = m_ownRemovals.constFind(id);
    return it != m_ownRemovals.constEnd() && QDateTime::currentMSecsSinceEpoch() - it.value() <= withinMs;
}

void RestClient::removeRelationship(const QString &userId, ObjectHandler onOk, ErrorHandler onError)
{
    noteOwnRemoval(userId);
    const QString path = QStringLiteral("/users/@me/relationships/%1").arg(userId);
    QNetworkReply *reply = m_network.deleteResource(buildRequest(path));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::closeDirectChannel(const QString &channelId, ObjectHandler onOk, ErrorHandler onError)
{
    noteOwnRemoval(channelId);
    const QString path = QStringLiteral("/channels/%1").arg(channelId);
    QNetworkReply *reply = m_network.deleteResource(buildRequest(path));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::blockUser(const QString &userId, ObjectHandler onOk, ErrorHandler onError)
{
    noteOwnRemoval(userId);
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

void RestClient::fetchChannel(const QString &channelId, ObjectHandler onOk, ErrorHandler onError)
{
    QNetworkReply *reply = m_network.get(buildRequest(QStringLiteral("/channels/%1").arg(channelId)));
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

void RestClient::setMemberRole(const QString &guildId, const QString &userId, const QString &roleId, bool give,
                               ObjectHandler onOk, ErrorHandler onError)
{
    const QString path = QStringLiteral("/guilds/%1/members/%2/roles/%3").arg(guildId, userId, roleId);
    QNetworkReply *reply = give ? m_network.put(buildRequest(path), QByteArray())
                                : m_network.deleteResource(buildRequest(path));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::ackBulk(const QList<QPair<QString, QString>> &channelsAndMessages, ObjectHandler onOk,
                         ErrorHandler onError)
{
    QJsonArray states;
    for (const auto &pair : channelsAndMessages) {
        states.append(QJsonObject{
            {QStringLiteral("channel_id"), pair.first},
            {QStringLiteral("message_id"), pair.second},
            {QStringLiteral("read_state_type"), 0},
        });
    }
    const QJsonObject body{{QStringLiteral("read_states"), states}};
    QNetworkReply *reply = m_network.post(buildRequest(QStringLiteral("/read-states/ack-bulk")),
                                          QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

namespace {

void attachCaptcha(QNetworkRequest &request, const RestClient::CaptchaProof &captcha)
{
    if (captcha.key.isEmpty())
        return;
    request.setRawHeader("X-Captcha-Key", captcha.key.toUtf8());
    if (!captcha.rqtoken.isEmpty())
        request.setRawHeader("X-Captcha-Rqtoken", captcha.rqtoken.toUtf8());
    if (!captcha.sessionId.isEmpty())
        request.setRawHeader("X-Captcha-Session-Id", captcha.sessionId.toUtf8());
}

} // namespace

void RestClient::editCurrentUser(const QJsonObject &fields, ObjectHandler onOk, ErrorHandler onError,
                                 const QString &mfaToken, const CaptchaProof &captcha)
{
    QNetworkRequest request = buildRequest(QStringLiteral("/users/@me"));
    if (!mfaToken.isEmpty())
        request.setRawHeader("X-Discord-MFA-Authorization", mfaToken.toUtf8());
    attachCaptcha(request, captcha);
    QNetworkReply *reply = m_network.sendCustomRequest(request, QByteArrayLiteral("PATCH"),
                                                       QJsonDocument(fields).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::editCurrentProfile(const QJsonObject &fields, ObjectHandler onOk, ErrorHandler onError,
                                    const CaptchaProof &captcha)
{
    QNetworkRequest request = buildRequest(QStringLiteral("/users/@me/profile"));
    attachCaptcha(request, captcha);
    QNetworkReply *reply = m_network.sendCustomRequest(request, QByteArrayLiteral("PATCH"),
                                                       QJsonDocument(fields).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::editGuildMember(const QString &guildId, const QJsonObject &fields, ObjectHandler onOk,
                                 ErrorHandler onError, const CaptchaProof &captcha)
{
    QNetworkRequest request = buildRequest(QStringLiteral("/guilds/%1/members/@me").arg(guildId));
    attachCaptcha(request, captcha);
    QNetworkReply *reply = m_network.sendCustomRequest(request, QByteArrayLiteral("PATCH"),
                                                       QJsonDocument(fields).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::modifyGuildMember(const QString &guildId, const QString &userId, const QJsonObject &fields,
                                   ObjectHandler onOk, ErrorHandler onError)
{
    if (guildId.isEmpty() || userId.isEmpty() || fields.isEmpty())
        return;
    QNetworkRequest request = buildRequest(QStringLiteral("/guilds/%1/members/%2").arg(guildId, userId));
    QNetworkReply *reply = m_network.sendCustomRequest(request, QByteArrayLiteral("PATCH"),
                                                       QJsonDocument(fields).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::fetchCollectibles(ArrayHandler onOk, ErrorHandler onError)
{
    QNetworkReply *reply = m_network.get(buildRequest(QStringLiteral("/users/@me/collectibles-purchases")));
    dispatch(reply, nullptr, std::move(onOk), std::move(onError));
}

void RestClient::fetchDetectableApplications(ArrayHandler onOk, ErrorHandler onError)
{
    QNetworkReply *reply = m_network.get(buildRequest(QStringLiteral("/applications/detectable")));
    connect(reply, &QNetworkReply::finished, this, [reply, onOk = std::move(onOk), onError = std::move(onError)]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (status >= 200 && status < 300) {
            if (doc.isArray() && onOk)
                onOk(doc.array());
            else if (doc.isObject() && onOk) {
                const QJsonObject object = doc.object();
                for (const QString &key : {QStringLiteral("applications"), QStringLiteral("detectable")}) {
                    if (object.value(key).isArray()) {
                        onOk(object.value(key).toArray());
                        return;
                    }
                }
                Error error;
                error.httpStatus = status;
                error.body = object;
                error.message = QStringLiteral("game list was not a list");
                if (onError)
                    onError(error);
            }
            return;
        }
        Error error;
        error.httpStatus = status;
        error.body = doc.isObject() ? doc.object() : QJsonObject{};
        error.message = error.body.value(QStringLiteral("message")).toString();
        if (error.message.isEmpty())
            error.message = reply->errorString();
        if (onError)
            onError(error);
    });
}

void RestClient::finishMfa(const QString &ticket, const QString &type, const QString &data, ObjectHandler onOk,
                           ErrorHandler onError)
{
    const QJsonObject body{
        {QStringLiteral("ticket"), ticket},
        {QStringLiteral("mfa_type"), type},
        {QStringLiteral("data"), data},
    };
    QNetworkReply *reply = m_network.post(buildRequest(QStringLiteral("/mfa/finish")),
                                          QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::sendFriendRequest(const QString &username, int discriminator, ObjectHandler onOk,
                                   ErrorHandler onError, const CaptchaProof &captcha)
{
    const QJsonObject body{
        {QStringLiteral("username"), username},
        {QStringLiteral("discriminator"), discriminator > 0 ? QJsonValue(discriminator) : QJsonValue()},
    };

    QNetworkRequest request = buildRequest(QStringLiteral("/users/@me/relationships"));
    request.setRawHeader("X-Context-Properties", QByteArrayLiteral(R"({"location":"Add Friend"})").toBase64());
    if (!captcha.key.isEmpty()) {
        request.setRawHeader("X-Captcha-Key", captcha.key.toUtf8());
        if (!captcha.rqtoken.isEmpty())
            request.setRawHeader("X-Captcha-Rqtoken", captcha.rqtoken.toUtf8());
        if (!captcha.sessionId.isEmpty())
            request.setRawHeader("X-Captcha-Session-Id", captcha.sessionId.toUtf8());
    }
    QNetworkReply *reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::fetchInvite(const QString &code, ObjectHandler onOk, ErrorHandler onError)
{
    const QString path = QStringLiteral("/invites/%1?with_counts=true&with_expiration=true")
                             .arg(QString::fromUtf8(QUrl::toPercentEncoding(code)));
    QNetworkReply *reply = m_network.get(buildRequest(path));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::acceptInvite(const QString &code, const QString &sessionId, const QByteArray &context,
                              ObjectHandler onOk, ErrorHandler onError, const CaptchaProof &captcha)
{
    QJsonObject body;
    if (!sessionId.isEmpty())
        body.insert(QStringLiteral("session_id"), sessionId);

    const QString path = QStringLiteral("/invites/%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(code)));
    QNetworkRequest request = buildRequest(path);
    if (!context.isEmpty())
        request.setRawHeader("X-Context-Properties", context.toBase64());
    if (!captcha.key.isEmpty()) {
        request.setRawHeader("X-Captcha-Key", captcha.key.toUtf8());
        if (!captcha.rqtoken.isEmpty())
            request.setRawHeader("X-Captcha-Rqtoken", captcha.rqtoken.toUtf8());
        if (!captcha.sessionId.isEmpty())
            request.setRawHeader("X-Captcha-Session-Id", captcha.sessionId.toUtf8());
    }
    QNetworkReply *reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::lookupGift(const QString &code, ObjectHandler onOk, ErrorHandler onError)
{
    // The same query the official client sends when it renders a gift card.
    // with_subscription_plan is what names the gift ("Nitro, 1 month") rather
    // than leaving it as a bare SKU id.
    const QString path = QStringLiteral("/entitlements/gift-codes/%1"
                                        "?with_application=false&with_subscription_plan=true")
                             .arg(code);
    QNetworkReply *reply = m_network.get(buildRequest(path));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::redeemGift(const QString &code, const QString &channelId, ObjectHandler onOk,
                            ErrorHandler onError, const CaptchaProof &captcha)
{
    // The same body the official client sends. A free gift has no payment
    // source. Leaving channel_id out is a 400, and the code is never claimed.
    QJsonObject body{
        {QStringLiteral("channel_id"),
         channelId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(channelId)},
        {QStringLiteral("payment_source_id"), QJsonValue(QJsonValue::Null)},
    };
    QNetworkRequest request = buildRequest(QStringLiteral("/entitlements/gift-codes/%1/redeem").arg(code));
    if (!captcha.key.isEmpty()) {
        request.setRawHeader("X-Captcha-Key", captcha.key.toUtf8());
        if (!captcha.rqtoken.isEmpty())
            request.setRawHeader("X-Captcha-Rqtoken", captcha.rqtoken.toUtf8());
        if (!captcha.sessionId.isEmpty())
            request.setRawHeader("X-Captcha-Session-Id", captcha.sessionId.toUtf8());
    }
    QNetworkReply *reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::fetchNote(const QString &userId, ObjectHandler onOk, ErrorHandler onError)
{
    QNetworkReply *reply = m_network.get(buildRequest(QStringLiteral("/users/@me/notes/%1").arg(userId)));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::updateStatus(const QString &status, ObjectHandler onOk, ErrorHandler onError)
{
    const QJsonObject body{{QStringLiteral("status"), status}};
    QNetworkReply *reply = m_network.sendCustomRequest(
        buildRequest(QStringLiteral("/users/@me/settings")), QByteArrayLiteral("PATCH"),
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::updateGuildFolders(const QJsonArray &folders, ObjectHandler onOk, ErrorHandler onError)
{
    // The same endpoint the status goes through. Discord keeps it in step with
    // the settings blob, so this change arrives on every other device as a
    // settings update - which is how the official client picks it up.
    const QJsonObject body{{QStringLiteral("guild_folders"), folders}};
    QNetworkReply *reply = m_network.sendCustomRequest(
        buildRequest(QStringLiteral("/users/@me/settings")), QByteArrayLiteral("PATCH"),
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::listApplications(ArrayHandler onOk, ErrorHandler onError, const CaptchaProof &captcha)
{
    QNetworkRequest request = buildRequest(QStringLiteral("/applications"));
    if (!captcha.key.isEmpty()) {
        request.setRawHeader("X-Captcha-Key", captcha.key.toUtf8());
        if (!captcha.rqtoken.isEmpty())
            request.setRawHeader("X-Captcha-Rqtoken", captcha.rqtoken.toUtf8());
        if (!captcha.sessionId.isEmpty())
            request.setRawHeader("X-Captcha-Session-Id", captcha.sessionId.toUtf8());
    }
    QNetworkReply *reply = m_network.get(request);
    dispatch(reply, nullptr, std::move(onOk), std::move(onError));
}

void RestClient::createApplication(const QString &name, ObjectHandler onOk, ErrorHandler onError,
                                   const CaptchaProof &captcha)
{
    const QJsonObject body{
        {QStringLiteral("name"), name},
        {QStringLiteral("description"), QStringLiteral("Singularity, a Discord client")},
    };
    QNetworkRequest request = buildRequest(QStringLiteral("/applications"));
    if (!captcha.key.isEmpty()) {
        request.setRawHeader("X-Captcha-Key", captcha.key.toUtf8());
        if (!captcha.rqtoken.isEmpty())
            request.setRawHeader("X-Captcha-Rqtoken", captcha.rqtoken.toUtf8());
        if (!captcha.sessionId.isEmpty())
            request.setRawHeader("X-Captcha-Session-Id", captcha.sessionId.toUtf8());
    }
    QNetworkReply *reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::createInteraction(const QJsonObject &body, ObjectHandler onOk, ErrorHandler onError)
{
    QJsonObject sent = body;
    if (!sent.contains(QStringLiteral("nonce"))) {
        const qint64 nonce = QDateTime::currentMSecsSinceEpoch() * 1000
            + QRandomGenerator::global()->bounded(1000);
        sent.insert(QStringLiteral("nonce"), QString::number(nonce));
    }
    const QByteArray json = QJsonDocument(sent).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_network.post(buildRequest(QStringLiteral("/interactions")), json);
    dispatch(reply, std::move(onOk), nullptr, std::move(onError));
}

void RestClient::proxyApplicationAsset(const QString &applicationId, const QString &url, ArrayHandler onOk,
                                       ErrorHandler onError)
{
    QJsonArray urls;
    urls.append(url);
    const QJsonObject body{{QStringLiteral("urls"), urls}};
    const QString path = QStringLiteral("/applications/%1/external-assets").arg(applicationId);
    QNetworkReply *reply = m_network.post(buildRequest(path),
                                          QJsonDocument(body).toJson(QJsonDocument::Compact));
    dispatch(reply, nullptr, std::move(onOk), std::move(onError));
}

void RestClient::updateUserVolume(const QString &userId, int volume, bool muted, ObjectHandler onOk,
                                  ErrorHandler onError)
{
    const QJsonObject body{
        {QStringLiteral("volume"), volume},
        {QStringLiteral("muted"), muted},
    };
    const QString path = QStringLiteral("/users/@me/audio-settings/user/%1").arg(userId);
    QNetworkReply *reply = m_network.sendCustomRequest(
        buildRequest(path), QByteArrayLiteral("PATCH"),
        QJsonDocument(body).toJson(QJsonDocument::Compact));
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
