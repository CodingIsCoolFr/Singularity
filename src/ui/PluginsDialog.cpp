#include "ui/PluginsDialog.h"

#include "plugin/PluginHost.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

PluginsDialog::PluginsDialog(PluginHost *host, QWidget *parent)
    : QDialog(parent)
    , m_host(host)
{
    setWindowTitle(QStringLiteral("Plugins"));
    resize(720, 460);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    m_list = new QListWidget(this);
    m_list->setFixedWidth(220);
    layout->addWidget(m_list);

    m_pages = new QStackedWidget(this);
    layout->addWidget(m_pages, 1);

    buildList();

    connect(m_list, &QListWidget::currentRowChanged, this, &PluginsDialog::showPluginAt);
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
}

void PluginsDialog::buildList()
{
    const QList<Plugin *> plugins = m_host->plugins();
    for (Plugin *plugin : plugins) {
        m_list->addItem(plugin->name());

        auto *page = new QWidget(m_pages);
        auto *pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(4, 0, 4, 0);

        auto *heading = new QLabel(plugin->name(), page);
        heading->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600; color: %1;")
                                   .arg(QLatin1String(Theme::TextPrimary)));
        pageLayout->addWidget(heading);

        auto *description = new QLabel(plugin->description(), page);
        description->setWordWrap(true);
        description->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));
        pageLayout->addWidget(description);

        auto *toggle = new QCheckBox(QStringLiteral("Enabled"), page);
        toggle->setChecked(m_host->isEnabled(plugin->id()));
        const QString pluginId = plugin->id();
        connect(toggle, &QCheckBox::toggled, this, [this, pluginId](bool on) {
            m_host->setEnabled(pluginId, on);
        });
        pageLayout->addWidget(toggle);

        if (QWidget *settings = plugin->createSettingsWidget(page)) {
            auto *separator = new QLabel(QStringLiteral("Settings"), page);
            separator->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; letter-spacing: 1px; "
                                                    "margin-top: 8px;")
                                         .arg(QLatin1String(Theme::TextFaint)));
            pageLayout->addWidget(separator);
            pageLayout->addWidget(settings, 1);
        } else {
            pageLayout->addStretch(1);
        }

        m_pages->addWidget(page);
    }
}

void PluginsDialog::showPluginAt(int row)
{
    if (row >= 0 && row < m_pages->count())
        m_pages->setCurrentIndex(row);
}
