#pragma once

#include "plugin/Plugin.h"

#include <QSet>

// Keeps deleted messages on screen instead of letting them vanish, and marks
// edited messages so you can see something changed.
class MessageLoggerPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("message-logger"); }
    QString name() const override { return QStringLiteral("Message logger"); }
    QString description() const override
    {
        return QStringLiteral("Keeps deleted messages visible and tags edited ones.");
    }
    bool enabledByDefault() const override { return true; }

    void onUnload() override;
    void onGatewayEvent(const QString &eventType, const QJsonObject &data) override;
    bool onBeforeMessageDelete(const QString &channelId, const QString &messageId) override;
    QString decorateMessageHeader(const MessageInfo &message) override;
    // Deleted and edited tags matter just as much on a grouped row.
    QString decorateMessageGutter(const MessageInfo &message) override
    {
        return decorateMessageHeader(message);
    }

private:
    QSet<QString> m_deletedIds;
};
