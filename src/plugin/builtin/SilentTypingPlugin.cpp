#include "plugin/builtin/SilentTypingPlugin.h"

bool SilentTypingPlugin::onBeforeTyping(const QString &channelId)
{
    Q_UNUSED(channelId)
    // Returning false cancels the outgoing typing request.
    return false;
}
