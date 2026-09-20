#include "plugin/builtin/QuickTextPlugin.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

QMap<QString, QString> QuickTextPlugin::defaultRules()
{
    return {
        {QStringLiteral(":shrug:"), QStringLiteral("¯\\_(ツ)_/¯")},
        {QStringLiteral(":tableflip:"), QStringLiteral("(╯°□°)╯︵ ┻━┻")},
        {QStringLiteral(":unflip:"), QStringLiteral("┬─┬ノ( º _ ºノ)")},
        {QStringLiteral(":lenny:"), QStringLiteral("( ͡° ͜ʖ ͡°)")},
    };
}

void QuickTextPlugin::onLoad(PluginContext *context)
{
    Plugin::onLoad(context);
    loadRules();
}

void QuickTextPlugin::loadRules()
{
    m_rules.clear();
    if (!context()) {
        m_rules = defaultRules();
        return;
    }

    // Rules are stored as one "code=text" line per entry.
    const QStringList lines = context()->setting(QStringLiteral("rules")).toStringList();
    if (lines.isEmpty()) {
        m_rules = defaultRules();
        saveRules();
        return;
    }

    for (const QString &line : lines) {
        const int split = line.indexOf(QLatin1Char('='));
        if (split <= 0)
            continue;
        m_rules.insert(line.left(split), line.mid(split + 1));
    }
}

void QuickTextPlugin::saveRules()
{
    if (!context())
        return;
    QStringList lines;
    for (auto it = m_rules.constBegin(); it != m_rules.constEnd(); ++it)
        lines.append(it.key() + QLatin1Char('=') + it.value());
    context()->setSetting(QStringLiteral("rules"), lines);
}

bool QuickTextPlugin::onOutgoingMessage(QString &content, const QString &channelId)
{
    Q_UNUSED(channelId)
    for (auto it = m_rules.constBegin(); it != m_rules.constEnd(); ++it) {
        if (it.key().isEmpty())
            continue;
        content.replace(it.key(), it.value());
    }
    return true;
}

QWidget *QuickTextPlugin::createSettingsWidget(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);

    auto *table = new QTableWidget(page);
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({QStringLiteral("Code"), QStringLiteral("Replaced with")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->setVisible(false);

    const auto fill = [table](const QMap<QString, QString> &rules) {
        table->setRowCount(static_cast<int>(rules.size()));
        int row = 0;
        for (auto it = rules.constBegin(); it != rules.constEnd(); ++it, ++row) {
            table->setItem(row, 0, new QTableWidgetItem(it.key()));
            table->setItem(row, 1, new QTableWidgetItem(it.value()));
        }
    };
    fill(m_rules);

    auto *addButton = new QPushButton(QStringLiteral("Add row"), page);
    QObject::connect(addButton, &QPushButton::clicked, page, [table]() {
        const int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(QString()));
        table->setItem(row, 1, new QTableWidgetItem(QString()));
        table->editItem(table->item(row, 0));
    });

    auto *saveButton = new QPushButton(QStringLiteral("Save rules"), page);
    QObject::connect(saveButton, &QPushButton::clicked, page, [this, table]() {
        m_rules.clear();
        for (int row = 0; row < table->rowCount(); ++row) {
            QTableWidgetItem *codeItem = table->item(row, 0);
            QTableWidgetItem *textItem = table->item(row, 1);
            if (!codeItem || codeItem->text().trimmed().isEmpty())
                continue;
            m_rules.insert(codeItem->text().trimmed(), textItem ? textItem->text() : QString());
        }
        saveRules();
    });

    layout->addWidget(new QLabel(QStringLiteral("Codes are swapped when the message is sent."), page));
    layout->addWidget(table, 1);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(addButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(saveButton);
    layout->addLayout(buttonRow);

    return page;
}
