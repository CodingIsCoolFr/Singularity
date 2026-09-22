#include "ui/FriendsPage.h"

#include "core/Logger.h"
#include "core/MessageStore.h"
#include "core/RestClient.h"
#include "ui/ListDelegates.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QDateTime>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
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

QPixmap roundedImage(const QImage &image, int size, int radius)
{
    if (image.isNull() || size <= 0)
        return {};
    const QPixmap source = QPixmap::fromImage(image).scaled(size, size, Qt::KeepAspectRatioByExpanding,
                                                            Qt::SmoothTransformation);
    QPixmap out(size, size);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, size, size), radius, radius);
    painter.setClipPath(path);
    painter.drawPixmap((size - source.width()) / 2, (size - source.height()) / 2, source);
    return out;
}

} // namespace

FriendsPage::FriendsPage(MessageStore *store, RestClient *rest, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
    , m_rest(rest)
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *mainColumn = new QWidget(this);
    auto *layout = new QVBoxLayout(mainColumn);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    root->addWidget(mainColumn, 1);

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

    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &) {
        if (isVisible())
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
    m_list->viewport()->setAutoFillBackground(false);
    // Rows are drawn, not built, so a few hundred friends stay smooth.
    m_list->setUniformItemSizes(true);

    auto *delegate = new FriendDelegate(m_list, m_list);
    m_list->setItemDelegate(delegate);

    connect(delegate, &FriendDelegate::messageRequested, this, &FriendsPage::startDirectMessage);
    connect(delegate, &FriendDelegate::profileRequested, this, &FriendsPage::openProfile);

    layout->addWidget(m_list, 1);

    m_activity = new QWidget(this);
    m_activity->setFixedWidth(300);
    m_activity->setObjectName(QStringLiteral("MemberList"));
    auto *activityColumn = new QVBoxLayout(m_activity);
    activityColumn->setContentsMargins(16, 16, 16, 16);
    activityColumn->setSpacing(10);

    auto *activityTitle = new QLabel(QStringLiteral("Active Now"), m_activity);
    activityTitle->setStyleSheet(QStringLiteral("color: %1; font-size: 16px; font-weight: 700;")
                                     .arg(QLatin1String(Theme::TextPrimary)));
    activityColumn->addWidget(activityTitle);

    auto *scroll = new QScrollArea(m_activity);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));
    auto *host = new QWidget(scroll);
    host->setStyleSheet(QStringLiteral("background: transparent;"));
    m_activityLayout = new QVBoxLayout(host);
    m_activityLayout->setContentsMargins(0, 0, 0, 0);
    m_activityLayout->setSpacing(8);
    m_activityLayout->addStretch(1);
    scroll->setWidget(host);
    activityColumn->addWidget(scroll, 1);
    root->addWidget(m_activity);

    connect(m_store, &MessageStore::voiceStatesChanged, this, [this]() {
        if (isVisible())
            m_artworkTimer.start();
    });
    connect(m_store, &MessageStore::userChanged, this, [this](const QString &) {
        if (isVisible())
            m_artworkTimer.start();
    });

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

    if (m_activity)
        m_activity->setVisible(tab == Tab::Online || tab == Tab::All);
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
        item->setData(SingularityRoles::Id, person.id);
        item->setData(SingularityRoles::Subtitle, activityLine(*m_store, person.id));
        item->setData(SingularityRoles::Status, m_store->presenceBubble(person.id));

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
    rebuildActivity();
}

void FriendsPage::rebuildActivity()
{
    if (!m_activityLayout)
        return;

    while (QLayoutItem *item = m_activityLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            delete widget;
        delete item;
    }

    const auto displayOf = [this](const QString &userId) {
        const UserInfo info = m_store->user(userId);
        const QString name = info.displayName();
        return name.isEmpty() ? userId : name;
    };

    const auto namesOf = [&](const QStringList &ids) {
        if (ids.isEmpty())
            return QString();
        if (ids.size() == 1)
            return displayOf(ids.first());
        if (ids.size() == 2)
            return QStringLiteral("%1 and %2").arg(displayOf(ids.at(0)), displayOf(ids.at(1)));
        return QStringLiteral("%1, %2, and %3 others")
            .arg(displayOf(ids.at(0)), displayOf(ids.at(1)))
            .arg(ids.size() - 2);
    };

    struct Card
    {
        QStringList userIds;
        QString title;
        QString subtitle;
        QString detail;
        QString guildId;
        QString channelId;
        QUrl art;
        QUrl badge;
        QUrl guildIcon;
    };
    QList<Card> cards;

    const auto askName = [this](const QString &userId) {
        if (userId.isEmpty() || m_namesAsked.contains(userId))
            return;
        const UserInfo info = m_store->user(userId);
        if (!info.username.isEmpty())
            return;
        m_namesAsked.insert(userId);
        m_rest->fetchUser(
            userId,
            [this](const QJsonObject &user) {
                m_store->rememberUser(user);
                if (isVisible())
                    m_artworkTimer.start();
            },
            [](const RestClient::Error &) {});
    };

    QHash<QString, QString> voiceGuild;
    for (const UserInfo &person : m_store->usersWithRelationship(1)) {
        const VoiceStateInfo state = m_store->voiceState(person.id);
        if (!state.channelId.isEmpty()) {
            voiceGuild.insert(state.channelId, state.guildId);
            continue;
        }

        const PresenceInfo presence = m_store->presence(person.id);
        for (const ActivityInfo &activity : presence.activities) {
            if (activity.isCustomStatus() || activity.name.isEmpty())
                continue;
            Card card;
            card.userIds = {person.id};
            card.title = displayOf(person.id);
            card.subtitle = activity.name;
            if (!activity.details.isEmpty())
                card.detail = activity.details;
            else if (!activity.state.isEmpty())
                card.detail = activity.state;
            if (activity.startMs > 0) {
                const qint64 minutes =
                    (QDateTime::currentMSecsSinceEpoch() - activity.startMs) / 60000;
                const QString elapsed = minutes > 0 ? QStringLiteral("for %1 minutes").arg(minutes)
                                                    : QStringLiteral("for less than a minute");
                card.detail = card.detail.isEmpty() ? elapsed : card.detail + QStringLiteral(" — ") + elapsed;
            }
            card.art = MediaCache::activityAssetUrl(activity.applicationId, activity.largeImage);
            card.badge = MediaCache::activityAssetUrl(activity.applicationId, activity.smallImage);
            cards.append(card);
            break;
        }
    }

    for (auto it = voiceGuild.cbegin(); it != voiceGuild.cend(); ++it) {
        const QString channelId = it.key();
        QStringList members = m_store->voiceMembers(channelId);
        if (members.isEmpty())
            continue;

        QStringList friendsFirst;
        QStringList rest;
        for (const QString &id : members) {
            askName(id);
            if (m_store->user(id).isFriend())
                friendsFirst.append(id);
            else
                rest.append(id);
        }
        Card card;
        card.userIds = friendsFirst + rest;
        card.title = namesOf(card.userIds);
        card.guildId = it.value();
        card.channelId = channelId;
        const ChannelInfo channel = m_store->channel(channelId);
        const GuildInfo guild = m_store->guild(card.guildId);
        const bool full = channel.userLimit > 0 && members.size() >= channel.userLimit;
        card.subtitle = full ? QStringLiteral("Voice channel full") : QStringLiteral("In a Voice Channel");
        card.detail = channel.name;
        if (full) {
            card.guildId.clear();
            card.channelId.clear();
        }
        card.guildIcon = MediaCache::guildIconUrl(guild.id, guild.iconHash, 32);
        cards.prepend(card);
    }

    if (cards.isEmpty()) {
        auto *quiet = new QLabel(QStringLiteral("It's quiet for now.\nWhen a friend is in a call or "
                                                "playing something, it shows up here."),
                                 m_activityLayout->parentWidget());
        quiet->setWordWrap(true);
        quiet->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;")
                                 .arg(QLatin1String(Theme::TextMuted)));
        m_activityLayout->addWidget(quiet);
        m_activityLayout->addStretch(1);
        return;
    }

    for (const Card &card : cards) {
        auto *frame = new QFrame(m_activityLayout->parentWidget());
        frame->setObjectName(QStringLiteral("ChatColumn"));
        frame->setCursor(Qt::PointingHandCursor);
        auto *box = new QVBoxLayout(frame);
        box->setContentsMargins(12, 10, 12, 10);
        box->setSpacing(6);

        auto *top = new QHBoxLayout;
        top->setSpacing(8);
        auto *faces = new QHBoxLayout;
        faces->setSpacing(-8);
        const int shown = qMin(card.channelId.isEmpty() ? 1 : 5, card.userIds.size());
        for (int i = 0; i < shown; ++i) {
            const UserInfo info = m_store->user(card.userIds.at(i));
            const QUrl url = MediaCache::avatarUrl(info.id, info.avatarHash, 64);
            const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
            auto *face = new QLabel(frame);
            face->setPixmap(picture.isNull() ? MediaCache::initialsAvatar(displayOf(info.id), 32)
                                             : MediaCache::circular(picture, 32));
            faces->addWidget(face);
        }
        top->addLayout(faces);

        auto *words = new QVBoxLayout;
        words->setSpacing(0);
        auto *title = new QLabel(card.title, frame);
        title->setWordWrap(true);
        title->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; font-weight: 600;")
                                 .arg(QLatin1String(Theme::TextPrimary)));
        words->addWidget(title);
        auto *subtitle = new QLabel(card.subtitle, frame);
        subtitle->setWordWrap(true);
        subtitle->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                                    .arg(QLatin1String(Theme::TextMuted)));
        words->addWidget(subtitle);
        top->addLayout(words, 1);

        if (!card.badge.isEmpty()) {
            const QImage badge = MediaCache::instance().image(card.badge);
            if (!badge.isNull()) {
                auto *mark = new QLabel(frame);
                mark->setPixmap(roundedImage(badge, 22, 6));
                top->addWidget(mark, 0, Qt::AlignTop);
            }
        }
        box->addLayout(top);

        if (!card.art.isEmpty() || !card.detail.isEmpty() || !card.guildIcon.isEmpty()) {
            auto *bottom = new QHBoxLayout;
            bottom->setSpacing(8);
            if (!card.guildIcon.isEmpty()) {
                const QImage icon = MediaCache::instance().image(card.guildIcon);
                if (!icon.isNull()) {
                    auto *mark = new QLabel(frame);
                    mark->setPixmap(MediaCache::circular(icon, 20));
                    bottom->addWidget(mark, 0, Qt::AlignVCenter);
                }
            }
            if (!card.art.isEmpty()) {
                const QImage art = MediaCache::instance().image(card.art);
                if (!art.isNull()) {
                    auto *picture = new QLabel(frame);
                    picture->setPixmap(roundedImage(art, 52, 8));
                    bottom->addWidget(picture);
                }
            }
            if (!card.detail.isEmpty()) {
                auto *detail = new QLabel(card.detail, frame);
                detail->setWordWrap(true);
                detail->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                                         .arg(QLatin1String(Theme::TextMuted)));
                bottom->addWidget(detail, 1);
            }
            box->addLayout(bottom);
        }

        for (QWidget *child : frame->findChildren<QWidget *>())
            child->setAttribute(Qt::WA_TransparentForMouseEvents);
        frame->installEventFilter(this);
        frame->setProperty("profileId", card.userIds.isEmpty() ? QString() : card.userIds.first());
        frame->setProperty("joinGuild", card.guildId);
        frame->setProperty("joinChannel", card.channelId);
        m_activityLayout->addWidget(frame);
    }
    m_activityLayout->addStretch(1);
}

bool FriendsPage::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        const QString channelId = watched->property("joinChannel").toString();
        const QString guildId = watched->property("joinGuild").toString();
        if (!channelId.isEmpty() && !guildId.isEmpty()) {
            emit joinVoiceChannel(guildId, channelId);
            return true;
        }
        const QString id = watched->property("profileId").toString();
        if (!id.isEmpty()) {
            emit openProfile(id);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
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
