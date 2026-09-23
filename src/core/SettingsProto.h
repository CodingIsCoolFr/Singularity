#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

// One person's loudness, as Discord stores it.
//
// 100 is normal and 200 is twice that. Mute is its own flag: turning someone
// all the way down does not mute them, and muting them does not move the slider.
struct UserAudioLevel
{
    int volume = 100;
    bool muted = false;
};

// Field 16 of Discord's preloaded user settings protobuf: the per-person
// volumes. `present` is false when that field is not in this blob, which on a
// partial update means "leave the volumes alone".
//
// A missing volume inside an entry is 0, not "unchanged". Protobuf does not
// write a zero, so the absence is the value.
bool audioContextFromProto(const QByteArray &bytes, QHash<QString, UserAudioLevel> *levels, bool *present);

// Field 11 of the same blob, StatusSettings, and inside it field 1, the
// status as a StringValue: "online", "idle", "dnd" or "invisible". Numbers
// checked against discord-protos' PreloadedUserSettings.proto, not counted.
//
// Empty when the blob does not carry it, which on a partial update means the
// status did not change.
QString statusFromProto(const QByteArray &bytes);

// One entry of the server rail as Discord stores it. A server on its own is
// stored as a "folder" with no id holding just that server; a real folder has
// an id, and may have a name and a colour.
struct DiscordFolder
{
    QStringList guildIds;
    qint64 id = 0;          // 0: not a real folder, just one loose server
    QString name;           // empty: unnamed
    bool hasColor = false;
    quint64 color = 0;      // 0xRRGGBB
};

// Field 14 of the settings blob, GuildFolders, in rail order. Numbers checked
// against discord-protos' PreloadedUserSettings.proto:
//   GuildFolders { repeated GuildFolder folders = 1; repeated fixed64 guild_positions = 2; }
//   GuildFolder  { repeated fixed64 guild_ids = 1; Int64Value id = 2;
//                  StringValue name = 3; UInt64Value color = 4; }
// `present` is false when the blob does not carry field 14, which on a partial
// update means the folders did not change.
bool guildFoldersFromProto(const QByteArray &bytes, QList<DiscordFolder> *folders, bool *present);
