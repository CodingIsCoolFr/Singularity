#include "ui/Sounds.h"

#include "core/AppConfig.h"
#include "core/DiscordIdentity.h"
#include "core/Logger.h"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMediaDevices>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSoundEffect>
#include <QStandardPaths>
#include <QtEndian>

#include <memory>
#include <utility>

namespace {

// Discord plays its sounds at 40 per cent; Sounds' own volume scales that.
constexpr float DiscordVolume = 0.4f;

// A sound fetched this long after it was asked for belongs to a moment that
// has passed. A loop is different: a ring is still ringing.
constexpr qint64 StaleAfterMs = 2500;

// Discord's file for each sound, from its web client in October 2026.
// discover() replaces any that Discord has since changed.
const QHash<QString, QString> &builtInAssets()
{
    static const QHash<QString, QString> assets{
        {QStringLiteral("message1"), QStringLiteral("3ed22d14f3c30bc4.mp3")},
        {QStringLiteral("deafen"), QStringLiteral("529ff198eac567af.mp3")},
        {QStringLiteral("undeafen"), QStringLiteral("b150f03c89944403.mp3")},
        {QStringLiteral("mute"), QStringLiteral("2d3b4ba32c34c862.mp3")},
        {QStringLiteral("unmute"), QStringLiteral("e74c4a06134a20e4.mp3")},
        {QStringLiteral("disconnect"), QStringLiteral("dd0911025fe00b2e.mp3")},
        {QStringLiteral("user_join"), QStringLiteral("b135ff6c8e091b43.mp3")},
        {QStringLiteral("user_leave"), QStringLiteral("7b9a183742515fc2.mp3")},
        {QStringLiteral("user_moved"), QStringLiteral("4003230e4086b680.mp3")},
        {QStringLiteral("call_calling"), QStringLiteral("5146737af413d88e.mp3")},
        {QStringLiteral("call_ringing"), QStringLiteral("c2a7111bb44b8da0.mp3")},
        {QStringLiteral("stream_started"), QStringLiteral("abe52a3c92953edb.mp3")},
        {QStringLiteral("stream_ended"), QStringLiteral("cbff07b5a83e0321.mp3")},
        {QStringLiteral("stream_user_joined"), QStringLiteral("3d2af2764581bb2e.mp3")},
        {QStringLiteral("stream_user_left"), QStringLiteral("24ae9dc0184bd570.mp3")},
        {QStringLiteral("camera_on"), QStringLiteral("855607d0932ea396.mp3")},
        {QStringLiteral("camera_off"), QStringLiteral("b1dd204729fe78b4.mp3")},
        {QStringLiteral("discodo"), QStringLiteral("cb85be48267eb1b4.mp3")},
        {QStringLiteral("bit_message1"), QStringLiteral("795109c70fe6ea7b.mp3")},
        {QStringLiteral("bop_message1"), QStringLiteral("909464bf55b13935.mp3")},
        {QStringLiteral("ducky_message1"), QStringLiteral("7d8709c9f90d5d1e.mp3")},
        {QStringLiteral("lofi_message1"), QStringLiteral("5aa5ffdc82cb3c74.mp3")},
        {QStringLiteral("asmr_message1"), QStringLiteral("7abbc45d8e7b179a.mp3")},
        {QStringLiteral("call_ringing_beat"), QStringLiteral("574220f72e718dad.mp3")},
        {QStringLiteral("call_ringing_snow_halation"), QStringLiteral("82954088613583bc.mp3")},
        {QStringLiteral("halloween_call_calling"), QStringLiteral("ef24e229a3413c8a.mp3")},
        {QStringLiteral("halloween_call_ringing"), QStringLiteral("7a287e2f88531f25.mp3")},
        {QStringLiteral("halloween_message1"), QStringLiteral("c269de500ca3cb7b.mp3")},
        {QStringLiteral("halloween_deafen"), QStringLiteral("514663b657bc4c7d.mp3")},
        {QStringLiteral("halloween_undeafen"), QStringLiteral("4dfa756a71484219.mp3")},
        {QStringLiteral("halloween_mute"), QStringLiteral("d5e69d2c2aac0a27.mp3")},
        {QStringLiteral("halloween_unmute"), QStringLiteral("6f0d538ed066a1a6.mp3")},
        {QStringLiteral("halloween_disconnect"), QStringLiteral("9b8ef0a59d731dd6.mp3")},
        {QStringLiteral("halloween_user_join"), QStringLiteral("00b48ea9fc787786.mp3")},
        {QStringLiteral("halloween_user_leave"), QStringLiteral("c9b6b9d1ef992f1b.mp3")},
        {QStringLiteral("winter_call_calling"), QStringLiteral("a07935fe464c324c.mp3")},
        {QStringLiteral("winter_call_ringing"), QStringLiteral("daf6c21d9f92979a.mp3")},
        {QStringLiteral("winter_deafen"), QStringLiteral("70fe5e248588bb13.mp3")},
        {QStringLiteral("winter_undeafen"), QStringLiteral("5b518eba0a73ec4a.mp3")},
        {QStringLiteral("winter_mute"), QStringLiteral("81034fa14558e50f.mp3")},
        {QStringLiteral("winter_unmute"), QStringLiteral("b2c49178b1a4e3d9.mp3")},
        {QStringLiteral("winter_disconnect"), QStringLiteral("600991ad4560f288.mp3")},
        {QStringLiteral("winter_user_join"), QStringLiteral("b766ee70384a56ce.mp3")},
        {QStringLiteral("winter_user_leave"), QStringLiteral("cf2c36fde50eb05d.mp3")},
    };
    return assets;
}

// What each pack changes, and the prefix its files carry. Anything a pack
// does not list comes from Classic - Discord's own rule.
struct PackRule
{
    QString prefix;
    QSet<QString> sounds;
};

const QHash<QString, PackRule> &packRules()
{
    static const QSet<QString> message{QStringLiteral("message1")};
    static const QHash<QString, PackRule> rules{
        {QStringLiteral("retro"), {QStringLiteral("bit_"), message}},
        {QStringLiteral("bop"), {QStringLiteral("bop_"), message}},
        {QStringLiteral("ducky"), {QStringLiteral("ducky_"), message}},
        {QStringLiteral("lofi"), {QStringLiteral("lofi_"), message}},
        {QStringLiteral("asmr"), {QStringLiteral("asmr_"), message}},
        {QStringLiteral("halloween"),
         {QStringLiteral("halloween_"),
          {QStringLiteral("call_calling"), QStringLiteral("call_ringing"), QStringLiteral("message1"),
           QStringLiteral("deafen"), QStringLiteral("undeafen"), QStringLiteral("mute"), QStringLiteral("unmute"),
           QStringLiteral("disconnect"), QStringLiteral("user_join"), QStringLiteral("user_leave")}}},
        {QStringLiteral("winter_holiday"),
         {QStringLiteral("winter_"),
          {QStringLiteral("call_calling"), QStringLiteral("call_ringing"), QStringLiteral("deafen"),
           QStringLiteral("undeafen"), QStringLiteral("mute"), QStringLiteral("unmute"),
           QStringLiteral("disconnect"), QStringLiteral("user_join"), QStringLiteral("user_leave")}}},
    };
    return rules;
}

// A plain 16 bit PCM WAV, which is what QSoundEffect reads.
QByteArray wavFile(const QByteArray &pcm, int sampleRate, int channels)
{
    QByteArray out;
    out.reserve(44 + pcm.size());
    const auto put16 = [&out](quint16 value) {
        char bytes[2];
        qToLittleEndian(value, bytes);
        out.append(bytes, 2);
    };
    const auto put32 = [&out](quint32 value) {
        char bytes[4];
        qToLittleEndian(value, bytes);
        out.append(bytes, 4);
    };
    out.append("RIFF", 4);
    put32(quint32(36 + pcm.size()));
    out.append("WAVE", 4);
    out.append("fmt ", 4);
    put32(16);
    put16(1);                                          // PCM
    put16(quint16(channels));
    put32(quint32(sampleRate));
    put32(quint32(sampleRate * channels * 2));         // bytes a second
    put16(quint16(channels * 2));                      // bytes a frame
    put16(16);
    out.append("data", 4);
    put32(quint32(pcm.size()));
    out.append(pcm);
    return out;
}

// One decoded buffer as 16 bit samples, whatever the decoder handed over.
QByteArray asInt16(const QAudioBuffer &buffer)
{
    const QAudioFormat format = buffer.format();
    const qsizetype samples = buffer.sampleCount();
    QByteArray out(samples * 2, Qt::Uninitialized);
    auto *to = reinterpret_cast<qint16 *>(out.data());
    switch (format.sampleFormat()) {
    case QAudioFormat::Int16:
        return QByteArray(buffer.constData<char>(), samples * 2);
    case QAudioFormat::Float: {
        const float *from = buffer.constData<float>();
        for (qsizetype i = 0; i < samples; ++i)
            to[i] = qint16(qBound(-32768.0f, from[i] * 32767.0f, 32767.0f));
        return out;
    }
    case QAudioFormat::Int32: {
        const qint32 *from = buffer.constData<qint32>();
        for (qsizetype i = 0; i < samples; ++i)
            to[i] = qint16(from[i] >> 16);
        return out;
    }
    case QAudioFormat::UInt8: {
        const quint8 *from = buffer.constData<quint8>();
        for (qsizetype i = 0; i < samples; ++i)
            to[i] = qint16((int(from[i]) - 128) << 8);
        return out;
    }
    default:
        return {};
    }
}

} // namespace

Sounds &Sounds::instance()
{
    // Never destroyed as a static would be, after the audio system has gone:
    // its players are let go as the program starts to quit instead.
    static Sounds *sounds = new Sounds;
    return *sounds;
}

Sounds::Sounds()
{
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        m_loops.clear();
        qDeleteAll(m_effects);
        m_effects.clear();
    });

    m_assets = builtInAssets();
    // Names found in Discord's web client on an earlier run, newer than the
    // ones above.
    const QVariantMap found = AppConfig::instance().value(QStringLiteral("sounds/assets")).toMap();
    for (auto it = found.cbegin(); it != found.cend(); ++it) {
        if (m_assets.contains(it.key()))
            m_assets.insert(it.key(), it.value().toString());
    }
}

QList<Sounds::Pack> Sounds::packs()
{
    return {
        {"classic", "Classic"},     {"retro", "Retro"},   {"bop", "Bop"},
        {"ducky", "Ducky"},         {"lofi", "Lofi"},     {"asmr", "ASMR"},
        {"discodo", "Discodo"},     {"halloween", "Halloween"},
        {"winter_holiday", "Winter Holiday"},
    };
}

QList<Sounds::Switch> Sounds::switches()
{
    return {
        {"message1", "Message"},
        {"deafen", "Deafen"},
        {"undeafen", "Undeafen"},
        {"mute", "Mute"},
        {"unmute", "Unmute"},
        {"disconnect", "Voice disconnected"},
        {"user_join", "User join"},
        {"user_leave", "User leave"},
        {"user_moved", "User moved"},
        {"call_calling", "Outgoing ring"},
        {"call_ringing", "Incoming ring"},
        {"stream_started", "Stream started"},
        {"stream_ended", "Stream ended"},
        {"stream_user_joined", "Viewer join"},
        {"stream_user_left", "Viewer leave"},
        {"camera_on", "Camera on"},
        {"camera_off", "Camera off"},
    };
}

QString Sounds::holidayOn(const QDate &day)
{
    const int month = day.month();
    const int date = day.day();
    if ((month == 10 && date >= 7) || (month == 11 && date <= 2))
        return QStringLiteral("halloween");
    if (month == 12 || (month == 1 && date <= 6))
        return QStringLiteral("winter_holiday");
    return {};
}

QString Sounds::activePack() const
{
    AppConfig &config = AppConfig::instance();
    QString chosen = config.value(QStringLiteral("sounds/pack"), QStringLiteral("classic")).toString();
    if (chosen != QLatin1String("classic") && !packRules().contains(chosen) && chosen != QLatin1String("discodo"))
        chosen = QStringLiteral("classic");

    // Discord moves only people on Classic onto a holiday's pack; anybody who
    // picked another keeps it.
    if (chosen == QLatin1String("classic") && config.value(QStringLiteral("sounds/holidays"), true).toBool()) {
        const QString holiday = holidayOn(QDate::currentDate());
        if (!holiday.isEmpty())
            return holiday;
    }
    return chosen;
}

QString Sounds::resolve(const QString &name, const QString &pack) const
{
    // Discodo, by pack or by the Home easter egg, is a message sound.
    if (name == QLatin1String("message1")
        && (pack == QLatin1String("discodo")
            || AppConfig::instance().value(QStringLiteral("sounds/discodo"), false).toBool()))
        return QStringLiteral("discodo");

    const auto rule = packRules().constFind(pack);
    if (rule != packRules().constEnd() && rule->sounds.contains(name))
        return rule->prefix + name;
    return name;
}

bool Sounds::toggleDiscodo()
{
    AppConfig &config = AppConfig::instance();
    const bool on = !config.value(QStringLiteral("sounds/discodo"), false).toBool();
    config.setValue(QStringLiteral("sounds/discodo"), on);
    wlog(QStringLiteral("sounds"), QStringLiteral("Discodo %1").arg(on ? QStringLiteral("on") : QStringLiteral("off")));
    return on;
}

bool Sounds::allowed(const QString &name) const
{
    AppConfig &config = AppConfig::instance();
    if (!config.value(QStringLiteral("sounds/enabled"), true).toBool())
        return false;
    return !config.value(QStringLiteral("sounds/off/%1").arg(name), false).toBool();
}

float Sounds::volume() const
{
    const int percent = qBound(0, AppConfig::instance().value(QStringLiteral("sounds/volume"), 100).toInt(), 100);
    return DiscordVolume * float(percent) / 100.0f;
}

QString Sounds::folder() const
{
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/sounds");
    QDir().mkpath(path);
    return path;
}

void Sounds::applyDevice(QSoundEffect *effect)
{
    // The same speakers as the call, as Discord does.
    const QByteArray wanted = AppConfig::instance().value(QStringLiteral("voice/outputDevice")).toByteArray();
    if (wanted != m_deviceId || m_device.isNull()) {
        m_deviceId = wanted;
        m_device = QMediaDevices::defaultAudioOutput();
        for (const QAudioDevice &device : QMediaDevices::audioOutputs()) {
            if (!wanted.isEmpty() && device.id() == wanted) {
                m_device = device;
                break;
            }
        }
        for (QSoundEffect *each : std::as_const(m_effects))
            each->setAudioDevice(m_device);
    }
    if (effect)
        effect->setAudioDevice(m_device);
}

QSoundEffect *Sounds::effectFor(const QString &wav)
{
    QSoundEffect *effect = m_effects.value(wav);
    if (!effect) {
        effect = new QSoundEffect(this);
        effect->setSource(QUrl::fromLocalFile(wav));
        m_effects.insert(wav, effect);
    }
    applyDevice(effect);
    effect->setVolume(volume());
    return effect;
}

void Sounds::play(const QString &name)
{
    if (!allowed(name))
        return;
    const QString file = resolve(name, activePack());

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastPlayed.value(file, 0) < 150)
        return;
    m_lastPlayed.insert(file, now);

    QElapsedTimer asked;
    asked.start();
    whenReady(file, [this, asked](const QString &wav) {
        if (asked.elapsed() > StaleAfterMs)
            return;
        QSoundEffect *effect = effectFor(wav);
        effect->setLoopCount(1);
        if (effect->isLoaded()) {
            effect->play();
            return;
        }
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = connect(effect, &QSoundEffect::loadedChanged, this, [effect, connection]() {
            if (!effect->isLoaded())
                return;
            QObject::disconnect(*connection);
            effect->play();
        });
    });
}

void Sounds::startLoop(const QString &name)
{
    if (m_loops.contains(name) || !allowed(name))
        return;

    const QString pack = activePack();
    QString file = resolve(name, pack);

    // Discord's easter egg, as its client rolls it: on Classic, a 500 out of
    // 1 to 1000 rings with one of two remixes instead.
    if (name == QLatin1String("call_ringing") && pack == QLatin1String("classic")
        && QRandomGenerator::global()->bounded(1, 1001) == 500) {
        file = QRandomGenerator::global()->bounded(2) == 0 ? QStringLiteral("call_ringing_beat")
                                                           : QStringLiteral("call_ringing_snow_halation");
        wlog(QStringLiteral("sounds"), QStringLiteral("the one-in-a-thousand ring: %1").arg(file));
    }

    m_loops.insert(name, nullptr);   // asked for; filled in once the sound is here
    whenReady(file, [this, name](const QString &wav) {
        if (!m_loops.contains(name) || m_loops.value(name))
            return;   // stopped meanwhile, or already ringing
        QSoundEffect *effect = effectFor(wav);
        effect->setLoopCount(QSoundEffect::Infinite);
        m_loops.insert(name, effect);
        if (effect->isLoaded()) {
            effect->play();
            return;
        }
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = connect(effect, &QSoundEffect::loadedChanged, this, [this, effect, name, connection]() {
            if (!effect->isLoaded())
                return;
            QObject::disconnect(*connection);
            if (m_loops.value(name) == effect)
                effect->play();
        });
    });
}

void Sounds::stopLoop(const QString &name)
{
    if (QSoundEffect *effect = m_loops.take(name)) {
        effect->stop();
        effect->setLoopCount(1);
    }
}

void Sounds::prefetch()
{
    const QString pack = activePack();
    for (const Switch &each : switches())
        whenReady(resolve(QLatin1String(each.name), pack), [](const QString &) {});
}

void Sounds::whenReady(const QString &file, std::function<void(const QString &wav)> ready)
{
    const QString asset = m_assets.value(file);
    if (asset.isEmpty() || m_failed.contains(file))
        return;

    const QString wav = folder() + QLatin1Char('/') + QFileInfo(asset).completeBaseName() + QStringLiteral(".wav");
    if (QFileInfo::exists(wav)) {
        ready(wav);
        return;
    }

    auto &queue = m_waiting[file];
    queue.append(std::move(ready));
    if (queue.size() == 1)
        download(file);
}

void Sounds::download(const QString &file)
{
    const QString asset = m_assets.value(file);
    const QString mp3 = folder() + QLatin1Char('/') + asset;
    if (QFileInfo::exists(mp3)) {
        decode(file, mp3);
        return;
    }

    QNetworkRequest request(QUrl(QStringLiteral("https://discord.com/assets/%1").arg(asset)));
    request.setHeader(QNetworkRequest::UserAgentHeader, DiscordIdentity::userAgent());
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, mp3]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (status == 200 && !body.isEmpty()) {
            QSaveFile out(mp3);
            if (out.open(QIODevice::WriteOnly) && out.write(body) == body.size() && out.commit()) {
                decode(file, mp3);
                return;
            }
            fail(file, QStringLiteral("could not write %1").arg(mp3));
            return;
        }
        // Gone from Discord: it has a new name. Learn the new names once,
        // then try again.
        if ((status == 404 || status == 403) && !m_discovered) {
            m_afterDiscovery.append(file);
            discover();
            return;
        }
        fail(file, QStringLiteral("HTTP %1 %2").arg(status).arg(reply->errorString()));
    });
}

void Sounds::decode(const QString &file, const QString &mp3)
{
    auto *decoder = new QAudioDecoder(this);
    QAudioFormat wanted;
    wanted.setSampleRate(48000);
    wanted.setChannelCount(2);
    wanted.setSampleFormat(QAudioFormat::Int16);
    decoder->setAudioFormat(wanted);
    decoder->setSource(QUrl::fromLocalFile(mp3));

    struct Pcm
    {
        QByteArray samples;
        int rate = 0;
        int channels = 0;
    };
    auto pcm = std::make_shared<Pcm>();

    connect(decoder, &QAudioDecoder::bufferReady, this, [decoder, pcm]() {
        const QAudioBuffer buffer = decoder->read();
        if (!buffer.isValid())
            return;
        if (pcm->rate == 0) {
            pcm->rate = buffer.format().sampleRate();
            pcm->channels = buffer.format().channelCount();
        }
        pcm->samples += asInt16(buffer);
    });
    connect(decoder, &QAudioDecoder::finished, this, [this, decoder, pcm, file, mp3]() {
        decoder->deleteLater();
        if (pcm->samples.isEmpty() || pcm->rate <= 0 || pcm->channels <= 0) {
            fail(file, QStringLiteral("nothing came out of %1").arg(QFileInfo(mp3).fileName()));
            return;
        }
        const QString wav = QFileInfo(mp3).absolutePath() + QLatin1Char('/') + QFileInfo(mp3).completeBaseName()
                            + QStringLiteral(".wav");
        QSaveFile out(wav);
        const QByteArray bytes = wavFile(pcm->samples, pcm->rate, pcm->channels);
        if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
            fail(file, QStringLiteral("could not write %1").arg(wav));
            return;
        }
        finish(file, wav);
    });
    connect(decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), this,
            [this, decoder, file](QAudioDecoder::Error) {
                const QString why = decoder->errorString();
                decoder->deleteLater();
                fail(file, QStringLiteral("could not decode: %1").arg(why));
            });
    decoder->start();
}

void Sounds::finish(const QString &file, const QString &wav)
{
    const auto waiting = m_waiting.take(file);
    for (const auto &ready : waiting)
        ready(wav);
}

void Sounds::fail(const QString &file, const QString &why)
{
    m_failed.insert(file);
    m_waiting.remove(file);
    wlog(QStringLiteral("sounds"), QStringLiteral("%1 cannot be played: %2").arg(file, why));
}

void Sounds::discover()
{
    if (m_discovering)
        return;
    m_discovering = true;
    wlog(QStringLiteral("sounds"), QStringLiteral("a sound has moved on Discord's side; reading the new names"));

    const auto giveUp = [this](const QString &why) {
        m_discovering = false;
        m_discovered = true;
        const QStringList files = std::exchange(m_afterDiscovery, {});
        for (const QString &file : files)
            fail(file, why);
    };

    QNetworkRequest page(QUrl(QStringLiteral("https://discord.com/app")));
    page.setHeader(QNetworkRequest::UserAgentHeader, DiscordIdentity::userAgent());
    QNetworkReply *reply = m_network.get(page);
    connect(reply, &QNetworkReply::finished, this, [this, reply, giveUp]() {
        reply->deleteLater();
        static const QRegularExpression scriptName(QStringLiteral(R"(/assets/web\.[0-9a-f]+\.js)"));
        const QRegularExpressionMatch script = scriptName.match(QString::fromUtf8(reply->readAll()));
        if (!script.hasMatch()) {
            giveUp(QStringLiteral("Discord's web client no longer names its main script the way it did"));
            return;
        }

        QNetworkRequest request(QUrl(QStringLiteral("https://discord.com") + script.captured()));
        request.setHeader(QNetworkRequest::UserAgentHeader, DiscordIdentity::userAgent());
        QNetworkReply *code = m_network.get(request);
        connect(code, &QNetworkReply::finished, this, [this, code, giveUp]() {
            code->deleteLater();
            const QString text = QString::fromUtf8(code->readAll());

            // The client's table of sounds - "./user_join.mp3":"135579" - and
            // each entry's module, which names the file:
            //   135579(e,t,n){"use strict";e.exports=n.p+"b135ff6c8e091b43.mp3"}
            static const QRegularExpression entry(QStringLiteral(R"re("\./([a-z0-9_]+)\.mp3":"(\d+)")re"));
            static const QRegularExpression module(
                QStringLiteral(R"re((\d+)\(\w,\w,\w\)\{"use strict";\w\.exports=\w\.p\+"([0-9a-f]+\.mp3)"\})re"));
            QHash<QString, QString> fileOfModule;
            for (auto it = module.globalMatch(text); it.hasNext();) {
                const QRegularExpressionMatch match = it.next();
                fileOfModule.insert(match.captured(1), match.captured(2));
            }
            QVariantMap learned;
            for (auto it = entry.globalMatch(text); it.hasNext();) {
                const QRegularExpressionMatch match = it.next();
                const QString file = fileOfModule.value(match.captured(2));
                if (!file.isEmpty() && m_assets.contains(match.captured(1)))
                    learned.insert(match.captured(1), file);
            }
            if (learned.size() < 10) {
                giveUp(QStringLiteral("Discord's web client no longer lists its sounds the way it did"));
                return;
            }

            for (auto it = learned.cbegin(); it != learned.cend(); ++it)
                m_assets.insert(it.key(), it.value().toString());
            AppConfig::instance().setValue(QStringLiteral("sounds/assets"), learned);
            wlog(QStringLiteral("sounds"), QStringLiteral("learned %1 sound names from Discord").arg(learned.size()));

            m_discovering = false;
            m_discovered = true;
            const QStringList files = std::exchange(m_afterDiscovery, {});
            for (const QString &file : files)
                download(file);
        });
    });
}
