#pragma once

#include "core/SettingsProto.h"

#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

// How the server rail is arranged: the order of the tiles, and which servers
// are tucked inside folders.
//
// Discord's own arrangement is the truth. It arrives at sign-in inside the
// settings blob (field 14), and whenever another device changes it; it is
// adopted as it is, and a change made here is sent back so the official client
// and your phone show the same rail. Only whether each folder is open is kept
// on this machine, because Discord keeps that per device too.
class RailLayout
{
public:
    struct Folder
    {
        QString id;          // Discord's folder id, as a decimal number
        QString name;        // empty: unnamed, shown as "Folder"
        QStringList guildIds;
        bool open = true;
        bool hasColor = false;
        quint64 color = 0;   // 0xRRGGBB
    };

    // One row in the arrangement: either a server on its own, or a folder.
    struct Entry
    {
        bool isFolder = false;
        QString guildId;   // when isFolder is false
        Folder folder;     // when isFolder is true
    };

    void load();
    void save() const;

    // Drops servers that are gone and appends ones we have not seen before, so
    // leaving or joining a server never leaves a hole or hides a tile.
    void reconcile(const QStringList &knownGuildIds);

    QList<Entry> entries() const { return m_entries; }

    // Used after a drag, when the view is the source of truth.
    void setEntries(const QList<Entry> &entries) { m_entries = entries; }

    // Moves one tile so it sits at `toRow`. A server dropped onto a folder
    // goes inside it.
    void moveEntry(int fromRow, int toRow);
    void moveGuildIntoFolder(const QString &guildId, const QString &folderId);
    void moveGuildOutOfFolders(const QString &guildId);

    QString createFolder(const QString &name, const QString &firstGuildId);
    void renameFolder(const QString &folderId, const QString &name);
    void dissolveFolder(const QString &folderId);
    void setFolderOpen(const QString &folderId, bool open);

    const Folder *folder(const QString &folderId) const;

    // Replaces the arrangement with Discord's. Open and closed carry over by
    // folder id. Servers Discord did not mention are appended, ones it
    // mentions that we are not in are dropped.
    void applyFromDiscord(const QList<DiscordFolder> &folders, const QStringList &knownGuildIds);

    // The arrangement in the shape Discord's settings endpoint takes: every
    // tile in order, a loose server as a folder with no id holding just it.
    QJsonArray toDiscordFolders();

    // A server dropped onto another: a new folder holding the target first
    // and the dropped one after it, in the target's place - what the official
    // client does.
    QString mergeIntoNewFolder(const QString &targetGuildId, const QString &droppedGuildId);

    // A server dropped onto a folder, or onto a server already in one. It
    // goes in after `afterGuildId`, or at the end when that is empty.
    void insertIntoFolder(const QString &guildId, const QString &folderId, const QString &afterGuildId);

    // The folder a server is in, or empty.
    QString folderOf(const QString &guildId) const;

private:
    static QString newFolderId();
    int rowOfGuild(const QString &guildId) const;
    void removeGuildEverywhere(const QString &guildId);

    QList<Entry> m_entries;
};
