#pragma once

#include "core/MessageStore.h"

#include <QJsonObject>
#include <QString>
#include <QVariant>
#include <QWidget>

class RestClient;
class GatewayClient;
class PluginHost;

// Everything a plugin is allowed to touch. Plugins never reach for globals, so
// the host can decide later what to expose or take away.
class PluginContext
{
public:
    PluginContext(const QString &pluginId, RestClient *rest, GatewayClient *gateway, MessageStore *store,
                  PluginHost *host)
        : m_pluginId(pluginId)
        , m_rest(rest)
        , m_gateway(gateway)
        , m_store(store)
        , m_host(host)
    {
    }

    RestClient *rest() const { return m_rest; }
    GatewayClient *gateway() const { return m_gateway; }
    MessageStore *store() const { return m_store; }

    // Per plugin settings. Keys are namespaced by plugin id automatically.
    QVariant setting(const QString &key, const QVariant &fallback = {}) const;
    void setSetting(const QString &key, const QVariant &value);

    void log(const QString &line) const;

private:
    QString m_pluginId;
    RestClient *m_rest = nullptr;
    GatewayClient *m_gateway = nullptr;
    MessageStore *m_store = nullptr;
    PluginHost *m_host = nullptr;
};

// Base class for every built-in plugin.
//
// Hooks that return bool follow one rule: return false to cancel the action the
// client was about to take. The first plugin that says no wins.
class Plugin
{
public:
    virtual ~Plugin() = default;

    // Identity. The id is the settings key, so it must never change.
    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual QString description() const = 0;
    virtual bool enabledByDefault() const { return true; }

    // Lifecycle.
    virtual void onLoad(PluginContext *context) { m_context = context; }
    virtual void onUnload() { m_context = nullptr; }

    // Called for every gateway dispatch, whatever the type.
    virtual void onGatewayEvent(const QString &eventType, const QJsonObject &data) {
        Q_UNUSED(eventType) Q_UNUSED(data)
    }

    // Called right before a message leaves. Edit `content` in place to rewrite
    // it. Return false to cancel the send.
    virtual bool onOutgoingMessage(QString &content, const QString &channelId) {
        Q_UNUSED(content) Q_UNUSED(channelId)
        return true;
    }

    // Called before the typing indicator is sent. Return false to stay silent.
    virtual bool onBeforeTyping(const QString &channelId) {
        Q_UNUSED(channelId)
        return true;
    }

    // Called when Discord says a message was deleted. Return false to keep the
    // message on screen instead of dropping it.
    virtual bool onBeforeMessageDelete(const QString &channelId, const QString &messageId) {
        Q_UNUSED(channelId) Q_UNUSED(messageId)
        return true;
    }

    // Extra text appended to a message header when it is drawn, such as a
    // timestamp or a "deleted" tag. Return an empty string to add nothing.
    virtual QString decorateMessageHeader(const MessageInfo &message) {
        Q_UNUSED(message)
        return {};
    }

    // The same idea for a message that joined the block above it. Those rows
    // have no header, only the narrow strip left of the text, so keep it very
    // short. Return an empty string to add nothing.
    virtual QString decorateMessageGutter(const MessageInfo &message) {
        Q_UNUSED(message)
        return {};
    }

    // Optional settings page shown inside the Plugins window.
    virtual QWidget *createSettingsWidget(QWidget *parent) {
        Q_UNUSED(parent)
        return nullptr;
    }

protected:
    PluginContext *context() const { return m_context; }

private:
    PluginContext *m_context = nullptr;
};
