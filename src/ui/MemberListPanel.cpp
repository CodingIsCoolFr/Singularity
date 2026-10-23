#include "ui/MemberListPanel.h"

#include "core/AppConfig.h"
#include "core/MessageStore.h"
#include "ui/ListDelegates.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QAbstractListModel>
#include <QColor>
#include <QIcon>
#include <QLabel>
#include <QListView>
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
    QStringList games;
    for (const ActivityInfo &activity : presence.activities) {
        if (activity.isCustomStatus()) {
            custom = activity.emoji.isEmpty()
                ? activity.state
                : QStringLiteral("%1 %2").arg(activity.emoji, activity.state).trimmed();
            continue;
        }
        const QString line = (activity.type == 2 && !activity.details.isEmpty())
            ? activity.details
            : activity.name;
        if (line.isEmpty() || games.size() >= 2)
            continue;
        games.append(line);
    }

    if (!games.isEmpty())
        return games.join(QStringLiteral(", "));

    return custom;
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

const QString CallGroup = QStringLiteral("call");

} // namespace

// The rows the list draws, worked out when they are drawn rather than made up
// front.
//
// A server can have tens of thousands of people online. Discord's list is
// virtual: it draws the forty on screen and fetches only their pictures, and
// so does this. Every row used to be a QListWidgetItem built on each change,
// with its picture asked for there and then, which was fine for the hundred
// rows that were all that ever arrived and is not for the whole list.
class MemberRowsModel : public QAbstractListModel
{
public:
    MemberRowsModel(MessageStore *store, QObject *parent)
        : QAbstractListModel(parent)
        , m_store(store)
    {
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : int(m_rows.size());
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size())
            return Qt::NoItemFlags;
        const MemberRow &row = m_rows.at(index.row());
        return row.loaded() && !row.heading ? Qt::ItemIsEnabled : Qt::NoItemFlags;
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size())
            return {};
        const MemberRow &row = m_rows.at(index.row());

        if (role == SingularityRoles::Heading)
            return row.heading;
        if (row.heading) {
            if (role == SingularityRoles::Id)
                return row.groupId;
            if (role == Qt::DisplayRole) {
                if (row.groupId == CallGroup)
                    return QStringLiteral("In this call — %1").arg(row.groupCount);
                // The count off the separate list, falling back to whatever
                // the heading itself carried.
                return headingName(row.groupId, m_guild, m_groupCounts.value(row.groupId, row.groupCount));
            }
            return {};
        }

        // A place Discord has not sent yet: no data, drawn as a placeholder.
        if (row.userId.isEmpty())
            return {};

        switch (role) {
        case SingularityRoles::Id:
            return row.userId;
        case SingularityRoles::GuildId:
            return m_guildId;
        default:
            break;
        }

        const Shown &shown = shownFor(row);
        switch (role) {
        case Qt::DisplayRole:
            return shown.name;
        case Qt::DecorationRole:
            return shown.face;
        case SingularityRoles::Status:
            return shown.status;
        case SingularityRoles::Subtitle:
            return shown.subtitle;
        case SingularityRoles::NameColour:
            return shown.colour;
        case SingularityRoles::AvatarUrl:
            return shown.avatarUrl;
        default:
            return {};
        }
    }

    // New rows. True when the list changed shape and was reset; false when
    // the same people sit in the same places, where only a repaint is needed
    // - a status tick does not change who is listed, and rebuilding faces
    // for one bubble is what stalled the black hole for a moment.
    bool show(const QString &guildId, const QList<MemberRow> &rows, const QHash<QString, int> &groupCounts,
              const GuildInfo &guild)
    {
        m_shown.clear();
        m_groupCounts = groupCounts;
        m_guild = guild;
        if (guildId == m_guildId && sameShape(rows)) {
            m_rows = rows;
            return false;
        }
        beginResetModel();
        m_guildId = guildId;
        m_rows = rows;
        endResetModel();
        return true;
    }

    // A picture arrived for this person. True when they were on screen and
    // the list wants painting again.
    bool forgetFace(const QString &userId) { return m_shown.remove(userId); }

private:
    struct Shown
    {
        QString name;
        QString status;
        QString subtitle;
        QString avatarUrl;
        QVariant colour;
        QIcon face;
    };

    bool sameShape(const QList<MemberRow> &rows) const
    {
        if (rows.size() != m_rows.size())
            return false;
        for (int i = 0; i < rows.size(); ++i) {
            const MemberRow &a = rows.at(i);
            const MemberRow &b = m_rows.at(i);
            if (a.heading != b.heading || (a.heading ? a.groupId != b.groupId : a.userId != b.userId))
                return false;
        }
        return true;
    }

    // Everything a row shows about its person, read from the store the first
    // time the row is drawn after a change.
    const Shown &shownFor(const MemberRow &row) const
    {
        const auto found = m_shown.constFind(row.userId);
        if (found != m_shown.constEnd())
            return found.value();

        const UserInfo info = m_store->user(row.userId);
        Shown shown;
        shown.name = row.nickname.isEmpty() ? info.displayName() : row.nickname;
        if (shown.name.isEmpty())
            shown.name = row.userId;
        shown.status = m_store->presenceBubble(row.userId);
        shown.subtitle = doingNow(*m_store, row.userId);

        // The colour their top role gives them. Zero means the role sets no
        // colour, which is different from black.
        const RoleInfo role = m_guild.roles.value(row.colourRoleId);
        if (role.hasColour())
            shown.colour = QColor(QRgb(role.colour));

        // Asked for here, so only the people drawn have theirs fetched.
        const QUrl url = MediaCache::avatarUrl(row.userId, info.avatarHash, 64);
        const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
        shown.face = picture.isNull() ? QIcon(MediaCache::initialsAvatar(shown.name, 32))
                                      : QIcon(MediaCache::circular(picture, 32));
        if (info.avatarHash.startsWith(QLatin1String("a_")))
            shown.avatarUrl = url.toString();

        return *m_shown.insert(row.userId, shown);
    }

    MessageStore *m_store = nullptr;
    QString m_guildId;
    GuildInfo m_guild;
    QList<MemberRow> m_rows;
    QHash<QString, int> m_groupCounts;
    mutable QHash<QString, Shown> m_shown;
};

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

    m_model = new MemberRowsModel(m_store, this);

    m_list = new QListView(m_body);
    m_list->setObjectName(QStringLiteral("MemberList"));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    // Headings and people are one height, as Discord's are (both about 42
    // pixels), so laying out a list of any length is a multiplication.
    m_list->setUniformItemSizes(true);
    m_list->setMouseTracking(true);
    m_list->setModel(m_model);
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

    connect(m_list, &QListView::clicked, this, [this](const QModelIndex &index) {
        if (!index.isValid() || index.data(SingularityRoles::Heading).toBool())
            return;
        const QString userId = index.data(SingularityRoles::Id).toString();
        if (!userId.isEmpty())
            emit profileRequested(userId);
    });

    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListView::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex index = m_list->indexAt(pos);
        if (!index.isValid() || index.data(SingularityRoles::Heading).toBool())
            return;
        const QString userId = index.data(SingularityRoles::Id).toString();
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

    // More of the list is asked for once scrolling settles, not on every
    // step of it, and a second apart at most.
    m_rangeTimer = new QTimer(this);
    m_rangeTimer->setSingleShot(true);
    m_rangeTimer->setInterval(250);
    connect(m_rangeTimer, &QTimer::timeout, this, [this]() {
        if (m_sinceRangeAsk.isValid() && m_sinceRangeAsk.elapsed() < 1000) {
            m_rangeTimer->start(int(1000 - m_sinceRangeAsk.elapsed()));
            return;
        }
        askForVisibleRows();
    });
    const auto settle = [this]() { m_rangeTimer->start(250); };
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, settle);
    // A taller panel shows more rows, which changes the scroll range.
    connect(m_list->verticalScrollBar(), &QScrollBar::rangeChanged, this, settle);

    // Pictures arrive later than the list does, so a face that was a grey
    // circle when the row was drawn has to be put in when it turns up.
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
    // Another server's list starts at its top, as Discord's does.
    m_list->scrollToTop();
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
    // Only a face that was drawn needs drawing again; pictures arrive for the
    // whole window, not just this list.
    if (m_model->forgetFace(userId))
        m_list->viewport()->update();
}

void MemberListPanel::askForVisibleRows()
{
    if (m_guildId.isEmpty() || m_collapsed || m_focusIsVoice || !isVisible())
        return;
    const int count = m_model->rowCount();
    if (count <= 0)
        return;

    const QModelIndex top = m_list->indexAt(QPoint(8, 1));
    const QModelIndex bottom = m_list->indexAt(QPoint(8, m_list->viewport()->height() - 2));
    const int first = top.isValid() ? top.row() : 0;
    const int last = bottom.isValid() ? bottom.row() : count - 1;

    m_sinceRangeAsk.start();
    emit rangesWanted(m_guildId, memberListPages(first, last));
}

void MemberListPanel::rebuild()
{
    if (m_guildId.isEmpty() || m_collapsed) {
        // Nothing is drawn while folded away. The list is rebuilt when it
        // comes back, so there is no point keeping it up to date in the dark.
        return;
    }

    const GuildInfo guild = m_store->guild(m_guildId);
    QList<MemberRow> rows;
    QHash<QString, int> groupCounts;

    // A voice channel's people are the call, not the hundred members of
    // whichever text channel was asked about first. That other list is why
    // the panel showed a different crowd from the channel you were in.
    if (m_focusIsVoice && !m_focusChannel.isEmpty()) {
        const QStringList members = m_store->voiceMembers(m_focusChannel);
        m_counts->setText(QStringLiteral("<span style='color:%1'>●</span> %2 in this call")
                              .arg(QLatin1String(Theme::Green))
                              .arg(members.size()));

        MemberRow title;
        title.heading = true;
        title.groupId = CallGroup;
        title.groupCount = int(members.size());
        rows.reserve(members.size() + 1);
        rows.append(title);
        for (const QString &userId : members) {
            MemberRow person;
            person.userId = userId;
            rows.append(person);
        }
    } else {
        const MemberList list = m_store->memberList(m_guildId);
        m_counts->setText(
            QStringLiteral("<span style='color:%1'>●</span> %2 online "
                           "<span style='color:%3'>· %4 members</span>")
                .arg(QLatin1String(Theme::Green))
                .arg(list.onlineCount)
                .arg(QLatin1String(Theme::TextFaint))
                .arg(list.memberCount));
        rows = list.rows;
        groupCounts = list.groupCounts;
    }

    // Keeping the scroll position. A list that jumps back to the top every
    // few seconds is unusable.
    const int scroll = m_list->verticalScrollBar()->value();
    if (m_model->show(m_guildId, rows, groupCounts, guild)) {
        m_list->doItemsLayout();
        m_list->verticalScrollBar()->setValue(scroll);
    }
    m_list->viewport()->update();

    // A list that changed may have new places on screen, a new channel's
    // list above all: its pages are asked for afresh.
    if (!m_focusIsVoice)
        m_rangeTimer->start(250);
}
