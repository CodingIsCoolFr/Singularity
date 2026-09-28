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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

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
    "singularity.exe", "discord.exe", "dwm.exe", "csrss.exe", "svchost.exe",
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

struct Windowed
{
    QString title;
    QString exe;
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
    m_shown = config.value(QStringLiteral("presence/showGames"), true).toBool();
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
    if (!m_timer.isActive())
        m_timer.start();
    scan();
}

void GameActivity::refresh()
{
    scan();
}

void GameActivity::setShown(bool on)
{
    if (m_shown == on)
        return;
    m_shown = on;
    AppConfig::instance().setValue(QStringLiteral("presence/showGames"), on);
    rebuild();
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
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const QJsonObject known = it.value().toObject();
        m_catalog.insert(it.key(), Known{known.value(QStringLiteral("n")).toString(),
                                         known.value(QStringLiteral("id")).toString()});
    }
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
    QJsonObject compact;
    m_catalog.clear();
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
            const QString base = exeBase(row.value(QStringLiteral("name")).toString());
            if (base.isEmpty() || m_catalog.contains(base))
                continue;
            m_catalog.insert(base, Known{name, id});
            compact.insert(base, QJsonObject{{QStringLiteral("n"), name}, {QStringLiteral("id"), id}});
        }
    }

    QFile file(catalogPath());
    QDir().mkpath(QFileInfo(catalogPath()).absolutePath());
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(compact).toJson(QJsonDocument::Compact));

    wlog(QStringLiteral("presence"),
         QStringLiteral("game list has %1 programs").arg(m_catalog.size()));
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
        next.append(activity);
    };

    for (const Saved &game : m_saved) {
        if (!game.enabled)
            continue;
        const QString base = exeBase(game.exe);
        const bool always = game.exe.trimmed().isEmpty();
        if (!always && !running.contains(base))
            continue;
        const Known known = m_catalog.value(base);
        push(game.name, game.details, game.exe, known.id);
    }

    if (m_detect) {
        for (const QString &exe : running) {
            const Known known = m_catalog.value(exe);
            if (known.name.isEmpty())
                continue;
            push(known.name, {}, exe, known.id);
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
