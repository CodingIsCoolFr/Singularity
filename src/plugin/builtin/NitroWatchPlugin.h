#pragma once

#include "plugin/Plugin.h"

#include <QPointer>
#include <QSet>
#include <QString>

class QWidget;

// Spots a Nitro gift the moment it is posted and puts a Claim button in front
// of you. It does not claim anything by itself, and that is the whole design.
//
// Why it stops short of claiming. Discord does not catch automated accounts by
// measuring how fast you click. It catches them on the shape of the account:
// a redeem arriving from a session that never opened the channel, never
// scrolled, never moved a pointer, and that redeems again next week at another
// hour of the night. Adding a random delay does not hide any of that, because
// none of it is about timing. It only makes the tool slower than a person who
// is already looking at the message.
//
// Dead codes are also posted deliberately, to see who redeems them. A person
// glances at a suspicious link and moves on. Anything redeeming on its own
// takes the bait every time and marks itself in the process.
//
// So the part worth automating is the part that is slow for a person: noticing.
// Wisp watches every channel at once, including ones you are not looking at,
// and takes you from "a gift exists" to "one button" instantly. The press
// stays yours, which is also what keeps the account ordinary.
class NitroWatchPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("nitro-watch"); }
    QString name() const override { return QStringLiteral("Nitro watch"); }
    QString description() const override
    {
        return QStringLiteral("Spots gift links the instant they are posted and offers a Claim "
                              "button. Never claims on its own.");
    }
    bool enabledByDefault() const override { return false; }

    void onLoad(PluginContext *context) override;
    void onUnload() override;
    void onGatewayEvent(const QString &eventType, const QJsonObject &data) override;
    QWidget *createSettingsWidget(QWidget *parent) override;

private:
    // Every real gift code in a piece of text, and nothing else.
    //
    // Pulled out of the event handler so it can be checked on its own. The
    // host is verified against what actually matched, so a link that merely
    // contains "discord.gift/" somewhere in its path is not mistaken for one.
    static QStringList findGiftCodes(const QString &text);

    // Runs the above over known cases at load. If any comes out wrong the
    // watcher switches itself off rather than guess at links.
    void selfCheck();
    bool m_matcherTrusted = true;

    void offer(const QString &code, const QString &fromUserId, const QString &channelId);
    void claim(const QString &code);
    void warnAboutFake(const QString &host);

    bool watchEnabled() const;
    bool warnFakes() const;

    // Codes already put in front of you. A gift posted in two channels, or
    // edited, must not raise two alerts.
    QSet<QString> m_seen;

    // Only one alert on screen at a time; a new gift replaces the old one.
    QPointer<QWidget> m_alert;
};
