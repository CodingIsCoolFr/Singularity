#include "ui/MemberListPanel.h"

#include "core/AppConfig.h"
#include "core/MessageStore.h"
#include "ui/ListDelegates.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int PanelWidth = 232;
constexpr int StripWidth = 34;

// What somebody is doing, in one line.
//
// A custom status is kept back and only used when nothing else is running,
// which is the order the official client uses: what you are playing says more
// than what you typed about yourself last week.
QString doingNow(const MessageStore &store, const QString &userId)
{
    const PresenceInfo presence = store.presence(userId);

    QString custom;
    for (const ActivityInfo &activity : presence.activities) {
        if (activity.isCustomStatus()) {
            custom = activity.emoji.isEmpty()
                ? activity.state
                : QStringLiteral("%1 %2").arg(activity.emoji, activity.state).trimmed();
            continue;
        }
        if (activity.type == 2 && !activity.details.isEmpty())
            return activity.details;
        if (!activity.name.isEmpty())
            return activity.name;
    }

    return custom;
}

// The rows already on screen, in order. A status tick does not change who is
// listed, and rebuilding a hundred faces for one bubble is what stalled the
// black hole for a moment.
bool sameOrder(QListWidget *list, const QStringList &ids, const QList<bool> &headings)
{
    if (list->count() != ids.size())
        return false;
    for (int i = 0; i < ids.size(); ++i) {
        QListWidgetItem *item = list->item(i);
        if (item->data(SingularityRoles::Heading).toBool() != headings.at(i))
            return false;
        if (item->data(SingularityRoles::Id).toString() != ids.at(i))
            return false;
    }
    return true;
}

void setFace(QListWidgetItem *item, const QString &userId, const QString &name, const QString &avatarHash)
{
    const QUrl url = MediaCache::avatarUrl(userId, avatarHash, 64);
    const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
    item->setIcon(picture.isNull() ? QIcon(MediaCache::initialsAvatar(name, 32))
                                   : QIcon(MediaCache::circular(picture, 32)));
}

// Discord's own names for the two groups that are not roles.
QString headingName(const QString &groupId, const GuildInfo &guild, int count)
{
    if (groupId == QLatin1String("online"))
        return QStringLiteral("Online — %1").arg(count);
    if (groupId == QLatin1String("offline"))
        return QStringLiteral("Offline — %1").arg(count);

    const RoleInfo role = guild.roles.value(groupId);
    const QString name = role.name.isEmpty() ? QStringLiteral("Members") : role.name;
    return QStringLiteral("%1 — %2").arg(name).arg(count);
}

} // namespace

MemberListPanel::MemberListPanel(MessageStore *store, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
{
    setObjectName(QStringLiteral("MemberPanel"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The header stays visible when the rest is folded away, so there is
    // always something to click to bring it back.
    m_header = new QWidget(this);
    QWidget *header = m_header;
    header->setObjectName(QStringLiteral("MemberHeader"));
    header->setFixedHeight(40);

    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);

    m_toggle = new QPushButton(header);
    m_toggle->setObjectName(QStringLiteral("MemberToggle"));
    m_toggle->setCursor(Qt::PointingHandCursor);
    m_toggle->setFlat(true);
    connect(m_toggle, &QPushButton::clicked, this, [this]() { setCollapsed(!m_collapsed); });
    headerLayout->addWidget(m_toggle);

    layout->addWidget(header);

    m_body = new QWidget(this);
    auto *bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(0, 0, 0, 6);
    bodyLayout->setSpacing(0);

    // Two totals, and they are not the same question. How many people are
    // here right now is what you look at before speaking; how many there are
    // altogether is what the server is. Discord shows both and so does this.
    m_counts = new QLabel(m_body);
    m_counts->setObjectName(QStringLiteral("MemberCounts"));
    m_counts->setContentsMargins(14, 2, 14, 8);
    bodyLayout->addWidget(m_counts);

    m_list = new QListWidget(m_body);
    m_list->setObjectName(QStringLiteral("MemberList"));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setUniformItemSizes(false);
    m_list->setMouseTracking(true);
    m_list->setItemDelegate(new MemberDelegate(m_list, m_list));

    // Let the panel's own glass show through the list.
    //
    // A stylesheet alone does not do this, which is why the panel went clear
    // and the names stayed on a grey slab: a scrolling view does not paint
    // its own background, its viewport does, and the viewport fills itself
    // from the palette's Base colour before any rule here is consulted. Both
    // have to be told.
    m_list->viewport()->setAutoFillBackground(false);
    m_list->setAutoFillBackground(false);

    QPalette clear = m_list->palette();
    clear.setColor(QPalette::Base, Qt::transparent);
    clear.setColor(QPalette::Window, Qt::transparent);
    m_list->setPalette(clear);
    m_list->viewport()->setPalette(clear);

    bodyLayout->addWidget(m_list, 1);

    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (!item || item->data(SingularityRoles::Heading).toBool())
            return;
        const QString userId = item->data(SingularityRoles::Id).toString();
        if (!userId.isEmpty())
            emit profileRequested(userId);
    });

    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_list->itemAt(pos);
        if (!item || item->data(SingularityRoles::Heading).toBool())
            return;
        const QString userId = item->data(SingularityRoles::Id).toString();
        if (userId.isEmpty())
            return;
        emit volumeMenuRequested(userId, m_list->viewport()->mapToGlobal(pos));
    });

    layout->addWidget(m_body, 1);

    // Changes are collected and applied a few times a second at most.
    //
    // A server with 1.4 million members sends a member list change every one
    // to four seconds for the whole session, and a busy voice channel sends
    // voice state changes faster than that. Each one rebuilt the list on the
    // window's thread - the same thread that plays the black hole - so the
    // list was a steady source of dropped frames while nobody was looking at
    // it change. Nobody can read a list that changes faster than this anyway.
    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(350);
    connect(m_rebuildTimer, &QTimer::timeout, this, [this]() { rebuild(); });

    connect(m_store, &MessageStore::memberListChanged, this, [this](const QString &guildId) {
        if (guildId == m_guildId && !m_focusIsVoice && !m_rebuildTimer->isActive())
            m_rebuildTimer->start();
    });
    connect(m_store, &MessageStore::voiceStatesChanged, this, [this]() {
        if (m_focusIsVoice && !m_rebuildTimer->isActive())
            m_rebuildTimer->start();
    });

    // Pictures arrive later than the list does, so a face that was a grey
    // circle when the row was built has to be put in when it turns up.
    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &url) {
        const QString path = url.path();
        if (!path.startsWith(QLatin1String("/avatars/")))
            return;
        const QStringList parts = path.split(QLatin1Char('/'));
        if (parts.size() < 3)
            return;
        refreshAvatar(parts.at(2));
    });

    setCollapsed(AppConfig::instance()
                     .value(QStringLiteral("appearance/memberListCollapsed"), false)
                     .toBool());
}

void MemberListPanel::setGuild(const QString &guildId)
{
    // A direct message and the friends page have no members to list, so the
    // whole panel goes rather than standing there empty.
    //
    // Set before the early return below, not after. Skipping out when the id
    // had not changed also skipped this, and since both the starting id and
    // the id of "no server" are empty, the panel was never hidden at startup
    // - it sat there beside the friends list with nothing in it.
    setVisible(!guildId.isEmpty());

    if (m_guildId == guildId)
        return;

    m_guildId = guildId;
    m_focusChannel.clear();
    m_focusIsVoice = false;
    rebuild();
}

void MemberListPanel::setFocusChannel(const QString &channelId, bool voice)
{
    if (m_focusChannel == channelId && m_focusIsVoice == voice)
        return;
    m_focusChannel = channelId;
    m_focusIsVoice = voice;
    rebuild();
}

void MemberListPanel::setCollapsed(bool collapsed)
{
    m_collapsed = collapsed;
    m_body->setVisible(!collapsed);
    setFixedWidth(collapsed ? StripWidth : PanelWidth);

    // Folded, the whole strip is the button.
    //
    // It used to be a forty pixel tall header with fourteen pixels of padding
    // inside a thirty pixel wide strip, so the arrow that brings it back was
    // clipped out of existence - there was no way to reopen it at all. Now the
    // button fills the strip top to bottom, and the padding that positions the
    // word "Members" is dropped when there is no word.
    m_header->setFixedHeight(collapsed ? QWIDGETSIZE_MAX : 40);
    m_header->setMinimumHeight(collapsed ? 0 : 40);

    m_toggle->setStyleSheet(
        collapsed
            ? QStringLiteral("QPushButton { background: transparent; border: none; padding: 0; "
                             "color: %1; font-size: 16px; }"
                             "QPushButton:hover { background: %2; color: %3; }")
                  .arg(QLatin1String(Theme::TextMuted), QLatin1String(Theme::SurfaceHover),
                       QLatin1String(Theme::TextPrimary))
            : QString());

    m_toggle->setText(collapsed ? QStringLiteral("‹") : QStringLiteral("Members  ›"));
    m_toggle->setToolTip(collapsed ? QStringLiteral("Show the member list")
                                   : QStringLiteral("Hide the member list"));

    AppConfig::instance().setValue(QStringLiteral("appearance/memberListCollapsed"), collapsed);

    // Nothing is kept up to date while folded, so opening it has to build the
    // list rather than reveal a stale one.
    if (!collapsed)
        rebuild();

    emit collapsedChanged(collapsed);
}

void MemberListPanel::refresh()
{
    rebuild();
}

void MemberListPanel::refreshAvatar(const QString &userId)
{
    if (userId.isEmpty() || m_collapsed || m_guildId.isEmpty())
        return;

    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (item->data(SingularityRoles::Heading).toBool())
            continue;
        if (item->data(SingularityRoles::Id).toString() != userId)
            continue;
        const UserInfo info = m_store->user(userId);
        const QString name = item->text().isEmpty() ? info.displayName() : item->text();
        setFace(item, userId, name, info.avatarHash);
        return;
    }
}

void MemberListPanel::rebuild()
{
    if (m_guildId.isEmpty() || m_collapsed) {
        // Nothing is drawn while folded away. The list is rebuilt when it
        // comes back, so there is no point keeping it up to date in the dark.
        return;
    }

    const GuildInfo guild = m_store->guild(m_guildId);

    // A voice channel's people are the call, not the hundred members of
    // whichever text channel was asked about first. That other list is why
    // the panel showed a different crowd from the channel you were in.
    if (m_focusIsVoice && !m_focusChannel.isEmpty()) {
        const QStringList members = m_store->voiceMembers(m_focusChannel);
        m_counts->setText(QStringLiteral("<span style='color:%1'>●</span> %2 in this call")
                              .arg(QLatin1String(Theme::Green))
                              .arg(members.size()));

        QStringList ids;
        QList<bool> headings;
        ids.reserve(members.size() + 1);
        headings.reserve(members.size() + 1);
        ids.append(QStringLiteral("call"));
        headings.append(true);
        for (const QString &userId : members) {
            ids.append(userId);
            headings.append(false);
        }

        if (sameOrder(m_list, ids, headings)) {
            m_list->item(0)->setText(QStringLiteral("In this call — %1").arg(members.size()));
            for (int i = 0; i < members.size(); ++i) {
                QListWidgetItem *item = m_list->item(i + 1);
                item->setData(SingularityRoles::Status, m_store->presenceBubble(members.at(i)));
                item->setData(SingularityRoles::Subtitle, doingNow(*m_store, members.at(i)));
            }
            return;
        }

        const int scroll = m_list->verticalScrollBar() ? m_list->verticalScrollBar()->value() : 0;
        m_list->setUpdatesEnabled(false);
        m_list->clear();

        auto *heading = new QListWidgetItem(m_list);
        heading->setData(SingularityRoles::Heading, true);
        heading->setData(SingularityRoles::Id, QStringLiteral("call"));
        heading->setText(QStringLiteral("In this call — %1").arg(members.size()));
        heading->setFlags(Qt::NoItemFlags);

        for (const QString &userId : members) {
            const UserInfo info = m_store->user(userId);
            const QString name = info.displayName().isEmpty() ? userId : info.displayName();
            auto *item = new QListWidgetItem(name, m_list);
            item->setData(SingularityRoles::Id, userId);
            item->setData(SingularityRoles::Status, m_store->presenceBubble(userId));
            item->setData(SingularityRoles::Subtitle, doingNow(*m_store, userId));
            setFace(item, userId, name, info.avatarHash);
        }

        m_list->setUpdatesEnabled(true);
        if (m_list->verticalScrollBar())
            m_list->verticalScrollBar()->setValue(scroll);
        return;
    }

    const MemberList list = m_store->memberList(m_guildId);

    m_counts->setText(
        QStringLiteral("<span style='color:%1'>●</span> %2 online "
                       "<span style='color:%3'>· %4 members</span>")
            .arg(QLatin1String(Theme::Green))
            .arg(list.onlineCount)
            .arg(QLatin1String(Theme::TextFaint))
            .arg(list.memberCount));

    QStringList ids;
    QList<bool> headings;
    ids.reserve(list.rows.size());
    headings.reserve(list.rows.size());
    for (const MemberRow &row : list.rows) {
        ids.append(row.heading ? row.groupId : row.userId);
        headings.append(row.heading);
    }

    // Same people, same order. Write the new status onto the rows that are
    // already there and leave the pictures alone.
    if (sameOrder(m_list, ids, headings)) {
        for (int i = 0; i < list.rows.size(); ++i) {
            const MemberRow &row = list.rows.at(i);
            QListWidgetItem *item = m_list->item(i);
            if (row.heading) {
                const int count = list.groupCounts.value(row.groupId, row.groupCount);
                item->setText(headingName(row.groupId, guild, count));
                continue;
            }
            const UserInfo info = m_store->user(row.userId);
            const QString name = row.nickname.isEmpty() ? info.displayName() : row.nickname;
            item->setText(name);
            item->setData(SingularityRoles::Status, m_store->presenceBubble(row.userId));
            item->setData(SingularityRoles::Subtitle, doingNow(*m_store, row.userId));
            const RoleInfo role = guild.roles.value(row.colourRoleId);
            if (role.hasColour())
                item->setData(SingularityRoles::NameColour, QColor(QRgb(role.colour)));
            else
                item->setData(SingularityRoles::NameColour, QVariant());
        }
        return;
    }

    // Keeping the scroll position. A list that jumps back to the top every
    // few seconds is unusable.
    const int scroll = m_list->verticalScrollBar() ? m_list->verticalScrollBar()->value() : 0;

    m_list->setUpdatesEnabled(false);
    m_list->clear();

    for (const MemberRow &row : list.rows) {
        auto *item = new QListWidgetItem(m_list);

        if (row.heading) {
            item->setData(SingularityRoles::Heading, true);
            item->setData(SingularityRoles::Id, row.groupId);

            // The count off the separate list, falling back to whatever the
            // heading itself carried - which is usually nothing.
            const int count = list.groupCounts.value(row.groupId, row.groupCount);
            item->setText(headingName(row.groupId, guild, count));
            item->setFlags(Qt::NoItemFlags);
            continue;
        }

        const UserInfo info = m_store->user(row.userId);
        const QString name = row.nickname.isEmpty() ? info.displayName() : row.nickname;

        item->setData(SingularityRoles::Id, row.userId);
        item->setText(name);
        item->setData(SingularityRoles::Status, m_store->presenceBubble(row.userId));

        // What they are doing, under the name.
        item->setData(SingularityRoles::Subtitle, doingNow(*m_store, row.userId));

        // The colour their top role gives them. Zero means the role sets no
        // colour, which is different from black.
        const RoleInfo role = guild.roles.value(row.colourRoleId);
        if (role.hasColour())
            item->setData(SingularityRoles::NameColour, QColor(QRgb(role.colour)));

        setFace(item, row.userId, name, info.avatarHash);
    }

    m_list->setUpdatesEnabled(true);
    if (m_list->verticalScrollBar())
        m_list->verticalScrollBar()->setValue(scroll);
}
