#pragma once

#include "plugin/Plugin.h"

#include <QList>
#include <QObject>
#include <QString>

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

    // Runs inside the audio path, every 20 milliseconds, while a call is up.
    void runMicrophoneFrame(qint16 *samples, int frames, int channels, int sampleRate);

signals:
    void pluginLogged(const QString &pluginId, const QString &line);
    void pluginToggled(const QString &pluginId, bool enabled);
    void repaintRequested();

private:
    friend class PluginContext;

    void add(std::unique_ptr<Plugin> plugin);
    Entry *find(const QString &pluginId);

    RestClient *m_rest = nullptr;
    GatewayClient *m_gateway = nullptr;
    MessageStore *m_store = nullptr;
    QList<Entry *> m_entries;
};
