#include "plugin/PluginHost.h"

#include "core/AppConfig.h"
#include "plugin/builtin/MessageLoggerPlugin.h"
#include "plugin/builtin/AnonymousPlugin.h"
#include "plugin/builtin/BigFilesPlugin.h"
#include "plugin/builtin/FakeNitroPlugin.h"
#include "plugin/builtin/MicShaperPlugin.h"
#include "plugin/builtin/NitroWatchPlugin.h"
#include "plugin/builtin/PresenceHintsPlugin.h"
#include "plugin/builtin/QuickTextPlugin.h"
#include "plugin/builtin/RelationshipNotifierPlugin.h"
#include "plugin/builtin/SilentTypingPlugin.h"
#include "plugin/builtin/TimestampsPlugin.h"

// ---------------------------------------------------------------------------
// PluginContext
// ---------------------------------------------------------------------------

QVariant PluginContext::setting(const QString &key, const QVariant &fallback) const
{
    return AppConfig::instance().pluginValue(m_pluginId, key, fallback);
}

void PluginContext::setSetting(const QString &key, const QVariant &value)
{
    AppConfig::instance().setPluginValue(m_pluginId, key, value);
}

void PluginContext::log(const QString &line) const
{
    if (m_host)
        emit m_host->pluginLogged(m_pluginId, line);
}

void PluginContext::notify(const QString &title, const QString &text) const
{
    if (m_host)
        emit m_host->notificationRequested(m_pluginId, title, text);
}

// ---------------------------------------------------------------------------
// PluginHost
// ---------------------------------------------------------------------------

PluginHost::PluginHost(RestClient *rest, GatewayClient *gateway, MessageStore *store, QObject *parent)
    : QObject(parent)
    , m_rest(rest)
    , m_gateway(gateway)
    , m_store(store)
{
}

PluginHost::~PluginHost()
{
    for (Entry *entry : m_entries) {
        if (entry->enabled)
            entry->plugin->onUnload();
        delete entry;
    }
    m_entries.clear();
}

void PluginHost::registerBuiltins()
{
    add(std::make_unique<TimestampsPlugin>());
    add(std::make_unique<SilentTypingPlugin>());
    add(std::make_unique<MessageLoggerPlugin>());
    add(std::make_unique<QuickTextPlugin>());
    add(std::make_unique<PresenceHintsPlugin>());
    add(std::make_unique<AnonymousPlugin>());
    add(std::make_unique<NitroWatchPlugin>());
    add(std::make_unique<MicShaperPlugin>());
    add(std::make_unique<RelationshipNotifierPlugin>());
    add(std::make_unique<BigFilesPlugin>());
    add(std::make_unique<FakeNitroPlugin>());
}

void PluginHost::add(std::unique_ptr<Plugin> plugin)
{
    Entry *entry = new Entry;
    const QString pluginId = plugin->id();

    entry->context = std::make_unique<PluginContext>(pluginId, m_rest, m_gateway, m_store, this);
    entry->plugin = std::move(plugin);
    entry->enabled = AppConfig::instance().pluginEnabled(pluginId, entry->plugin->enabledByDefault());

    if (entry->enabled)
        entry->plugin->onLoad(entry->context.get());

    m_entries.append(entry);
}

PluginHost::Entry *PluginHost::find(const QString &pluginId)
{
    for (Entry *entry : m_entries) {
        if (entry->plugin->id() == pluginId)
            return entry;
    }
    return nullptr;
}

QList<Plugin *> PluginHost::plugins() const
{
    QList<Plugin *> result;
    result.reserve(m_entries.size());
    for (Entry *entry : m_entries)
        result.append(entry->plugin.get());
    return result;
}

bool PluginHost::isEnabled(const QString &pluginId) const
{
    for (Entry *entry : m_entries) {
        if (entry->plugin->id() == pluginId)
            return entry->enabled;
    }
    return false;
}

void PluginHost::setEnabled(const QString &pluginId, bool enabled)
{
    Entry *entry = find(pluginId);
    if (!entry || entry->enabled == enabled)
        return;

    {
        QMutexLocker gate(&m_audioGate);
        entry->enabled = enabled;
        if (enabled)
            entry->plugin->onLoad(entry->context.get());
        else
            entry->plugin->onUnload();
    }

    AppConfig::instance().setPluginEnabled(pluginId, enabled);
    AppConfig::instance().sync();

    emit pluginToggled(pluginId, enabled);
    emit repaintRequested();
}

void PluginHost::dispatchGatewayEvent(const QString &eventType, const QJsonObject &data)
{
    for (Entry *entry : m_entries) {
        if (entry->enabled)
            entry->plugin->onGatewayEvent(eventType, data);
    }
}

bool PluginHost::runOutgoingMessage(QString &content, const QString &channelId)
{
    for (Entry *entry : m_entries) {
        if (!entry->enabled)
            continue;
        if (!entry->plugin->onOutgoingMessage(content, channelId))
            return false;
    }
    return true;
}

bool PluginHost::runOversizedFile(const QString &path, Plugin::UploadProgress progress,
                                  Plugin::UploadDone done)
{
    for (Entry *entry : m_entries) {
        if (entry->enabled && entry->plugin->onOversizedFile(path, progress, done))
            return true;
    }
    return false;
}

bool PluginHost::runBeforeTyping(const QString &channelId)
{
    for (Entry *entry : m_entries) {
        if (!entry->enabled)
            continue;
        if (!entry->plugin->onBeforeTyping(channelId))
            return false;
    }
    return true;
}

bool PluginHost::runBeforeMessageDelete(const QString &channelId, const QString &messageId)
{
    for (Entry *entry : m_entries) {
        if (!entry->enabled)
            continue;
        if (!entry->plugin->onBeforeMessageDelete(channelId, messageId))
            return false;
    }
    return true;
}

QString PluginHost::runDecorateHeader(const MessageInfo &message)
{
    QString result;
    for (Entry *entry : m_entries) {
        if (!entry->enabled)
            continue;
        const QString part = entry->plugin->decorateMessageHeader(message);
        if (part.isEmpty())
            continue;
        // Qt's rich text engine ignores margins on inline spans, so the gap
        // has to be a real space or the tags run into the author name.
        result += QStringLiteral("&#160;&#160;") + part;
    }
    return result;
}

void PluginHost::runMicrophoneFrame(qint16 *samples, int frames, int channels, int sampleRate)
{
    // No logging and nothing clever in this loop. It runs fifty times a second
    // inside the audio path, and anything slow here is heard rather than read.
    //
    // It runs on the media thread now, not the window's, so a plugin being
    // switched on or off at the same moment would be changed underneath it.
    // Tried rather than waited for: audio never blocks on a click.
    if (!m_audioGate.tryLock())
        return;
    for (Entry *entry : m_entries) {
        if (entry->enabled)
            entry->plugin->onMicrophoneFrame(samples, frames, channels, sampleRate);
    }
    m_audioGate.unlock();
}

QString PluginHost::runDecorateGutter(const MessageInfo &message)
{
    QString result;
    for (Entry *entry : m_entries) {
        if (!entry->enabled)
            continue;
        const QString part = entry->plugin->decorateMessageGutter(message);
        if (!part.isEmpty())
            result += part;
    }
    return result;
}
