#pragma once

#include "plugin/Plugin.h"

// Send any server's emoji anywhere, the way Vencord's FakeNitro does.
//
// Without Nitro, Discord turns an emoji from another server - or any animated
// one - into plain ":name:" text when the message arrives. This swaps those
// for a link to the emoji's picture instead, which Discord shows as an image.
// It is not real Nitro: nothing on your account changes, and people on the
// normal Discord app see a small picture rather than a true emoji.
class FakeNitroPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("fake-nitro"); }
    QString name() const override { return QStringLiteral("Fake Nitro"); }
    QString description() const override
    {
        return QStringLiteral("Use any server's emoji anywhere, including animated ones. Emoji you "
                              "cannot send are sent as picture links instead, like Vencord's FakeNitro.");
    }
    bool enabledByDefault() const override { return false; }

    void onGatewayEvent(const QString &eventType, const QJsonObject &data) override;
    bool onOutgoingMessage(QString &content, const QString &channelId) override;
    QWidget *createSettingsWidget(QWidget *parent) override;

    // Pure, so the rules can be checked without a running client.
    // `guildEmojiIds` are the emoji of the server the message goes to.
    static QString rewrite(const QString &content, int premiumType, const QStringList &guildEmojiIds,
                           int size, bool shortLinks);

private:
    int m_premiumType = 0;   // 0 none, 1 Classic, 2 Nitro, 3 Basic
};
