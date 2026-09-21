#pragma once

#include <QString>
#include <QWidget>

class MessageStore;
class QLabel;
class QListWidget;
class QPushButton;

// The people in a server, down the right hand side.
//
// Discord decides the order and this does not argue with it. The server
// already knows which roles are shown separately, how they rank, and who is
// offline; sorting the list again here would produce something that disagreed
// with the official client for no benefit. What arrives is a flat list with
// the headings already in it, and that is what gets drawn.
//
// Only the first hundred rows are ever asked for, which is why the heading
// carries the server's own totals rather than counting what is on screen.
class MemberListPanel : public QWidget
{
    Q_OBJECT

public:
    explicit MemberListPanel(MessageStore *store, QWidget *parent = nullptr);

    // Which server to show. An empty id is a direct message, where there is
    // no member list and the panel hides itself.
    void setGuild(const QString &guildId);
    QString guildId() const { return m_guildId; }

    // Folded away to a narrow strip, remembered between runs.
    void setCollapsed(bool collapsed);
    bool isCollapsed() const { return m_collapsed; }

    void refresh();

signals:
    void profileRequested(const QString &userId);
    void collapsedChanged(bool collapsed);

private:
    void rebuild();

    MessageStore *m_store = nullptr;
    QString m_guildId;
    bool m_collapsed = false;

    QWidget *m_header = nullptr;
    QWidget *m_body = nullptr;
    QLabel *m_counts = nullptr;
    QPushButton *m_toggle = nullptr;
    QListWidget *m_list = nullptr;
};
