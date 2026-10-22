#pragma once

#include "plugin/Plugin.h"

#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class RestClient;
class GatewayClient;
class MessageStore;

// Owns every plugin, decides which ones are live, and runs the hooks.
//
// Plugins are compiled in rather than loaded from disk. That keeps the client
// a single trusted binary: nothing outside the build can inject code.
class PluginHost : public QObject
{
    Q_OBJECT

public:
    struct Entry
    {
        std::unique_ptr<Plugin> plugin;
        std::unique_ptr<PluginContext> context;
        bool enabled = false;
    };

    PluginHost(RestClient *rest, GatewayClient *gateway, MessageStore *store, QObject *parent = nullptr);
    ~PluginHost() override;

    void registerBuiltins();

    QList<Plugin *> plugins() const;
    bool isEnabled(const QString &pluginId) const;
    void setEnabled(const QString &pluginId, bool enabled);

    // Hook fan-out. Disabled plugins are skipped.
    void dispatchGatewayEvent(const QString &eventType, const QJsonObject &data);
    bool runOutgoingMessage(QString &content, const QString &channelId);
    bool runBeforeTyping(const QString &channelId);
    bool runBeforeMessageDelete(const QString &channelId, const QString &messageId);
    QString runDecorateHeader(const MessageInfo &message);
    QString runDecorateGutter(const MessageInfo &message);
    // Every enabled plugin's pictures for one name, in plugin order.
    QList<NameIcon> runNameIcons(const QString &userId, NamePlace place, int size, qreal ratio);

    // False when no enabled plugin will carry the file.
    bool runOversizedFile(const QString &path, Plugin::UploadProgress progress, Plugin::UploadDone done);

    // Runs inside the audio path, every 20 milliseconds, while a call is up.
    void runMicrophoneFrame(qint16 *samples, int frames, int channels, int sampleRate);

    void runServerRail(const QStringList &joinedGuildIds);

    // The window's server rail, lent to plugins through their context. The
    // window sets these once it exists; until then the rail is not ready.
    struct RailHooks
    {
        std::function<bool()> ready;
        std::function<QList<RailLayout::Entry>()> entries;
        std::function<void(const QList<RailLayout::Entry> &, const QString &)> replace;
    };
    void setRailHooks(RailHooks hooks) { m_railHooks = std::move(hooks); }

signals:
    void pluginLogged(const QString &pluginId, const QString &line);
    void notificationRequested(const QString &pluginId, const QString &title, const QString &text,
                               const QString &channelId);
    void pluginToggled(const QString &pluginId, bool enabled);
    void repaintRequested();
    // The pictures beside someone's name changed; empty for everyone's.
    void nameIconsChanged(const QString &userId);

private:
    friend class PluginContext;

    void add(std::unique_ptr<Plugin> plugin);
    Entry *find(const QString &pluginId);

    RestClient *m_rest = nullptr;
    GatewayClient *m_gateway = nullptr;
    MessageStore *m_store = nullptr;
    QList<Entry *> m_entries;
    RailHooks m_railHooks;

    // Held while a plugin is switched on or off, and tried - never waited
    // on - by the microphone path, which runs on the media thread. If the
    // window holds it, that one 20 ms slice goes out unprocessed rather than
    // the audio stopping to wait for a click to finish.
    QMutex m_audioGate;
};
