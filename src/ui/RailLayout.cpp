#include "ui/RailLayout.h"

#include "core/AppConfig.h"
#include "core/Logger.h"

#include <QUuid>

namespace {

// Field separator inside one saved folder record. A unit separator cannot
// appear in a folder name typed by a person.
const QChar FieldSeparator = QChar(0x1F);

constexpr auto OrderKey = "rail/order";
constexpr auto FoldersKey = "rail/folders";

const QString GuildPrefix = QStringLiteral("g:");
const QString FolderPrefix = QStringLiteral("f:");

} // namespace

void RailLayout::load()
{
    m_entries.clear();

    AppConfig &config = AppConfig::instance();

    // Folders first, so the order list can point at them.
    QHash<QString, Folder> folders;
    const QStringList records = config.value(QLatin1String(FoldersKey)).toStringList();
    for (const QString &record : records) {
        const QStringList fields = record.split(FieldSeparator);
        if (fields.size() < 4)
            continue;

        Folder folder;
        folder.id = fields.at(0);
        folder.name = fields.at(1);
        folder.open = fields.at(2) == QLatin1String("1");
        folder.guildIds = fields.at(3).split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (!folder.id.isEmpty())
            folders.insert(folder.id, folder);
    }

    const QStringList order = config.value(QLatin1String(OrderKey)).toStringList();
    for (const QString &token : order) {
        if (token.startsWith(FolderPrefix)) {
            const QString id = token.mid(FolderPrefix.size());
            if (!folders.contains(id))
                continue;
            Entry entry;
            entry.isFolder = true;
            entry.folder = folders.take(id);
            m_entries.append(entry);
        } else if (token.startsWith(GuildPrefix)) {
            Entry entry;
            entry.guildId = token.mid(GuildPrefix.size());
            if (!entry.guildId.isEmpty())
                m_entries.append(entry);
        }
    }

    // A folder that lost its place in the order still deserves to exist.
    for (const Folder &folder : folders) {
        Entry entry;
        entry.isFolder = true;
        entry.folder = folder;
        m_entries.append(entry);
    }
}

void RailLayout::save() const
{
    QStringList order;
    QStringList records;

    for (const Entry &entry : m_entries) {
        if (entry.isFolder) {
            order.append(FolderPrefix + entry.folder.id);
            records.append(QStringList{entry.folder.id, entry.folder.name,
                                       entry.folder.open ? QStringLiteral("1") : QStringLiteral("0"),
                                       entry.folder.guildIds.join(QLatin1Char(','))}
                               .join(FieldSeparator));
        } else {
            order.append(GuildPrefix + entry.guildId);
        }
    }

    AppConfig &config = AppConfig::instance();
    config.setValue(QLatin1String(OrderKey), order);
    config.setValue(QLatin1String(FoldersKey), records);
}

void RailLayout::reconcile(const QStringList &knownGuildIds)
{
    const QSet<QString> known(knownGuildIds.begin(), knownGuildIds.end());

    QSet<QString> placed;
    QList<Entry> kept;

    for (Entry entry : m_entries) {
        if (entry.isFolder) {
            // Drop servers you have left, and remember the rest.
            QStringList surviving;
            for (const QString &guildId : entry.folder.guildIds) {
                if (known.contains(guildId) && !placed.contains(guildId)) {
                    surviving.append(guildId);
                    placed.insert(guildId);
                }
            }
            entry.folder.guildIds = surviving;

            // An empty folder is clutter, so it goes.
            if (!surviving.isEmpty())
                kept.append(entry);
            continue;
        }

        if (known.contains(entry.guildId) && !placed.contains(entry.guildId)) {
            placed.insert(entry.guildId);
            kept.append(entry);
        }
    }

    // Servers we have never seen go on the end, in the order Discord gave.
    for (const QString &guildId : knownGuildIds) {
        if (placed.contains(guildId))
            continue;
        Entry entry;
        entry.guildId = guildId;
        kept.append(entry);
    }

    m_entries = kept;
}

void RailLayout::moveEntry(int fromRow, int toRow)
{
    if (fromRow < 0 || fromRow >= m_entries.size())
        return;
    toRow = qBound(0, toRow, m_entries.size() - 1);
    if (fromRow == toRow)
        return;
    m_entries.move(fromRow, toRow);
}

int RailLayout::rowOfGuild(const QString &guildId) const
{
    for (int row = 0; row < m_entries.size(); ++row) {
        if (!m_entries.at(row).isFolder && m_entries.at(row).guildId == guildId)
            return row;
    }
    return -1;
}

void RailLayout::removeGuildEverywhere(const QString &guildId)
{
    for (int row = m_entries.size() - 1; row >= 0; --row) {
        Entry &entry = m_entries[row];
        if (entry.isFolder) {
            entry.folder.guildIds.removeAll(guildId);
            if (entry.folder.guildIds.isEmpty())
                m_entries.removeAt(row);
        } else if (entry.guildId == guildId) {
            m_entries.removeAt(row);
        }
    }
}

void RailLayout::moveGuildIntoFolder(const QString &guildId, const QString &folderId)
{
    if (guildId.isEmpty() || folderId.isEmpty())
        return;

    removeGuildEverywhere(guildId);

    for (Entry &entry : m_entries) {
        if (entry.isFolder && entry.folder.id == folderId) {
            entry.folder.guildIds.append(guildId);
            save();
            return;
        }
    }

    // The folder vanished, so put the server back on its own rather than
    // losing it entirely.
    Entry entry;
    entry.guildId = guildId;
    m_entries.append(entry);
    save();
}

void RailLayout::moveGuildOutOfFolders(const QString &guildId)
{
    removeGuildEverywhere(guildId);

    Entry entry;
    entry.guildId = guildId;
    m_entries.append(entry);
    save();
}

QString RailLayout::createFolder(const QString &name, const QString &firstGuildId)
{
    Folder folder;
    folder.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    folder.name = name.trimmed().isEmpty() ? QStringLiteral("Folder") : name.trimmed();
    folder.open = true;

    // The new folder takes the place of the server that started it.
    int insertAt = rowOfGuild(firstGuildId);
    if (insertAt < 0)
        insertAt = m_entries.size();

    removeGuildEverywhere(firstGuildId);
    insertAt = qBound(0, insertAt, m_entries.size());

    if (!firstGuildId.isEmpty())
        folder.guildIds.append(firstGuildId);

    Entry entry;
    entry.isFolder = true;
    entry.folder = folder;
    m_entries.insert(insertAt, entry);

    save();
    wlog(QStringLiteral("rail"), QStringLiteral("made folder \"%1\"").arg(folder.name));
    return folder.id;
}

void RailLayout::renameFolder(const QString &folderId, const QString &name)
{
    for (Entry &entry : m_entries) {
        if (entry.isFolder && entry.folder.id == folderId) {
            entry.folder.name = name.trimmed().isEmpty() ? QStringLiteral("Folder") : name.trimmed();
            save();
            return;
        }
    }
}

void RailLayout::dissolveFolder(const QString &folderId)
{
    for (int row = 0; row < m_entries.size(); ++row) {
        if (!m_entries.at(row).isFolder || m_entries.at(row).folder.id != folderId)
            continue;

        // The servers inside take the folder's place, so none are lost.
        const QStringList guildIds = m_entries.at(row).folder.guildIds;
        m_entries.removeAt(row);

        int insertAt = row;
        for (const QString &guildId : guildIds) {
            Entry entry;
            entry.guildId = guildId;
            m_entries.insert(insertAt++, entry);
        }

        save();
        return;
    }
}

void RailLayout::setFolderOpen(const QString &folderId, bool open)
{
    for (Entry &entry : m_entries) {
        if (entry.isFolder && entry.folder.id == folderId) {
            if (entry.folder.open == open)
                return;
            entry.folder.open = open;
            save();
            return;
        }
    }
}

const RailLayout::Folder *RailLayout::folder(const QString &folderId) const
{
    for (const Entry &entry : m_entries) {
        if (entry.isFolder && entry.folder.id == folderId)
            return &entry.folder;
    }
    return nullptr;
}
