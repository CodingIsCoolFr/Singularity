#include "ui/SettingsDialog.h"

#include "core/AppConfig.h"
#include "core/GameActivity.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QLabel *pageTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 300; color: %1; "
                                        "letter-spacing: -0.5px; margin-bottom: 4px;")
                             .arg(QLatin1String(Theme::TextPrimary)));
    return label;
}

QLabel *groupTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 700; "
                                        "letter-spacing: 0.8px; margin-top: 22px;")
                             .arg(QLatin1String(Theme::TextMuted)));
    return label;
}

QLabel *hint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(QLatin1String(Theme::TextFaint)));
    return label;
}

} // namespace

QWidget *SettingsDialog::buildActivityPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(40, 28, 40, 40);
    layout->setSpacing(10);

    layout->addWidget(pageTitle(QStringLiteral("Activity"), page));
    layout->addWidget(hint(QStringLiteral(
                               "Choose what other people see you playing. A game Discord already knows is "
                               "picked up on its own and keeps its picture. Add one it does not know, or "
                               "leave the program blank to show a line of your own."),
                           page));

    auto *share = new QCheckBox(QStringLiteral("Show the Singularity card"), page);
    share->setChecked(AppConfig::instance().value(QStringLiteral("presence/shareActivity"), true).toBool());
    connect(share, &QCheckBox::toggled, this, &SettingsDialog::activityShareChanged);
    layout->addWidget(share);

    auto *showGames = new QCheckBox(QStringLiteral("Show games I'm playing"), page);
    auto *detect = new QCheckBox(QStringLiteral("Detect games automatically"), page);
    if (m_games) {
        showGames->setChecked(m_games->shown());
        detect->setChecked(m_games->detect());
    }
    connect(showGames, &QCheckBox::toggled, this, [this](bool on) {
        if (m_games)
            m_games->setShown(on);
    });
    connect(detect, &QCheckBox::toggled, this, [this](bool on) {
        if (m_games)
            m_games->setDetect(on);
    });
    layout->addWidget(showGames);
    layout->addWidget(detect);

    layout->addWidget(groupTitle(QStringLiteral("SHOWING NOW"), page));
    auto *showing = new QLabel(page);
    showing->setWordWrap(true);
    showing->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;").arg(QLatin1String(Theme::TextMuted)));
    layout->addWidget(showing);

    const QString listStyle =
        QStringLiteral("QListWidget { background: %1; border: 1px solid %2; border-radius: 8px; padding: 4px; }"
                       "QListWidget::item { color: %3; border-radius: 6px; padding: 4px; }"
                       "QListWidget::item:selected { background: %4; color: %5; }")
            .arg(QLatin1String(Theme::SurfaceInput), QLatin1String(Theme::Border), QLatin1String(Theme::TextPrimary),
                 QLatin1String(Theme::SurfaceHover), QLatin1String(Theme::TextPrimary));

    layout->addWidget(groupTitle(QStringLiteral("YOUR GAMES"), page));
    auto *saved = new QListWidget(page);
    saved->setStyleSheet(listStyle);
    saved->setMinimumHeight(120);
    layout->addWidget(saved);

    auto *name = new QLineEdit(page);
    name->setPlaceholderText(QStringLiteral("Name people see"));
    auto *details = new QLineEdit(page);
    details->setPlaceholderText(QStringLiteral("Line under the name, optional"));
    auto *exe = new QLineEdit(page);
    exe->setPlaceholderText(QStringLiteral("Program, or leave empty to always show this"));
    layout->addWidget(name);
    layout->addWidget(details);
    layout->addWidget(exe);

    auto *buttons = new QHBoxLayout;
    auto *browse = new QPushButton(QStringLiteral("Browse"), page);
    auto *add = new QPushButton(QStringLiteral("Add"), page);
    auto *saveEdit = new QPushButton(QStringLiteral("Save changes"), page);
    auto *remove = new QPushButton(QStringLiteral("Remove"), page);
    buttons->addWidget(browse);
    buttons->addWidget(add);
    buttons->addWidget(saveEdit);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    layout->addWidget(groupTitle(QStringLiteral("RUNNING NOW"), page));
    layout->addWidget(hint(QStringLiteral("A program that is open. Add it if it was not picked up on its own."), page));
    auto *running = new QListWidget(page);
    running->setStyleSheet(listStyle);
    running->setMinimumHeight(140);
    layout->addWidget(running);
    auto *addRunning = new QPushButton(QStringLiteral("Add the selected program"), page);
    layout->addWidget(addRunning, 0, Qt::AlignLeft);

    auto *savedKey = new QString;
    auto *runningKey = new QString;
    const auto refresh = [=]() {
        if (!m_games)
            return;
        const QStringList lines = m_games->showing();
        showing->setText(lines.isEmpty() ? QStringLiteral("Nothing right now.") : lines.join(QStringLiteral("\n")));

        QString nextSaved;
        const QList<GameActivity::Saved> games = m_games->saved();
        for (const GameActivity::Saved &game : games)
            nextSaved += game.name + QLatin1Char('|') + game.exe + QLatin1Char('|') + game.details + QLatin1Char(';');
        if (nextSaved != *savedKey) {
            const int row = saved->currentRow();
            saved->clear();
            for (int i = 0; i < games.size(); ++i) {
                const GameActivity::Saved &game = games.at(i);
                QString label = game.name;
                if (!game.details.isEmpty())
                    label += QStringLiteral(" — ") + game.details;
                if (game.exe.isEmpty())
                    label += QStringLiteral("  (always)");
                else
                    label += QStringLiteral("  ") + QFileInfo(game.exe).fileName();
                auto *item = new QListWidgetItem(label, saved);
                item->setData(Qt::UserRole, i);
            }
            if (row >= 0 && row < saved->count())
                saved->setCurrentRow(row);
            *savedKey = nextSaved;
        }

        QString nextRunning;
        const QList<GameActivity::Running> open = m_games->running();
        for (const GameActivity::Running &row : open)
            nextRunning += row.exe + QLatin1Char('|');
        if (nextRunning != *runningKey) {
            const int row = running->currentRow();
            running->clear();
            for (const GameActivity::Running &program : open) {
                QString label = program.title;
                if (program.known)
                    label += QStringLiteral("  · known");
                auto *item = new QListWidgetItem(label, running);
                item->setData(Qt::UserRole, program.exe);
                item->setData(Qt::UserRole + 1, program.title);
            }
            if (row >= 0 && row < running->count())
                running->setCurrentRow(row);
            *runningKey = nextRunning;
        }
    };

    connect(saved, &QListWidget::currentRowChanged, page, [=](int row) {
        if (!m_games || row < 0 || row >= m_games->saved().size())
            return;
        const GameActivity::Saved game = m_games->saved().at(row);
        name->setText(game.name);
        details->setText(game.details);
        exe->setText(game.exe);
    });
    connect(browse, &QPushButton::clicked, page, [=]() {
        const QString path = QFileDialog::getOpenFileName(page, QStringLiteral("Choose the program"), {},
                                                          QStringLiteral("Programs (*.exe)"));
        if (!path.isEmpty())
            exe->setText(path);
    });
    connect(add, &QPushButton::clicked, page, [=]() {
        if (!m_games)
            return;
        m_games->addSaved(
            GameActivity::Saved{name->text().trimmed(), exe->text().trimmed(), details->text().trimmed(), true});
        name->clear();
        details->clear();
        exe->clear();
    });
    connect(saveEdit, &QPushButton::clicked, page, [=]() {
        if (!m_games || saved->currentRow() < 0)
            return;
        m_games->updateSaved(saved->currentRow(), GameActivity::Saved{name->text().trimmed(), exe->text().trimmed(),
                                                                     details->text().trimmed(), true});
    });
    connect(remove, &QPushButton::clicked, page, [=]() {
        if (!m_games || saved->currentRow() < 0)
            return;
        m_games->removeSaved(saved->currentRow());
        name->clear();
        details->clear();
        exe->clear();
    });
    connect(addRunning, &QPushButton::clicked, page, [=]() {
        if (!m_games || !running->currentItem())
            return;
        const QString path = running->currentItem()->data(Qt::UserRole).toString();
        QString title = running->currentItem()->data(Qt::UserRole + 1).toString().trimmed();
        if (title.size() > 80)
            title.truncate(80);
        if (title.isEmpty())
            title = QFileInfo(path).completeBaseName();
        m_games->addSaved(GameActivity::Saved{title, path, {}, true});
    });

    if (m_games) {
        connect(m_games, &GameActivity::changed, page, refresh);
        m_games->refresh();
    }
    refresh();

    layout->addStretch(1);
    return page;
}
