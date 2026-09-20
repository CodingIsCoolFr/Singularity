#include "core/TokenStore.h"

#include "core/Logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
// dpapi.h has to come after windows.h.
#include <dpapi.h>
#endif

namespace TokenStore {

namespace {

// Sealing ties the bytes to this Windows account. Nothing here protects
// against the account's own owner, which is the right level: it stops another
// user on the machine, and stops the file being useful if it is copied away.
QByteArray seal(const QByteArray &plain)
{
#ifdef Q_OS_WIN
    DATA_BLOB in{static_cast<DWORD>(plain.size()),
                 reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()))};
    DATA_BLOB out{};

    if (!CryptProtectData(&in, L"Singularity session", nullptr, nullptr, nullptr, 0, &out)) {
        wlog(QStringLiteral("token"), QStringLiteral("could not seal the token, error %1")
                                          .arg(static_cast<uint>(GetLastError())));
        return {};
    }

    const QByteArray sealed(reinterpret_cast<const char *>(out.pbData), static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return sealed;
#else
    return plain;
#endif
}

QByteArray unseal(const QByteArray &sealed)
{
#ifdef Q_OS_WIN
    DATA_BLOB in{static_cast<DWORD>(sealed.size()),
                 reinterpret_cast<BYTE *>(const_cast<char *>(sealed.constData()))};
    DATA_BLOB out{};

    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        // Normal after a Windows account change, or if the file was copied
        // from another machine. Not worth alarming anyone about.
        wlog(QStringLiteral("token"),
             QStringLiteral("the stored token could not be read back, so signing in again"));
        return {};
    }

    const QByteArray plain(reinterpret_cast<const char *>(out.pbData), static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return plain;
#else
    return sealed;
#endif
}

} // namespace

QString filePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + QStringLiteral("/session.dat");
}

QString load()
{
    QFile file(filePath());
    if (!file.exists())
        return {};

    if (!file.open(QIODevice::ReadOnly)) {
        wlog(QStringLiteral("token"), QStringLiteral("could not read %1: %2")
                                          .arg(filePath(), file.errorString()));
        return {};
    }

    return QString::fromUtf8(unseal(file.readAll()));
}

bool save(const QString &token)
{
    if (token.isEmpty()) {
        clear();
        return true;
    }

    const QString path = filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    const QByteArray sealed = seal(token.toUtf8());
    if (sealed.isEmpty())
        return false;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        wlog(QStringLiteral("token"), QStringLiteral("could not open %1: %2")
                                          .arg(path, file.errorString()));
        return false;
    }

    file.write(sealed);
    if (!file.commit()) {
        wlog(QStringLiteral("token"), QStringLiteral("could not finish writing %1: %2")
                                          .arg(path, file.errorString()));
        return false;
    }

    // Read it straight back. A save that quietly does nothing is the exact bug
    // this file exists to end.
    if (load() != token) {
        wlog(QStringLiteral("token"), QStringLiteral("wrote the token but it did not read back"));
        return false;
    }

    wlog(QStringLiteral("token"), QStringLiteral("saved, so the next start signs in by itself"));
    return true;
}

void clear()
{
    QFile file(filePath());
    if (file.exists() && !file.remove()) {
        wlog(QStringLiteral("token"), QStringLiteral("could not delete %1: %2")
                                          .arg(filePath(), file.errorString()));
    }
}

} // namespace TokenStore
