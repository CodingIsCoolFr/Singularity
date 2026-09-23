#include "core/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace {
constexpr int MaxHistoryLines = 2000;
}

Logger::Logger()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);

    m_file.setFileName(dir + QStringLiteral("/singularity.log"));
    // Truncate on every start so the file always describes this run.
    if (m_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_stream.setDevice(&m_file);
        m_stream << QStringLiteral("=== Singularity started %1 ===\n")
                        .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
        m_stream.flush();
    }
}

Logger::~Logger()
{
    if (m_file.isOpen()) {
        m_stream.flush();
        m_file.close();
    }
}

Logger &Logger::instance()
{
    static Logger logger;
    return logger;
}

QString Logger::filePath() const
{
    return QFileInfo(m_file).absoluteFilePath();
}

void Logger::log(const QString &source, const QString &message)
{
    const QString line = QStringLiteral("%1  [%2] %3")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                                  source.leftJustified(8), message);

    {
        const QMutexLocker lock(&m_mutex);

        m_history.append(line);
        if (m_history.size() > MaxHistoryLines)
            m_history.remove(0, m_history.size() - MaxHistoryLines);

        if (m_file.isOpen()) {
            m_stream << line << '\n';
            // Voice lines are flushed as they happen, because that tally is how a
            // broken call gets diagnosed. Everything else is flushed in batches:
            // forcing the file out on every gateway notice stalled the thread
            // that plays the call.
            const bool immediate = source == QLatin1String("voice") || source == QLatin1String("share")
                || source == QLatin1String("dave") || source == QLatin1String("hang");
            if (immediate || ++m_sinceFlush >= 24) {
                m_stream.flush();
                m_sinceFlush = 0;
            }
        }
    }

    // Outside the lock: whoever listens runs on its own thread's time.
    emit lineLogged(line);
}

QStringList Logger::history() const
{
    const QMutexLocker lock(&m_mutex);
    return m_history;
}
