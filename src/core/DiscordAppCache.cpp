#include "core/DiscordAppCache.h"

#include "core/Logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <functional>

namespace DiscordAppCache {

namespace {

// Local storage files are a few megabytes. Anything far past that, or a block
// claiming to unpack into more, is not what is being looked for.
constexpr qint64 MaxFileBytes = 256LL * 1024 * 1024;
constexpr quint64 MaxBlockBytes = 64ULL * 1024 * 1024;

// Where in a key the value we want lives: the settings blob the app keeps,
// per type, in local storage. "UserSettingsProtoLastWriteTimes" sits beside it
// and is not it.
const QByteArray SettingsKey = QByteArrayLiteral("UserSettingsProtoStore");

using Take = std::function<void(const QByteArray &key, const QByteArray &value)>;

bool readVarint(const QByteArray &buf, qsizetype &pos, quint64 *out)
{
    quint64 result = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (pos >= buf.size())
            return false;
        const quint8 byte = quint8(buf.at(pos++));
        result |= quint64(byte & 0x7F) << shift;
        if (!(byte & 0x80)) {
            *out = result;
            return true;
        }
    }
    return false;
}

// Snappy, which LevelDB packs its blocks with: the unpacked length, then a
// run of literal bytes and references back into what came out already.
bool unsnappy(const QByteArray &in, QByteArray *out)
{
    qsizetype pos = 0;
    quint64 length = 0;
    if (!readVarint(in, pos, &length) || length > MaxBlockBytes)
        return false;

    QByteArray result;
    result.reserve(qsizetype(length));
    while (pos < in.size()) {
        const quint8 tag = quint8(in.at(pos++));
        const int kind = tag & 3;

        if (kind == 0) {
            quint64 n = tag >> 2;
            if (n >= 60) {
                const int extra = int(n) - 59;
                if (pos + extra > in.size())
                    return false;
                n = 0;
                for (int i = 0; i < extra; ++i)
                    n |= quint64(quint8(in.at(pos + i))) << (8 * i);
                pos += extra;
            }
            n += 1;
            if (n > quint64(in.size() - pos))
                return false;
            result.append(in.constData() + pos, qsizetype(n));
            pos += qsizetype(n);
        } else {
            quint64 n = 0;
            quint64 offset = 0;
            if (kind == 1) {
                if (pos >= in.size())
                    return false;
                n = ((tag >> 2) & 7) + 4;
                offset = (quint64(tag >> 5) << 8) | quint8(in.at(pos++));
            } else {
                const int bytes = kind == 2 ? 2 : 4;
                if (pos + bytes > in.size())
                    return false;
                n = (tag >> 2) + 1;
                for (int i = 0; i < bytes; ++i)
                    offset |= quint64(quint8(in.at(pos + i))) << (8 * i);
                pos += bytes;
            }
            if (offset == 0 || offset > quint64(result.size()))
                return false;
            // A reference may overlap what it is copying, so byte by byte.
            const qsizetype from = result.size() - qsizetype(offset);
            for (quint64 i = 0; i < n; ++i)
                result.append(result.at(from + qsizetype(i)));
        }
        if (quint64(result.size()) > length)
            return false;
    }
    if (quint64(result.size()) != length)
        return false;
    *out = result;
    return true;
}

// One block of a table file, found by its offset and size, unpacked. The
// byte after it says how it was packed: 0 not at all, 1 Snappy.
bool readBlock(const QByteArray &file, quint64 offset, quint64 size, QByteArray *out)
{
    if (offset > quint64(file.size()) || size + 5 > quint64(file.size()) - offset)
        return false;
    const QByteArray raw = file.mid(qsizetype(offset), qsizetype(size));
    const quint8 packing = quint8(file.at(qsizetype(offset + size)));
    if (packing == 0) {
        *out = raw;
        return true;
    }
    return packing == 1 && unsnappy(raw, out);
}

// The key/value pairs in a block. Each key only spells out where it differs
// from the one before; a table of restart points ends the block.
bool forEachInBlock(const QByteArray &block, const Take &take)
{
    if (block.size() < 4)
        return false;
    const quint32 restarts = qFromLittleEndian<quint32>(block.constData() + block.size() - 4);
    const qint64 end = qint64(block.size()) - 4 - 4 * qint64(restarts);
    if (end < 0)
        return false;

    qsizetype pos = 0;
    QByteArray key;
    while (pos < end) {
        quint64 shared = 0;
        quint64 unshared = 0;
        quint64 valueLength = 0;
        if (!readVarint(block, pos, &shared) || !readVarint(block, pos, &unshared)
            || !readVarint(block, pos, &valueLength))
            return false;
        if (shared > quint64(key.size()) || unshared > quint64(end - pos)
            || valueLength > quint64(end - pos) - unshared)
            return false;
        key = key.left(qsizetype(shared)) + block.mid(pos, qsizetype(unshared));
        pos += qsizetype(unshared);
        take(key, block.mid(pos, qsizetype(valueLength)));
        pos += qsizetype(valueLength);
    }
    return true;
}

// A .ldb file: a footer pointing at an index, the index at the blocks.
void readTable(const QByteArray &file, const Take &take)
{
    if (file.size() < 48)
        return;
    const QByteArray footer = file.right(48);
    if (qFromLittleEndian<quint64>(footer.constData() + 40) != 0xdb4775248b80fb57ULL)
        return;

    qsizetype pos = 0;
    quint64 metaOffset = 0;
    quint64 metaSize = 0;
    quint64 indexOffset = 0;
    quint64 indexSize = 0;
    if (!readVarint(footer, pos, &metaOffset) || !readVarint(footer, pos, &metaSize)
        || !readVarint(footer, pos, &indexOffset) || !readVarint(footer, pos, &indexSize))
        return;

    QByteArray index;
    if (!readBlock(file, indexOffset, indexSize, &index))
        return;

    forEachInBlock(index, [&](const QByteArray &, const QByteArray &handle) {
        qsizetype at = 0;
        quint64 offset = 0;
        quint64 size = 0;
        QByteArray block;
        if (!readVarint(handle, at, &offset) || !readVarint(handle, at, &size)
            || !readBlock(file, offset, size, &block))
            return;
        forEachInBlock(block, [&](const QByteArray &internalKey, const QByteArray &value) {
            // The last eight bytes are a sequence number and a kind, and the
            // kind is in the lowest byte: 1 a value, 0 a deletion.
            if (internalKey.size() < 8 || quint8(internalKey.at(internalKey.size() - 8)) != 1)
                return;
            take(internalKey.left(internalKey.size() - 8), value);
        });
    });
}

// One record of the log: a batch of puts and deletes.
void readBatch(const QByteArray &batch, const Take &take)
{
    if (batch.size() < 12)
        return;
    const quint32 count = qFromLittleEndian<quint32>(batch.constData() + 8);
    qsizetype pos = 12;
    for (quint32 i = 0; i < count && pos < batch.size(); ++i) {
        const quint8 kind = quint8(batch.at(pos++));
        quint64 keyLength = 0;
        if (!readVarint(batch, pos, &keyLength) || keyLength > quint64(batch.size() - pos))
            return;
        const QByteArray key = batch.mid(pos, qsizetype(keyLength));
        pos += qsizetype(keyLength);
        if (kind != 1)
            continue;   // a deletion carries no value
        quint64 valueLength = 0;
        if (!readVarint(batch, pos, &valueLength) || valueLength > quint64(batch.size() - pos))
            return;
        take(key, batch.mid(pos, qsizetype(valueLength)));
        pos += qsizetype(valueLength);
    }
}

// A .log file: writes not yet folded into a table, in 32 KB blocks of
// records. A record too big for what is left of a block is split across
// blocks as first, middle and last pieces.
void readLog(const QByteArray &file, const Take &take)
{
    constexpr qsizetype BlockSize = 32768;
    qsizetype pos = 0;
    QByteArray pending;
    while (pos + 7 <= file.size()) {
        const qsizetype left = BlockSize - pos % BlockSize;
        if (left < 7) {
            pos += left;   // too little room for a header: padding
            continue;
        }
        const quint16 length = qFromLittleEndian<quint16>(file.constData() + pos + 4);
        const quint8 type = quint8(file.at(pos + 6));
        if (pos + 7 + length > file.size())
            break;   // being written as we read
        const QByteArray body = file.mid(pos + 7, length);
        pos += 7 + length;

        switch (type) {
        case 1:   // whole
            readBatch(body, take);
            pending.clear();
            break;
        case 2:   // first
            pending = body;
            break;
        case 3:   // middle
            pending += body;
            break;
        case 4:   // last
            pending += body;
            readBatch(pending, take);
            pending.clear();
            break;
        default:
            break;
        }
    }
}

// Chromium keeps a local storage value as one byte saying how its text is
// stored - 0 UTF-16, 1 Latin-1 - and then the text.
QString storageText(const QByteArray &value)
{
    if (value.isEmpty())
        return {};
    const QByteArray body = value.mid(1);
    if (value.at(0) == 0)
        return QString::fromUtf16(reinterpret_cast<const char16_t *>(body.constData()), body.size() / 2);
    return QString::fromLatin1(body);
}

// The folders inside one cached copy: {"_state": {"1": {"proto": base64}}},
// where "1" is the preloaded settings blob that carries them.
bool foldersFromValue(const QByteArray &value, QList<DiscordFolder> *folders)
{
    const QJsonObject root = QJsonDocument::fromJson(storageText(value).toUtf8()).object();
    const QString proto = root.value(QStringLiteral("_state"))
                              .toObject()
                              .value(QStringLiteral("1"))
                              .toObject()
                              .value(QStringLiteral("proto"))
                              .toString();
    if (proto.isEmpty())
        return false;
    bool present = false;
    return guildFoldersFromProto(QByteArray::fromBase64(proto.toLatin1()), folders, &present) && present
           && !folders->isEmpty();
}

QString fingerprint(const QList<DiscordFolder> &folders)
{
    QStringList parts;
    for (const DiscordFolder &folder : folders)
        parts << QString::number(folder.id) + QLatin1Char(':') + folder.guildIds.join(QLatin1Char(','));
    return parts.join(QLatin1Char('|'));
}

} // namespace

int FolderSnapshot::folderCount() const
{
    int count = 0;
    for (const DiscordFolder &folder : folders) {
        if (folder.id != 0)
            ++count;
    }
    return count;
}

int FolderSnapshot::serverCount() const
{
    int count = 0;
    for (const DiscordFolder &folder : folders)
        count += int(folder.guildIds.size());
    return count;
}

QList<FolderSnapshot> folderSnapshots()
{
    const QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty())
        return {};

    struct App {
        const char *folder;
        const char *name;
    };
    constexpr App apps[] = {
        {"discord", "Discord"},
        {"discordptb", "Discord PTB"},
        {"discordcanary", "Discord Canary"},
    };

    QList<FolderSnapshot> found;
    QSet<QString> seen;
    for (const App &app : apps) {
        const QDir dir(QDir(appData).filePath(QLatin1String(app.folder) + QStringLiteral("/Local Storage/leveldb")));
        if (!dir.exists())
            continue;

        // Newest first, so an arrangement found in several files is dated by
        // the latest one that still had it.
        const QFileInfoList files =
            dir.entryInfoList({QStringLiteral("*.ldb"), QStringLiteral("*.log")}, QDir::Files, QDir::Time);
        for (const QFileInfo &info : files) {
            if (info.size() > MaxFileBytes)
                continue;
            QFile file(info.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QByteArray bytes = file.readAll();

            const Take take = [&](const QByteArray &key, const QByteArray &value) {
                if (!key.contains(SettingsKey))
                    return;
                QList<DiscordFolder> folders;
                if (!foldersFromValue(value, &folders))
                    return;
                const QString print = fingerprint(folders);
                if (seen.contains(print))
                    return;
                seen.insert(print);
                FolderSnapshot snapshot;
                snapshot.app = QString::fromLatin1(app.name);
                snapshot.savedAt = info.lastModified();
                snapshot.folders = folders;
                found.append(snapshot);
            };
            if (info.suffix() == QLatin1String("ldb"))
                readTable(bytes, take);
            else
                readLog(bytes, take);
        }
    }

    std::stable_sort(found.begin(), found.end(), [](const FolderSnapshot &a, const FolderSnapshot &b) {
        return a.savedAt > b.savedAt;
    });
    wlog(QStringLiteral("rail"), QStringLiteral("the Discord app's cache holds %1 copies of the server folders")
                                     .arg(found.size()));
    return found;
}

} // namespace DiscordAppCache
