#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// How the server rail is arranged: the order of the tiles, and which servers
// are tucked inside folders.
//
// Discord keeps this on its own servers in a private format Singularity does not
// speak, so this arrangement is saved on this machine only. It will not match
// the official client or your phone, and that is stated plainly in the app.
class RailLayout
{
public:
    struct Folder
    {
        QString id;
        QString name;
        QStringList guildIds;
        bool open = true;
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

private:
    int rowOfGuild(const QString &guildId) const;
    void removeGuildEverywhere(const QString &guildId);

    QList<Entry> m_entries;
};
