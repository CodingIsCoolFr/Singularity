#include "core/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace {
constexpr int MaxHistoryLines = 2000;
bool g_sideFile = false;

// The last five runs are kept: singularity.prev.log is the one before this,
// then prev2 up to prev5. Keeping only one lost a crash on 26 September
// 2026: the copy that crashed was followed by a launch that only handed over
// to a newer version, and that launch's few lines pushed the crash's log out.
void keepPreviousLogs(const QString &dir, const QString &current)
{
    constexpr int Kept = 5;
    const auto name = [&dir](int n) {
        return dir + (n == 1 ? QStringLiteral("/singularity.prev.log")
                             : QStringLiteral("/singularity.prev%1.log").arg(n));
    };
    QFile::remove(name(Kept));
    for (int n = Kept - 1; n >= 1; --n)
        QFile::rename(name(n), name(n + 1));
    QFile::rename(current, name(1));
}
}

void Logger::useSideFile()
{
    g_sideFile = true;
}

Logger::Logger()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);

    const QString path = dir + (g_sideFile ? QStringLiteral("/singularity.other.log")
                                           : QStringLiteral("/singularity.log"));

    // The last run's log is kept as singularity.prev.log. After a crash the
    // program is usually started again at once, and truncating would wipe
    // the only record of what went wrong.
    if (!g_sideFile && QFileInfo(path).size() > 0)
        keepPreviousLogs(dir, path);

    m_file.setFileName(path);
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

void Logger::claimMainFile()
{
    const QMutexLocker lock(&m_mutex);
    if (!g_sideFile)
        return;
    g_sideFile = false;

    const QString side = m_file.fileName();
    const QString dir = QFileInfo(side).absolutePath();
    const QString path = dir + QStringLiteral("/singularity.log");
    m_stream.flush();
    m_file.close();
    QFile::remove(side);

    if (QFileInfo(path).size() > 0)
        keepPreviousLogs(dir, path);

    m_file.setFileName(path);
    if (m_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_stream.setDevice(&m_file);
        m_stream << QStringLiteral("=== Singularity started %1 (moved here from the side log) ===\n")
                        .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
        for (const QString &line : std::as_const(m_history))
            m_stream << line << '\n';
        m_stream.flush();
        m_sinceFlush = 0;
    }
}

void Logger::flush()
{
    const QMutexLocker lock(&m_mutex);
    if (m_file.isOpen()) {
        m_stream.flush();
        m_file.flush();
        m_sinceFlush = 0;
    }
}

QStringList Logger::history() const
{
    const QMutexLocker lock(&m_mutex);
    return m_history;
}
