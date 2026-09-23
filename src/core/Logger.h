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

    // `source` is a short tag such as "gateway" or "rest".
    void log(const QString &source, const QString &message);

    QStringList history() const;
    QString filePath() const;

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
