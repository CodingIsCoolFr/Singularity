#include "ui/FriendsPage.h"

#include "core/Logger.h"
#include "core/MessageStore.h"
#include "core/RestClient.h"
#include "ui/ListDelegates.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QStyle>
#include <QVBoxLayout>

namespace {

constexpr int AvatarPixels = 34;

QString activityLine(const MessageStore &store, const QString &userId)
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

    if (!custom.isEmpty())
        return custom;

    const QString hint = store.activityHint(userId);
    if (!hint.isEmpty())
        return QStringLiteral("shows offline, but %1").arg(hint);

    if (presence.status == QLatin1String("dnd"))
        return QStringLiteral("Do not disturb");
    if (presence.status == QLatin1String("idle"))
        return QStringLiteral("Away");
    if (presence.isOnline())
        return QStringLiteral("Online");
    return QStringLiteral("Offline");
}

} // namespace

FriendsPage::FriendsPage(MessageStore *store, RestClient *rest, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
    , m_rest(rest)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    layout->addWidget(buildTabBar());

    // Faces arrive long after the rows do.
    //
    // Without this the page drew the initials circle once and never looked
    // again, so every friend kept a grey placeholder for the whole session
    // even though the picture had been downloaded seconds later. Redraw when
    // the arrivals stop rather than on each one: a few hundred friends means a
    // few hundred arrivals, and rebuilding on every one makes the list thrash.
    m_artworkTimer.setSingleShot(true);
    m_artworkTimer.setInterval(250);
    connect(&m_artworkTimer, &QTimer::timeout, this, [this]() {
        if (!isVisible())
            return;

        // A rebuild starts at the top, which would drag the list out from
        // under anyone scrolling through it.
        const int scroll = m_list->verticalScrollBar()->value();
        refresh();
        m_list->verticalScrollBar()->setValue(scroll);
    });

    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &url) {
        if (!isVisible())
            return;
        if (!url.path().startsWith(QLatin1String("/avatars/"))
            && !url.path().startsWith(QLatin1String("/embed/avatars/"))) {
            return;
        }
        m_artworkTimer.start();
    });

    auto *top = new QWidget(this);
    auto *topLayout = new QVBoxLayout(top);
    topLayout->setContentsMargins(24, 12, 24, 6);
    topLayout->setSpacing(8);

    m_search = new QLineEdit(top);
    m_search->setPlaceholderText(QStringLiteral("Search"));
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &) { refresh(); });
    topLayout->addWidget(m_search);

    m_heading = new QLabel(top);
    m_heading->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 700; "
                                            "letter-spacing: 0.6px;")
                                 .arg(QLatin1String(Theme::TextMuted)));
    topLayout->addWidget(m_heading);
    layout->addWidget(top);

    m_list = new QListWidget(this);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; }"));
    // Rows are drawn, not built, so a few hundred friends stay smooth.
    m_list->setUniformItemSizes(true);

    auto *delegate = new FriendDelegate(m_list, m_list);
    m_list->setItemDelegate(delegate);

    connect(delegate, &FriendDelegate::messageRequested, this, &FriendsPage::startDirectMessage);
    connect(delegate, &FriendDelegate::profileRequested, this, &FriendsPage::openProfile);

    layout->addWidget(m_list, 1);

    setTab(Tab::Online);
}

QWidget *FriendsPage::buildTabBar()
{
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("ChatHeader"));
    bar->setFixedHeight(56);

    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(20, 8, 20, 8);
    layout->setSpacing(16);

    auto *title = new QLabel(QStringLiteral("Friends"), bar);
    title->setObjectName(QStringLiteral("ChannelTitle"));
    layout->addWidget(title);

    const auto makeTab = [&](const QString &text, Tab tab) {
        auto *button = new QPushButton(text, bar);
        button->setObjectName(QStringLiteral("TabButton"));
        button->setFlat(true);
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QPushButton::clicked, this, [this, tab]() { setTab(tab); });
        layout->addWidget(button);
        return button;
    };

    m_onlineTab = makeTab(QStringLiteral("Online"), Tab::Online);
    m_allTab = makeTab(QStringLiteral("All"), Tab::All);
    m_pendingTab = makeTab(QStringLiteral("Pending"), Tab::Pending);
    m_blockedTab = makeTab(QStringLiteral("Blocked"), Tab::Blocked);

    layout->addStretch(1);
    return bar;
}

void FriendsPage::setTab(Tab tab)
{
    m_tab = tab;

    const QList<QPushButton *> tabs{m_onlineTab, m_allTab, m_pendingTab, m_blockedTab};
    const QList<Tab> kinds{Tab::Online, Tab::All, Tab::Pending, Tab::Blocked};

    for (int i = 0; i < tabs.size(); ++i) {
        tabs.at(i)->setProperty("active", kinds.at(i) == tab);
        // Qt only restyles when told the property changed.
        tabs.at(i)->style()->unpolish(tabs.at(i));
        tabs.at(i)->style()->polish(tabs.at(i));
    }

    refresh();
}

void FriendsPage::refresh()
{
    const QString filter = m_search ? m_search->text().trimmed() : QString();

    QList<UserInfo> people;
    QString heading;

    switch (m_tab) {
    case Tab::Online:
        people = m_store->usersWithRelationship(1);
        heading = QStringLiteral("ONLINE");
        break;
    case Tab::All:
        people = m_store->usersWithRelationship(1);
        heading = QStringLiteral("ALL FRIENDS");
        break;
    case Tab::Pending:
        people = m_store->usersWithRelationship(3) + m_store->usersWithRelationship(4);
        heading = QStringLiteral("PENDING");
        break;
    case Tab::Blocked:
        people = m_store->usersWithRelationship(2);
        heading = QStringLiteral("BLOCKED");
        break;
    }

    // Filling the list fires a signal per row, which is wasted work here.
    m_list->setUpdatesEnabled(false);
    m_list->clear();

    for (const UserInfo &person : people) {
        if (m_tab == Tab::Online) {
            const bool around = m_store->presence(person.id).isOnline()
                || !m_store->activityHint(person.id).isEmpty();
            if (!around)
                continue;
        }

        // A name we never learned would otherwise show as a blank row.
        QString name = person.displayName();
        if (name.isEmpty())
            name = person.username.isEmpty() ? person.id : person.username;

        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive)
            && !person.username.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }

        auto *item = new QListWidgetItem(name);
        item->setData(WispRoles::Id, person.id);
        item->setData(WispRoles::Subtitle, activityLine(*m_store, person.id));
        item->setData(WispRoles::Status, m_store->presenceBubble(person.id));

        // The plain round picture. The status bubble is painted on top by the
        // delegate, so nothing is composed per row.
        const QUrl url = MediaCache::avatarUrl(person.id, person.avatarHash, 80);
        const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
        item->setIcon(picture.isNull() ? MediaCache::initialsAvatar(name, AvatarPixels)
                                       : MediaCache::circular(picture, AvatarPixels));

        m_list->addItem(item);
    }

    m_list->setUpdatesEnabled(true);
    m_heading->setText(QStringLiteral("%1 — %2").arg(heading).arg(m_list->count()));
}

void FriendsPage::startDirectMessage(const QString &userId)
{
    const QString existing = m_store->directChannelWith(userId);
    if (!existing.isEmpty()) {
        emit openDirectMessage(existing);
        return;
    }

    m_rest->openDirectMessage(
        userId,
        [this](const QJsonObject &channel) {
            const QString channelId = channel.value(QStringLiteral("id")).toString();
            if (channelId.isEmpty())
                return;
            m_store->ingestChannelObject(channel);
            emit openDirectMessage(channelId);
        },
        [](const RestClient::Error &error) {
            wlog(QStringLiteral("friends"), QStringLiteral("could not open a chat: HTTP %1 %2")
                                                .arg(error.httpStatus).arg(error.message));
        });
}
