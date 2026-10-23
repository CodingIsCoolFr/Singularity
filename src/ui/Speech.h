#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <memory>

class QAudioSink;
class QFile;
class QNetworkReply;
class SpeechQueue;
class SpeechWorker;

// Reads messages aloud: Discord's "Speak Message", in a voice that sounds like
// a person rather than a machine.
//
// Discord's own uses the voices built into the system (the browser's
// speechSynthesis), which on Windows means Zira or David. This can too, and
// does until the natural voice is on the machine, or if it is switched off.
//
// The natural voice is Kokoro, an open model (Apache 2.0) whose voices are
// among the most lifelike that run on an ordinary computer, played through
// sherpa-onnx (Apache 2.0). Both are fetched once, about 370 MB, from their
// own releases on GitHub, checked against fixed SHA-256 hashes before anything
// is unpacked, and run entirely here: what is read never leaves the machine.
// The engine is loaded with QLibrary on a thread of its own, so the model is
// read from disk once and the window never waits on it.
//
// Speech comes out a sentence at a time as it is made, so a long message
// starts at once instead of after the whole thing is ready, and Stop Speaking
// halts it mid-sentence.
class Speech : public QObject
{
    Q_OBJECT

public:
    static Speech &instance();

    // Says `text`, stopping anything already being said - Discord's Speak
    // Message. With `queue` it waits its turn instead, as new messages read
    // out as they arrive do.
    void speak(const QString &text, bool queue = false);
    void stop();
    bool speaking() const;

    enum class Natural { Missing, Downloading, Unpacking, Ready, Failed };
    Natural naturalState() const { return m_state; }
    int downloadPercent() const;
    QString failure() const { return m_failure; }

    // Fetches the engine and the voice, once. Safe to call again.
    void downloadNatural();
    static constexpr qint64 DownloadMegabytes = 370;

    struct Voice
    {
        const char *id;      // Kokoro's name for it
        const char *label;
        int speaker;         // its number in the v1.0 voice table
    };
    static QList<Voice> voices();

signals:
    // Download progress, the voice becoming ready or failing, and speech
    // starting or stopping. Settings listens to show where things are.
    void changed();

private:
    // Hands audio and its end back through onAudio and onDone.
    friend class SpeechWorker;

    Speech();
    ~Speech() override;   // where QFile and the queue are whole

    bool naturalInstalled() const;
    bool useNatural() const;
    void startNext();
    void finishCurrent();
    void onAudio(int request, const QByteArray &pcm);
    void onDone(int request, bool ok, const QString &why);

    // The system's own voice, as Discord uses: SAPI on Windows.
    void speakWithSystem(const QString &text, bool queue);
    void stopSystem();
    bool systemSpeaking() const;
    void *m_sapi = nullptr;   // ISpVoice, kept out of this header

    // Downloading, one package after the other.
    void fetchNext();
    void unpack(int index, const QString &archive);
    void failDownload(const QString &why);
    QString folder() const;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    int m_package = 0;
    qint64 m_received = 0;     // bytes of finished packages and the current one
    Natural m_state = Natural::Missing;
    QString m_failure;

    // Playing.
    QThread m_thread;
    SpeechWorker *m_worker = nullptr;
    QAudioSink *m_sink = nullptr;
    SpeechQueue *m_pcm = nullptr;
    QStringList m_queue;
    QString m_current;   // what is being said, for the system's voice if this one fails
    int m_request = 0;
    bool m_busy = false;

    // The natural voice is about 500 MB once loaded (2026-10-22: 742 MB in
    // use before the first message was read aloud, 1264 MB after), and it
    // stayed for the rest of the session. It is put away after this long
    // with nothing said, and loaded again - about a second - the next time.
    QTimer m_putAway;
};
