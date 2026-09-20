#pragma once

#include "plugin/Plugin.h"

// Stops the client from telling a channel that you are typing.
class SilentTypingPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("silent-typing"); }
    QString name() const override { return QStringLiteral("Silent typing"); }
    QString description() const override
    {
        return QStringLiteral("Never sends the \"is typing...\" signal to other people.");
    }
    bool enabledByDefault() const override { return false; }

    bool onBeforeTyping(const QString &channelId) override;
};
