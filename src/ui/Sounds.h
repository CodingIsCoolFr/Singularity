#pragma once

#include <QAudioDevice>
#include <QByteArray>
#include <QDate>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QSoundEffect;

// Discord's own sounds, chosen and played the way Discord chooses and plays
// them (worked out from its web client, October 2026).
//
// Sound packs. Each pack replaces some sounds and leaves the rest to Classic:
// Retro, Bop, Ducky, Lofi and ASMR only change the message sound, Discodo
// makes it "discodo", and Halloween and Winter Holiday change the calls,
// joins, leaves, mutes, deafens and the disconnect.
//
// Holidays. Discord ships each holiday as a start time, an end time and a
// pack, and moves anybody on Classic onto that pack while it runs, back
// again after. This does the same, by the calendar: Halloween from 7
// October to 2 November (Discord's own dates for 2026), Winter Holiday
// from 1 December to 6 January.
//
// Easter eggs. On Classic, one incoming call in a thousand rings with
// "call_ringing_beat" or "call_ringing_snow_halation" instead, chosen at
// random - Discord's own roll of 500 on a 1 to 1000. Clicking Home sixteen
// times quickly switches Discodo on or off, as it does in Discord.
//
// Where they come from. The files are Discord's, so they are fetched from
// Discord as its own client fetches them - discord.com/assets/<name>.mp3 -
// the first time each is needed, and kept. None are shipped in the
// installer. The names are the ones Discord used in October 2026; if Discord
// replaces one, the new name is read from its web client once and kept, so
// nothing here needs a new version of Singularity to keep working.
//
// Each file is turned once into a plain WAV and played with QSoundEffect,
// which is made for short sounds: quick to start, no player behind it.
class Sounds : public QObject
{
    Q_OBJECT

public:
    static Sounds &instance();

    // One sound, once, at Discord's volume for it. Nothing if sounds are off,
    // that one is switched off, or Singularity has been asked to stay quiet.
    void play(const QString &name);

    // A ring that repeats until stopped: "call_ringing" or "call_calling".
    void startLoop(const QString &name);
    void stopLoop(const QString &name);

    // The pack sounds come from now: a holiday's while it lasts, if you are
    // on Classic and holidays are on; otherwise the one you picked.
    QString activePack() const;

    // "halloween" or "winter_holiday" on the days they run, empty otherwise.
    static QString holidayOn(const QDate &day);

    // Discodo, from clicking Home sixteen times. Returns the new state.
    bool toggleDiscodo();

    // Fetches every sound the active pack uses, so the first one played is
    // not late. Cheap: they are a few kilobytes each, and kept.
    void prefetch();

    struct Pack
    {
        const char *id;
        const char *label;
    };
    static QList<Pack> packs();

    // The sounds the settings page lets you switch off, Discord's names first.
    struct Switch
    {
        const char *name;
        const char *label;
    };
    static QList<Switch> switches();

private:
    Sounds();

    // The file a sound comes from in a pack: "user_join" in Halloween is
    // "halloween_user_join". Falls back to Classic, as Discord does.
    QString resolve(const QString &name, const QString &pack) const;

    // Calls `ready` with the WAV once it exists, fetching and decoding it
    // first if it has to. Never called if it cannot be had.
    void whenReady(const QString &file, std::function<void(const QString &wav)> ready);
    void download(const QString &file);
    void decode(const QString &file, const QString &mp3);
    void finish(const QString &file, const QString &wav);
    void fail(const QString &file, const QString &why);

    // Reads the current names out of Discord's web client, once per run,
    // when a file has gone missing.
    void discover();

    QSoundEffect *effectFor(const QString &wav);
    void applyDevice(QSoundEffect *effect);
    float volume() const;
    bool allowed(const QString &name) const;
    QString folder() const;

    QNetworkAccessManager m_network;
    QHash<QString, QString> m_assets;               // sound -> Discord's file name
    QHash<QString, QSoundEffect *> m_effects;        // wav path -> its player
    QHash<QString, QList<std::function<void(const QString &)>>> m_waiting;
    QSet<QString> m_failed;
    QStringList m_afterDiscovery;                    // files to try again once names are known
    bool m_discovering = false;
    bool m_discovered = false;

    QHash<QString, QSoundEffect *> m_loops;          // "call_ringing" -> what is ringing

    QByteArray m_deviceId;
    QAudioDevice m_device;

    // The same sound twice within a moment is one sound: five people joining
    // at once is a join, not five.
    QHash<QString, qint64> m_lastPlayed;
};
