#include "core/GameActivity.h"

#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/RestClient.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>

namespace {

QString exeBase(const QString &path)
{
    return QFileInfo(path).fileName().toLower();
}

QString catalogPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + QStringLiteral("/detectable-games.json");
}

const char *const kSkip[] = {
    "explorer.exe", "searchhost.exe", "startmenuexperiencehost.exe", "textinputhost.exe",
    "applicationframehost.exe", "systemsettings.exe", "shellexperiencehost.exe",
    "singularity.exe", "discord.exe", "firefox.exe", "chrome.exe", "msedge.exe",
    "opera.exe", "brave.exe", "dwm.exe", "csrss.exe", "svchost.exe",
    "runtimebroker.exe", "sihost.exe", "taskhostw.exe", "ctfmon.exe",
};

bool skipped(const QString &exe)
{
    const QString base = exeBase(exe);
    for (const char *name : kSkip) {
        if (base == QLatin1String(name))
            return true;
    }
    return false;
}

QSet<QString> runningExes()
{
    QSet<QString> names;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return names;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            names.insert(QString::fromWCharArray(entry.szExeFile).toLower());
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return names;
}

// Another program's command line, as Task Manager shows it. Windows hands it
// over to anyone allowed to ask what a process is (ProcessCommandLineInformation,
// Windows 8.1 and later). Empty when it cannot be read.
QString commandLineOf(quint32 pid)
{
    using Query = LONG(WINAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<Query>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess")));
    constexpr ULONG ProcessCommandLineInformation = 60;
    if (!query || pid == 0)
        return {};
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    QString text;
    ULONG needed = 0;
    query(process, ProcessCommandLineInformation, nullptr, 0, &needed);
    if (needed > sizeof(UNICODE_STRING) && needed < (1u << 20)) {
        QByteArray buffer(int(needed), Qt::Uninitialized);
        if (query(process, ProcessCommandLineInformation, buffer.data(), needed, &needed) >= 0) {
            const auto *line = reinterpret_cast<const UNICODE_STRING *>(buffer.constData());
            if (line->Buffer)
                text = QString::fromWCharArray(line->Buffer, line->Length / int(sizeof(wchar_t)));
        }
    }
    CloseHandle(process);
    return text;
}

struct Windowed
{
    QString title;
    QString exe;
    quint32 pid = 0;
};

BOOL CALLBACK collectWindow(HWND window, LPARAM param)
{
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER))
        return TRUE;
    const int length = GetWindowTextLengthW(window);
    if (length <= 0)
        return TRUE;

    wchar_t title[512];
    GetWindowTextW(window, title, 512);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return TRUE;
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    if (!ok)
        return TRUE;

    auto *list = reinterpret_cast<QList<Windowed> *>(param);
    Windowed row;
    row.title = QString::fromWCharArray(title).trimmed();
    row.exe = QString::fromWCharArray(path);
    row.pid = pid;
    if (!row.title.isEmpty() && !skipped(row.exe))
        list->append(row);
    return TRUE;
}

QList<Windowed> windowedPrograms()
{
    QList<Windowed> list;
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&list));
    return list;
}

} // namespace

GameActivity::GameActivity(QObject *parent)
    : QObject(parent)
{
    AppConfig &config = AppConfig::instance();
    m_shown = config.value(QStringLiteral("presence/showGames"), false).toBool();
    // On unless switched off, as in Discord's own client. Game Time and Game
    // Variety, the 2026 badges, only grow for games Discord sees you playing,
    // and with this off it saw only the ones added by hand.
    m_detect = config.value(QStringLiteral("presence/detectGames"), true).toBool();
    loadSaved();
    loadCatalog();

    m_timer.setInterval(5000);
    connect(&m_timer, &QTimer::timeout, this, &GameActivity::scan);
}

void GameActivity::start(RestClient *rest)
{
    if (!m_catalogAsked && rest)
        fetchCatalog(rest);
    m_ready = true;
    updateTimer();
}

void GameActivity::watchRunning(QObject *watcher)
{
    if (!watcher)
        return;
    ++m_watchers;
    connect(watcher, &QObject::destroyed, this, [this]() {
        --m_watchers;
        updateTimer();
    });
    updateTimer();
}

// Programs are looked at only while there is a reason to: games go on your
// status, or the Activity page is open to add one. Until 0.9.3 this ran every
// five seconds from sign-in whatever the switch said, which the privacy page
// had been promising it did not.
void GameActivity::updateTimer()
{
    const bool wanted = m_ready && (m_shown || m_watchers > 0);
    if (wanted && !m_timer.isActive()) {
        m_timer.start();
        scan();
    } else if (!wanted && m_timer.isActive()) {
        m_timer.stop();
        m_running.clear();
        m_runningKey.clear();
    }
}

void GameActivity::refresh()
{
    scan();
}

bool GameActivity::wantsExe(const QString &exe) const
{
    if (!m_shown || exe.trimmed().isEmpty())
        return false;
    const QString base = exeBase(exe);
    if (skipped(base))
        return false;
    for (const Saved &game : m_saved) {
        if (game.enabled && exeBase(game.exe) == base)
            return true;
    }
    return m_detect && m_catalog.contains(base);
}

void GameActivity::setShown(bool on)
{
    if (m_shown == on)
        return;
    m_shown = on;
    AppConfig::instance().setValue(QStringLiteral("presence/showGames"), on);
    rebuild();
    updateTimer();
}

void GameActivity::setDetect(bool on)
{
    if (m_detect == on)
        return;
    m_detect = on;
    AppConfig::instance().setValue(QStringLiteral("presence/detectGames"), on);
    rebuild();
}

void GameActivity::addSaved(const Saved &game)
{
    if (game.name.trimmed().isEmpty())
        return;
    m_saved.append(game);
    storeSaved();
    rebuild();
}

void GameActivity::updateSaved(int index, const Saved &game)
{
    if (index < 0 || index >= m_saved.size() || game.name.trimmed().isEmpty())
        return;
    m_saved[index] = game;
    storeSaved();
    rebuild();
}

void GameActivity::removeSaved(int index)
{
    if (index < 0 || index >= m_saved.size())
        return;
    m_saved.removeAt(index);
    storeSaved();
    rebuild();
}

QStringList GameActivity::showing() const
{
    QStringList lines;
    for (const QJsonValue &value : m_activities) {
        const QJsonObject activity = value.toObject();
        QString line = activity.value(QStringLiteral("name")).toString();
        const QString details = activity.value(QStringLiteral("details")).toString();
        if (!details.isEmpty())
            line += QStringLiteral(" — ") + details;
        lines.append(line);
    }
    return lines;
}

void GameActivity::loadSaved()
{
    m_saved.clear();
    const QJsonArray array = QJsonDocument::fromJson(
                                 AppConfig::instance().value(QStringLiteral("presence/games")).toByteArray())
                                 .array();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        Saved game;
        game.name = object.value(QStringLiteral("name")).toString();
        game.exe = object.value(QStringLiteral("exe")).toString();
        game.details = object.value(QStringLiteral("details")).toString();
        game.enabled = object.value(QStringLiteral("enabled")).toBool(true);
        if (!game.name.isEmpty())
            m_saved.append(game);
    }

    // A browser window title is not a game. One got saved that way.
    const auto browser = [](const QString &exe) {
        const QString base = exeBase(exe);
        return base == QLatin1String("firefox.exe") || base == QLatin1String("chrome.exe")
            || base == QLatin1String("msedge.exe") || base == QLatin1String("opera.exe")
            || base == QLatin1String("brave.exe");
    };
    const int before = m_saved.size();
    m_saved.erase(std::remove_if(m_saved.begin(), m_saved.end(),
                                 [&](const Saved &game) {
                                     return browser(game.exe)
                                         && (game.name.contains(QLatin1Char('-'))
                                             || game.name.contains(QStringLiteral("—")));
                                 }),
                  m_saved.end());
    if (m_saved.size() != before)
        storeSaved();
}

void GameActivity::storeSaved()
{
    QJsonArray array;
    for (const Saved &game : m_saved) {
        array.append(QJsonObject{
            {QStringLiteral("name"), game.name},
            {QStringLiteral("exe"), game.exe},
            {QStringLiteral("details"), game.details},
            {QStringLiteral("enabled"), game.enabled},
        });
    }
    AppConfig::instance().setValue(QStringLiteral("presence/games"),
                                   QJsonDocument(array).toJson(QJsonDocument::Compact));
}

void GameActivity::loadCatalog()
{
    QFile file(catalogPath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    // {"game.exe": [{"n": name, "id": id, "p": "folder/game.exe"}, ...]}. A
    // file from before paths were kept has one object per name and no "p";
    // it is matched by name alone until Discord's list is read again.
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const QJsonArray rows = it.value().isArray() ? it.value().toArray() : QJsonArray{it.value()};
        QList<Known> &list = m_catalog[it.key()];
        for (const QJsonValue &value : rows) {
            const QJsonObject row = value.toObject();
            const QString path = row.value(QStringLiteral("p")).toString();
            list.append(Known{row.value(QStringLiteral("n")).toString(), row.value(QStringLiteral("id")).toString(),
                              path.isEmpty() ? it.key() : path, row.value(QStringLiteral("a")).toString(),
                              row.value(QStringLiteral("x")).toBool()});
        }
    }
}

GameActivity::Known GameActivity::match(const QString &fullPath, quint32 pid) const
{
    const QString path = QDir::fromNativeSeparators(fullPath).toLower();
    const auto found = m_catalog.constFind(exeBase(fullPath));
    if (found == m_catalog.constEnd())
        return {};

    QString commandLine;
    bool commandLineRead = false;
    const Known *best = nullptr;
    int bestScore = -1;
    for (const Known &known : found.value()) {
        const bool fits = known.exact || path == known.path || path.endsWith(QLatin1Char('/') + known.path);
        if (!fits)
            continue;
        if (!known.args.isEmpty()) {
            if (!commandLineRead) {
                commandLine = commandLineOf(pid).toLower();
                commandLineRead = true;
            }
            if (!commandLine.contains(known.args))
                continue;
        }
        // One that names its command line is the more particular match;
        // after that, the longer path.
        const int score = (known.args.isEmpty() ? 0 : 1 << 20) + int(known.path.size());
        if (score > bestScore) {
            best = &known;
            bestScore = score;
        }
    }
    return best ? *best : Known{};
}

void GameActivity::fetchCatalog(RestClient *rest)
{
    m_catalogAsked = true;
    wlog(QStringLiteral("presence"), QStringLiteral("asking Discord which programs are games"));
    rest->fetchDetectableApplications(
        [this](const QJsonArray &apps) { applyCatalog(apps); },
        [](const RestClient::Error &error) {
            wlog(QStringLiteral("presence"),
                 QStringLiteral("could not load the game list: %1").arg(error.message));
        });
}

void GameActivity::applyCatalog(const QJsonArray &apps)
{
    wlog(QStringLiteral("presence"), QStringLiteral("reading Discord's game list (%1)").arg(apps.size()));
    m_catalog.clear();
    int programs = 0;
    int byCommandLine = 0;
    for (const QJsonValue &value : apps) {
        const QJsonObject app = value.toObject();
        const QString name = app.value(QStringLiteral("name")).toString();
        const QString id = app.value(QStringLiteral("id")).toString();
        if (name.isEmpty())
            continue;
        const QJsonArray executables = app.value(QStringLiteral("executables")).toArray();
        for (const QJsonValue &executable : executables) {
            const QJsonObject row = executable.toObject();
            if (row.value(QStringLiteral("os")).toString() != QLatin1String("win32"))
                continue;
            if (row.value(QStringLiteral("is_launcher")).toBool())
                continue;
            // ">name" is a file name in any folder. A few are told apart only
            // by their command line ("hl2.exe -game tf", javaw.exe running
            // Minecraft), which match() reads when it comes to one of them.
            QString path = QDir::fromNativeSeparators(row.value(QStringLiteral("name")).toString()).toLower();
            const bool exact = path.startsWith(QLatin1Char('>'));
            if (exact)
                path = path.mid(1);
            const QString args = row.value(QStringLiteral("arguments")).toString().trimmed().toLower();
            if (!args.isEmpty())
                ++byCommandLine;
            const QString base = exeBase(path);
            if (base.isEmpty())
                continue;
            QList<Known> &list = m_catalog[base];
            const bool again = std::any_of(list.cbegin(), list.cend(), [&](const Known &known) {
                return known.id == id && known.path == path && known.args == args && known.exact == exact;
            });
            if (again)
                continue;
            list.append(Known{name, id, path, args, exact});
            ++programs;
        }
    }

    QJsonObject compact;
    for (auto it = m_catalog.cbegin(); it != m_catalog.cend(); ++it) {
        QJsonArray rows;
        for (const Known &known : it.value()) {
            QJsonObject row{{QStringLiteral("n"), known.name},
                            {QStringLiteral("id"), known.id},
                            {QStringLiteral("p"), known.path}};
            if (!known.args.isEmpty())
                row.insert(QStringLiteral("a"), known.args);
            if (known.exact)
                row.insert(QStringLiteral("x"), true);
            rows.append(row);
        }
        compact.insert(it.key(), rows);
    }
    QFile file(catalogPath());
    QDir().mkpath(QFileInfo(catalogPath()).absolutePath());
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(compact).toJson(QJsonDocument::Compact));

    wlog(QStringLiteral("presence"),
         QStringLiteral("game list has %1 programs under %2 file names, %3 told apart by their command line")
             .arg(programs)
             .arg(m_catalog.size())
             .arg(byCommandLine));
    if (m_timer.isActive())
        scan();
}

void GameActivity::scan()
{
    const QSet<QString> running = runningExes();
    QList<Running> windows;
    QSet<QString> seen;
    for (const Windowed &window : windowedPrograms()) {
        const QString base = exeBase(window.exe);
        if (seen.contains(base))
            continue;
        seen.insert(base);
        Running row;
        row.title = window.title;
        row.exe = window.exe;
        row.known = m_catalog.contains(base);
        row.pid = window.pid;
        windows.append(row);
        if (windows.size() >= 40)
            break;
    }
    m_running = windows;
    QString runningKey;
    for (const Running &row : windows)
        runningKey += exeBase(row.exe) + QLatin1Char('|');

    if (!m_shown) {
        const bool listMoved = runningKey != m_runningKey;
        m_runningKey = runningKey;
        if (!m_activities.isEmpty()) {
            m_activities = {};
            emit changed();
        } else if (listMoved) {
            emit changed();
        }
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonArray next;
    QSet<QString> used;

    const auto push = [&](const QString &name, const QString &details, const QString &exe,
                          const QString &applicationId) {
        if (name.isEmpty() || next.size() >= 4)
            return;
        const QString key = applicationId.isEmpty() ? name.toLower() : applicationId;
        if (used.contains(key))
            return;
        used.insert(key);

        const QString stampKey = exe.isEmpty() ? name.toLower() : exeBase(exe);
        if (!m_started.contains(stampKey))
            m_started.insert(stampKey, now);

        QJsonObject activity{
            {QStringLiteral("name"), name},
            {QStringLiteral("type"), 0},
            {QStringLiteral("instance"), false},
            {QStringLiteral("timestamps"), QJsonObject{{QStringLiteral("start"), m_started.value(stampKey)}}},
        };
        if (!details.isEmpty())
            activity.insert(QStringLiteral("details"), details);
        if (!applicationId.isEmpty())
            activity.insert(QStringLiteral("application_id"), applicationId);
        if (!exe.isEmpty())
            activity.insert(QStringLiteral("_exe"), exeBase(exe));
        next.append(activity);
    };

    for (const Saved &game : m_saved) {
        if (!game.enabled)
            continue;
        const QString base = exeBase(game.exe);
        const bool always = game.exe.trimmed().isEmpty();
        if (!always && !running.contains(base))
            continue;
        const Known known = always ? Known{} : match(game.exe);
        push(game.name, game.details, game.exe, known.id);
    }

    // Programs with a window only, matched by their full path. A process
    // with no window is not a game you are playing: Roblox left
    // RobloxPlayerBeta running with no window, and that was enough to put
    // ROBLOX on the profile.
    if (m_detect) {
        for (const Running &row : windows) {
            const Known known = match(row.exe, row.pid);
            if (known.name.isEmpty())
                continue;
            push(known.name, {}, row.exe, known.id);
        }
    }

    // Drop clocks for programs that are gone, so the next launch starts over.
    for (auto it = m_started.begin(); it != m_started.end();) {
        if (!used.contains(it.key()) && !running.contains(it.key()))
            it = m_started.erase(it);
        else
            ++it;
    }

    if (next == m_activities && runningKey == m_runningKey)
        return;
    m_activities = next;
    m_runningKey = runningKey;
    emit changed();
}

void GameActivity::rebuild()
{
    m_activities = {};
    scan();
    if (m_activities.isEmpty())
        emit changed();
}
