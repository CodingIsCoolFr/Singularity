#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

#include "core/InstallTidy.h"

#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QThreadPool>
#include <QVersionNumber>

#include <algorithm>

namespace {

#ifdef _WIN32

QString clean(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool isInside(const QString &path, const QString &folder)
{
    return path.startsWith(folder + QLatin1Char('/'), Qt::CaseInsensitive);
}

// Every place a Singularity shortcut is likely to be. The taskbar and Start
// pins are the ones the installer never touches.
QStringList shortcutFolders()
{
    const QString roaming = qEnvironmentVariable("APPDATA");
    return {
        roaming + QStringLiteral("/Microsoft/Windows/Start Menu/Programs"),
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation),
        roaming + QStringLiteral("/Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar"),
        roaming + QStringLiteral("/Microsoft/Internet Explorer/Quick Launch/User Pinned/StartMenu"),
    };
}

// Points each Singularity shortcut that leads somewhere under `root` at `exe`.
// Returns the folders that shortcuts still lead to afterwards - normally just
// this copy's - so nothing a shortcut needs gets removed.
QSet<QString> repointShortcuts(const QString &root, const QString &exe, const QStringList &folders)
{
    QSet<QString> stillUsed;
    int changed = 0;

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    for (const QString &folder : folders) {
        if (folder.isEmpty() || !QFileInfo::exists(folder))
            continue;

        QDirIterator it(folder, {QStringLiteral("*.lnk")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString lnk = it.next();
            if (!QFileInfo(lnk).fileName().contains(QLatin1String("Singularity"), Qt::CaseInsensitive))
                continue;

            IShellLinkW *link = nullptr;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                        reinterpret_cast<void **>(&link)))) {
                continue;
            }
            IPersistFile *file = nullptr;
            if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file)))) {
                link->Release();
                continue;
            }

            const std::wstring lnkPath = QDir::toNativeSeparators(lnk).toStdWString();
            wchar_t target[MAX_PATH] = {};
            if (SUCCEEDED(file->Load(lnkPath.c_str(), STGM_READWRITE))
                && SUCCEEDED(link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH))) {
                const QString was = clean(QString::fromWCharArray(target));

                // Only our own: an exe named Singularity.exe somewhere under
                // this install. Anything else with the name in it is left be.
                const bool ours = isInside(was, root)
                                  && QFileInfo(was).fileName().compare(QLatin1String("Singularity.exe"),
                                                                       Qt::CaseInsensitive) == 0;
                if (ours && was.compare(exe, Qt::CaseInsensitive) != 0) {
                    const std::wstring exeNative = QDir::toNativeSeparators(exe).toStdWString();
                    const std::wstring dirNative =
                        QDir::toNativeSeparators(QFileInfo(exe).absolutePath()).toStdWString();
                    link->SetPath(exeNative.c_str());
                    link->SetWorkingDirectory(dirNative.c_str());
                    link->SetIconLocation(exeNative.c_str(), 0);
                    if (SUCCEEDED(file->Save(nullptr, TRUE))) {
                        ++changed;
                        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, lnkPath.c_str(), nullptr);
                        wlog(QStringLiteral("install"),
                             QStringLiteral("shortcut %1 now opens this copy (it opened %2)")
                                 .arg(QFileInfo(lnk).fileName(), was));
                        stillUsed.insert(QFileInfo(exe).absolutePath().toLower());
                    } else {
                        stillUsed.insert(QFileInfo(was).absolutePath().toLower());
                    }
                } else if (ours) {
                    stillUsed.insert(QFileInfo(was).absolutePath().toLower());
                }
            }

            file->Release();
            link->Release();
        }
    }

    if (SUCCEEDED(com))
        CoUninitialize();

    if (changed > 0)
        wlog(QStringLiteral("install"), QStringLiteral("pointed %1 shortcut(s) at this copy").arg(changed));
    return stillUsed;
}

qint64 folderBytes(const QString &path)
{
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

#endif

} // namespace

namespace {
void tidy(const QString &exePath, const QStringList &folders, bool wait);
}

void InstallTidy::run()
{
#ifdef _WIN32
    tidy(QCoreApplication::applicationFilePath(), shortcutFolders(), false);
#endif
}

void InstallTidy::runWith(const QString &exePath, const QStringList &shortcutFolders)
{
#ifdef _WIN32
    tidy(exePath, shortcutFolders, true);
#else
    Q_UNUSED(exePath) Q_UNUSED(shortcutFolders)
#endif
}

namespace {

void tidy(const QString &exePath, const QStringList &folders, bool wait)
{
#ifdef _WIN32
    const QString exe = clean(exePath);
    const QString here = clean(QFileInfo(exe).absolutePath());

    QDir versions(here);
    if (!versions.cdUp() || versions.dirName().compare(QLatin1String("versions"), Qt::CaseInsensitive) != 0)
        return;
    QDir rootDir = versions;
    if (!rootDir.cdUp())
        return;
    const QString root = clean(rootDir.absolutePath());
    const QString versionsPath = clean(versions.absolutePath());

    // Shortcuts first. Nothing is removed while a shortcut might still lead
    // to it.
    const QSet<QString> stillUsed = repointShortcuts(root, exe, folders);

    const QVersionNumber current = QVersionNumber::fromString(QFileInfo(here).fileName());
    if (current.isNull())
        return;

    // Older copies, newest first. The two most recent stay, as somewhere to
    // go back to if this one misbehaves.
    QList<QPair<QVersionNumber, QString>> older;
    QStringList leftovers;
    const QStringList names = QDir(versionsPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : names) {
        if (name.endsWith(QLatin1String(".removing"))) {
            leftovers << name;
            continue;
        }
        const QVersionNumber v = QVersionNumber::fromString(name);
        if (v.isNull() || v.toString() != name || !(v < current))
            continue;
        older.append({v, name});
    }
    std::sort(older.begin(), older.end(),
              [](const auto &a, const auto &b) { return QVersionNumber::compare(a.first, b.first) > 0; });

    QStringList doomed = leftovers;
    for (int i = 2; i < older.size(); ++i) {
        const QString path = versionsPath + QLatin1Char('/') + older.at(i).second;
        if (stillUsed.contains(path.toLower())) {
            wlog(QStringLiteral("install"),
                 QStringLiteral("keeping %1: a shortcut still opens it").arg(older.at(i).second));
            continue;
        }
        doomed << older.at(i).second;
    }
    if (doomed.isEmpty())
        return;

    // Off the window's thread: thirty-odd folders of DLLs take a moment. The
    // global pool is waited for when the program closes, so this is never cut
    // off halfway with the log already gone.
    QThreadPool::globalInstance()->start([versionsPath, doomed]() {
        int removed = 0;
        qint64 freed = 0;
        for (const QString &name : doomed) {
            QDir dir(versionsPath);
            QString target = name;

            // Renamed before it is emptied. Windows will not rename a folder
            // whose program is running, so a copy that is still open fails
            // here, whole, instead of losing half its files. The ".removing"
            // name also means a removal cut short is finished next time.
            if (!name.endsWith(QLatin1String(".removing"))) {
                target = name + QStringLiteral(".removing");
                if (!dir.rename(name, target)) {
                    wlog(QStringLiteral("install"),
                         QStringLiteral("left %1 in place: it is in use").arg(name));
                    continue;
                }
            }
            const QString path = versionsPath + QLatin1Char('/') + target;
            const qint64 bytes = folderBytes(path);
            if (QDir(path).removeRecursively()) {
                ++removed;
                freed += bytes;
            }
        }
        if (removed > 0) {
            wlog(QStringLiteral("install"),
                 QStringLiteral("removed %1 old cop%2, %3 MB freed")
                     .arg(removed)
                     .arg(removed == 1 ? QStringLiteral("y") : QStringLiteral("ies"))
                     .arg(freed / (1024 * 1024)));
        }
    });
    if (wait)
        QThreadPool::globalInstance()->waitForDone();
#else
    Q_UNUSED(exePath) Q_UNUSED(folders) Q_UNUSED(wait)
#endif
}

} // namespace
