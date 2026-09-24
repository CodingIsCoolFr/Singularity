#pragma once

#include "plugin/Plugin.h"

#include <QHash>
#include <QSet>
#include <QString>

// Tells you, with a Windows notification, when your friends list or your
// servers change because of somebody else:
//
//   - a friend removed you
//   - someone sent you a friend request, or cancelled one they had sent
//   - someone accepted a friend request you sent
//   - you were removed from a server or a group chat
//
// It also remembers the lists between runs, so what changed while
// Singularity was closed is reported at the next start ("while you were
// away"). The lists are kept per account.
//
// Anything you did yourself in Singularity - unfriending, cancelling a
// request, blocking, closing a chat - is skipped: RestClient remembers its own
// removals, and Discord's echo of them is ignored. One thing it cannot tell
// apart: the same action taken on another device, such as unfriending someone
// on your phone. Discord sends that exactly as if they had done it.
class RelationshipNotifierPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("relationship-notifier"); }
    QString name() const override { return QStringLiteral("Relationship notifier"); }
    QString description() const override
    {
        return QStringLiteral("Notifies you when someone removes you as a friend, sends, accepts or "
                              "cancels a friend request, or removes you from a server or group - "
                              "including while Singularity was closed.");
    }

    void onLoad(PluginContext *context) override;
    void onUnload() override;
    void onGatewayEvent(const QString &eventType, const QJsonObject &data) override;
    QWidget *createSettingsWidget(QWidget *parent) override;

private:
    // id -> display name, one map per kind of thing that can go away.
    struct Lists {
        QHash<QString, QString> friends;
        QHash<QString, QString> incoming;   // requests sent to you
        QHash<QString, QString> outgoing;   // requests you sent
        QHash<QString, QString> guilds;
        QHash<QString, QString> groups;
    };

    void onReady(const QJsonObject &data);
    void compareWithLastRun(const Lists &now);
    void save() const;
    Lists loadSaved() const;

    QString personName(const QString &userId, const QJsonObject &user = {}) const;
    QString groupName(const QString &channelId) const;
    void tell(const char *setting, const QString &title, const QString &text);
    bool wants(const char *setting) const;

    QString m_selfId;
    QHash<QString, int> m_relationship;   // user -> Discord's type, as we last saw it
    Lists m_lists;
    QSet<QString> m_groupsAnnounced;      // one notice per group, whichever event comes first
    bool m_ready = false;
};
