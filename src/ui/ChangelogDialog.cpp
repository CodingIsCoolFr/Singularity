#include "ui/ChangelogDialog.h"

#include "core/Logger.h"
#include "core/Updater.h"
#include "ui/Theme.h"
#include "ui/UpdateFlow.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QNetworkReply>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextList>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>

#include <algorithm>

namespace {

// GitHub hands releases out a hundred at a time. Five pages is five hundred
// releases, which is further back than anybody reads.
constexpr int PerPage = 100;
constexpr int MaxPages = 5;

QString cachePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/changelog.json");
}

QVersionNumber versionOf(const QJsonObject &release)
{
    QString tag = release.value(QStringLiteral("tag_name")).toString();
    if (tag.startsWith(QLatin1Char('v')))
        tag.remove(0, 1);
    return QVersionNumber::fromString(tag);
}

QString shortDate(const QJsonObject &release)
{
    const QDateTime when =
        QDateTime::fromString(release.value(QStringLiteral("published_at")).toString(), Qt::ISODate);
    return when.isValid() ? when.toLocalTime().toString(QStringLiteral("MMM d, yyyy"))
                          : QString();
}

} // namespace

ChangelogDialog::ChangelogDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("What's new"));
    resize(980, 640);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    m_status = new QLabel(QStringLiteral("Loading releases..."), this);
    m_status->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(QLatin1String(Theme::TextFaint)));
    layout->addWidget(m_status);

    auto *body = new QHBoxLayout;
    body->setSpacing(12);

    m_list = new QListWidget(this);
    m_list->setFixedWidth(230);
    m_list->setStyleSheet(
        QStringLiteral("QListWidget { background-color: %1; color: %2; border: 1px solid %3; "
                       "border-radius: 10px; padding: 4px; font-size: 13px; }"
                       "QListWidget::item { padding: 7px 8px; border-radius: 6px; }"
                       "QListWidget::item:selected { background-color: %4; color: %5; }"
                       "QListWidget::item:hover { background-color: %4; }")
            .arg(QLatin1String(Theme::SurfaceRail), QLatin1String(Theme::TextMuted),
                 QLatin1String(Theme::Border), QLatin1String(Theme::SurfaceHover),
                 QLatin1String(Theme::TextPrimary)));
    body->addWidget(m_list);

    auto *right = new QVBoxLayout;
    right->setSpacing(4);

    m_title = new QLabel(this);
    m_title->setStyleSheet(QStringLiteral("color: %1; font-size: 20px; font-weight: 600;")
                               .arg(QLatin1String(Theme::TextPrimary)));
    right->addWidget(m_title);

    m_date = new QLabel(this);
    m_date->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;").arg(QLatin1String(Theme::TextFaint)));
    right->addWidget(m_date);

    m_notes = new QTextBrowser(this);
    m_notes->setOpenExternalLinks(true);
    m_notes->setStyleSheet(
        QStringLiteral("QTextBrowser { background-color: %1; color: %2; border: 1px solid %3; "
                       "border-radius: 10px; padding: 14px; font-size: 13px; }")
            .arg(QLatin1String(Theme::SurfaceRail), QLatin1String(Theme::LightGray),
                 QLatin1String(Theme::Border)));
    m_notes->document()->setDefaultStyleSheet(
        QStringLiteral("a { color: %1; } code { background-color: %2; } "
                       "h2 { color: %3; } h3 { color: %3; }")
            .arg(QLatin1String(Theme::Accent), QLatin1String(Theme::SurfaceHover),
                 QLatin1String(Theme::TextPrimary)));
    right->addWidget(m_notes, 1);

    body->addLayout(right, 1);
    layout->addLayout(body, 1);

    auto *buttons = new QHBoxLayout;

    m_openButton = new QPushButton(QStringLiteral("Open on GitHub"), this);
    m_openButton->setEnabled(false);
    connect(m_openButton, &QPushButton::clicked, this, [this]() {
        const int row = m_list->currentRow();
        if (row < 0 || row >= m_releases.size())
            return;
        const QUrl page(m_releases.at(row).toObject().value(QStringLiteral("html_url")).toString());
        // Only ever GitHub. The address comes from a web service's reply, and
        // the button should not become a way to open anything else.
        if (page.scheme() == QLatin1String("https") && page.host() == QLatin1String("github.com"))
            QDesktopServices::openUrl(page);
    });
    buttons->addWidget(m_openButton);

    auto *checkButton = new QPushButton(QStringLiteral("Check for updates"), this);
    connect(checkButton, &QPushButton::clicked, this,
            [this]() { UpdateFlow::run(false, parentWidget()); });
    buttons->addWidget(checkButton);

    buttons->addStretch(1);

    auto *closeButton = new QPushButton(QStringLiteral("Close"), this);
    closeButton->setObjectName(QStringLiteral("PrimaryButton"));
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(closeButton);

    layout->addLayout(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, &ChangelogDialog::showRelease);

    loadSaved();
    fetchPage(1, QJsonArray());
}

void ChangelogDialog::loadSaved()
{
    QFile file(cachePath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isArray() && !doc.array().isEmpty())
        display(doc.array(), false);
}

void ChangelogDialog::fetchPage(int page, QJsonArray collected)
{
    const QUrl url(QStringLiteral("https://api.github.com/repos/%1/releases?per_page=%2&page=%3")
                       .arg(Updater::channel())
                       .arg(PerPage)
                       .arg(page));
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "Singularity");

    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, page, collected]() mutable {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            wlog(QStringLiteral("update"),
                 QStringLiteral("could not fetch the release list: %1").arg(reply->errorString()));
            m_status->setText(m_releases.isEmpty()
                                  ? QStringLiteral("Could not reach GitHub, and there is no saved copy yet.")
                                  : QStringLiteral("Could not reach GitHub - showing the copy saved last time."));
            return;
        }

        const QJsonArray pageItems = QJsonDocument::fromJson(reply->readAll()).array();
        for (const QJsonValue &item : pageItems) {
            // Drafts are not releases yet. They only appear to the owner, but
            // saying so costs nothing.
            if (!item.toObject().value(QStringLiteral("draft")).toBool())
                collected.append(item);
        }

        if (pageItems.size() == PerPage && page < MaxPages) {
            fetchPage(page + 1, collected);
            return;
        }

        QFile file(cachePath());
        QDir().mkpath(QFileInfo(file).absolutePath());
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(QJsonDocument(collected).toJson(QJsonDocument::Compact));

        display(collected, true);
    });
}

void ChangelogDialog::display(const QJsonArray &releases, bool fromNetwork)
{
    // Newest first, by version rather than by date: a release re-published
    // later must not jump above newer ones.
    QList<QJsonObject> sorted;
    for (const QJsonValue &value : releases)
        sorted.append(value.toObject());
    std::sort(sorted.begin(), sorted.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return versionOf(a) > versionOf(b);
    });

    const QString keep = m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString()
                                               : QString();

    m_releases = QJsonArray();
    m_list->blockSignals(true);
    m_list->clear();

    const QVersionNumber running = QVersionNumber::fromString(QApplication::applicationVersion());
    int runningRow = -1;
    int keepRow = -1;
    int newer = 0;

    for (const QJsonObject &release : sorted) {
        const QVersionNumber version = versionOf(release);
        QString label = version.toString();
        if (version == running) {
            label += QStringLiteral("  -  installed");
            runningRow = m_list->count();
        } else if (version > running) {
            label += QStringLiteral("  -  new");
            ++newer;
        }

        auto *item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, version.toString());
        item->setToolTip(shortDate(release));
        if (version > running)
            item->setForeground(QColor(QLatin1String(Theme::Accent)));
        m_list->addItem(item);
        m_releases.append(release);

        if (version.toString() == keep)
            keepRow = m_list->count() - 1;
    }
    m_list->blockSignals(false);

    // Open on what is installed - "what did I just get" is the usual reason
    // for opening this - unless somebody was already reading something else.
    const int row = keepRow >= 0 ? keepRow : (runningRow >= 0 ? runningRow : 0);
    if (m_list->count() > 0) {
        m_list->setCurrentRow(row);
        showRelease(row);
    }

    QString status = QStringLiteral("%1 releases").arg(m_releases.size());
    if (newer > 0)
        status += QStringLiteral(" - %1 newer than yours").arg(newer);
    if (!fromNetwork)
        status += QStringLiteral(" - checking for more...");
    m_status->setText(status);
}

void ChangelogDialog::showRelease(int row)
{
    if (row < 0 || row >= m_releases.size()) {
        m_openButton->setEnabled(false);
        return;
    }

    const QJsonObject release = m_releases.at(row).toObject();
    m_title->setText(QStringLiteral("Singularity %1").arg(versionOf(release).toString()));
    m_date->setText(shortDate(release));

    // Markdown, but with raw HTML switched off. The notes come from a web
    // service; whatever is in them is shown as text, never run as markup.
    m_notes->document()->setMarkdown(
        readableNotes(release.value(QStringLiteral("body")).toString()),
        QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub
                                        | QTextDocument::MarkdownNoHTML));

    // Room to breathe. Qt's Markdown reader sets no space between paragraphs,
    // and the style sheet cannot add it - that only reaches HTML - so long
    // notes came out as one wall of text.
    QTextCursor cursor(m_notes->document());
    for (QTextBlock block = m_notes->document()->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        if (format.headingLevel() > 0) {
            format.setTopMargin(block == m_notes->document()->begin() ? 0 : 18);
            format.setBottomMargin(6);
        } else {
            format.setBottomMargin(block.textList() ? 4 : 10);
        }
        format.setLineHeight(130, QTextBlockFormat::ProportionalHeight);
        cursor.setPosition(block.position());
        cursor.setBlockFormat(format);
    }

    m_notes->verticalScrollBar()->setValue(0);
    m_openButton->setEnabled(true);
}

QString ChangelogDialog::readableNotes(const QString &body)
{
    QStringList kept;
    const QStringList lines = body.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();

        // Everything from the standing warning down is the same in every
        // release.
        if (line.startsWith(QLatin1String("## Warning")))
            break;

        // The first line only repeats the version in the title.
        if (kept.isEmpty() && line.startsWith(QLatin1String("Singularity ")) && line.endsWith(QLatin1Char('.')))
            continue;

        if (line.startsWith(QLatin1String("**Download `")) || line.startsWith(QLatin1String("Website:")))
            continue;

        kept.append(lines.at(i));
    }

    // Leading blank lines left behind by what was removed.
    while (!kept.isEmpty() && kept.first().trimmed().isEmpty())
        kept.removeFirst();

    const QString text = kept.join(QLatin1Char('\n')).trimmed();
    return text.isEmpty() ? QStringLiteral("_No notes for this release._") : text;
}
