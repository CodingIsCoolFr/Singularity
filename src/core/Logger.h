#pragma once

#include <QFile>
#include <QMutex>
#include <QObject>
#include <QStringList>
#include <QTextStream>

// One place every part of the app reports to.
//
// Lines go to a file next to the settings and to anything watching the
// `lineLogged` signal, so the Log window can show them live.
//
// Safe to call from any thread. The voice engine, the video decoder, the
// picture unpacker and the window all write here, at the same time, and for a
// long while nothing stopped them: two threads appending to the same list and
// the same file at once can corrupt both.
class Logger : public QObject
{
    Q_OBJECT

public:
    static Logger &instance();

    // Called before the first line when another copy of the program is
    // already running and owns singularity.log. This copy then writes to
    // singularity.other.log, so a second launch - which mostly just brings
    // the running window forward - no longer wipes the running copy's log.
    static void useSideFile();

    // `source` is a short tag such as "gateway" or "rest".
    void log(const QString &source, const QString &message);

    QStringList history() const;
    QString filePath() const;

    // Writes out everything still held in memory. For the last moments
    // before the program stops, when there is no later batch to wait for.
    void flush();

signals:
    void lineLogged(const QString &line);

private:
    Logger();
    ~Logger() override;

    mutable QMutex m_mutex;
    QFile m_file;
    QTextStream m_stream;
    QStringList m_history;
    int m_sinceFlush = 0;
};

// Short helper so call sites stay readable.
inline void wlog(const QString &source, const QString &message)
{
    Logger::instance().log(source, message);
}
