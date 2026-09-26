#pragma once

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkReply;

// Checks whether a newer Singularity has been published, and fetches it.
//
// It asks a separate, public repository rather than the source one. The source
// is private, and a private repository answers 404 to anyone without a token,
// so an updater pointed at it would need a token shipped inside the program.
// Anything shipped inside a program can be taken out of it, and that token
// would grant read access to the whole source. A public channel carrying only
// the installers costs nothing and gives nothing away.
//
// Nothing is installed without being asked. The check is quiet, the download
// is not started until somebody agrees to it, and the installer is not run
// until it has finished and been looked at.
class Updater : public QObject
{
    Q_OBJECT

public:
    explicit Updater(QObject *parent = nullptr);

    // Where releases are published. Public on purpose; see above.
    static QString channel() { return QStringLiteral("CodingIsCoolFr/singularity-updates"); }

    // `quiet` keeps silent when already up to date, which is what a check on
    // startup should do. A check somebody asked for should say either way.
    void check(bool quiet);

    // Fetches the installer found by the last check.
    void download();

    QString latestVersion() const { return m_latestVersion; }
    bool busy() const { return m_reply != nullptr; }

    // A check that has been waiting over half a minute. The next check()
    // abandons it and starts again.
    bool stuck() const
    {
        return m_reply && m_replyIsCheck && m_replyAge.isValid() && m_replyAge.elapsed() > 30000;
    }

signals:
    void updateAvailable(const QString &version, const QString &notes, qint64 bytes);
    void upToDate();
    // Every check ends with this, quiet or not, found or not - after
    // updateAvailable when there is one. The startup window waits on it.
    void checkFinished(bool updateFound);
    void progress(qint64 received, qint64 total);
    void readyToInstall(const QString &installerPath);
    void failed(const QString &reason);

private:
    void onCheckFinished(QNetworkReply *reply, bool quiet);
    void onDownloadFinished(QNetworkReply *reply);

    // Only addresses GitHub actually serves releases from are followed. A
    // release body is text from a web service, and a download address taken
    // out of one is not to be trusted simply because of where it was found.
    static bool isTrustedHost(const QUrl &url);

    // Compares "0.1.10" against "0.1.9" as numbers rather than as text, where
    // the former sorts first and the update never appears.
    static bool isNewer(const QString &candidate, const QString &current);

    QNetworkAccessManager m_network;
    QNetworkReply *m_reply = nullptr;

    // Whether the request in flight is a check (small, quick) or the
    // installer download (large, slow), and how long it has been going. A
    // check that has gone quiet for long is abandoned rather than waited on:
    // one lost reply used to block every later check, including the menu's.
    bool m_replyIsCheck = false;
    QElapsedTimer m_replyAge;

    QString m_latestVersion;
    QString m_notes;
    QUrl m_assetUrl;
    qint64 m_assetSize = 0;
};
