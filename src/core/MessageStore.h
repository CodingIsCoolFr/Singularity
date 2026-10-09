#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

// A role inside one server. Colour is a plain RGB number, so this header stays
// free of any drawing types.
struct RoleInfo
{
    QString id;
    QString name;
    int colour = 0;
    int position = 0;
    quint64 permissions = 0;   // Discord's permission bits
    bool managed = false;      // a bot's or an integration's role: nobody can hand it out

    bool hasColour() const { return colour != 0; }
};

// What you may do with roles in one server, worked out the way Discord does:
// the owner may do anything; otherwise Administrator or Manage Roles on any
// role you hold (or @everyone), and then only roles below your highest one.
struct RolePower
{
    bool owner = false;
    bool canManage = false;
    int highest = 0;   // position of your highest role

    bool canAssign(const RoleInfo &role, const QString &guildId) const
    {
        if (!canManage || role.managed || role.id == guildId)   // guild id = @everyone
            return false;
        return owner || role.position < highest;
    }
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
    QString smallImage;
    QString largeText;
    QString buttonLabel;
    QString buttonUrl;
    QStringList buttonLabels;
    QStringList buttonUrls;
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

    bool streaming = false;   // sharing a screen, which Discord shows as LIVE
    bool video = false;       // camera on
    bool muted = false;       // muted themselves, or a moderator muted them
    bool deafened = false;    // cannot hear anyone
    bool serverMuted = false; // a moderator muted them (Discord's `mute`)
    bool serverDeafened = false;

    // When this person was first seen in this channel.
    //
    // Discord does not say how long somebody has been sitting there, so this
    // is counted from when we noticed, and the official client does the same.
    // For people already present when you signed in, that is the moment you
    // arrived rather than the moment they did.
    QDateTime since;
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
    QString lastMessageId;

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

// One line of a server's member list: either a heading, or a person.
struct MemberRow
{
    bool heading = false;

    // For a heading: the role id, or "online" / "offline". Discord sends the
    // role id rather than its name, so the name is looked up against the
    // guild's roles when it is drawn.
    QString groupId;
    int groupCount = 0;

    // For a person.
    QString userId;
    QString nickname;     // server nickname, may be empty
    QString colourRoleId; // the role that decides their colour, may be empty
};

struct MemberList
{
    QList<MemberRow> rows;

    // How many people each group holds, by group id. Sent alongside the rows
    // rather than on them.
    QHash<QString, int> groupCounts;

    // Discord's own totals, which are not the same as the number of rows:
    // only the first hundred rows are ever asked for.
    int onlineCount = 0;
    int memberCount = 0;

    // Which member list this is. A second subscription used to write into
    // the same rows, and every person showed up twice.
    QString listId;
};

struct EmojiInfo
{
    QString name;
    QString id;
    bool animated = false;
};

struct GuildSticker
{
    QString id;
    QString name;
    int formatType = 1;
};

struct GuildInfo
{
    QString id;
    QString name;
    QString iconHash;

    // The picture over the channel list, and the boost bar under it.
    QString ownerId;
    QString bannerHash;              // "a_" prefix = animated
    int premiumTier = 0;             // boost level, 0 to 3
    int boostCount = 0;              // premium_subscription_count
    bool boostBarEnabled = false;    // premium_progress_bar_enabled
    QStringList features;            // "BANNER", "GUILD_TAGS", ...

    QList<QString> channelIds;
    QHash<QString, RoleInfo> roles;
    QList<EmojiInfo> emojis;
    QList<GuildSticker> stickers;
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

    // gifv embeds keep the moving picture here (an mp4 on Discord's proxy).
    // imageUrl is the poster, which is one frame and never plays.
    QString videoUrl;
    int imageWidth = 0;
    int imageHeight = 0;
    int colour = 0;

    // A card that is only a picture needs no title or border.
    //
    // Discord's own rule, from its web client: there is a picture or a video,
    // and either it is a gifv, or it is not "rich" and has no author and no
    // title. The old test knew only image and gifv, so a plain video link, or
    // a site whose preview Discord files under another type, got an empty card.
    bool isPictureOnly() const
    {
        if (imageUrl.isEmpty() && videoUrl.isEmpty())
            return false;
        if (type == QLatin1String("gifv"))
            return true;
        return type != QLatin1String("rich") && authorName.isEmpty() && title.isEmpty();
    }

    // Discord hides the words of a message that is nothing but one link, when
    // the link's preview is a single picture or gifv. Only these two kinds.
    bool replacesItsLink() const
    {
        return (type == QLatin1String("image") || type == QLatin1String("gifv"))
               && (!imageUrl.isEmpty() || !videoUrl.isEmpty());
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

// One kind of reaction on a message, with a running count.
//
// Discord sends these as a summary rather than a list of who did what, so a
// count and whether you are one of them is all there is to hold.
struct ReactionInfo
{
    QString name;   // the character itself, or a custom emoji's name
    QString id;     // set only for a custom emoji, which has a picture
    bool animated = false;
    int count = 0;
    bool mine = false;

    bool isCustom() const { return !id.isEmpty(); }

    // Two reactions are the same one if they are the same emoji.
    bool matches(const ReactionInfo &other) const
    {
        return id.isEmpty() ? (other.id.isEmpty() && name == other.name) : id == other.id;
    }
};

struct MessageInfo
{
    QString id;
    QString channelId;
    QString authorId;
    QString authorName;
    QString authorAvatar;
    QString content;
    // Discord's message type: 0 default, 19 reply, 20 slash command. Anything
    // else is a system line (friend accepted, pin, boost, and the rest).
    int type = 0;
    QDateTime timestamp;
    QList<Attachment> attachments;
    QList<EmbedInfo> embeds;
    QList<StickerInfo> stickers;
    QList<ReactionInfo> reactions;
    bool edited = false;
    bool deleted = false;   // set by the MessageLogger plugin
    bool pendingLocal = false;

    // A forwarded message: message_reference type 1. Its own content is
    // empty; what it carries is a copy of the original taken when it was
    // forwarded (message_snapshots), which never changes afterwards. A
    // snapshot has words, pictures, embeds and stickers, but no author or id.
    QList<MessageInfo> snapshots;

    // Buttons under a message, and Discord's "Components V2" layout (boxes,
    // text blocks, dividers, pictures), kept as Discord sent them and drawn
    // by the chat. A bot like Dyno sends a V2 message with no content and no
    // embeds at all, which is why it used to read "(no text)".
    QJsonArray components;
    // The application that owns the buttons. A press is sent to this id.
    QString applicationId;
    int flags = 0;

    // A slash command's answer (type 20, or 23 for one run from a menu). The
    // line above it says who ran which command, the way Discord draws it.
    // From interaction_metadata, or the older `interaction` when that is all
    // there is.
    QString interactionUserId;
    QString interactionUserName;
    QString interactionUserAvatar;
    QString interactionName;

    // A Rich Presence / game invite. Discord sends this as `activity` plus
    // `application`, with no words and no embeds, which is why it used to
    // read "(no text)".
    int activityType = 0;   // 1 join, 2 spectate, 3 listen, 5 join request
    QString activityPartyId;
    QString activityApplicationId;
    QString activityApplicationName;
    QString activityApplicationIcon;
    QString forwardedFromChannelId;
    QString forwardedFromGuildId;
    QString forwardedFromMessageId;

    // A reply (message type 19). The line above the message names who was
    // answered and a short bit of what they said. referenced_message is null
    // when that message is gone.
    QString replyToId;
    QString replyAuthorId;
    QString replyAuthorName;
    QString replyAuthorAvatar;
    QString replyContent;
    bool replyEdited = false;
    bool replyMissing = false;
    bool replyHasAttachment = false;

    // A private call (message type 3). ended is invalid while the call is
    // still going. participants is who actually joined, which is how a missed
    // call is told apart from one you were in.
    bool callPresent = false;
    QStringList callParticipants;
    QDateTime callEnded;
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

    // A large server arrives later, already full, after an empty placeholder.
    // Also GUILD_UPDATE, which leaves the channels out: those are kept.
    void applyGuild(const QJsonObject &rawGuild);

    // User accounts get a second payload right after READY. That is where
    // voice states and most presences actually live, so without this you only
    // ever see yourself in a voice channel.
    void ingestReadySupplemental(const QJsonObject &payload);

    // GUILD_MEMBER_LIST_UPDATE: the answer to asking for a channel's member
    // list. This is the only place server members' statuses arrive, so
    // without it everyone in a server looks offline.
    void ingestMemberListUpdate(const QJsonObject &payload);

    // The next member-list answer may belong to a channel we just opened.
    // Until it arrives, an update for a different list is ignored.
    void noteMemberListRequest(const QString &guildId);

    // The member list itself, in Discord's own order.
    //
    // Kept as one flat list with the headings in it rather than as a map of
    // role to people, because the order is Discord's answer and not something
    // to be recomputed: it already knows which roles are hoisted, how they
    // rank, and who is offline. Sorting it again here would disagree with the
    // official client for no benefit.
    MemberList memberList(const QString &guildId) const;

    QList<GuildInfo> guilds() const;
    GuildInfo guild(const QString &guildId) const;

    // Your own roles in each server (READY's merged_members, then
    // GUILD_MEMBER_UPDATE), and what they let you do with roles.
    void setSelfRoles(const QString &guildId, const QStringList &roleIds);
    RolePower rolePower(const QString &guildId) const;
    // Owner and Administrator count as every bit. Otherwise @everyone plus
    // the roles you hold, which is what Mute / Deafen / Move Members read.
    quint64 selfPermissions(const QString &guildId) const;
    QStringList selfRoles(const QString &guildId) const { return m_selfRoles.value(guildId); }
    ChannelInfo channel(const QString &channelId) const;
    QList<ChannelInfo> channelsOfGuild(const QString &guildId) const;
    QList<ChannelGroup> groupedChannels(const QString &guildId) const;
    QList<ChannelInfo> directChannels() const;

    QList<MessageInfo> messages(const QString &channelId) const;
    bool hasHistory(const QString &channelId) const;

    // One line for the log: how much conversation is being held on to.
    QString memorySummary() const;

    // Drop the history of channels nobody has looked at for a while.
    //
    // Every channel opened, and every channel fetched ahead of a click, left
    // its messages here for the rest of the session. Fetching ahead made that
    // worse on purpose - six channels per server, whether or not any of them
    // were ever opened - so the store had to learn to let go.
    void trimHistories(const QString &keepChannelId);

    // Used for CHANNEL_CREATE, so a chat opened from a profile card shows up,
    // and for CHANNEL_UPDATE, which replaces the channel with the new copy.
    void ingestChannelObject(const QJsonObject &rawChannel);

    // CHANNEL_DELETE, and closing a direct message. Returns false when the
    // channel was already gone, so a second notice does not rebuild the list.
    bool forgetChannel(const QString &channelId);
    // A server we left or were removed from (GUILD_DELETE), and its channels.
    bool forgetGuild(const QString &guildId);

    void setHistory(const QString &channelId, const QJsonArray &rawMessages);

    // Puts a batch of older messages on the front of a channel, skipping any
    // already there. Returns how many were actually new, so the view knows
    // whether it reached the beginning of the channel.
    int prependHistory(const QString &channelId, const QJsonArray &rawMessages);

    // The oldest message being held for a channel, which is where the next
    // request back in time starts from.
    QString oldestMessageId(const QString &channelId) const;

    // What you have and have not read.
    //
    // A channel is unread when a message has arrived that you were not looking
    // at. Mentions are counted on top of that, because a server shows a number
    // for those and only a dot for everything else.
    void ingestReadState(const QJsonObject &readyPayload);
    void noteIncoming(const QString &channelId, const QString &messageId, bool mention, bool seen);
    void markChannelRead(const QString &channelId, const QString &messageId);
    void markChannelsRead(const QList<QPair<QString, QString>> &channelsAndMessages);
    bool isUnread(const QString &channelId) const;

    // Every chat with something unread, each with the newest message id
    // (what an ack needs). For "Read All".
    QList<QPair<QString, QString>> unreadChannels() const;
    int mentionCount(const QString &channelId) const;
    bool guildHasUnread(const QString &guildId) const;
    int guildMentionCount(const QString &guildId) const;

    // MESSAGE_REACTION_ADD and MESSAGE_REACTION_REMOVE. `selfUserId` decides
    // whether the change was yours, which is what highlights the pill.
    void applyReaction(const QJsonObject &data, bool added, const QString &selfUserId);

    // MESSAGE_REACTION_REMOVE_ALL, and REMOVE_EMOJI when an emoji is given.
    void clearReactions(const QJsonObject &data);
    // Adds one message. Returns how many older messages were dropped to keep
    // the channel under its cap, or -1 when this message was already stored.
    int appendMessage(const QJsonObject &rawMessage);
    void updateMessage(const QJsonObject &rawMessage);
    // A private call ended. Marks the still-open call messages in that channel
    // so the chat stops offering Join.
    void endCall(const QString &channelId);
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
    void setVoiceStates(const QJsonArray &states);
    void clearVoiceStates();

    // Everyone sitting in one channel is out of it once the channel is gone.
    void dropVoiceStatesInChannel(const QString &channelId);

    // Replaces every voice state for one server. A large server's first
    // snapshot is partial, and patching later updates onto it leaves people
    // in channels they left.
    void replaceGuildVoiceStates(const QString &guildId, const QJsonArray &states);

    void clearMemberList(const QString &guildId);

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
    void readStateChanged();
    // A direct chat moved, because a newer message landed in it.
    void directOrderChanged();
    void messageAdded(const QString &channelId, const MessageInfo &message);
    void messageChanged(const QString &channelId, const QString &messageId);
    void userChanged(const QString &userId);
    void voiceStatesChanged();
    void memberListChanged(const QString &guildId);

private:
    void ingestGuild(const QJsonObject &rawGuild);
    void ingestChannel(const QJsonObject &rawChannel, const QString &guildId);

    // Records one voice state without telling anyone; returns whose it was.
    // The public setters call this and then signal once.
    QString applyVoiceState(const QJsonObject &rawState);
    // Most recent conversation first, which is the order Discord shows.
    void bumpDirectChannel(const QString &channelId);

    QHash<QString, MemberList> m_memberLists;
    QSet<QString> m_memberListFresh;
    QHash<QString, GuildInfo> m_guilds;
    QHash<QString, QStringList> m_selfRoles;   // guild id -> your role ids
    QString m_selfUserId;
    QList<QString> m_guildOrder;
    QHash<QString, ChannelInfo> m_channels;
    QList<QString> m_directOrder;
    QHash<QString, QList<MessageInfo>> m_messages;

    struct ReadMark
    {
        QString lastReadId;
        int mentions = 0;
        bool unread = false;
    };
    QHash<QString, ReadMark> m_reads;
    static bool newerId(const QString &a, const QString &b);
    QHash<QString, bool> m_historyLoaded;

    // Channel ids, least recently touched first.
    QList<QString> m_historyOrder;
    QHash<QString, UserInfo> m_users;
    QHash<QString, PresenceInfo> m_presences;
    QHash<QString, QDateTime> m_lastSeenActive;
    QHash<QString, VoiceStateInfo> m_voiceStates;
};
