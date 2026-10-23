#pragma once

#include "plugin/Plugin.h"

#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

// Keyword alerts: a Windows notification the moment one of your words or
// phrases is posted anywhere you are - every server and channel, direct and
// group chats too - including the ones you never open. Discord only tells you
// about mentions of you and your roles, and its own highlights decide for you
// what is worth seeing.
//
// Every server you are in is subscribed, the way Nitro watch does it, because
// a server of more than 250 members sends no messages at all until it is.
// A channel that keeps saying the word alerts once every half minute, not on
// every message; your own messages never alert you.
class KeywordAlertsPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("keyword-alerts"); }
    QString name() const override { return QStringLiteral("Keyword alerts"); }
    QString description() const override
    {
        return QStringLiteral("Notifies you the moment a word or phrase you choose is posted in any server, "
                              "channel or chat you are in, even ones you never open.");
    }
    bool enabledByDefault() const override { return false; }

    void onLoad(PluginContext *context) override;
    void onGatewayEvent(const QString &eventType, const QJsonObject &data) override;
    void onServerRail(const QStringList &joinedGuildIds) override;
    QWidget *createSettingsWidget(QWidget *parent) override;

    // The words that appear in `text`, in the order they were listed. Whole
    // words only when asked: "art" then matches "art" and "Art!" but not
    // "start". Case never matters.
    static QStringList matches(const QString &text, const QStringList &words, bool wholeWords);

    // One word or phrase a line, trimmed, blanks and repeats dropped.
    static QStringList parseWords(const QString &lines);

private:
    void watchEveryServer();
    void rebuild();
    void consider(const QJsonObject &message);

    QString m_selfId;
    QStringList m_words;
    QList<QRegularExpression> m_patterns;   // one per word
    QHash<QString, qint64> m_lastAlert;     // channel -> when it last alerted, ms
};
