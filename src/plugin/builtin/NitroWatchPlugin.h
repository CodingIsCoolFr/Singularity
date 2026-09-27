#pragma once

#include "core/RestClient.h"
#include "plugin/Plugin.h"

#include <QPointer>
#include <QSet>
#include <QString>

class QLabel;
class QPushButton;
class QWidget;

// Spots a Nitro gift the moment it is posted, asks Discord whether it is still
// live, and puts a Claim button in front of you if it is. It does not claim
// anything by itself, and that is the whole design.
//
// The check is the half that makes this useful rather than annoying. A code is
// looked up through GET /entitlements/gift-codes/{code}, which reads and does
// not redeem, and is the same call the official client makes to draw a gift
// card. Made-up codes come back 404, spent ones have uses == max_uses, and
// neither ever reaches the screen. Nothing is consumed by asking.
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
// Singularity watches every channel at once, including ones you are not looking at,
// and takes you from "a gift exists" to "one button" instantly. The press
// stays yours, which is also what keeps the account ordinary.
class NitroWatchPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("nitro-watch"); }
    QString name() const override { return QStringLiteral("Nitro watch"); }
    QString description() const override
    {
        return QStringLiteral("Spots gift links the instant they are posted, checks each one with "
                              "Discord, and offers a Claim button for the real ones. A Windows "
                              "notification is raised for those. Never claims on its own.");
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

    // Content plus embed / button URLs. Discord's own gift card often has an
    // empty content string; the link lives in the first embed.
    static QString messageText(const QJsonObject &data);

    // Runs the above over known cases at load. If any comes out wrong the
    // watcher switches itself off rather than guess at links.
    void selfCheck();
    bool m_matcherTrusted = true;

    // What Discord says a spotted code actually is.
    struct Gift {
        QString what;       // "Nitro, 1 month", or a plain description
        bool claimable = false;
        QString whyNot;     // when it is not: spent, expired, already yours
        int left = 0;       // uses remaining
    };

    // Asks Discord about the code and only then decides whether to raise an
    // alert. This is the whole of the "skip the fakes" job: a made-up code is
    // a 404, a spent one has uses == max_uses, and neither ever reaches the
    // screen. Nothing here claims anything.
    void inspect(const QString &code, const QString &fromUserId, const QString &channelId);
    static Gift readGift(const QJsonObject &body);

    void offer(const QString &code, const QString &fromUserId, const QString &channelId,
               const Gift &gift);
    void claim(const QString &code, const QString &channelId, QLabel *status, QPushButton *button,
               const RestClient::CaptchaProof &proof = {});
    void warnAboutFake(const QString &host);

    bool watchEnabled() const;
    bool warnFakes() const;
    bool checkFirst() const;
    bool windowsNotify() const;

    // Codes already put in front of you. A gift posted in two channels, or
    // edited, must not raise two alerts.
    QSet<QString> m_seen;

    // Only one alert on screen at a time; a new gift replaces the old one.
    QPointer<QWidget> m_alert;
};
