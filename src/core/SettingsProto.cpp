#include "core/SettingsProto.h"

#include <QtNumeric>

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
