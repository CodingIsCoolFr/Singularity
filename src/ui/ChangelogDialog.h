#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QNetworkAccessManager>

class QLabel;
class QListWidget;
class QPushButton;
class QTextBrowser;

// Every release, newest first, with its notes.
//
// Read from the same public channel the updater uses, so what is listed here
// is exactly what was shipped. A copy is kept on disk: the window opens at
// once with what it knew last time, works with no connection at all, and
// refreshes itself in the background.
class ChangelogDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChangelogDialog(QWidget *parent = nullptr);

private:
    void loadSaved();
    void fetchPage(int page, QJsonArray collected);
    void display(const QJsonArray &releases, bool fromNetwork);
    void showRelease(int row);

    // A release body carries lines every release has - the download line,
    // the website, the account warning. Worth reading once, not eighty times.
    static QString readableNotes(const QString &body);

    QNetworkAccessManager m_network;
    QJsonArray m_releases;

    QLabel *m_status = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_date = nullptr;
    QTextBrowser *m_notes = nullptr;
    QPushButton *m_openButton = nullptr;
};
