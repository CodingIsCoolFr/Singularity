#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
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

    // Leave Server. Discord then sends GUILD_DELETE to every session.
    void leaveGuild(const QString &guildId, ObjectHandler onOk, ErrorHandler onError);

    // The still picture shown on our Go Live tile until someone watches it.
    // POST /streams/{key}/preview, as the official client does at the start
    // and then every five minutes.
    void uploadStreamPreview(const QString &streamKey, const QByteArray &jpeg, ObjectHandler onOk,
                             ErrorHandler onError);

    // Forwards a message into another channel, as Discord's Forward does.
    void forwardMessage(const QString &toChannelId, const QString &fromChannelId, const QString &fromGuildId,
                        const QString &messageId, ObjectHandler onOk, ErrorHandler onError);

    // Discord's GIF picker.
    //
    // gifCategories is the first page: {"categories": [{name, src}], "gifs":
    // [one trending gif, for the Trending tile]}. The other two answer with a
    // plain list of gifs: {id, url, src, gif_src, preview, width, height}.
    // selectGif is the note the official client sends when one is picked.
    void gifCategories(ObjectHandler onOk, ErrorHandler onError);
    void trendingGifs(ArrayHandler onOk, ErrorHandler onError);
    void searchGifs(const QString &query, ArrayHandler onOk, ErrorHandler onError);
    void selectGif(const QString &gifId, const QString &query);

    // The standard sticker packs, plus anything a server has is already on the guild.
    void fetchStickerPacks(ObjectHandler onOk, ErrorHandler onError);

    // PATCH and DELETE one message you sent.
    void editMessage(const QString &channelId, const QString &messageId, const QString &content,
                     ObjectHandler onOk, ErrorHandler onError);
    void deleteMessage(const QString &channelId, const QString &messageId, ObjectHandler onOk,
                       ErrorHandler onError);

    // PUT .../reactions/{emoji}/@me. `emoji` is a character, or "name:id" for
    // a custom one. DELETE the same path takes yours off.
    void addReaction(const QString &channelId, const QString &messageId, const QString &emoji,
                     ObjectHandler onOk, ErrorHandler onError);
    void removeReaction(const QString &channelId, const QString &messageId, const QString &emoji,
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

    // One channel by id, for when a link points at a channel we have not loaded
    // yet. The reply carries its guild and type, so we can jump to it.
    void fetchChannel(const QString &channelId, ObjectHandler onOk, ErrorHandler onError);

    // GET /users/{id}/profile - the whole profile: banner, bio, badges,
    // connections, mutual servers and friends, and the member record for one
    // server when `guildId` is given.
    void fetchUserProfile(const QString &userId, const QString &guildId, ObjectHandler onOk,
                          ErrorHandler onError);

    // POST /channels/{id}/invites - makes a share link for a channel.
    void createInvite(const QString &channelId, ObjectHandler onOk, ErrorHandler onError);

    // PUT / DELETE /guilds/{guild}/members/{user}/roles/{role} - hand a role
    // to someone or take it away (discord.py-self add_role / remove_role).
    // Needs Manage Roles, and only below your own highest role. Discord then
    // sends GUILD_MEMBER_UPDATE to every client, so it shows everywhere.
    void setMemberRole(const QString &guildId, const QString &userId, const QString &roleId, bool give,
                       ObjectHandler onOk, ErrorHandler onError);

    // POST /read-states/ack-bulk {"read_states":[{channel_id, message_id,
    // read_state_type: 0}]} - marks many chats read at once, as Discord's
    // "Mark as read" on a folder and Vencord's Read All do. Up to 100 a call.
    void ackBulk(const QList<QPair<QString, QString>> &channelsAndMessages, ObjectHandler onOk,
                 ErrorHandler onError);

    // Editing your own account and profile (checked against docs.discord.food
    // and discord.py-self). Pictures go as data URIs; null clears a field.
    //
    // PATCH /users/@me: avatar, global_name, username (+ password),
    // avatar_decoration_id / avatar_decoration_sku_id. The answer is the user,
    // sometimes with a fresh "token" that replaces the old one.
    // All three can come back with a captcha demand ("You need to update your
    // app to perform this action."); the proof goes in `captcha` on the retry.
    void editCurrentUser(const QJsonObject &fields, ObjectHandler onOk, ErrorHandler onError,
                         const QString &mfaToken = {}, const CaptchaProof &captcha = {});
    // PATCH /users/@me/profile: bio (190), pronouns (40), accent_color,
    // banner (Nitro).
    void editCurrentProfile(const QJsonObject &fields, ObjectHandler onOk, ErrorHandler onError,
                            const CaptchaProof &captcha = {});
    // PATCH /guilds/{id}/members/@me: nick, and with Nitro a server avatar,
    // banner and bio.
    void editGuildMember(const QString &guildId, const QJsonObject &fields, ObjectHandler onOk,
                         ErrorHandler onError, const CaptchaProof &captcha = {});
    // PATCH /guilds/{guild}/members/{user}: server mute, server deafen, or
    // move them. `channel_id` null disconnects them from voice. Needs Mute
    // Members, Deafen Members, or Move Members.
    void modifyGuildMember(const QString &guildId, const QString &userId, const QJsonObject &fields,
                           ObjectHandler onOk, ErrorHandler onError);
    // GET /users/@me/collectibles-purchases - what you own from the shop.
    // type 0 = avatar decoration, 1 = profile effect, 2 = nameplate.
    void fetchCollectibles(ArrayHandler onOk, ErrorHandler onError);

    // GET /applications/detectable — the programs Discord treats as games.
    void fetchDetectableApplications(ArrayHandler onOk, ErrorHandler onError);
    // POST /mfa/finish {ticket, mfa_type, data} -> {token}. The answer to a
    // 401 with code 60003 ("Two factor is required for this operation"): the
    // token goes back in X-Discord-MFA-Authorization on the same request.
    void finishMfa(const QString &ticket, const QString &type, const QString &data, ObjectHandler onOk,
                   ErrorHandler onError);

    // POST /users/@me/relationships {"username", "discriminator"} - a friend
    // request by name, from the Friends page's Add Friend tab. Sent with
    // X-Context-Properties {"location":"Add Friend"}, as the real client does
    // (discord.py-self send_friend_request). discriminator is null for the new
    // unique usernames and the four digits for an old Name#1234. 204 on
    // success; 80004 = nobody by that name, 80007 = already friends,
    // 80000 = they do not take requests.
    void sendFriendRequest(const QString &username, int discriminator, ObjectHandler onOk, ErrorHandler onError,
                           const CaptchaProof &captcha = {});

    // GET /invites/{code}?with_counts=true&with_expiration=true - what an
    // invite leads to (server, channel, how many online and in total)
    // without joining. 10006 = the invite is unknown or has expired.
    void fetchInvite(const QString &code, ObjectHandler onOk, ErrorHandler onError);

    // POST /invites/{code} {"session_id"} - joins the server. `context` is the
    // X-Context-Properties JSON (sent base64, as the real client does) naming
    // where the join was pressed. A captcha demand comes back as a 400 with
    // captcha_key; the person solves it and it is sent again with the proof.
    void acceptInvite(const QString &code, const QString &sessionId, const QByteArray &context,
                      ObjectHandler onOk, ErrorHandler onError, const CaptchaProof &captcha = {});

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

    // POST /interactions. A button press or a filled-in popup. 204 means
    // Discord accepted it; the bot's answer arrives on the gateway.
    void createInteraction(const QJsonObject &body, ObjectHandler onOk, ErrorHandler onError);

    // Private notes you keep about someone. Only you can read them.
    void fetchNote(const QString &userId, ObjectHandler onOk, ErrorHandler onError);
    void saveNote(const QString &userId, const QString &note, ObjectHandler onOk, ErrorHandler onError);

    // GET /users/{id}/profile is not available to every account, so avatars are
    // fetched straight from the CDN instead.
    static QString avatarUrl(const QString &userId, const QString &avatarHash, int size = 64);

    // True if this program itself removed that friend, request or block, or
    // closed that chat, in the last `withinMs`. The echo Discord sends back
    // for our own action looks exactly like somebody else's, and a notifier
    // must not tell you about something you just did.
    bool removedByUs(const QString &id, qint64 withinMs = 60000) const;

signals:
    void unauthorized();

private:
    QNetworkRequest buildRequest(const QString &path) const;
    void dispatch(QNetworkReply *reply, ObjectHandler onObject, ArrayHandler onArray, ErrorHandler onError);

    QNetworkAccessManager m_network;

    // GIF answers by request path, with when they arrived. See gifGet.
    QHash<QString, QPair<qint64, QJsonDocument>> m_gifCache;
    void gifGet(const QString &path, ObjectHandler onObject, ArrayHandler onArray, ErrorHandler onError,
                int attempt = 0);
    QString m_token;

    void noteOwnRemoval(const QString &id);
    QHash<QString, qint64> m_ownRemovals;   // id -> when, ms since epoch
};
