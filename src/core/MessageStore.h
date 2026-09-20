#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

// A role inside one server. Colour is a plain RGB number, so this header stays
// free of any drawing types.
struct RoleInfo
{
    QString id;
    QString name;
    int colour = 0;
    int position = 0;

    bool hasColour() const { return colour != 0; }
};

// One line of "what this person is doing right now".
struct ActivityInfo
{
    // 0 playing, 1 streaming, 2 listening, 3 watching, 4 custom status,
    // 5 competing.
    int type = 0;
    QString name;
    QString details;
    QString state;
    QString applicationId;
    QString largeImage;   // raw asset key, may carry an "mp:" prefix
    QString largeText;
    QString emoji;
    qint64 startMs = 0;
    qint64 endMs = 0;

    bool isCustomStatus() const { return type == 4; }
};

struct PresenceInfo
{
    QString status;   // online, idle, dnd, offline
    QList<ActivityInfo> activities;

    bool isOnline() const { return !status.isEmpty() && status != QLatin1String("offline"); }
};

// Where someone is sitting in voice, if anywhere.
struct VoiceStateInfo
{
    QString channelId;
    QString guildId;
};

// What we know about one person. Filled in from READY and from every message
// that goes past, so it improves the longer the client runs.
struct UserInfo
{
    QString id;
    QString username;     // the @handle
    QString globalName;   // the chosen display name, may be empty
    QString avatarHash;
    bool bot = false;

    // Discord relationship kinds: 1 friend, 2 blocked, 3 they asked you,
    // 4 you asked them. 0 means no link.
    int relationship = 0;
    QDateTime friendsSince;

    QString displayName() const { return globalName.isEmpty() ? username : globalName; }
    bool isFriend() const { return relationship == 1; }
    bool isBlocked() const { return relationship == 2; }
    bool isPending() const { return relationship == 3 || relationship == 4; }

    // Discord ids carry the moment the account was made.
    QDateTime createdAt() const;
};

struct ChannelInfo
{
    QString id;
    QString name;
    QString guildId;   // empty for direct messages
    QString parentId;  // the category this channel sits under, if any
    QString topic;
    QStringList recipientIds;   // direct messages only
    QString rtcRegion;          // voice channels only
    int type = 0;
    int position = 0;
    int bitrate = 0;            // voice channels only
    int userLimit = 0;          // voice channels only, 0 means no limit

    bool isTextLike() const { return type == 0 || type == 1 || type == 3 || type == 5; }
    bool isDirect() const { return type == 1 || type == 3; }
    bool isCategory() const { return type == 4; }
    bool isVoice() const { return type == 2 || type == 13; }
};

// One category heading and the channels under it. A group with an empty name
// holds the channels that sit above every category.
struct ChannelGroup
{
    QString name;
    QList<ChannelInfo> channels;
};

struct GuildInfo
{
    QString id;
    QString name;
    QString iconHash;
    QList<QString> channelIds;
    QHash<QString, RoleInfo> roles;
};

struct Attachment
{
    QString url;
    QString filename;
    QString contentType;
    qint64 size = 0;
    int width = 0;
    int height = 0;

    bool isImage() const { return width > 0 && height > 0; }
};

// A link preview. Discord builds these itself for links people post, which is
// how a Tenor address turns into a playing picture.
struct EmbedInfo
{
    QString type;          // rich, image, gifv, video, link, article
    QString url;           // where the card points
    QString title;
    QString description;
    QString providerName;
    QString authorName;

    // Always the proxied address, never the original host, so the client only
    // ever fetches pictures through Discord.
    QString imageUrl;
    int imageWidth = 0;
    int imageHeight = 0;
    int colour = 0;

    // A card that is only a picture needs no title or border.
    bool isPictureOnly() const
    {
        return (type == QLatin1String("image") || type == QLatin1String("gifv")) && !imageUrl.isEmpty();
    }
};

struct StickerInfo
{
    QString id;
    QString name;

    // 1 png, 2 animated png, 3 lottie (a JSON animation), 4 gif.
    int formatType = 1;

    // Lottie stickers are vector animations that Qt cannot draw.
    bool isDrawable() const { return formatType != 3; }
};

struct MessageInfo
{
    QString id;
    QString channelId;
    QString authorId;
    QString authorName;
    QString authorAvatar;
    QString content;
    QDateTime timestamp;
    QList<Attachment> attachments;
    QList<EmbedInfo> embeds;
    QList<StickerInfo> stickers;
    bool edited = false;
    bool deleted = false;   // set by the MessageLogger plugin
    bool pendingLocal = false;
};

// In-memory mirror of everything the UI needs to draw. The store is
// deliberately dumb: it holds state and reports changes, and decides nothing.
class MessageStore : public QObject
{
    Q_OBJECT

public:
    explicit MessageStore(QObject *parent = nullptr);

    void clear();
    void ingestReady(const QJsonObject &readyPayload);

    // User accounts get a second payload right after READY. That is where
    // voice states and most presences actually live, so without this you only
    // ever see yourself in a voice channel.
    void ingestReadySupplemental(const QJsonObject &payload);

    // GUILD_MEMBER_LIST_UPDATE: the answer to asking for a channel's member
    // list. This is the only place server members' statuses arrive, so
    // without it everyone in a server looks offline.
    void ingestMemberListUpdate(const QJsonObject &payload);

    QList<GuildInfo> guilds() const;
    GuildInfo guild(const QString &guildId) const;
    ChannelInfo channel(const QString &channelId) const;
    QList<ChannelInfo> channelsOfGuild(const QString &guildId) const;
    QList<ChannelGroup> groupedChannels(const QString &guildId) const;
    QList<ChannelInfo> directChannels() const;

    QList<MessageInfo> messages(const QString &channelId) const;
    bool hasHistory(const QString &channelId) const;

    // Used for CHANNEL_CREATE, so a chat opened from a profile card shows up.
    void ingestChannelObject(const QJsonObject &rawChannel);

    void setHistory(const QString &channelId, const QJsonArray &rawMessages);
    void appendMessage(const QJsonObject &rawMessage);
    void updateMessage(const QJsonObject &rawMessage);
    void markDeleted(const QString &channelId, const QString &messageId);
    void removeMessage(const QString &channelId, const QString &messageId);

    static MessageInfo parseMessage(const QJsonObject &raw);

    // Display name for a user id, learned from READY and from messages.
    QString userName(const QString &userId) const;
    UserInfo user(const QString &userId) const;
    void rememberUser(const QJsonObject &rawUser);
    void setRelationship(const QString &userId, int type, const QDateTime &since = {});

    // Everyone with a given link to you: 1 friend, 2 blocked, 3 they asked,
    // 4 you asked. Sorted so the list never jumps around.
    QList<UserInfo> usersWithRelationship(int type) const;

    // The direct message channel with one person, if the client knows of one.
    QString directChannelWith(const QString &userId) const;

    // Roles, newest first by rank, for the ids a member carries.
    QList<RoleInfo> resolveRoles(const QString &guildId, const QStringList &roleIds) const;

    // Who is online and what they are doing.
    PresenceInfo presence(const QString &userId) const;
    void setPresence(const QString &userId, const QJsonObject &rawPresence);

    // Where someone is sitting in voice.
    VoiceStateInfo voiceState(const QString &userId) const;
    void setVoiceState(const QJsonObject &rawState);

    // Who is sitting in one voice channel right now.
    QStringList voiceMembers(const QString &channelId) const;

    // A note that someone did something a connected client had to be running
    // to do. Only the Presence hints plugin writes these, so with that plugin
    // switched off the store never holds any.
    void noteActivity(const QString &userId);
    QDateTime lastSeenActive(const QString &userId) const;
    void forgetActivity(const QString &userId);

    // "active 3 min ago", or empty when there is nothing to say. Only returns
    // text for someone Discord reports as offline, inside the window the
    // plugin is set to.
    QString activityHint(const QString &userId) const;

    // The status name to draw a bubble with. Returns "hint" when the only
    // thing we have is a guess, so the drawing can show it hollow.
    QString presenceBubble(const QString &userId) const;

    static ActivityInfo parseActivity(const QJsonObject &raw);

// One signals block, at the end. Splitting them lets an ordinary function
// drift into a signals section, which moc rejects in a very confusing way.
signals:
    void channelHistoryChanged(const QString &channelId);
    void messageAdded(const QString &channelId, const MessageInfo &message);
    void messageChanged(const QString &channelId, const QString &messageId);
    void userChanged(const QString &userId);
    void voiceStatesChanged();

private:
    void ingestGuild(const QJsonObject &rawGuild);
    void ingestChannel(const QJsonObject &rawChannel, const QString &guildId);

    QHash<QString, GuildInfo> m_guilds;
    QList<QString> m_guildOrder;
    QHash<QString, ChannelInfo> m_channels;
    QList<QString> m_directOrder;
    QHash<QString, QList<MessageInfo>> m_messages;
    QHash<QString, bool> m_historyLoaded;
    QHash<QString, UserInfo> m_users;
    QHash<QString, PresenceInfo> m_presences;
    QHash<QString, QDateTime> m_lastSeenActive;
    QHash<QString, VoiceStateInfo> m_voiceStates;
};
