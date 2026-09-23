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

    // What Discord hands back when it wants a person to finish a check, and
    // what we hand back after they have.
    struct CaptchaProof
    {
        QString key;
        QString rqtoken;
        QString sessionId;
    };

    // POST /channels/{id}/messages
    //
    // `replyTo` quotes that message. `files` are uploaded with it. Either may
    // be empty. A message needs text or a file; Discord refuses one with neither.
    // `captcha` is set only on the retry after the person finished the check.
    void sendMessage(const QString &channelId, const QString &content, const QString &replyTo,
                     const QStringList &files, ObjectHandler onOk, ErrorHandler onError,
                     const QString &stickerId = {}, const CaptchaProof &captcha = {});

    // Discord's GIF picker. An empty query is the trending row.
    void searchGifs(const QString &query, ObjectHandler onOk, ErrorHandler onError);

    // The standard sticker packs, plus anything a server has is already on the guild.
    void fetchStickerPacks(ObjectHandler onOk, ErrorHandler onError);

    // PATCH and DELETE one message you sent.
    void editMessage(const QString &channelId, const QString &messageId, const QString &content,
                     ObjectHandler onOk, ErrorHandler onError);
    void deleteMessage(const QString &channelId, const QString &messageId, ObjectHandler onOk,
                       ErrorHandler onError);

    // PUT .../reactions/{emoji}/@me. `emoji` is a character, or "name:id" for
    // a custom one.
    void addReaction(const QString &channelId, const QString &messageId, const QString &emoji,
                     ObjectHandler onOk, ErrorHandler onError);

    // Tells Discord you have seen up to this message, which is what clears the
    // unread mark on every other client too.
    void ackMessage(const QString &channelId, const QString &messageId);

    // POST /channels/{id}/typing - fire and forget.
    void sendTyping(const QString &channelId);

    // POST /users/@me/channels - opens (or reuses) the direct chat with
    // someone and returns the channel.
    void openDirectMessage(const QString &userId, ObjectHandler onOk, ErrorHandler onError);

    // PUT /users/@me/relationships/{id} - sends a friend request.
    void addFriend(const QString &userId, ObjectHandler onOk, ErrorHandler onError,
                   const CaptchaProof &captcha = {});

    // DELETE /users/@me/relationships/{id} - removes a friend, cancels a
    // request, or unblocks.
    void removeRelationship(const QString &userId, ObjectHandler onOk, ErrorHandler onError);

    // DELETE /channels/{id} - closes a direct message for this account.
    // The history stays, and the chat comes back if someone messages again.
    // Discord sends CHANNEL_DELETE to every session, so the official client
    // drops the same row.
    void closeDirectChannel(const QString &channelId, ObjectHandler onOk, ErrorHandler onError);

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

    // GET /entitlements/gift-codes/{code} - says what a gift is, WITHOUT
    // claiming it.
    //
    // This is the call Discord's own client makes to draw the gift card, so
    // making it is ordinary client traffic rather than a sign of automation.
    // The answer carries `uses`, `max_uses`, `expires_at`, `redeemed` and the
    // subscription plan, which is everything needed to tell a live gift from
    // one that is spent, expired, already yours, or not Nitro at all.
    //
    // A code that does not exist comes back 404, which is how a made-up link
    // is told apart from a real one without pressing anything.
    void lookupGift(const QString &code, ObjectHandler onOk, ErrorHandler onError);

    // POST /entitlements/gift-codes/{code}/redeem - claims a gift.
    //
    // Only ever called because somebody pressed a button. Nothing in Singularity
    // calls this on its own, and nothing should: a client that redeems by
    // itself is the single clearest sign of an automated account.
    // `channelId` is the channel the gift was posted in. Discord refuses a
    // redeem that does not name it. `captcha` is set only on the retry after
    // the person finished the check.
    void redeemGift(const QString &code, const QString &channelId, ObjectHandler onOk,
                    ErrorHandler onError, const CaptchaProof &captcha = {});

    // PATCH /users/@me/settings — the status the real client stores, which is
    // what other people and your other sessions are shown.
    void updateStatus(const QString &status, ObjectHandler onOk, ErrorHandler onError);

    // The whole server rail, in order: every loose server as a folder of one
    // with no id, every real folder with its id, name and colour.
    void updateGuildFolders(const QJsonArray &folders, ObjectHandler onOk, ErrorHandler onError);

    // PATCH /users/@me/audio-settings/user/{id} — how loud this one person is.
    // Discord keeps it, and every session of the account hears the same number.
    // Volume is 0 to 200. Mute is separate from the slider.
    void updateUserVolume(const QString &userId, int volume, bool muted, ObjectHandler onOk,
                          ErrorHandler onError);

    // The Playing card only shows on the official client when it belongs to
    // an application this account owns.
    void listApplications(ArrayHandler onOk, ErrorHandler onError, const CaptchaProof &captcha = {});
    void createApplication(const QString &name, ObjectHandler onOk, ErrorHandler onError,
                           const CaptchaProof &captcha = {});
    void proxyApplicationAsset(const QString &applicationId, const QString &url, ArrayHandler onOk,
                               ErrorHandler onError);

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
