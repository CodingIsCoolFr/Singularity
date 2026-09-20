#pragma once

#include "plugin/Plugin.h"

// Marks people who look active even though Discord reports them offline.
//
// There is no way to read a hidden status: Discord simply sends nothing for
// someone who set themselves invisible. What it cannot hide is action. Typing,
// posting, and joining voice all need a client that is actually running, and
// those events still reach you.
//
// So this plugin watches for those signals and notes the time. A person who is
// invisible and doing nothing stays unseen, which is most of the point of
// being invisible.
class PresenceHintsPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("presence-hints"); }
    QString name() const override { return QStringLiteral("Presence hints"); }
    QString description() const override
    {
        return QStringLiteral("Marks people as active when they type, post or join voice, even if "
                              "Discord says they are offline. It cannot see someone who is "
                              "invisible and idle, and it only works where you can already see.");
    }

    // Off by default: this works against a choice the other person made.
    bool enabledByDefault() const override { return false; }

    void onLoad(PluginContext *context) override;
    void onUnload() override;
    void onGatewayEvent(const QString &eventType, const QJsonObject &data) override;
    QWidget *createSettingsWidget(QWidget *parent) override;

private:
    void note(const QString &userId, const QString &why);
    void announce(const QString &userId, const QString &why);

    QSet<QString> m_noted;
    QSet<QString> m_namesAsked;   // so an unknown person is looked up once
};
