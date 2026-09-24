#pragma once

#include "core/AppConfig.h"
#include "core/MessageStore.h"

#include <QJsonObject>
#include <QString>
#include <QVariant>
#include <QWidget>

#include <functional>

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

    // A Windows notification, like the ones Discord shows. Clicking it brings
    // Singularity forward.
    void notify(const QString &title, const QString &text) const;

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

    // Called with one 20 millisecond slice of your microphone, before it is
    // encoded and sent, so a plugin can change how you sound to other people.
    //
    // `samples` is interleaved: left, right, left, right. `frames` counts
    // positions in time, so the array holds `frames * channels` numbers.
    // Editing it in place is the point.
    //
    // Three rules, because this runs inside the audio path:
    //
    //   - Be quick. The whole slice is 20 milliseconds long and another one
    //     arrives every 20 milliseconds. Work that overruns is heard as
    //     stuttering, not as lateness.
    //   - Never allocate, lock, or wait here.
    //   - Keep values inside the range a 16 bit number can hold. Going past it
    //     wraps around, which sounds like tearing rather than loudness.
    virtual void onMicrophoneFrame(qint16 *samples, int frames, int channels, int sampleRate) {
        Q_UNUSED(samples) Q_UNUSED(frames) Q_UNUSED(channels) Q_UNUSED(sampleRate)
    }

    // A file too big for Discord to take. A plugin that can carry it some other
    // way returns true, then calls `done` exactly once: with a link to the file,
    // or with an empty link and a reason. `progress` may be called on the way.
    // Return false to leave the file alone. The first plugin that says yes wins.
    using UploadProgress = std::function<void(qint64 sent, qint64 total)>;
    using UploadDone = std::function<void(const QString &link, const QString &error)>;
    virtual bool onOversizedFile(const QString &path, UploadProgress progress, UploadDone done) {
        Q_UNUSED(path) Q_UNUSED(progress) Q_UNUSED(done)
        return false;
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

    // Settings that work whether the plugin is running or not.
    //
    // The context only exists while a plugin is loaded, so reading settings
    // through it means a disabled plugin sees none of its own saved values and
    // silently throws away any change made to them. Its settings page is still
    // on screen while it is off, so that is exactly when somebody sets it up.
    // These go straight to the config file instead.
    QVariant settingValue(const QString &key, const QVariant &fallback = {}) const
    {
        return AppConfig::instance().pluginValue(id(), key, fallback);
    }

    void setSettingValue(const QString &key, const QVariant &value)
    {
        AppConfig::instance().setPluginValue(id(), key, value);
    }

private:
    PluginContext *m_context = nullptr;
};
