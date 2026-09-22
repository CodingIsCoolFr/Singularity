#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

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
