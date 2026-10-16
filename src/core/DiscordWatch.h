#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

// Notices when Discord has changed something Singularity relies on.
//
// Discord changes its servers and its own client every week, and says little
// about the parts a third party client uses. When one of those changes breaks
// something here it rarely looks like a fault: a list stays empty, a button
// does nothing, or the gateway hangs up and the window says to sign in again.
// This tells those apart from the everyday refusals - no permission, a deleted
// channel, a rate limit - which are Discord working as it should.
//
// What counts as Discord having changed:
//   - an endpoint that is no longer there: 404 with no Discord error code,
//     405, 410
//   - a request Discord no longer understands: 50041 (API version), 50109
//     (unreadable JSON), and 50035 (the request's shape) when it repeats
//   - the gateway closing over the protocol rather than the session: an
//     opcode or a payload it no longer reads (4001, 4002), the API version
//     (4012), intents (4013, 4014), sharding (4010, 4011)
//   - an opcode arriving that this client has never heard of
//   - READY without what everything else is built on
//
// Nothing here leaves the machine. A finding is logged, kept for the session,
// and when it is major the window says so, with a report the person can
// choose to send. Paths are kept as their shape - "/channels/{id}/messages" -
// so a report carries no ids, codes or tokens.
class DiscordWatch : public QObject
{
    Q_OBJECT

public:
    static DiscordWatch &instance();

    struct Finding
    {
        QString key;    // what it is about, e.g. "GET /channels/{id}/messages"
        QString what;   // a sentence for a person
        bool major = false;
        int count = 0;
        QDateTime first;
        QDateTime last;
    };

    // Every failed REST request. `path` is the request's path, with or
    // without the /api/vN in front.
    void noteRestFailure(const QString &method, const QString &path, int httpStatus, int discordCode);

    // Every gateway close, whatever the code. Only protocol ones are kept.
    void noteGatewayClose(int code);

    void noteUnknownOpcode(int op);

    // Each READY, checked for what the rest of the client is built on.
    void noteReady(const QJsonObject &ready);

    QList<Finding> findings() const;
    bool hasMajor() const;

    // Plain text for a person or an issue: the version and every finding.
    QString report() const;

    // A GitHub "new issue" page with the report filled in. Opening it sends
    // nothing; the person reads it and decides.
    QUrl issueUrl() const;

    // "/api/v10/channels/123/messages?limit=50" -> "/channels/{id}/messages".
    // Words stay; numbers become {id}; anything else - an invite code, an
    // emoji, a webhook token - becomes {x}.
    static QString routeShape(const QString &path);

signals:
    // A finding has just become major. Once per finding per run.
    void broke(const QString &summary);

private:
    DiscordWatch() = default;
    void record(const QString &key, const QString &what, int majorAt);

    QHash<QString, Finding> m_findings;
    QStringList m_order;
};
