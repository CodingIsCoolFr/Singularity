#pragma once

#include "plugin/Plugin.h"

// Puts a readable timestamp on every message instead of the vague
// "today at ..." wording the official client uses.
class TimestampsPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("timestamps"); }
    QString name() const override { return QStringLiteral("Full timestamps"); }
    QString description() const override
    {
        return QStringLiteral("Shows the exact date and time on every message.");
    }
    bool enabledByDefault() const override { return true; }

    QString decorateMessageHeader(const MessageInfo &message) override;
    QString decorateMessageGutter(const MessageInfo &message) override;
    QWidget *createSettingsWidget(QWidget *parent) override;

private:
    QString format() const;
};
