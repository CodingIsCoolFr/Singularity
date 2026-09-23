#include "ui/RailLayout.h"

#include "core/AppConfig.h"
#include "core/Logger.h"

#include <QHash>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSet>

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
        if (fields.size() > 4 && !fields.at(4).isEmpty()) {
            folder.hasColor = true;
            folder.color = fields.at(4).toULongLong();
        }
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
                                       entry.folder.guildIds.join(QLatin1Char(',')),
                                       entry.folder.hasColor ? QString::number(entry.folder.color)
                                                             : QString()}
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
    folder.id = newFolderId();
    folder.name = name.trimmed();
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
            // Empty is allowed: Discord has unnamed folders too.
            entry.folder.name = name.trimmed();
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

QString RailLayout::newFolderId()
{
    // Discord's own client uses a random positive number. So does this, kept
    // under 2^31 so it survives every JSON reader on the way.
    return QString::number(QRandomGenerator::global()->bounded(1, 0x7FFFFFFF));
}

QString RailLayout::folderOf(const QString &guildId) const
{
    for (const Entry &entry : m_entries) {
        if (entry.isFolder && entry.folder.guildIds.contains(guildId))
            return entry.folder.id;
    }
    return {};
}

void RailLayout::applyFromDiscord(const QList<DiscordFolder> &folders, const QStringList &knownGuildIds)
{
    // Open and closed is ours, not Discord's.
    QHash<QString, bool> wasOpen;
    for (const Entry &entry : m_entries) {
        if (entry.isFolder)
            wasOpen.insert(entry.folder.id, entry.folder.open);
    }

    QList<Entry> rebuilt;
    for (const DiscordFolder &source : folders) {
        if (source.id == 0) {
            // A loose server, stored by Discord as a folder of one.
            for (const QString &guildId : source.guildIds) {
                Entry entry;
                entry.guildId = guildId;
                rebuilt.append(entry);
            }
            continue;
        }

        Entry entry;
        entry.isFolder = true;
        entry.folder.id = QString::number(source.id);
        entry.folder.name = source.name;
        entry.folder.guildIds = source.guildIds;
        entry.folder.hasColor = source.hasColor;
        entry.folder.color = source.color;
        entry.folder.open = wasOpen.value(entry.folder.id, false);
        rebuilt.append(entry);
    }

    m_entries = rebuilt;
    reconcile(knownGuildIds);
    save();
}

QJsonArray RailLayout::toDiscordFolders()
{
    QJsonArray out;
    bool renamed = false;

    for (Entry &entry : m_entries) {
        QJsonObject folder;
        QJsonArray ids;

        if (!entry.isFolder) {
            ids.append(entry.guildId);
            folder.insert(QStringLiteral("guild_ids"), ids);
            folder.insert(QStringLiteral("id"), QJsonValue::Null);
            folder.insert(QStringLiteral("name"), QJsonValue::Null);
            folder.insert(QStringLiteral("color"), QJsonValue::Null);
            out.append(folder);
            continue;
        }

        // Folders made before this version had a short text id, which Discord
        // cannot store. They get a number the first time they are sent.
        bool numeric = false;
        const qint64 id = entry.folder.id.toLongLong(&numeric);
        if (!numeric || id <= 0) {
            entry.folder.id = newFolderId();
            renamed = true;
        }

        for (const QString &guildId : entry.folder.guildIds)
            ids.append(guildId);
        folder.insert(QStringLiteral("guild_ids"), ids);
        folder.insert(QStringLiteral("id"), double(entry.folder.id.toLongLong()));
        folder.insert(QStringLiteral("name"),
                      entry.folder.name.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(entry.folder.name));
        folder.insert(QStringLiteral("color"),
                      entry.folder.hasColor ? QJsonValue(double(entry.folder.color)) : QJsonValue(QJsonValue::Null));
        out.append(folder);
    }

    if (renamed)
        save();
    return out;
}

QString RailLayout::mergeIntoNewFolder(const QString &targetGuildId, const QString &droppedGuildId)
{
    if (targetGuildId.isEmpty() || droppedGuildId.isEmpty() || targetGuildId == droppedGuildId)
        return {};

    // Onto a server that is already in a folder: join that folder instead.
    const QString existing = folderOf(targetGuildId);
    if (!existing.isEmpty()) {
        insertIntoFolder(droppedGuildId, existing, targetGuildId);
        return existing;
    }

    // Take the dropped one out first, so the target's row is counted after
    // the gap closes.
    removeGuildEverywhere(droppedGuildId);
    const int row = rowOfGuild(targetGuildId);
    if (row < 0)
        return {};

    Entry entry;
    entry.isFolder = true;
    entry.folder.id = newFolderId();
    entry.folder.guildIds = QStringList{targetGuildId, droppedGuildId};
    entry.folder.open = true;
    m_entries[row] = entry;

    save();
    wlog(QStringLiteral("rail"), QStringLiteral("made a folder from two servers"));
    return entry.folder.id;
}

void RailLayout::insertIntoFolder(const QString &guildId, const QString &folderId, const QString &afterGuildId)
{
    if (guildId.isEmpty() || folderId.isEmpty() || guildId == afterGuildId)
        return;

    removeGuildEverywhere(guildId);

    for (Entry &entry : m_entries) {
        if (!entry.isFolder || entry.folder.id != folderId)
            continue;
        const int after = afterGuildId.isEmpty() ? -1 : int(entry.folder.guildIds.indexOf(afterGuildId));
        entry.folder.guildIds.insert(after < 0 ? entry.folder.guildIds.size() : after + 1, guildId);
        save();
        return;
    }

    // The folder vanished while this was being dragged. Keep the server.
    Entry entry;
    entry.guildId = guildId;
    m_entries.append(entry);
    save();
}
