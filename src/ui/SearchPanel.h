#pragma once

#include "core/SearchQuery.h"

#include <QHash>
#include <QJsonObject>
#include <QLineEdit>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class MessageStore;
class RestClient;
class QFocusEvent;
class QKeyEvent;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

// The search box at the right of the channel header, as in Discord.
//
// It takes Discord's search line: words, and filters such as from:alex or
// has:image. While a filter is being typed the list under the box offers what
// fits - people, channels, the kinds has: knows, dates - and a pick writes the
// filter in. Enter searches. Escape, or emptying the box, ends the search.
class SearchBox : public QLineEdit
{
    Q_OBJECT

public:
    SearchBox(MessageStore *store, RestClient *rest, QWidget *parent = nullptr);

    // Where a search would look: a server, or the direct or group chat
    // `channelId` when guildId is empty. The placeholder says which.
    void setScope(const QString &guildId, const QString &channelId);

    // The people and channels picked from the list, by the filter written
    // for them ("from:alex" -> id), so the search need not guess who.
    QHash<QString, QString> picked() const { return m_picked; }

signals:
    void searchRequested(const QString &text);
    void cleared();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    // While the list is open: a click anywhere outside it and the box closes
    // it, as Discord's does.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Word
    {
        int start = 0;     // where the replacement starts
        int end = 0;       // and where it ends
        QString key;       // the filter being typed, empty for none
        QString value;     // what is typed after its colon
        bool isFilter = false;
        QString text;      // the whole word, when it is not a filter
    };
    Word wordAtCursor() const;

    void updateSuggestions();
    void hideSuggestions();
    void applySuggestion(QListWidgetItem *item);
    bool moveSelection(int step);
    void askDiscordForPeople();
    QStringList peopleHere(const QString &text) const;

    MessageStore *m_store = nullptr;
    RestClient *m_rest = nullptr;
    QString m_guildId;
    QString m_channelId;

    QListWidget *m_popup = nullptr;   // made on first use, on the window
    bool m_watchingClicks = false;    // the application's events filtered while the list shows
    QHash<QString, QString> m_picked;

    // Discord's member search, for people this client has not met yet:
    // asked a moment after typing stops, once per name.
    QTimer m_peopleTimer;
    QString m_peopleWanted;
    QSet<QString> m_peopleAsked;
};

// Search results, down the right in place of the member list, as in
// Discord: how many there are, newest or oldest or most relevant first, 25
// at a time with a page switch under them. Each shows who said it, where
// and when, with the words that matched marked. Clicking one asks the
// window to open the channel at that message.
class SearchPanel : public QWidget
{
    Q_OBJECT

public:
    SearchPanel(RestClient *rest, MessageStore *store, QWidget *parent = nullptr);

    // Whether results may come from age-restricted channels: the account's
    // nsfw_allowed, which Discord's client sends as include_nsfw.
    void setIncludeNsfw(bool on) { m_includeNsfw = on; }

    // Searches a server, or the direct or group chat `channelId` when guildId
    // is empty. `picked` is SearchBox::picked.
    void search(const QString &guildId, const QString &channelId, const QString &text,
                const QHash<QString, QString> &picked = {});

    // Drops the search. Anything still on its way for it is ignored.
    void clear();

    QString guildId() const { return m_guildId; }
    QString channelId() const { return m_channelId; }

signals:
    void jumpRequested(const QString &channelId, const QString &messageId);
    void closeRequested();

private:
    void resolveNames();
    void runPage();
    void showPage(const SearchQuery::Page &page);
    void showStatus(const QString &text);
    void setSort(SearchQuery::Sort sort);
    void turnPage(int step);
    QString personNamed(const QString &name) const;
    QString channelNamed(const QString &name) const;
    QString previewText(const QJsonObject &raw) const;

    RestClient *m_rest = nullptr;
    MessageStore *m_store = nullptr;
    QString m_guildId;
    QString m_channelId;
    bool m_includeNsfw = false;

    SearchQuery::Request m_request;
    QStringList m_terms;               // the words to mark in each result
    QHash<QString, QString> m_picked;
    QSet<QString> m_namesAsked;        // names this search has put to Discord's member search
    int m_serial = 0;                  // which search the answers belong to
    int m_total = 0;

    QLabel *m_count = nullptr;
    QPushButton *m_newest = nullptr;
    QPushButton *m_oldest = nullptr;
    QPushButton *m_relevant = nullptr;
    QLabel *m_status = nullptr;
    QListWidget *m_list = nullptr;
    QWidget *m_pager = nullptr;
    QPushButton *m_previous = nullptr;
    QPushButton *m_next = nullptr;
    QLabel *m_pageLabel = nullptr;
};
