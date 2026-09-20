#pragma once

#include "plugin/Plugin.h"

#include <QMap>

// Rewrites short codes as you send them, so ":shrug:" becomes the real thing.
class QuickTextPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("quick-text"); }
    QString name() const override { return QStringLiteral("Quick text"); }
    QString description() const override
    {
        return QStringLiteral("Swaps short codes for longer text when you send a message.");
    }
    bool enabledByDefault() const override { return true; }

    void onLoad(PluginContext *context) override;
    bool onOutgoingMessage(QString &content, const QString &channelId) override;
    QWidget *createSettingsWidget(QWidget *parent) override;

private:
    void loadRules();
    void saveRules();
    static QMap<QString, QString> defaultRules();

    QMap<QString, QString> m_rules;
};
