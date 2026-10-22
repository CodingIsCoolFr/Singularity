#pragma once

#include "plugin/Plugin.h"

#include <QHash>
#include <QImage>
#include <QJsonArray>

// Shows which devices someone is on - desktop, phone, browser, console or
// VR - as small icons in the colour of their status there, the way the
// PlatformIndicators plugins for Vencord (kemo, sunnie, Nuckyz and V) and
// BetterDiscord (Strencher) do: after their name in the member list, direct
// messages, friends list and chat, after their badges in the profile window,
// and on their picture in place of the status dot.
//
// Everyone else's devices are Discord's client_status, which comes with each
// presence. Your own are your sessions (READY's "sessions", then
// SESSIONS_REPLACE), because Discord sends no presence for yourself; each
// device shows the best status of its sessions, online over idle over the
// rest, as Vencord works it out.
class PlatformIndicatorsPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("platform-indicators"); }
    QString name() const override { return QStringLiteral("Platform indicators"); }
    QString description() const override
    {
        return QStringLiteral("Shows which devices people are on - desktop, phone, browser, console or VR - "
                              "next to their names, by their profile badges and on their pictures, each in "
                              "the colour of their status there. After the Vencord and BetterDiscord plugins "
                              "of the same name.");
    }

    void onLoad(PluginContext *context) override;
    void onUnload() override;
    void onGatewayEvent(const QString &eventType, const QJsonObject &data) override;
    QList<NameIcon> nameIcons(const QString &userId, NamePlace place, int size, qreal ratio) override;
    QWidget *createSettingsWidget(QWidget *parent) override;

    // Discord's platforms, in the order their icons are drawn.
    struct Platform
    {
        const char *id;     // client_status key
        const char *name;   // what the tooltip and the settings call it
    };
    static const QList<Platform> &platforms();

    // Where icons can go, each with its own switch, and the three groups of
    // places that each choose their own platforms.
    static QString placeKey(NamePlace place);
    static QString columnKey(NamePlace place);

private:
    void loadOptions();
    void takeSessions(const QJsonArray &sessions);
    QImage icon(const QString &platform, const QString &status, int size, qreal ratio);

    QString m_selfId;
    QHash<QString, QString> m_ownClients;   // your devices -> your status there

    // Read once and again when changed: nameIcons runs for every row painted.
    QHash<QString, bool> m_options;
    QHash<QString, QImage> m_icons;   // drawn icons by platform, colour and size
};
