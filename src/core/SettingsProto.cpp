#include "core/SettingsProto.h"

#include <QJsonArray>
#include <QJsonObject>

#include <QStringList>
#include <QtNumeric>

#include <climits>
#include <cstring>

namespace {

bool readVarint(const uchar *data, int size, int &index, quint64 &out)
{
    out = 0;
    int shift = 0;
    while (index < size && shift <= 63) {
        const quint8 byte = data[index++];
        out |= quint64(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
            return true;
        shift += 7;
    }
    return false;
}

bool skipField(const uchar *data, int size, int &index, int wire)
{
    quint64 length = 0;
    switch (wire) {
    case 0:
        return readVarint(data, size, index, length);
    case 1:
        if (index + 8 > size)
            return false;
        index += 8;
        return true;
    case 2:
        if (!readVarint(data, size, index, length) || length > quint64(size - index))
            return false;
        index += int(length);
        return true;
    case 5:
        if (index + 4 > size)
            return false;
        index += 4;
        return true;
    default:
        return false;
    }
}

bool parseContext(const uchar *data, int size, UserAudioLevel *level)
{
    int index = 0;
    bool sawVolume = false;
    while (index < size) {
        quint64 tag = 0;
        if (!readVarint(data, size, index, tag))
            return false;
        const int field = int(tag >> 3);
        const int wire = int(tag & 7);
        if (field == 1 && wire == 0) {
            quint64 value = 0;
            if (!readVarint(data, size, index, value))
                return false;
            level->muted = value != 0;
        } else if (field == 2 && wire == 5) {
            if (index + 4 > size)
                return false;
            float volume = 0.f;
            std::memcpy(&volume, data + index, 4);
            index += 4;
            if (qIsNaN(volume))
                volume = 0.f;
            level->volume = qBound(0, qRound(volume), 200);
            sawVolume = true;
        } else if (!skipField(data, size, index, wire)) {
            return false;
        }
    }
    if (!sawVolume)
        level->volume = 0;
    return true;
}

bool parseMapEntry(const uchar *data, int size, QString *userId, UserAudioLevel *level)
{
    int index = 0;
    bool sawKey = false;
    bool sawValue = false;
    while (index < size) {
        quint64 tag = 0;
        if (!readVarint(data, size, index, tag))
            return false;
        const int field = int(tag >> 3);
        const int wire = int(tag & 7);
        if (field == 1 && (wire == 0 || wire == 1)) {
            quint64 key = 0;
            if (wire == 0) {
                if (!readVarint(data, size, index, key))
                    return false;
            } else {
                if (index + 8 > size)
                    return false;
                std::memcpy(&key, data + index, 8);
                index += 8;
            }
            if (key != 0) {
                *userId = QString::number(key);
                sawKey = true;
            }
        } else if (field == 2 && wire == 2) {
            quint64 length = 0;
            if (!readVarint(data, size, index, length) || length > quint64(size - index))
                return false;
            if (!parseContext(data + index, int(length), level))
                return false;
            index += int(length);
            sawValue = true;
        } else if (!skipField(data, size, index, wire)) {
            return false;
        }
    }
    return sawKey && sawValue;
}

bool parseAudioSettings(const uchar *data, int size, QHash<QString, UserAudioLevel> *levels)
{
    int index = 0;
    while (index < size) {
        quint64 tag = 0;
        if (!readVarint(data, size, index, tag))
            return false;
        const int field = int(tag >> 3);
        const int wire = int(tag & 7);
        if (field == 1 && wire == 2) {
            quint64 length = 0;
            if (!readVarint(data, size, index, length) || length > quint64(size - index))
                return false;
            QString userId;
            UserAudioLevel level;
            if (!parseMapEntry(data + index, int(length), &userId, &level))
                return false;
            index += int(length);
            levels->insert(userId, level);
        } else if (!skipField(data, size, index, wire)) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace {

// The body of a length-delimited field with this number, or false.
bool findMessage(const uchar *data, int size, int wanted, const uchar **body, int *bodySize)
{
    int index = 0;
    while (index < size) {
        quint64 tag = 0;
        if (!readVarint(data, size, index, tag))
            return false;
        const int field = int(tag >> 3);
        const int wire = int(tag & 7);
        if (field == wanted && wire == 2) {
            quint64 length = 0;
            if (!readVarint(data, size, index, length) || length > quint64(size - index))
                return false;
            *body = data + index;
            *bodySize = int(length);
            return true;
        }
        if (!skipField(data, size, index, wire))
            return false;
    }
    return false;
}

} // namespace

namespace {

// The value inside a wrapper message (Int64Value, UInt64Value): field 1, a
// varint. An absent field 1 is zero, which is how protobuf writes a zero.
bool readWrappedVarint(const uchar *data, int size, quint64 *out)
{
    *out = 0;
    int index = 0;
    while (index < size) {
        quint64 tag = 0;
        if (!readVarint(data, size, index, tag))
            return false;
        if ((tag >> 3) == 1 && (tag & 7) == 0)
            return readVarint(data, size, index, *out);
        if (!skipField(data, size, index, int(tag & 7)))
            return false;
    }
    return true;
}

bool parseFolder(const uchar *data, int size, DiscordFolder *folder)
{
    int index = 0;
    while (index < size) {
        quint64 tag = 0;
        if (!readVarint(data, size, index, tag))
            return false;
        const int field = int(tag >> 3);
        const int wire = int(tag & 7);

        if (field == 1 && wire == 1) {
            // One id on its own.
            if (index + 8 > size)
                return false;
            quint64 id = 0;
            std::memcpy(&id, data + index, 8);
            index += 8;
            folder->guildIds.append(QString::number(id));
        } else if (field == 1 && wire == 2) {
            // Several ids packed together, eight bytes each - the proto3
            // default for a repeated number. Read either way.
            quint64 length = 0;
            if (!readVarint(data, size, index, length) || length > quint64(size - index) || length % 8)
                return false;
            for (quint64 at = 0; at < length; at += 8) {
                quint64 id = 0;
                std::memcpy(&id, data + index + at, 8);
                folder->guildIds.append(QString::number(id));
            }
            index += int(length);
        } else if ((field == 2 || field == 3 || field == 4) && wire == 2) {
            quint64 length = 0;
            if (!readVarint(data, size, index, length) || length > quint64(size - index))
                return false;
            const uchar *body = data + index;
            const int bodySize = int(length);
            index += bodySize;

            if (field == 2) {
                quint64 id = 0;
                if (!readWrappedVarint(body, bodySize, &id))
                    return false;
                folder->id = qint64(id);
            } else if (field == 4) {
                quint64 color = 0;
                if (!readWrappedVarint(body, bodySize, &color))
                    return false;
                folder->hasColor = true;
                folder->color = color;
            } else {
                // StringValue: field 1, the text.
                const uchar *text = nullptr;
                int textSize = 0;
                if (findMessage(body, bodySize, 1, &text, &textSize))
                    folder->name = QString::fromUtf8(reinterpret_cast<const char *>(text), textSize);
            }
        } else if (!skipField(data, size, index, wire)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool guildFoldersFromProto(const QByteArray &bytes, QList<DiscordFolder> *folders, bool *present)
{
    folders->clear();
    *present = false;
    if (bytes.isEmpty())
        return true;

    const auto *data = reinterpret_cast<const uchar *>(bytes.constData());
    const uchar *body = nullptr;
    int bodySize = 0;
    if (!findMessage(data, bytes.size(), 14, &body, &bodySize))
        return true; // not in this blob
    *present = true;

    int index = 0;
    while (index < bodySize) {
        quint64 tag = 0;
        if (!readVarint(body, bodySize, index, tag))
            return false;
        const int field = int(tag >> 3);
        const int wire = int(tag & 7);
        if (field == 1 && wire == 2) {
            quint64 length = 0;
            if (!readVarint(body, bodySize, index, length) || length > quint64(bodySize - index))
                return false;
            DiscordFolder folder;
            if (!parseFolder(body + index, int(length), &folder))
                return false;
            index += int(length);
            if (!folder.guildIds.isEmpty())
                folders->append(folder);
        } else if (!skipField(body, bodySize, index, wire)) {
            return false;
        }
    }
    return true;
}

QString statusFromProto(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return {};

    const auto *data = reinterpret_cast<const uchar *>(bytes.constData());
    const uchar *statusSettings = nullptr;
    int statusSize = 0;
    if (!findMessage(data, bytes.size(), 11, &statusSettings, &statusSize))
        return {};

    const uchar *wrapper = nullptr;
    int wrapperSize = 0;
    if (!findMessage(statusSettings, statusSize, 1, &wrapper, &wrapperSize))
        return {};

    const uchar *text = nullptr;
    int textSize = 0;
    if (!findMessage(wrapper, wrapperSize, 1, &text, &textSize))
        return {};

    const QString status = QString::fromUtf8(reinterpret_cast<const char *>(text), textSize);
    static const QStringList known{QStringLiteral("online"), QStringLiteral("idle"),
                                   QStringLiteral("dnd"), QStringLiteral("invisible")};
    return known.contains(status) ? status : QString();
}

bool audioContextFromProto(const QByteArray &bytes, QHash<QString, UserAudioLevel> *levels, bool *present)
{
    levels->clear();
    *present = false;
    if (bytes.isEmpty())
        return true;

    const auto *data = reinterpret_cast<const uchar *>(bytes.constData());
    const int size = bytes.size();
    int index = 0;
    while (index < size) {
        quint64 tag = 0;
        if (!readVarint(data, size, index, tag))
            return false;
        const int field = int(tag >> 3);
        const int wire = int(tag & 7);
        if (field == 16 && wire == 2) {
            quint64 length = 0;
            if (!readVarint(data, size, index, length) || length > quint64(size - index))
                return false;
            *present = true;
            if (!parseAudioSettings(data + index, int(length), levels))
                return false;
            index += int(length);
        } else if (!skipField(data, size, index, wire)) {
            return false;
        }
    }
    return true;
}

QHash<QString, int> commandFrecencyFromProto(const QByteArray &bytes)
{
    QHash<QString, int> out;
    const auto *data = reinterpret_cast<const uchar *>(bytes.constData());
    const uchar *frecency = nullptr;
    int frecencySize = 0;
    if (bytes.isEmpty() || !findMessage(data, bytes.size(), 7, &frecency, &frecencySize))
        return out;

    // ApplicationCommandFrecency { map<string, FrecencyItem> application_commands = 1; }
    int index = 0;
    while (index < frecencySize) {
        quint64 tag = 0;
        if (!readVarint(frecency, frecencySize, index, tag))
            return out;
        const int wire = int(tag & 7);
        if ((tag >> 3) != 1 || wire != 2) {
            if (!skipField(frecency, frecencySize, index, wire))
                return out;
            continue;
        }
        quint64 length = 0;
        if (!readVarint(frecency, frecencySize, index, length) || length > quint64(frecencySize - index))
            return out;
        const uchar *entry = frecency + index;
        const int entrySize = int(length);
        index += entrySize;

        QString key;
        quint64 totalUses = 0;
        qint64 score = 0;
        qint64 recency = 0;
        int at = 0;
        while (at < entrySize) {
            quint64 entryTag = 0;
            if (!readVarint(entry, entrySize, at, entryTag))
                break;
            const int field = int(entryTag >> 3);
            const int entryWire = int(entryTag & 7);
            if (field == 1 && entryWire == 2) {
                quint64 keyLength = 0;
                if (!readVarint(entry, entrySize, at, keyLength) || keyLength > quint64(entrySize - at))
                    break;
                key = QString::fromUtf8(reinterpret_cast<const char *>(entry + at), int(keyLength));
                at += int(keyLength);
            } else if (field == 2 && entryWire == 2) {
                // FrecencyItem { uint32 total_uses = 1; repeated uint64 recent_uses = 2;
                //                int32 frecency = 3; int32 score = 4; }
                quint64 itemLength = 0;
                if (!readVarint(entry, entrySize, at, itemLength) || itemLength > quint64(entrySize - at))
                    break;
                const uchar *item = entry + at;
                const int itemSize = int(itemLength);
                at += itemSize;
                int pos = 0;
                while (pos < itemSize) {
                    quint64 itemTag = 0;
                    if (!readVarint(item, itemSize, pos, itemTag))
                        break;
                    const int itemField = int(itemTag >> 3);
                    const int itemWire = int(itemTag & 7);
                    quint64 value = 0;
                    if (itemWire == 0 && readVarint(item, itemSize, pos, value)) {
                        if (itemField == 1)
                            totalUses = value;
                        else if (itemField == 3)
                            recency = qint64(qint32(quint32(value)));
                        else if (itemField == 4)
                            score = qint64(qint32(quint32(value)));
                    } else if (itemWire != 0 && !skipField(item, itemSize, pos, itemWire)) {
                        break;
                    }
                }
            } else if (!skipField(entry, entrySize, at, entryWire)) {
                break;
            }
        }
        if (key.isEmpty())
            continue;
        int rank = int(qBound(qint64(0), score, qint64(INT_MAX)));
        if (rank == 0)
            rank = int(qBound(qint64(0), recency, qint64(INT_MAX)));
        if (rank == 0)
            rank = int(qMin<quint64>(totalUses, INT_MAX));
        if (rank > 0)
            out.insert(key, rank);
    }
    return out;
}

namespace {

void putVarint(QByteArray &out, quint64 value)
{
    while (value >= 0x80) {
        out.append(char((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.append(char(value));
}

void putLengthDelimited(QByteArray &out, int field, const QByteArray &body)
{
    putVarint(out, (quint64(field) << 3) | 2);
    putVarint(out, quint64(body.size()));
    out.append(body);
}

void putFixed64(QByteArray &out, int field, quint64 value)
{
    putVarint(out, (quint64(field) << 3) | 1);
    for (int i = 0; i < 8; ++i)
        out.append(char((value >> (8 * i)) & 0xFF));
}

// google.protobuf.Int64Value / UInt64Value: { value = 1 (varint) }
QByteArray wrappedVarint(quint64 value)
{
    QByteArray inner;
    putVarint(inner, (1u << 3) | 0);
    putVarint(inner, value);
    return inner;
}

} // namespace

QByteArray guildFoldersToProto(const QJsonArray &folders)
{
    QByteArray guildFolders;
    for (const QJsonValue &value : folders) {
        const QJsonObject folder = value.toObject();
        QByteArray one;
        for (const QJsonValue &guildId : folder.value(QStringLiteral("guild_ids")).toArray()) {
            bool ok = false;
            const quint64 id = guildId.toString().toULongLong(&ok);
            if (ok)
                putFixed64(one, 1, id);
        }
        if (one.isEmpty())
            continue;
        const QJsonValue id = folder.value(QStringLiteral("id"));
        if (!id.isNull() && !id.isUndefined())
            putLengthDelimited(one, 2, wrappedVarint(quint64(qint64(id.toDouble()))));
        const QString name = folder.value(QStringLiteral("name")).toString();
        if (!name.isEmpty()) {
            QByteArray text;
            putLengthDelimited(text, 1, name.toUtf8());   // StringValue { value = 1 }
            putLengthDelimited(one, 3, text);
        }
        const QJsonValue color = folder.value(QStringLiteral("color"));
        if (!color.isNull() && !color.isUndefined())
            putLengthDelimited(one, 4, wrappedVarint(quint64(color.toDouble())));
        putLengthDelimited(guildFolders, 1, one);
    }

    QByteArray settings;
    putLengthDelimited(settings, 14, guildFolders);
    return settings;
}

