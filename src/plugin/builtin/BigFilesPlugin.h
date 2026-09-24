#pragma once

#include "plugin/Plugin.h"

#include <QList>
#include <QPointer>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

// Files too big for Discord go to GoFile instead, and the message gets the
// download link.
//
// Discord's size limit is checked on Discord's servers, so no client can lift
// it. What a client can do is carry the file somewhere else and send a link,
// which is all this does. Off by default: it hands your file to another
// company, so turning it on is you saying yes to that.
class BigFilesPlugin : public Plugin
{
public:
    BigFilesPlugin();
    ~BigFilesPlugin() override;

    QString id() const override { return QStringLiteral("big-files"); }
    QString name() const override { return QStringLiteral("Big files"); }
    QString description() const override
    {
        return QStringLiteral("Files too big for Discord are uploaded to GoFile, and your message gets "
                              "the download link instead.");
    }
    bool enabledByDefault() const override { return false; }

    void onLoad(PluginContext *context) override;
    void onUnload() override;
    bool onOversizedFile(const QString &path, UploadProgress progress, UploadDone done) override;
    QWidget *createSettingsWidget(QWidget *parent) override;

private:
    std::unique_ptr<QNetworkAccessManager> m_network;
    QList<QPointer<QNetworkReply>> m_uploads;
};
