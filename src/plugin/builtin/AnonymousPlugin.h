#pragma once

#include "plugin/Plugin.h"

#include <QString>

// Keeps back what does not need to be sent.
//
// A note on what this plugin is not. Most of the tracking people worry about
// in the official client is absent from Wisp already, not because a plugin
// switched it off but because the code to do it was never written: no
// analytics events, no read receipts, no reading of your real machine for a
// fingerprint, no watching which programs you have open. Those are facts about
// the client, and the settings page says so plainly rather than claiming them
// as features of this plugin.
//
// What is left here are the three things Wisp genuinely does send, and can
// stop sending.
class AnonymousPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("anonymous"); }
    QString name() const override { return QStringLiteral("Anonymous"); }
    QString description() const override
    {
        return QStringLiteral("Strips tracking codes out of links you send, and can hide your "
                              "typing and your status.");
    }

    // On by default, but only its harmless setting starts switched on. The two
    // that change what other people see are off until you ask for them.
    bool enabledByDefault() const override { return true; }

    void onLoad(PluginContext *context) override;
    void onUnload() override;

    bool onOutgoingMessage(QString &content, const QString &channelId) override;
    bool onBeforeTyping(const QString &channelId) override;

    QWidget *createSettingsWidget(QWidget *parent) override;

private:
    bool stripTracking() const;
    bool hideTyping() const;
    bool appearOffline() const;

    // Pushes the current "appear offline" choice at the gateway.
    void applyPresence();

    // Runs the link cleaner over known cases when the plugin loads. If any of
    // them comes out wrong the cleaner is switched off for the session, so a
    // fault here can never mangle a real message.
    void selfCheck() const;
    mutable bool m_linkCleanerTrusted = true;

    // Returns `text` with the tracking parameters removed from every link in
    // it. Everything that is not a link is left exactly as it was.
    static QString cleanLinks(const QString &text, int *removedCount = nullptr);
};
