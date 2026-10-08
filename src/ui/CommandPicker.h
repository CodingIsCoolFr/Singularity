#pragma once

#include <QListWidget>
#include <QUrl>

// The list that opens above the message box when you type "/".
//
// Laid out like Discord's: section headings (Frequently Used, Built-In, then
// one per app), and a row per command with the app's picture, "/name", what
// it does, and the app's name on the right. The same list shows an option's
// names, its fixed choices, or what the app suggests as you type.
//
// It never takes focus. The message box keeps the keyboard and moves the
// selection with step(), the way the @mention list works.
class CommandPicker : public QListWidget
{
    Q_OBJECT

public:
    enum Role {
        RowKind = Qt::UserRole + 40,   // 0 a row, 1 a heading, 2 a note
        Title,
        Detail,
        Right,
        Key,
        IconUrl,
        IconName,
    };

    explicit CommandPicker(QWidget *parent);

    void clearRows();
    void addHeader(const QString &text);
    void addNote(const QString &text);
    void addRow(const QString &title, const QString &detail, const QString &right, const QString &key,
                const QUrl &icon = {}, const QString &iconName = {});

    // Moves the selection, skipping headings and notes.
    void step(int delta);
    void selectKey(const QString &key);
    QString currentKey() const;
    bool hasRows() const;

    // Sizes the list to its rows and puts it just above `anchor`.
    void place(QWidget *anchor);

signals:
    void picked(const QString &key);
};
