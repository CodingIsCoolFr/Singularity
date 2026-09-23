#pragma once

#include <QPoint>
#include <QString>
#include <QWidget>

class QTimer;

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

    // When the open channel is a voice channel, the panel lists the people
    // in that call. A text channel keeps Discord's own member list.
    void setFocusChannel(const QString &channelId, bool voice);

    // Folded away to a narrow strip, remembered between runs.
    void setCollapsed(bool collapsed);
    bool isCollapsed() const { return m_collapsed; }

    void refresh();

signals:
    void profileRequested(const QString &userId);
    void volumeMenuRequested(const QString &userId, const QPoint &globalPos);
    void collapsedChanged(bool collapsed);

private:
    void rebuild();

    // Batches rebuilds triggered by the gateway. See the constructor.
    QTimer *m_rebuildTimer = nullptr;
    void refreshAvatar(const QString &userId);

    MessageStore *m_store = nullptr;
    QString m_guildId;
    QString m_focusChannel;
    bool m_focusIsVoice = false;
    bool m_collapsed = false;

    QWidget *m_header = nullptr;
    QWidget *m_body = nullptr;
    QLabel *m_counts = nullptr;
    QPushButton *m_toggle = nullptr;
    QListWidget *m_list = nullptr;
};
