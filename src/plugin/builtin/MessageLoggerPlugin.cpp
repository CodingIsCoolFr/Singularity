#include "plugin/builtin/MessageLoggerPlugin.h"

#include <QJsonArray>

void MessageLoggerPlugin::onUnload()
{
    m_deletedIds.clear();
    Plugin::onUnload();
}

void MessageLoggerPlugin::onGatewayEvent(const QString &eventType, const QJsonObject &data)
{
    // Bulk deletes arrive as one event holding many ids.
    if (eventType != QLatin1String("MESSAGE_DELETE_BULK"))
        return;

    const QString channelId = data.value(QStringLiteral("channel_id")).toString();
    const QJsonArray ids = data.value(QStringLiteral("ids")).toArray();
    for (const QJsonValue &value : ids) {
        const QString messageId = value.toString();
        if (messageId.isEmpty())
            continue;
        m_deletedIds.insert(messageId);
        if (context() && context()->store())
            context()->store()->markDeleted(channelId, messageId);
    }

    if (context())
        context()->log(QStringLiteral("kept %1 bulk deleted messages").arg(ids.size()));
}

bool MessageLoggerPlugin::onBeforeMessageDelete(const QString &channelId, const QString &messageId)
{
    Q_UNUSED(channelId)
    m_deletedIds.insert(messageId);
    // Returning false stops the client from dropping the message.
    return false;
}

QString MessageLoggerPlugin::decorateMessageHeader(const MessageInfo &message)
{
    QString result;
    if (message.deleted || m_deletedIds.contains(message.id))
        result += QStringLiteral("<span class=\"tag-deleted\">deleted</span>");
    if (message.edited)
        result += QStringLiteral("<span class=\"tag-edited\">edited</span>");
    return result;
}
