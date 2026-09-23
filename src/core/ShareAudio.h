#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <thread>

// The sound that goes out with a shared screen.
//
// Discord's own client does this with Windows' process loopback: instead of
// recording what comes out of the speakers, it asks Windows for the sound of
// one program (a window's owner, and every process it started), or for the
// sound of everything except one program. The second is how a whole-screen
// share can carry the game and the video without also carrying the call back
// to the people who are already hearing it.
//
// This needs Windows 10 build 20348 or later. Anything older answers the
// activation with an error, which is reported, and the picture still goes out.
//
// Runs on a thread of its own. The sound is handed out in whatever sized
// pieces Windows produces - usually 10 ms - as 48 kHz, stereo, 16 bit, which
// is exactly what the Opus encoder on the stream connection takes.
class ShareAudio : public QObject
{
    Q_OBJECT

public:
    // One program that can be heard on its own.
    struct App {
        quint32 processId = 0;
        QString title;   // its main window's title
        QString exeName; // e.g. "chrome.exe"
    };

    enum class Source {
        Nothing,
        EverythingButUs, // the whole computer, minus Singularity itself
        OneApp,          // one program and everything it started
    };

    explicit ShareAudio(QObject *parent = nullptr);
    ~ShareAudio() override;

    // Programs with a window someone could recognise, one entry per program.
    // Safe to call without starting anything.
    static QList<App> apps();

    // Called on the capture thread for every piece of sound. Set before start.
    using Sink = std::function<void(const QByteArray &pcm)>;
    void setSink(Sink sink) { m_sink = std::move(sink); }

    void start(Source source, quint32 processId = 0);
    void stop();
    bool isRunning() const { return m_thread.joinable(); }

signals:
    // Emitted from the capture thread; a queued connection brings it home.
    void failed(const QString &reason);

private:
    void run(Source source, quint32 processId);

    Sink m_sink;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
};
