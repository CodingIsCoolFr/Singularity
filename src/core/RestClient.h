#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

#include <functional>

// Thin wrapper over the Discord REST API.
//
// Every reply comes back through a callback so callers never touch
// QNetworkReply directly. A failed call reports the HTTP status and the raw
// body, because Discord puts the useful part (rate limit info, error codes) in
// the body.
class RestClient : public QObject
{
    Q_OBJECT

public:
    struct Error
    {
        int httpStatus = 0;
        QString message;
        QJsonObject body;
        bool isRateLimit() const { return httpStatus == 429; }
    };

    using ObjectHandler = std::function<void(const QJsonObject &)>;
    using ArrayHandler = std::function<void(const QJsonArray &)>;
    using ErrorHandler = std::function<void(const Error &)>;

    explicit RestClient(QObject *parent = nullptr);

    void setToken(const QString &token);
    QString token() const { return m_token; }

    // GET /users/@me - used to check that the token still works.
    void fetchCurrentUser(ObjectHandler onOk, ErrorHandler onError);

    // GET /channels/{id}/messages?limit=n
    //
    // `before` asks for the messages older than that one, which is how
    // scrolling back through a channel works. Leave it empty for the newest.
    void fetchMessages(const QString &channelId, int limit, ArrayHandler onOk, ErrorHandler onError,
                       const QString &before = QString());

    // POST /channels/{id}/messages
    void sendMessage(const QString &channelId, const QString &content, ObjectHandler onOk, ErrorHandler onError);

    // POST /channels/{id}/typing - fire and forget.
    void sendTyping(const QString &channelId);

    // POST /users/@me/channels - opens (or reuses) the direct chat with
    // someone and returns the channel.
    void openDirectMessage(const QString &userId, ObjectHandler onOk, ErrorHandler onError);

    // PUT /users/@me/relationships/{id} - sends a friend request.
    void addFriend(const QString &userId, ObjectHandler onOk, ErrorHandler onError);

    // DELETE /users/@me/relationships/{id} - removes a friend, cancels a
    // request, or unblocks.
    void removeRelationship(const QString &userId, ObjectHandler onOk, ErrorHandler onError);

    // PUT /users/@me/relationships/{id} with type 2 - blocks someone.
    void blockUser(const QString &userId, ObjectHandler onOk, ErrorHandler onError);

    // GET /users/{id} - just enough to put a name to an id.
    void fetchUser(const QString &userId, ObjectHandler onOk, ErrorHandler onError);

    // GET /users/{id}/profile - the whole profile: banner, bio, badges,
    // connections, mutual servers and friends, and the member record for one
    // server when `guildId` is given.
    void fetchUserProfile(const QString &userId, const QString &guildId, ObjectHandler onOk,
                          ErrorHandler onError);

    // POST /channels/{id}/invites - makes a share link for a channel.
    void createInvite(const QString &channelId, ObjectHandler onOk, ErrorHandler onError);

    // POST /entitlements/gift-codes/{code}/redeem - claims a gift.
    //
    // Only ever called because somebody pressed a button. Nothing in Wisp
    // calls this on its own, and nothing should: a client that redeems by
    // itself is the single clearest sign of an automated account.
    void redeemGift(const QString &code, ObjectHandler onOk, ErrorHandler onError);

    // Private notes you keep about someone. Only you can read them.
    void fetchNote(const QString &userId, ObjectHandler onOk, ErrorHandler onError);
    void saveNote(const QString &userId, const QString &note, ObjectHandler onOk, ErrorHandler onError);

    // GET /users/{id}/profile is not available to every account, so avatars are
    // fetched straight from the CDN instead.
    static QString avatarUrl(const QString &userId, const QString &avatarHash, int size = 64);

signals:
    void unauthorized();

private:
    QNetworkRequest buildRequest(const QString &path) const;
    void dispatch(QNetworkReply *reply, ObjectHandler onObject, ArrayHandler onArray, ErrorHandler onError);

    QNetworkAccessManager m_network;
    QString m_token;
};
