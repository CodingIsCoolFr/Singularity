#pragma once

#include "core/MessageStore.h"

#include <QElapsedTimer>
#include <QList>
#include <QPair>
#include <QPoint>
#include <QString>
#include <QWidget>

class QTimer;

class MemberRoster;
class MemberRowsModel;
class QLabel;
class QListView;
class QPushButton;

// The people in a server, down the right hand side.
//
// Discord decides the order and this does not argue with it. The server
// already knows which roles are shown separately, how they rank, and who is
// offline; sorting the list again here would produce something that disagreed
// with the official client for no benefit. What arrives is a flat list with
// the headings already in it, and that is what gets drawn.
//
// Discord sends a list 100 rows at a time, and only the pages a client asks
// for. The list is as long as the whole server from the start, rows not sent
// yet drawn as placeholders, and scrolling asks for the pages on screen the
// way Discord's own client does (rangesWanted). Until 0.9.6 only rows 0 to 99
// were ever asked for, so a big server's list simply stopped there.
//
// Over 1,000 members Discord's list holds only who is online. Under it this
// one carries on with an Offline group of its own: everyone MemberRoster has
// found (all of them where Discord's Members page is open to you, your
// friends and people seen everywhere else), faded as Discord draws offline
// people, and a line saying how many more there are.
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

    // Draw again soon with whatever changed. Batched with the gateway's own
    // changes: this is called for every member update in every server.
    void refresh();

    // Where the Offline group's people come from.
    void setRoster(MemberRoster *roster);

signals:
    void profileRequested(const QString &userId);
    void volumeMenuRequested(const QString &userId, const QPoint &globalPos);
    void collapsedChanged(bool collapsed);

    // The pages of the member list the rows on screen fall in
    // (memberListPages). Sent when scrolling settles, a second apart at most:
    // every request counts against the gateway's 120 a minute.
    void rangesWanted(const QString &guildId, const QList<QPair<int, int>> &ranges);

private:
    void rebuild();
    void askForVisibleRows();
    // Discord's rows, then the people it leaves out (see the class comment).
    // `note` is the line after them; empty when nobody is missing.
    void addOfflineSection(const MemberList &list, QList<MemberRow> &rows, QString &note);
    // The first and last rows on screen, -1 when there are none.
    QPair<int, int> visibleRows() const;

    // Batches rebuilds triggered by the gateway. See the constructor.
    QTimer *m_rebuildTimer = nullptr;
    QTimer *m_rangeTimer = nullptr;
    QElapsedTimer m_sinceRangeAsk;
    void refreshAvatar(const QString &userId);

    MessageStore *m_store = nullptr;
    MemberRoster *m_roster = nullptr;
    // Everyone known in the server, by name, for the Offline group: sorted
    // again only when who is known changes (m_offlineKnown counts them) or a
    // member update says names may have, not on every status change.
    QList<MemberRow> m_offlineSorted;
    QString m_offlineGuild;
    int m_offlineKnown = -1;
    bool m_offlineStale = true;
    // How many of the rows are Discord's; the rest are the Offline group.
    int m_discordRows = 0;
    QString m_guildId;
    QString m_focusChannel;
    bool m_focusIsVoice = false;
    bool m_collapsed = false;

    QWidget *m_header = nullptr;
    QWidget *m_body = nullptr;
    QLabel *m_counts = nullptr;
    QPushButton *m_toggle = nullptr;
    MemberRowsModel *m_model = nullptr;
    QListView *m_list = nullptr;
};
