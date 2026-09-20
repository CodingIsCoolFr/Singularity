#include "plugin/builtin/TimestampsPlugin.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>

namespace {
// Ordinary clock time with am and pm, not the 24 hour kind.
constexpr auto kDefaultFormat = "MMM d, yyyy  h:mm AP";
}

QString TimestampsPlugin::format() const
{
    if (!context())
        return QLatin1String(kDefaultFormat);
    return context()->setting(QStringLiteral("format"), QLatin1String(kDefaultFormat)).toString();
}

QString TimestampsPlugin::decorateMessageHeader(const MessageInfo &message)
{
    if (!message.timestamp.isValid())
        return {};
    return QStringLiteral("<span class=\"stamp\">%1</span>").arg(message.timestamp.toString(format()).toHtmlEscaped());
}

QString TimestampsPlugin::decorateMessageGutter(const MessageInfo &message)
{
    if (!message.timestamp.isValid())
        return {};
    // Only the clock time fits in the narrow strip beside a grouped message.
    return QStringLiteral("<span class=\"gutter\">%1</span>")
        .arg(message.timestamp.toString(QStringLiteral("h:mm AP")).toHtmlEscaped());
}

QWidget *TimestampsPlugin::createSettingsWidget(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QFormLayout(page);

    auto *combo = new QComboBox(page);
    combo->setEditable(true);
    combo->addItems({
        QStringLiteral("MMM d, yyyy  h:mm AP"),
        QStringLiteral("MMM d  h:mm AP"),
        QStringLiteral("h:mm AP"),
        QStringLiteral("h:mm:ss AP"),
        QStringLiteral("ddd h:mm AP"),
        // The 24 hour kind, for anyone who prefers it.
        QStringLiteral("yyyy-MM-dd HH:mm:ss"),
        QStringLiteral("HH:mm"),
    });
    combo->setCurrentText(format());

    QObject::connect(combo, &QComboBox::currentTextChanged, page, [this](const QString &text) {
        if (context())
            context()->setSetting(QStringLiteral("format"), text);
    });

    layout->addRow(QStringLiteral("Time format"), combo);
    layout->addRow(new QLabel(QStringLiteral("Uses Qt date format codes. Reopen a channel to see the change.")));
    return page;
}
