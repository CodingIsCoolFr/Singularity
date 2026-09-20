#include "ui/LogDialog.h"

#include "core/Logger.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QUrl>
#include <QVBoxLayout>

LogDialog::LogDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Log"));
    resize(860, 520);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto *path = new QLabel(Logger::instance().filePath(), this);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(QLatin1String(Theme::TextFaint)));
    layout->addWidget(path);

    m_view = new QPlainTextEdit(this);
    m_view->setReadOnly(true);
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_view->setStyleSheet(QStringLiteral("QPlainTextEdit { background-color: %1; color: %2; border: 1px solid %3; "
                                         "border-radius: 10px; font-family: Consolas, monospace; font-size: 12px; }")
                              .arg(QLatin1String(Theme::SurfaceRail), QLatin1String(Theme::LightGray),
                                   QLatin1String(Theme::Border)));
    m_view->setPlainText(Logger::instance().history().join(QLatin1Char('\n')));
    layout->addWidget(m_view, 1);

    // Follow new lines as they arrive.
    connect(&Logger::instance(), &Logger::lineLogged, this, [this](const QString &line) {
        QScrollBar *bar = m_view->verticalScrollBar();
        const bool atBottom = bar->value() >= bar->maximum() - 8;
        m_view->appendPlainText(line);
        if (atBottom)
            bar->setValue(bar->maximum());
    });

    auto *buttons = new QHBoxLayout;

    auto *copyButton = new QPushButton(QStringLiteral("Copy all"), this);
    connect(copyButton, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_view->toPlainText());
    });
    buttons->addWidget(copyButton);

    auto *openButton = new QPushButton(QStringLiteral("Open folder"), this);
    connect(openButton, &QPushButton::clicked, this, []() {
        const QFileInfo info(Logger::instance().filePath());
        QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
    });
    buttons->addWidget(openButton);

    buttons->addStretch(1);

    auto *closeButton = new QPushButton(QStringLiteral("Close"), this);
    closeButton->setObjectName(QStringLiteral("PrimaryButton"));
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(closeButton);

    layout->addLayout(buttons);

    m_view->verticalScrollBar()->setValue(m_view->verticalScrollBar()->maximum());
}
