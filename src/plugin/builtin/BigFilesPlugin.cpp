#include "plugin/builtin/BigFilesPlugin.h"

#include "core/Logger.h"
#include "ui/Theme.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QVBoxLayout>

namespace {

// GoFile's upload address for guests. No account, no key: the reply names a
// fresh public folder with the file in it. Checked against the live service on
// 2026-09-24 - a POST of one "file" form field returns
// {"status":"ok","data":{"downloadPage":"https://gofile.io/d/...", ...}}.
const char *const UploadUrl = "https://upload.gofile.io/uploadfile";

constexpr qint64 MiB = 1024 * 1024;

} // namespace

BigFilesPlugin::BigFilesPlugin() = default;
BigFilesPlugin::~BigFilesPlugin() = default;

void BigFilesPlugin::onLoad(PluginContext *context)
{
    Plugin::onLoad(context);
    m_network = std::make_unique<QNetworkAccessManager>();
}

void BigFilesPlugin::onUnload()
{
    // Switched off halfway through an upload: stop it. Each one reports back
    // as cancelled on its own, so nobody waits for a link that is not coming.
    // A copy, because each reply takes itself off the list as it finishes.
    const QList<QPointer<QNetworkReply>> running = m_uploads;
    for (const QPointer<QNetworkReply> &reply : running) {
        if (reply)
            reply->abort();
    }
    m_uploads.clear();
    m_network.reset();
    Plugin::onUnload();
}

bool BigFilesPlugin::onOversizedFile(const QString &path, UploadProgress progress, UploadDone done)
{
    if (!m_network)
        return false;

    const QFileInfo info(path);
    const QString name = info.fileName();

    auto *file = new QFile(path);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        done(QString(), QStringLiteral("the file could not be opened"));
        return true;
    }

    // The file is read straight from disk as it goes, never loaded whole, so a
    // two gigabyte modpack costs the same memory as a small one.
    auto *form = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart part;

    // Written as UTF-8 by hand. Qt's own header setter squeezes the name into
    // Latin-1, which mangles any name that is not plain English. A quote would
    // end the value early, so those become underscores.
    QString safeName = name;
    safeName.replace(QLatin1Char('"'), QLatin1Char('_'));
    safeName.replace(QLatin1Char('\\'), QLatin1Char('_'));
    part.setRawHeader("Content-Disposition",
                      QByteArray("form-data; name=\"file\"; filename=\"") + safeName.toUtf8() + '"');
    part.setRawHeader("Content-Type", "application/octet-stream");
    part.setBodyDevice(file);
    file->setParent(form);
    form->append(part);

    QNetworkRequest request{QUrl(QString::fromLatin1(UploadUrl))};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Singularity/%1").arg(QCoreApplication::applicationVersion()));

    // Gives up after a whole minute with no bytes moving, not after a minute
    // in total. A big upload on home internet can take a long time and still
    // be perfectly healthy.
    request.setTransferTimeout(60000);

    QNetworkReply *reply = m_network->post(request, form);
    form->setParent(reply);
    m_uploads.append(reply);

    wlog(QStringLiteral("bigfiles"),
         QStringLiteral("uploading %1 (%2 MB) to GoFile").arg(name).arg(info.size() / MiB));

    if (progress) {
        QObject::connect(reply, &QNetworkReply::uploadProgress, reply,
                         [progress](qint64 sent, qint64 total) { progress(sent, total); });
    }

    QObject::connect(reply, &QNetworkReply::finished, reply, [this, reply, name, done]() {
        m_uploads.removeAll(QPointer<QNetworkReply>(reply));
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            const QString why = reply->error() == QNetworkReply::OperationCanceledError
                                    ? QStringLiteral("the upload was stopped")
                                    : reply->errorString();
            wlog(QStringLiteral("bigfiles"), QStringLiteral("upload of %1 failed: %2").arg(name, why));
            done(QString(), why);
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QString status = root.value(QStringLiteral("status")).toString();
        const QString link =
            root.value(QStringLiteral("data")).toObject().value(QStringLiteral("downloadPage")).toString();

        // Only ever a GoFile page. Whatever else came back is not put into a
        // message you are about to send.
        if (status != QLatin1String("ok") || !link.startsWith(QLatin1String("https://gofile.io/"))) {
            const QString why = QStringLiteral("GoFile did not accept it (%1)")
                                    .arg(status.isEmpty() ? QStringLiteral("no answer") : status);
            wlog(QStringLiteral("bigfiles"), QStringLiteral("upload of %1: %2").arg(name, why));
            done(QString(), why);
            return;
        }

        wlog(QStringLiteral("bigfiles"), QStringLiteral("uploaded %1: %2").arg(name, link));
        done(link, QString());
    });

    return true;
}

QWidget *BigFilesPlugin::createSettingsWidget(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *how = new QLabel(
        QStringLiteral("Drop or attach a file as usual. If it is bigger than Discord lets you send, "
                       "it goes to GoFile instead. When it is done, the download link appears in your "
                       "message box. Press Enter to send it.\n\n"
                       "Files that fit inside Discord's limit still go to Discord, like normal."),
        page);
    how->setWordWrap(true);
    layout->addWidget(how);

    auto *note = new QLabel(
        QStringLiteral("GoFile is a free file host run by another company. No account is needed. "
                       "Anyone with the link can download the file. GoFile deletes files that nobody "
                       "has downloaded for a while. A folder has to be zipped first: right-click it, "
                       "then Send to > Compressed (zipped) folder."),
        page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(QLatin1String(Theme::TextFaint)));
    layout->addWidget(note);

    layout->addStretch(1);
    return page;
}
