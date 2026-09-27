#include "core/Updater.h"

#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>

namespace {

// GitHub serves the release list from one host and the files themselves from
// another, and follows redirects between them.
bool isGitHubHost(const QString &host)
{
    const QString lower = host.toLower();
    return lower == QLatin1String("api.github.com")
        || lower == QLatin1String("github.com")
        || lower == QLatin1String("objects.githubusercontent.com")
        || lower == QLatin1String("release-assets.githubusercontent.com");
}

// Splits "v0.1.10" into 0, 1, 10.
//
// Each piece stops at the first character that is not a digit, so a tag like
// "1.2.0-beta" reads as 1.2.0 rather than being refused outright.
QList<int> versionParts(const QString &text)
{
    QString cleaned = text.trimmed();
    if (cleaned.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        cleaned.remove(0, 1);

    QList<int> parts;
    for (const QString &piece : cleaned.split(QLatin1Char('.'))) {
        QString digits;
        for (const QChar c : piece) {
            if (!c.isDigit())
                break;
            digits.append(c);
        }
        parts.append(digits.isEmpty() ? 0 : digits.toInt());
    }
    return parts;
}

} // namespace

Updater::Updater(QObject *parent)
    : QObject(parent)
{
    m_network.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

bool Updater::isTrustedHost(const QUrl &url)
{
    return url.scheme() == QLatin1String("https") && isGitHubHost(url.host());
}

bool Updater::isNewer(const QString &candidate, const QString &current)
{
    const QList<int> a = versionParts(candidate);
    const QList<int> b = versionParts(current);

    for (int i = 0; i < qMax(a.size(), b.size()); ++i) {
        const int left = i < a.size() ? a.at(i) : 0;
        const int right = i < b.size() ? b.at(i) : 0;
        if (left != right)
            return left > right;
    }
    return false;
}

void Updater::check(bool quiet)
{
    if (m_reply) {
        // A check that has been going for more than half a minute is not
        // coming back. Before this, one lost reply at 13:43 left every check
        // after it - the two-minute ones and the menu's - silently skipped
        // for the rest of the session. Abandon it and ask again. Aborting
        // runs its finished handler, which clears m_reply.
        if (m_replyIsCheck && m_replyAge.isValid() && m_replyAge.elapsed() > 30000) {
            wlog(QStringLiteral("update"),
                 QStringLiteral("the last check never answered (%1 s); abandoning it")
                     .arg(m_replyAge.elapsed() / 1000));
            m_reply->abort();
        }
        if (m_reply)
            return;
    }

    const QUrl url(QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(channel()));

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "Singularity");

    // Ten seconds with nothing arriving and Qt gives up on its own; the check
    // is then asked once more on a new connection. The reply is a few
    // kilobytes, and a healthy one takes well under a second.
    request.setTransferTimeout(10000);

    m_replyIsCheck = true;
    m_replyAge.start();
    m_reply = m_network.get(request);
    connect(m_reply, &QNetworkReply::finished, this, [this, quiet]() {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->deleteLater();
        onCheckFinished(reply, quiet);
    });
}

void Updater::onCheckFinished(QNetworkReply *reply, bool quiet)
{
    // One check in a row of quick ones sat for the full twenty seconds and
    // failed (27 September, 09:44), while GitHub answered the others in well
    // under a second. That is a kept-alive connection that had quietly died.
    // Ask once more on a fresh connection before saying anything.
    const QNetworkReply::NetworkError error = reply->error();
    const bool timedOut = error == QNetworkReply::OperationCanceledError || error == QNetworkReply::TimeoutError;
    if (timedOut && !m_checkRetried) {
        wlog(QStringLiteral("update"), QStringLiteral("check timed out; asking again on a new connection"));
        m_network.clearConnectionCache();
        m_checkRetried = true;
        check(quiet);
        return;
    }
    if (error == QNetworkReply::NoError)
        m_checkRetried = false;

    if (error != QNetworkReply::NoError) {
        m_checkRetried = false;
        wlog(QStringLiteral("update"), QStringLiteral("check failed: %1").arg(reply->errorString()));
        if (!quiet)
            emit failed(reply->errorString());
        emit checkFinished(false);
        return;
    }

    const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
    const QString tag = release.value(QStringLiteral("tag_name")).toString();
    if (tag.isEmpty()) {
        if (!quiet)
            emit failed(QStringLiteral("The update channel returned nothing usable."));
        emit checkFinished(false);
        return;
    }

    const QString current = QCoreApplication::applicationVersion();
    if (!isNewer(tag, current)) {
        wlog(QStringLiteral("update"),
             QStringLiteral("latest is %1, running %2, nothing to do").arg(tag, current));
        if (!quiet)
            emit upToDate();
        emit checkFinished(false);
        return;
    }

    // Find the Windows installer among the attached files.
    m_assetUrl.clear();
    m_assetSize = 0;
    const QJsonArray assets = release.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        if (!name.endsWith(QLatin1String("-setup.exe"), Qt::CaseInsensitive))
            continue;

        const QUrl candidate(asset.value(QStringLiteral("browser_download_url")).toString());
        // The address comes out of a web service's reply, so it is checked
        // rather than trusted.
        if (!isTrustedHost(candidate))
            continue;

        m_assetUrl = candidate;
        m_assetSize = static_cast<qint64>(asset.value(QStringLiteral("size")).toDouble());
        break;
    }

    if (m_assetUrl.isEmpty()) {
        if (!quiet)
            emit failed(QStringLiteral("Version %1 exists but has no installer attached.").arg(tag));
        emit checkFinished(false);
        return;
    }

    m_latestVersion = tag;
    m_notes = release.value(QStringLiteral("body")).toString();

    wlog(QStringLiteral("update"), QStringLiteral("version %1 is available, running %2").arg(tag, current));
    emit updateAvailable(tag, m_notes, m_assetSize);
    emit checkFinished(true);
}

void Updater::download()
{
    if (m_reply || m_assetUrl.isEmpty())
        return;

    QNetworkRequest request(m_assetUrl);
    request.setRawHeader("User-Agent", "Singularity");

    // Gives up after a whole minute with nothing arriving, not after a minute
    // in total: a 55 MB installer on a slow line is still healthy at minute
    // three, but one that has stopped moving is never going to finish.
    request.setTransferTimeout(60000);

    m_replyIsCheck = false;
    m_replyAge.start();
    m_reply = m_network.get(request);
    connect(m_reply, &QNetworkReply::downloadProgress, this, &Updater::progress);
    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->deleteLater();
        onDownloadFinished(reply);
    });
}

void Updater::onDownloadFinished(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        emit failed(reply->errorString());
        return;
    }

    // Redirects are followed, so where it actually came from is checked as
    // well as where it was asked for.
    if (!isTrustedHost(reply->url())) {
        emit failed(QStringLiteral("The download was redirected somewhere unexpected."));
        return;
    }

    const QByteArray payload = reply->readAll();

    // A truncated installer is worse than no installer: it runs, fails part
    // way through, and leaves a half replaced program behind.
    if (m_assetSize > 0 && payload.size() != m_assetSize) {
        emit failed(QStringLiteral("The download was %1 bytes but should have been %2.")
                        .arg(payload.size())
                        .arg(m_assetSize));
        return;
    }

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString path = QDir(dir).filePath(
        QStringLiteral("Singularity-%1-setup.exe").arg(m_latestVersion));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        emit failed(QStringLiteral("Could not write to %1.").arg(path));
        return;
    }
    file.write(payload);
    file.close();

    wlog(QStringLiteral("update"), QStringLiteral("downloaded %1 to %2").arg(m_latestVersion, path));
    emit readyToInstall(path);
}
