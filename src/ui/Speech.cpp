#include "ui/Speech.h"

#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/sherpa/c-api.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLibrary>
#include <QMediaDevices>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#include <atomic>
#include <iterator>
#include <cstring>
#include <string>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sapi.h>

namespace {

// The engine and the voice, pinned. src/core/sherpa/c-api.h is the header
// from this same engine release; the structs in it are handed straight to the
// library, so the two move together or not at all.
constexpr char EngineFolder[] = "sherpa-onnx-v1.13.8-win-x64-shared-MD-Release";
constexpr char ModelFolder[] = "kokoro-multi-lang-v1_0";

struct Package
{
    const char *url;
    const char *sha256;   // GitHub's own digest for the file, checked before unpacking
    qint64 bytes;
    const char *proof;    // a file that exists once it is unpacked
};

const Package kPackages[] = {
    {"https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.8/"
     "sherpa-onnx-v1.13.8-win-x64-shared-MD-Release.tar.bz2",
     "3e971a04b2e0ba4dfa53d381a006367ce8c9f5f09b4ae00043e9845c2baded22", 20494724,
     "sherpa-onnx-v1.13.8-win-x64-shared-MD-Release/lib/sherpa-onnx-c-api.dll"},
    {"https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-multi-lang-v1_0.tar.bz2",
     "c5f7e2d2caf082bc1d20fb70334a61d99d20b484500aad32e7cf84c128ea3298", 349906910,
     "kokoro-multi-lang-v1_0/model.onnx"},
};
constexpr int PackageCount = int(std::size(kPackages));

// What the speakers are given: 48 kHz stereo, 16 bit, which every output on
// Windows takes. Kokoro makes 24 kHz mono, so each sample is spread over two.
constexpr int OutRate = 48000;
constexpr int OutFrameBytes = 4;

QByteArray toOutput(const float *samples, int count, int rate)
{
    if (!samples || count <= 0 || rate <= 0)
        return {};
    const double step = double(rate) / OutRate;
    const qint64 frames = qint64(double(count) / step);
    QByteArray out(frames * OutFrameBytes, Qt::Uninitialized);
    auto *to = reinterpret_cast<qint16 *>(out.data());
    for (qint64 i = 0; i < frames; ++i) {
        const double at = double(i) * step;
        const int a = qMin(int(at), count - 1);
        const int b = qMin(a + 1, count - 1);
        const float value = samples[a] + (samples[b] - samples[a]) * float(at - a);
        const auto sample = qint16(qBound(-1.0f, value, 1.0f) * 32767.0f);
        to[2 * i] = sample;
        to[2 * i + 1] = sample;
    }
    return out;
}

QAudioDevice outputDevice()
{
    // The same speakers as calls, as Discord does.
    const QByteArray wanted = AppConfig::instance().value(QStringLiteral("voice/outputDevice")).toByteArray();
    if (!wanted.isEmpty()) {
        for (const QAudioDevice &device : QMediaDevices::audioOutputs()) {
            if (device.id() == wanted)
                return device;
        }
    }
    return QMediaDevices::defaultAudioOutput();
}

int speechVolume()
{
    return qBound(0, AppConfig::instance().value(QStringLiteral("speech/volume"), 100).toInt(), 100);
}

int speechSpeed()
{
    return qBound(50, AppConfig::instance().value(QStringLiteral("speech/speed"), 100).toInt(), 200);
}

} // namespace

// ---------------------------------------------------------------------------
// The sound on its way to the speakers
// ---------------------------------------------------------------------------

// Speech as it is made, read by the sink. While the next sentence is still
// being made it plays silence, so the sink keeps running and the sentence
// starts the moment it lands; once everything is made and played it reports
// the end, and the sink goes idle.
class SpeechQueue : public QIODevice
{
public:
    void push(const QByteArray &pcm) { m_data.append(pcm); }
    void finish() { m_finished = true; }
    void clear()
    {
        m_data.clear();
        m_read = 0;
        m_finished = false;
    }
    bool finished() const { return m_finished; }
    bool drained() const { return m_read >= m_data.size(); }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return (m_data.size() - m_read) + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 have = m_data.size() - m_read;
        if (have <= 0) {
            if (m_finished)
                return 0;
            const qint64 silence = qMin<qint64>(maxSize, OutRate / 50 * OutFrameBytes) / OutFrameBytes * OutFrameBytes;
            std::memset(data, 0, size_t(silence));
            return silence;
        }
        const qint64 n = qMin(have, maxSize) / OutFrameBytes * OutFrameBytes;
        std::memcpy(data, m_data.constData() + m_read, size_t(n));
        m_read += n;
        if (m_read > (1 << 20)) {
            m_data.remove(0, m_read);
            m_read = 0;
        }
        return n;
    }
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    QByteArray m_data;
    qint64 m_read = 0;
    bool m_finished = false;
};

// ---------------------------------------------------------------------------
// The natural voice, on a thread of its own
// ---------------------------------------------------------------------------

class SpeechWorker : public QObject
{
public:
    // The request being said. Any other is stopped at its next sentence.
    std::atomic<int> current{0};

    ~SpeechWorker() override
    {
        if (m_tts && m_destroy)
            m_destroy(m_tts);
    }

    // Frees the voice; the next request loads it again.
    void release()
    {
        if (!m_tts || !m_destroy)
            return;
        m_destroy(m_tts);
        m_tts = nullptr;
        wlog(QStringLiteral("speech"), QStringLiteral("natural voice put away after a while unused"));
    }

    void say(int request, const QByteArray &utf8, int speaker, float speed, const QString &engineDir,
             const QString &modelDir, const QString &lexicon)
    {
        if (current.load() != request) {
            report(request, true, {});
            return;
        }
        QString why;
        if (!ensure(engineDir, modelDir, lexicon, &why)) {
            report(request, false, why);
            return;
        }

        SherpaOnnxGenerationConfig config;
        std::memset(&config, 0, sizeof config);
        config.silence_scale = 0.2f;
        config.speed = speed;
        config.sid = speaker;

        Call call{this, request, m_rate, false};
        QElapsedTimer clock;
        clock.start();
        const SherpaOnnxGeneratedAudio *audio =
            m_generate(m_tts, utf8.constData(), &config, &SpeechWorker::onProgress, &call);
        if (audio) {
            // Some models hand everything over only at the end.
            if (!call.delivered && audio->n > 0 && current.load() == request)
                deliver(request, toOutput(audio->samples, audio->n, audio->sample_rate));
            wlog(QStringLiteral("speech"),
                 QStringLiteral("said %1 characters in %2 ms").arg(utf8.size()).arg(clock.elapsed()));
            m_destroyAudio(audio);
        }
        report(request, audio != nullptr || current.load() != request,
               audio ? QString() : QStringLiteral("the voice made no sound"));
    }

private:
    struct Call
    {
        SpeechWorker *worker;
        int request;
        int rate;
        bool delivered;
    };

    // Called for each sentence as it is made. Returning 0 stops the rest.
    static int32_t onProgress(const float *samples, int32_t n, float, void *arg)
    {
        auto *call = static_cast<Call *>(arg);
        if (call->worker->current.load() != call->request)
            return 0;
        if (n > 0) {
            call->delivered = true;
            call->worker->deliver(call->request, toOutput(samples, n, call->rate));
        }
        return 1;
    }

    void deliver(int request, const QByteArray &pcm)
    {
        QMetaObject::invokeMethod(
            &Speech::instance(), [request, pcm]() { Speech::instance().onAudio(request, pcm); },
            Qt::QueuedConnection);
    }

    void report(int request, bool ok, const QString &why)
    {
        QMetaObject::invokeMethod(
            &Speech::instance(), [request, ok, why]() { Speech::instance().onDone(request, ok, why); },
            Qt::QueuedConnection);
    }

    bool ensure(const QString &engineDir, const QString &modelDir, const QString &lexicon, QString *why)
    {
        if (m_tts && lexicon == m_lexicon)
            return true;

        if (!m_api.isLoaded()) {
            // Its own onnxruntime first, by full path. Windows 11 carries a
            // different one in System32, and the engine must not find that.
            m_onnx.setFileName(engineDir + QStringLiteral("/lib/onnxruntime.dll"));
            if (!m_onnx.load()) {
                *why = QStringLiteral("could not load onnxruntime: %1").arg(m_onnx.errorString());
                return false;
            }
            m_api.setFileName(engineDir + QStringLiteral("/lib/sherpa-onnx-c-api.dll"));
            if (!m_api.load()) {
                *why = QStringLiteral("could not load the speech engine: %1").arg(m_api.errorString());
                return false;
            }
            m_create = reinterpret_cast<decltype(&SherpaOnnxCreateOfflineTts)>(
                m_api.resolve("SherpaOnnxCreateOfflineTts"));
            m_destroy = reinterpret_cast<decltype(&SherpaOnnxDestroyOfflineTts)>(
                m_api.resolve("SherpaOnnxDestroyOfflineTts"));
            m_generate = reinterpret_cast<decltype(&SherpaOnnxOfflineTtsGenerateWithConfig)>(
                m_api.resolve("SherpaOnnxOfflineTtsGenerateWithConfig"));
            m_destroyAudio = reinterpret_cast<decltype(&SherpaOnnxDestroyOfflineTtsGeneratedAudio)>(
                m_api.resolve("SherpaOnnxDestroyOfflineTtsGeneratedAudio"));
            m_sampleRate = reinterpret_cast<decltype(&SherpaOnnxOfflineTtsSampleRate)>(
                m_api.resolve("SherpaOnnxOfflineTtsSampleRate"));
            if (!m_create || !m_destroy || !m_generate || !m_destroyAudio || !m_sampleRate) {
                *why = QStringLiteral("the speech engine is missing parts it should have");
                return false;
            }
        }

        if (m_tts) {
            m_destroy(m_tts);
            m_tts = nullptr;
        }

        // Kept alive until the engine is made; it copies what it needs.
        const QByteArray model = (modelDir + QStringLiteral("/model.onnx")).toUtf8();
        const QByteArray voices = (modelDir + QStringLiteral("/voices.bin")).toUtf8();
        const QByteArray tokens = (modelDir + QStringLiteral("/tokens.txt")).toUtf8();
        const QByteArray dataDir = (modelDir + QStringLiteral("/espeak-ng-data")).toUtf8();
        const QByteArray dictDir = (modelDir + QStringLiteral("/dict")).toUtf8();
        const QByteArray lexiconPath = (modelDir + QLatin1Char('/') + lexicon).toUtf8();

        SherpaOnnxOfflineTtsConfig config;
        std::memset(&config, 0, sizeof config);
        config.model.kokoro.model = model.constData();
        config.model.kokoro.voices = voices.constData();
        config.model.kokoro.tokens = tokens.constData();
        config.model.kokoro.data_dir = dataDir.constData();
        config.model.kokoro.dict_dir = dictDir.constData();
        config.model.kokoro.lexicon = lexiconPath.constData();
        config.model.kokoro.length_scale = 1.0f;
        // Half the processor, at most eight: measured on a 32-thread machine,
        // eight made speech four times faster than it plays, and more did not
        // help.
        config.model.num_threads = qBound(2, QThread::idealThreadCount() / 2, 8);
        config.model.provider = "cpu";
        // A sentence at a time, so the first is heard while the rest are made.
        config.max_num_sentences = 1;
        config.silence_scale = 0.2f;

        QElapsedTimer clock;
        clock.start();
        m_tts = m_create(&config);
        if (!m_tts) {
            *why = QStringLiteral("the voice would not load");
            return false;
        }
        m_rate = m_sampleRate(m_tts);
        m_lexicon = lexicon;
        wlog(QStringLiteral("speech"),
             QStringLiteral("natural voice loaded in %1 ms, %2 Hz, %3 threads")
                 .arg(clock.elapsed())
                 .arg(m_rate)
                 .arg(config.model.num_threads));
        return true;
    }

    QLibrary m_onnx;
    QLibrary m_api;
    decltype(&SherpaOnnxCreateOfflineTts) m_create = nullptr;
    decltype(&SherpaOnnxDestroyOfflineTts) m_destroy = nullptr;
    decltype(&SherpaOnnxOfflineTtsGenerateWithConfig) m_generate = nullptr;
    decltype(&SherpaOnnxDestroyOfflineTtsGeneratedAudio) m_destroyAudio = nullptr;
    decltype(&SherpaOnnxOfflineTtsSampleRate) m_sampleRate = nullptr;
    const SherpaOnnxOfflineTts *m_tts = nullptr;
    QString m_lexicon;
    int m_rate = 24000;
};

// ---------------------------------------------------------------------------
// Speech
// ---------------------------------------------------------------------------

Speech &Speech::instance()
{
    // Made once and never destroyed as a static would be, after the audio
    // system has gone; it lets go of everything as the program quits instead.
    static Speech *speech = new Speech;
    return *speech;
}

Speech::Speech()
{
    m_worker = new SpeechWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.setObjectName(QStringLiteral("speech"));
    m_thread.start();

    m_state = naturalInstalled() ? Natural::Ready : Natural::Missing;

    m_putAway.setSingleShot(true);
    m_putAway.setInterval(5 * 60 * 1000);
    connect(&m_putAway, &QTimer::timeout, this, [this]() {
        SpeechWorker *worker = m_worker;
        QMetaObject::invokeMethod(worker, [worker]() { worker->release(); }, Qt::QueuedConnection);
    });

    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        stop();
        if (m_reply)
            m_reply->abort();
        m_thread.quit();
        m_thread.wait(3000);
        delete m_sink;
        m_sink = nullptr;
        if (m_sapi) {
            static_cast<ISpVoice *>(m_sapi)->Release();
            m_sapi = nullptr;
        }
    });
}

Speech::~Speech() = default;

QList<Speech::Voice> Speech::voices()
{
    // Kokoro v1.0's English voices, women first and Heart first: it is the
    // one its makers rate highest.
    return {
        {"af_heart", "Heart (American)", 3},     {"af_bella", "Bella (American)", 2},
        {"af_nicole", "Nicole (American, soft)", 6}, {"af_sarah", "Sarah (American)", 9},
        {"af_nova", "Nova (American)", 7},       {"af_sky", "Sky (American)", 10},
        {"af_aoede", "Aoede (American)", 1},     {"af_kore", "Kore (American)", 5},
        {"bf_emma", "Emma (British)", 21},       {"bf_isabella", "Isabella (British)", 22},
        {"bf_alice", "Alice (British)", 20},     {"bf_lily", "Lily (British)", 23},
        {"am_michael", "Michael (American)", 16}, {"am_adam", "Adam (American)", 11},
        {"bm_george", "George (British)", 26},   {"bm_lewis", "Lewis (British)", 27},
    };
}

QString Speech::folder() const
{
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/speech");
    QDir().mkpath(path);
    return path;
}

bool Speech::naturalInstalled() const
{
    const QString base = folder() + QLatin1Char('/');
    const QString engine = base + QLatin1String(EngineFolder) + QStringLiteral("/lib/");
    const QString model = base + QLatin1String(ModelFolder) + QLatin1Char('/');
    for (const QString &path : {engine + QStringLiteral("sherpa-onnx-c-api.dll"), engine + QStringLiteral("onnxruntime.dll"),
                                model + QStringLiteral("model.onnx"), model + QStringLiteral("voices.bin"),
                                model + QStringLiteral("tokens.txt"), model + QStringLiteral("lexicon-us-en.txt"),
                                model + QStringLiteral("lexicon-gb-en.txt"), model + QStringLiteral("espeak-ng-data")}) {
        if (!QFileInfo::exists(path))
            return false;
    }
    return true;
}

bool Speech::useNatural() const
{
    return m_state == Natural::Ready && AppConfig::instance().value(QStringLiteral("speech/natural"), true).toBool();
}

bool Speech::speaking() const
{
    return m_busy || systemSpeaking();
}

void Speech::speak(const QString &text, bool queue)
{
    const QString words = text.simplified();
    if (words.isEmpty())
        return;
    if (!queue)
        stop();
    if (!useNatural()) {
        speakWithSystem(words, queue);
        return;
    }
    m_queue.append(words);
    if (!m_busy)
        startNext();
}

void Speech::stop()
{
    // Anything still being made notices at its next sentence and stops.
    ++m_request;
    m_worker->current.store(m_request);
    m_queue.clear();
    if (m_sink)
        m_sink->stop();
    if (m_pcm)
        m_pcm->clear();
    m_busy = false;
    stopSystem();
    emit changed();
}

void Speech::startNext()
{
    if (m_queue.isEmpty()) {
        m_busy = false;
        m_putAway.start();
        emit changed();
        return;
    }
    const QString text = m_queue.takeFirst();
    m_busy = true;
    m_putAway.stop();
    const int request = ++m_request;
    m_worker->current.store(request);
    if (m_pcm)
        m_pcm->clear();
    // A different output chosen since the last time: a sink for it.
    if (m_sink && m_sink->state() == QtAudio::StoppedState) {
        delete m_sink;
        m_sink = nullptr;
    }

    const QString wanted = AppConfig::instance().value(QStringLiteral("speech/voice"), QStringLiteral("af_heart")).toString();
    Voice voice = voices().first();
    for (const Voice &each : voices()) {
        if (wanted == QLatin1String(each.id))
            voice = each;
    }
    // British voices read with British pronunciations.
    const QString lexicon = voice.id[0] == 'b' ? QStringLiteral("lexicon-gb-en.txt") : QStringLiteral("lexicon-us-en.txt");
    const float speed = float(speechSpeed()) / 100.0f;
    const QString engineDir = folder() + QLatin1Char('/') + QLatin1String(EngineFolder);
    const QString modelDir = folder() + QLatin1Char('/') + QLatin1String(ModelFolder);
    const QByteArray utf8 = text.toUtf8();
    const int speaker = voice.speaker;
    SpeechWorker *worker = m_worker;
    m_current = text;
    QMetaObject::invokeMethod(
        worker,
        [worker, request, utf8, speaker, speed, engineDir, modelDir, lexicon]() {
            worker->say(request, utf8, speaker, speed, engineDir, modelDir, lexicon);
        },
        Qt::QueuedConnection);
    emit changed();
}

void Speech::onAudio(int request, const QByteArray &pcm)
{
    if (request != m_request || pcm.isEmpty())
        return;
    if (!m_pcm) {
        m_pcm = new SpeechQueue;
        m_pcm->setParent(this);
        m_pcm->open(QIODevice::ReadOnly);
    }
    m_pcm->push(pcm);

    if (!m_sink) {
        QAudioFormat format;
        format.setSampleRate(OutRate);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);
        m_sink = new QAudioSink(outputDevice(), format, this);
        connect(m_sink, &QAudioSink::stateChanged, this, [this](QtAudio::State state) {
            if (state == QtAudio::IdleState && m_busy && m_pcm && m_pcm->finished() && m_pcm->drained())
                finishCurrent();
        });
    }
    if (m_sink->state() == QtAudio::StoppedState || m_sink->state() == QtAudio::IdleState) {
        m_sink->setVolume(qreal(speechVolume()) / 100.0);
        m_sink->start(m_pcm);
    }
}

void Speech::onDone(int request, bool ok, const QString &why)
{
    if (request != m_request)
        return;
    if (!ok) {
        // The natural voice could not speak. Say it anyway, with the
        // system's, and say why in the log.
        wlog(QStringLiteral("speech"), QStringLiteral("the natural voice could not speak: %1").arg(why));
        m_state = Natural::Failed;
        m_failure = why;
        m_busy = false;
        const QStringList rest = std::exchange(m_queue, {});
        speakWithSystem(m_current, false);
        for (const QString &each : rest)
            speakWithSystem(each, true);
        emit changed();
        return;
    }
    if (m_pcm)
        m_pcm->finish();
    // Nothing was made, or everything has already played.
    const bool playing = m_sink && m_sink->state() == QtAudio::ActiveState;
    if (!playing && (!m_pcm || m_pcm->drained()))
        finishCurrent();
}

void Speech::finishCurrent()
{
    if (m_sink)
        m_sink->stop();
    if (m_pcm)
        m_pcm->clear();
    m_busy = false;
    startNext();
}

// ---------------------------------------------------------------------------
// The system's voice
// ---------------------------------------------------------------------------

void Speech::speakWithSystem(const QString &text, bool queue)
{
    if (!m_sapi) {
        // Qt already started COM on this thread for drag and drop; this is a
        // no-op then, and the start of it otherwise.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ISpVoice *voice = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, __uuidof(ISpVoice),
                                    reinterpret_cast<void **>(&voice)))
            || !voice) {
            wlog(QStringLiteral("speech"), QStringLiteral("Windows' own voice is not available"));
            return;
        }
        m_sapi = voice;

        // A woman's voice when Windows has one.
        ISpObjectTokenCategory *category = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(SpObjectTokenCategory), nullptr, CLSCTX_ALL,
                                       __uuidof(ISpObjectTokenCategory), reinterpret_cast<void **>(&category)))
            && category) {
            if (SUCCEEDED(category->SetId(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices", FALSE))) {
                IEnumSpObjectTokens *tokens = nullptr;
                if (SUCCEEDED(category->EnumTokens(L"Gender=Female", nullptr, &tokens)) && tokens) {
                    ISpObjectToken *token = nullptr;
                    if (tokens->Next(1, &token, nullptr) == S_OK && token) {
                        voice->SetVoice(token);
                        token->Release();
                    }
                    tokens->Release();
                }
            }
            category->Release();
        }
    }

    auto *voice = static_cast<ISpVoice *>(m_sapi);
    // SAPI's rate runs from -10 to 10, about a tenth faster or slower a step.
    voice->SetRate(long(qBound(-10, qRound((speechSpeed() - 100) / 10.0), 10)));
    voice->SetVolume(USHORT(speechVolume()));
    const std::wstring wide = text.toStdWString();
    DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML;
    if (!queue)
        flags |= SPF_PURGEBEFORESPEAK;
    voice->Speak(wide.c_str(), flags, nullptr);
    emit changed();
}

void Speech::stopSystem()
{
    if (m_sapi)
        static_cast<ISpVoice *>(m_sapi)->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
}

bool Speech::systemSpeaking() const
{
    if (!m_sapi)
        return false;
    SPVOICESTATUS status;
    std::memset(&status, 0, sizeof status);
    if (FAILED(static_cast<ISpVoice *>(m_sapi)->GetStatus(&status, nullptr)))
        return false;
    return status.dwRunningState == SPRS_IS_SPEAKING;
}

// ---------------------------------------------------------------------------
// Fetching the natural voice
// ---------------------------------------------------------------------------

int Speech::downloadPercent() const
{
    qint64 total = 0;
    for (const Package &package : kPackages)
        total += package.bytes;
    return total > 0 ? int(qBound(qint64(0), m_received * 100 / total, qint64(100))) : 0;
}

void Speech::downloadNatural()
{
    if (m_state == Natural::Downloading || m_state == Natural::Unpacking)
        return;
    if (naturalInstalled()) {
        m_state = Natural::Ready;
        emit changed();
        return;
    }
    m_failure.clear();
    m_package = 0;
    m_received = 0;
    m_state = Natural::Downloading;
    wlog(QStringLiteral("speech"), QStringLiteral("fetching the natural voice"));
    emit changed();
    fetchNext();
}

void Speech::fetchNext()
{
    // Whatever is already unpacked from an earlier try is not fetched again.
    while (m_package < PackageCount
           && QFileInfo::exists(folder() + QLatin1Char('/') + QLatin1String(kPackages[m_package].proof))) {
        m_received += kPackages[m_package].bytes;
        ++m_package;
    }
    if (m_package >= PackageCount) {
        if (naturalInstalled()) {
            m_state = Natural::Ready;
            wlog(QStringLiteral("speech"), QStringLiteral("the natural voice is ready"));
            emit changed();
        } else {
            failDownload(QStringLiteral("files are missing after unpacking"));
        }
        return;
    }

    const Package &package = kPackages[m_package];
    const QUrl url(QString::fromLatin1(package.url));
    const QString archive = folder() + QLatin1Char('/') + url.fileName();
    m_file = std::make_unique<QFile>(archive + QStringLiteral(".part"));
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        failDownload(QStringLiteral("could not write %1").arg(m_file->fileName()));
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);

    QNetworkRequest request(url);
    // GitHub hands the file over from its storage host, by a redirect.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Singularity"));
    QNetworkReply *reply = m_network.get(request);
    m_reply = reply;

    const qint64 before = m_received;
    const int index = m_package;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        const QByteArray chunk = reply->readAll();
        m_hash->addData(chunk);
        m_file->write(chunk);
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, before](qint64 got, qint64) {
        const int was = downloadPercent();
        m_received = before + got;
        if (downloadPercent() != was)
            emit changed();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, archive, index]() {
        reply->deleteLater();
        const QByteArray rest = reply->readAll();
        m_hash->addData(rest);
        m_file->write(rest);
        m_file->close();

        if (reply->error() != QNetworkReply::NoError) {
            m_file->remove();
            failDownload(reply->errorString());
            return;
        }
        // Nothing is unpacked, and nothing from it ever run, unless it is
        // exactly the file that was published.
        const QByteArray digest = m_hash->result().toHex();
        if (digest != QByteArray(kPackages[index].sha256)) {
            m_file->remove();
            failDownload(QStringLiteral("%1 was not the file expected (SHA-256 %2)")
                             .arg(QFileInfo(archive).fileName(), QString::fromLatin1(digest)));
            return;
        }
        QFile::remove(archive);
        if (!m_file->rename(archive)) {
            failDownload(QStringLiteral("could not keep %1").arg(archive));
            return;
        }
        m_file.reset();
        unpack(index, archive);
    });
}

void Speech::unpack(int index, const QString &archive)
{
    m_state = Natural::Unpacking;
    emit changed();

    // Windows' own tar, which reads .tar.bz2.
    auto *tar = new QProcess(this);
    tar->setProgram(QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")))
                        .filePath(QStringLiteral("System32/tar.exe")));
    tar->setArguments({QStringLiteral("-xjf"), QDir::toNativeSeparators(archive), QStringLiteral("-C"),
                       QDir::toNativeSeparators(folder())});
    connect(tar, &QProcess::errorOccurred, this, [this, tar](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        tar->deleteLater();
        failDownload(QStringLiteral("Windows' tar could not be started to unpack the voice"));
    });
    connect(tar, &QProcess::finished, this, [this, tar, index, archive](int code, QProcess::ExitStatus status) {
        tar->deleteLater();
        const bool there = QFileInfo::exists(folder() + QLatin1Char('/') + QLatin1String(kPackages[index].proof));
        if (status != QProcess::NormalExit || code != 0 || !there) {
            failDownload(QStringLiteral("could not unpack %1: %2")
                             .arg(QFileInfo(archive).fileName(),
                                  QString::fromLocal8Bit(tar->readAllStandardError()).trimmed()));
            return;
        }
        QFile::remove(archive);
        m_received = 0;
        for (int i = 0; i <= index; ++i)
            m_received += kPackages[i].bytes;
        ++m_package;
        m_state = Natural::Downloading;
        emit changed();
        fetchNext();
    });
    tar->start();
}

void Speech::failDownload(const QString &why)
{
    m_state = Natural::Failed;
    m_failure = why;
    m_file.reset();
    wlog(QStringLiteral("speech"), QStringLiteral("the natural voice could not be fetched: %1").arg(why));
    emit changed();
}
