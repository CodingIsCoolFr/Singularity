#include "ui/FriendsPage.h"

#include "core/Logger.h"
#include "core/MessageStore.h"
#include "core/RestClient.h"
#include "ui/CaptchaDialog.h"
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
#include <QSizePolicy>
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

// The second line of a row. Requests say which way they go, as Discord does;
// everyone else says what they are doing.
QString subtitleFor(const MessageStore &store, const UserInfo &person)
{
    if (person.relationship == 3)
        return QStringLiteral("Incoming friend request");
    if (person.relationship == 4)
        return QStringLiteral("Outgoing friend request");
    if (person.relationship == 2)
        return QStringLiteral("Blocked");
    return activityLine(store, person.id);
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

// A label whose width is whatever the card gives it.
//
// QLabel's own hint is the text on one line. Inside the Active Now scroller
// that hint became the width of the page, so the names ran past the window
// and the only way to read them was to scroll sideways.
class FittingLabel : public QLabel
{
public:
    explicit FittingLabel(const QString &text, QWidget *parent = nullptr)
        : QLabel(text, parent)
    {
        setWordWrap(true);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }

    QSize minimumSizeHint() const override
    {
        return {0, fontMetrics().lineSpacing()};
    }

    QSize sizeHint() const override
    {
        int width = this->width();
        if (width < 40)
            width = parentWidget() && parentWidget()->width() > 40 ? parentWidget()->width() - 24 : 280;
        return {0, heightForWidth(width)};
    }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override
    {
        if (width < 1)
            width = 1;
        return fontMetrics()
            .boundingRect(QRect(0, 0, width, 10000), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text())
            .height();
    }
};

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
    m_addPanel = buildAddPanel();
    m_addPanel->hide();
    layout->addWidget(m_addPanel);

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
    m_top = top;
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
    connect(delegate, &FriendDelegate::acceptRequested, this, &FriendsPage::acceptRequest);
    connect(delegate, &FriendDelegate::removeRequested, this, &FriendsPage::removeRelationship);

    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_list->itemAt(pos);
        if (!item)
            return;
        const QString userId = item->data(SingularityRoles::Id).toString();
        if (userId.isEmpty())
            return;
        emit personMenuRequested(userId, m_list->viewport()->mapToGlobal(pos));
    });

    layout->addWidget(m_list, 1);

    m_activity = new QWidget(this);
    m_activity->setFixedWidth(360);
    m_activity->setObjectName(QStringLiteral("MemberList"));
    auto *activityColumn = new QVBoxLayout(m_activity);
    activityColumn->setContentsMargins(16, 16, 12, 16);
    activityColumn->setSpacing(10);

    auto *activityTitle = new QLabel(QStringLiteral("Active Now"), m_activity);
    activityTitle->setStyleSheet(QStringLiteral("color: %1; font-size: 16px; font-weight: 700;")
                                     .arg(QLatin1String(Theme::TextPrimary)));
    activityColumn->addWidget(activityTitle);

    m_activityScroll = new QScrollArea(m_activity);
    m_activityScroll->setWidgetResizable(true);
    m_activityScroll->setFrameShape(QFrame::NoFrame);
    m_activityScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_activityScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));
    auto *host = new QWidget(m_activityScroll);
    host->setMinimumWidth(0);
    host->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    host->setStyleSheet(QStringLiteral("background: transparent;"));
    m_activityLayout = new QVBoxLayout(host);
    m_activityLayout->setContentsMargins(0, 0, 4, 0);
    m_activityLayout->setSpacing(8);
    m_activityLayout->addStretch(1);
    m_activityScroll->setWidget(host);
    m_activityScroll->viewport()->installEventFilter(this);
    activityColumn->addWidget(m_activityScroll, 1);
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

    // Filled in green, as Discord draws it; green writing once it is open.
    m_addTab = makeTab(QStringLiteral("Add Friend"), Tab::Add);
    m_addTab->setObjectName(QStringLiteral("AddFriendTab"));
    m_addTab->setStyleSheet(QStringLiteral(
        "QPushButton#AddFriendTab { background-color: %1; color: #ffffff; border: none; border-radius: 4px; "
        "padding: 3px 10px; font-weight: 600; }"
        "QPushButton#AddFriendTab[active=\"true\"] { background-color: transparent; color: %1; }")
                                .arg(QLatin1String(Theme::Green)));

    layout->addStretch(1);
    return bar;
}

QWidget *FriendsPage::buildAddPanel()
{
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(30, 20, 30, 20);
    layout->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("ADD FRIEND"), panel);
    title->setStyleSheet(QStringLiteral("color: %1; font-size: 16px; font-weight: 700;")
                             .arg(QLatin1String(Theme::TextPrimary)));
    auto *intro = new QLabel(QStringLiteral("You can add friends with their Discord username."), panel);
    intro->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));

    // The box and the button sit together in one dark bar, as in Discord.
    auto *bar = new QWidget(panel);
    bar->setObjectName(QStringLiteral("AddFriendBar"));
    bar->setStyleSheet(QStringLiteral("#AddFriendBar { background-color: %1; border: 1px solid %2; "
                                      "border-radius: 8px; }")
                           .arg(QLatin1String(Theme::SurfaceInput), QLatin1String(Theme::Border)));
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(12, 8, 8, 8);

    m_addField = new QLineEdit(bar);
    m_addField->setPlaceholderText(QStringLiteral("You can add friends with their Discord username."));
    m_addField->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; font-size: 15px; "
                                             "color: %1; }")
                                  .arg(QLatin1String(Theme::TextPrimary)));

    m_addSend = new QPushButton(QStringLiteral("Send Friend Request"), bar);
    m_addSend->setCursor(Qt::PointingHandCursor);
    m_addSend->setEnabled(false);
    m_addSend->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; color: #ffffff; border: none; border-radius: 4px; "
        "padding: 8px 16px; font-weight: 600; }"
        "QPushButton:disabled { background-color: %2; color: %3; }")
                                 .arg(QLatin1String(Theme::Green), QLatin1String(Theme::SurfaceHover),
                                      QLatin1String(Theme::TextFaint)));

    barLayout->addWidget(m_addField, 1);
    barLayout->addWidget(m_addSend);

    m_addResult = new QLabel(panel);
    m_addResult->setWordWrap(true);
    m_addResult->hide();

    layout->addWidget(title);
    layout->addWidget(intro);
    layout->addSpacing(6);
    layout->addWidget(bar);
    layout->addWidget(m_addResult);

    connect(m_addField, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_addSend->setEnabled(!m_addBusy && !text.trimmed().isEmpty());
        m_addResult->hide();
    });
    connect(m_addField, &QLineEdit::returnPressed, this, [this]() {
        if (m_addSend->isEnabled())
            sendFriendRequest();
    });
    connect(m_addSend, &QPushButton::clicked, this, [this]() { sendFriendRequest(); });
    return panel;
}

void FriendsPage::showAddResult(bool ok, const QString &text)
{
    m_addResult->setStyleSheet(QStringLiteral("color: %1;").arg(ok ? QLatin1String(Theme::Green)
                                                                   : QStringLiteral("#f23f43")));
    m_addResult->setText(text);
    m_addResult->show();
}

void FriendsPage::sendFriendRequest(const RestClient::CaptchaProof &captcha)
{
    // "@name" and "Name#1234" both come in from copying. The new unique
    // names have no number; an old tag keeps its four digits.
    QString name = m_addField->text().trimmed();
    if (name.startsWith(QLatin1Char('@')))
        name.remove(0, 1);
    int discriminator = 0;
    const int hash = name.lastIndexOf(QLatin1Char('#'));
    if (hash > 0) {
        bool numeric = false;
        const int number = name.mid(hash + 1).toInt(&numeric);
        if (numeric && name.size() - hash - 1 == 4) {
            discriminator = number;
            name = name.left(hash);
        }
    }
    if (name.isEmpty())
        return;

    m_addBusy = true;
    m_addSend->setEnabled(false);
    m_addSend->setText(QStringLiteral("Sending…"));

    const auto done = [this]() {
        m_addBusy = false;
        m_addSend->setText(QStringLiteral("Send Friend Request"));
        m_addSend->setEnabled(!m_addField->text().trimmed().isEmpty());
    };

    m_rest->sendFriendRequest(
        name, discriminator,
        [this, name, done](const QJsonObject &) {
            done();
            wlog(QStringLiteral("friends"), QStringLiteral("friend request sent to %1").arg(name));
            showAddResult(true, QStringLiteral("Success! Your friend request to %1 was sent.").arg(name));
            m_addField->clear();
            m_addResult->show();
        },
        [this, name, done](const RestClient::Error &error) {
            if (CaptchaDialog::isDemand(error.body)) {
                const QString token = CaptchaDialog::solve(
                    this, error.body.value(QStringLiteral("captcha_sitekey")).toString(),
                    error.body.value(QStringLiteral("captcha_rqdata")).toString());
                if (token.isEmpty()) {
                    done();
                    showAddResult(false, QStringLiteral("Discord asked for a captcha, and it was not finished, "
                                                        "so the request was not sent."));
                    return;
                }
                RestClient::CaptchaProof proof;
                proof.key = token;
                proof.rqtoken = error.body.value(QStringLiteral("captcha_rqtoken")).toString();
                proof.sessionId = error.body.value(QStringLiteral("captcha_session_id")).toString();
                sendFriendRequest(proof);
                return;
            }

            done();
            const int code = error.body.value(QStringLiteral("code")).toInt();
            wlog(QStringLiteral("friends"), QStringLiteral("friend request to %1 failed: HTTP %2, code %3, %4")
                                                .arg(name).arg(error.httpStatus).arg(code).arg(error.message));
            QString text;
            if (code == 80004)
                text = QStringLiteral("Hm, didn't work. Double check that the username is correct.");
            else if (code == 80007)
                text = QStringLiteral("You're already friends with that user!");
            else if (code == 80000 || code == 80001)
                text = QStringLiteral("That person is not taking friend requests right now.");
            else if (code == 80003)
                text = QStringLiteral("You can't send a friend request to yourself.");
            else if (error.isRateLimit())
                text = QStringLiteral("Discord says to slow down. Try again in a minute.");
            else
                text = error.message.isEmpty() ? QStringLiteral("Could not send the request.")
                                               : error.message.left(180);
            showAddResult(false, text);
        },
        captcha);
}

void FriendsPage::setTab(Tab tab)
{
    m_tab = tab;

    // Add Friend has its own panel in place of the list.
    const bool adding = tab == Tab::Add;
    if (m_addPanel)
        m_addPanel->setVisible(adding);
    if (m_search)
        m_search->setVisible(!adding);
    if (adding && m_addField)
        m_addField->setFocus();

    const QList<QPushButton *> tabs{m_onlineTab, m_allTab, m_pendingTab, m_blockedTab, m_addTab};
    const QList<Tab> kinds{Tab::Online, Tab::All, Tab::Pending, Tab::Blocked, Tab::Add};

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
    case Tab::Add:
        // Requests you have sent show under the box, so a new one appears
        // there the moment Discord confirms it.
        people = m_store->usersWithRelationship(4);
        heading = QStringLiteral("SENT REQUESTS");
        break;
    }

    QList<UserInfo> shown;
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

        shown.append(person);
    }

    bool samePeople = m_list->count() == shown.size();
    for (int i = 0; samePeople && i < shown.size(); ++i) {
        if (m_list->item(i)->data(SingularityRoles::Id).toString() != shown.at(i).id)
            samePeople = false;
    }
    if (samePeople) {
        for (int i = 0; i < shown.size(); ++i) {
            const UserInfo &person = shown.at(i);
            QListWidgetItem *item = m_list->item(i);
            QString name = person.displayName();
            if (name.isEmpty())
                name = person.username.isEmpty() ? person.id : person.username;
            item->setText(name);
            item->setData(SingularityRoles::Relationship, person.relationship);
            item->setData(SingularityRoles::Subtitle, subtitleFor(*m_store, person));
            item->setData(SingularityRoles::Status, m_store->presenceBubble(person.id));
            const QUrl url = MediaCache::avatarUrl(person.id, person.avatarHash, 80);
            const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
            if (!picture.isNull())
                item->setIcon(MediaCache::circular(picture, AvatarPixels));
        }
        m_heading->setText(QStringLiteral("%1 — %2").arg(heading).arg(m_list->count()));
        rebuildActivity();
        return;
    }

    // Filling the list fires a signal per row, which is wasted work here.
    m_list->setUpdatesEnabled(false);
    m_list->clear();

    for (const UserInfo &person : shown) {
        QString name = person.displayName();
        if (name.isEmpty())
            name = person.username.isEmpty() ? person.id : person.username;

        auto *item = new QListWidgetItem(name);
        item->setData(SingularityRoles::Id, person.id);
        item->setData(SingularityRoles::Relationship, person.relationship);
        item->setData(SingularityRoles::Subtitle, subtitleFor(*m_store, person));
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

    QScrollBar *bar = m_activityScroll ? m_activityScroll->verticalScrollBar() : nullptr;
    const int scrollY = bar ? bar->value() : 0;

    QStringList keys;
    keys.reserve(cards.size());
    for (const Card &card : cards) {
        keys.append(card.channelId.isEmpty()
                        ? QStringLiteral("game\n%1\n%2").arg(card.userIds.value(0), card.subtitle)
                        : QStringLiteral("voice\n%1").arg(card.channelId));
    }

    QList<QFrame *> frames;
    for (int i = 0; i < m_activityLayout->count(); ++i) {
        auto *frame = qobject_cast<QFrame *>(m_activityLayout->itemAt(i)->widget());
        if (frame)
            frames.append(frame);
    }

    bool sameCards = !cards.isEmpty() && frames.size() == cards.size();
    for (int i = 0; sameCards && i < cards.size(); ++i) {
        if (frames.at(i)->property("cardKey").toString() != keys.at(i))
            sameCards = false;
        if (!cards.at(i).art.isEmpty() && !MediaCache::instance().image(cards.at(i).art).isNull()) {
            auto *picture = frames.at(i)->findChild<QLabel *>(QStringLiteral("ActivityArt"));
            if (!picture || picture->pixmap().isNull())
                sameCards = false;
        }
    }
    if (sameCards) {
        for (int i = 0; i < cards.size(); ++i) {
            QFrame *frame = frames.at(i);
            if (auto *title = frame->findChild<QLabel *>(QStringLiteral("ActivityTitle"))) {
                title->setText(cards.at(i).title);
                title->updateGeometry();
            }
            if (auto *subtitle = frame->findChild<QLabel *>(QStringLiteral("ActivitySubtitle"))) {
                subtitle->setText(cards.at(i).subtitle);
                subtitle->updateGeometry();
            }
            if (auto *detail = frame->findChild<QLabel *>(QStringLiteral("ActivityDetail"))) {
                detail->setText(cards.at(i).detail);
                detail->updateGeometry();
            }
        }
        return;
    }

    while (QLayoutItem *item = m_activityLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            delete widget;
        delete item;
    }

    if (cards.isEmpty()) {
        auto *quiet = new FittingLabel(QStringLiteral("It's quiet for now. When a friend is in a call or "
                                                      "playing something, it shows up here."),
                                       m_activityLayout->parentWidget());
        quiet->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;")
                                 .arg(QLatin1String(Theme::TextMuted)));
        m_activityLayout->addWidget(quiet);
        m_activityLayout->addStretch(1);
        fitActivityWidth();
        return;
    }

    for (const Card &card : cards) {
        auto *frame = new QFrame(m_activityLayout->parentWidget());
        frame->setObjectName(QStringLiteral("ChatColumn"));
        frame->setCursor(Qt::PointingHandCursor);
        frame->setMinimumWidth(0);
        frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto *box = new QVBoxLayout(frame);
        box->setContentsMargins(12, 10, 12, 10);
        box->setSpacing(4);

        // Faces on their own row, then the words across the whole card.
        // Sitting the name beside five avatars left it a few letters wide,
        // which is why it ran off the edge of the window.
        auto *facesRow = new QHBoxLayout;
        facesRow->setSpacing(8);
        auto *faces = new QHBoxLayout;
        faces->setSpacing(-8);
        const int shown = qMin(card.channelId.isEmpty() ? 1 : 5, card.userIds.size());
        for (int i = 0; i < shown; ++i) {
            const UserInfo info = m_store->user(card.userIds.at(i));
            const QUrl url = MediaCache::avatarUrl(info.id, info.avatarHash, 64);
            const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
            auto *face = new QLabel(frame);
            face->setFixedSize(32, 32);
            face->setPixmap(picture.isNull() ? MediaCache::initialsAvatar(displayOf(info.id), 32)
                                             : MediaCache::circular(picture, 32));
            faces->addWidget(face);
        }
        facesRow->addLayout(faces);
        facesRow->addStretch(1);

        if (!card.badge.isEmpty()) {
            const QImage badge = MediaCache::instance().image(card.badge);
            if (!badge.isNull()) {
                auto *mark = new QLabel(frame);
                mark->setPixmap(roundedImage(badge, 22, 6));
                facesRow->addWidget(mark, 0, Qt::AlignTop);
            }
        }
        box->addLayout(facesRow);

        auto *title = new FittingLabel(card.title, frame);
        title->setObjectName(QStringLiteral("ActivityTitle"));
        title->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; font-weight: 600;")
                                 .arg(QLatin1String(Theme::TextPrimary)));
        box->addWidget(title);
        auto *subtitle = new FittingLabel(card.subtitle, frame);
        subtitle->setObjectName(QStringLiteral("ActivitySubtitle"));
        subtitle->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                                    .arg(QLatin1String(Theme::TextMuted)));
        box->addWidget(subtitle);

        if (!card.art.isEmpty() || !card.detail.isEmpty() || !card.guildIcon.isEmpty()) {
            auto *bottom = new QHBoxLayout;
            bottom->setSpacing(8);
            if (!card.guildIcon.isEmpty()) {
                const QImage icon = MediaCache::instance().image(card.guildIcon);
                if (!icon.isNull()) {
                    auto *mark = new QLabel(frame);
                    mark->setFixedSize(20, 20);
                    mark->setPixmap(MediaCache::circular(icon, 20));
                    bottom->addWidget(mark, 0, Qt::AlignVCenter);
                }
            }
            if (!card.art.isEmpty()) {
                const QImage art = MediaCache::instance().image(card.art);
                if (!art.isNull()) {
                    auto *picture = new QLabel(frame);
                    picture->setObjectName(QStringLiteral("ActivityArt"));
                    picture->setFixedSize(52, 52);
                    picture->setPixmap(roundedImage(art, 52, 8));
                    bottom->addWidget(picture, 0, Qt::AlignTop);
                }
            }
            if (!card.detail.isEmpty()) {
                auto *detail = new FittingLabel(card.detail, frame);
                detail->setObjectName(QStringLiteral("ActivityDetail"));
                detail->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                                         .arg(QLatin1String(Theme::TextMuted)));
                bottom->addWidget(detail, 1, Qt::AlignVCenter);
            }
            box->addLayout(bottom);
        }

        for (QWidget *child : frame->findChildren<QWidget *>())
            child->setAttribute(Qt::WA_TransparentForMouseEvents);
        frame->installEventFilter(this);
        frame->setProperty("cardKey", keys.at(m_activityLayout->count()));
        frame->setProperty("profileId", card.userIds.isEmpty() ? QString() : card.userIds.first());
        frame->setProperty("joinGuild", card.guildId);
        frame->setProperty("joinChannel", card.channelId);
        m_activityLayout->addWidget(frame);
    }
    m_activityLayout->addStretch(1);
    fitActivityWidth();
    if (bar)
        bar->setValue(scrollY);
}

void FriendsPage::fitActivityWidth()
{
    if (!m_activityScroll)
        return;
    QWidget *host = m_activityScroll->widget();
    if (!host)
        return;
    const int width = m_activityScroll->viewport()->width();
    if (width > 0)
        host->setMaximumWidth(width);
}

bool FriendsPage::eventFilter(QObject *watched, QEvent *event)
{
    if (m_activityScroll && watched == m_activityScroll->viewport() && event->type() == QEvent::Resize) {
        fitActivityWidth();
        return false;
    }
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

void FriendsPage::acceptRequest(const QString &userId)
{
    if (userId.isEmpty() || m_busy.contains(userId))
        return;
    m_busy.insert(userId);

    const auto accepted = [this, userId](const QJsonObject &) {
        m_busy.remove(userId);
        m_store->setRelationship(userId, 1);
        refresh();
    };
    const auto failed = [this, userId](const RestClient::Error &error) {
        m_busy.remove(userId);
        wlog(QStringLiteral("friends"), QStringLiteral("accepting a request failed: HTTP %1 %2")
                                            .arg(error.httpStatus).arg(error.message));
        emit statusMessage(error.message.isEmpty() ? QStringLiteral("Could not accept that request.")
                                                   : error.message.left(180));
    };

    // Accepting is the same call as sending a request. Discord sometimes
    // asks for a check first; the person solves it, exactly as in its own
    // client, and the same call goes again with the answer.
    m_rest->addFriend(userId, accepted, [this, userId, accepted, failed](const RestClient::Error &error) {
        if (!CaptchaDialog::isDemand(error.body)) {
            failed(error);
            return;
        }
        const QString token = CaptchaDialog::solve(
            this, error.body.value(QStringLiteral("captcha_sitekey")).toString(),
            error.body.value(QStringLiteral("captcha_rqdata")).toString());
        if (token.isEmpty()) {
            m_busy.remove(userId);
            emit statusMessage(QStringLiteral("Discord asked for a check, and it was not finished."));
            return;
        }
        RestClient::CaptchaProof proof;
        proof.key = token;
        proof.rqtoken = error.body.value(QStringLiteral("captcha_rqtoken")).toString();
        proof.sessionId = error.body.value(QStringLiteral("captcha_session_id")).toString();
        m_rest->addFriend(userId, accepted, failed, proof);
    });
}

void FriendsPage::removeRelationship(const QString &userId)
{
    if (userId.isEmpty() || m_busy.contains(userId))
        return;
    m_busy.insert(userId);

    const int was = m_store->user(userId).relationship;
    m_rest->removeRelationship(
        userId,
        [this, userId](const QJsonObject &) {
            m_busy.remove(userId);
            m_store->setRelationship(userId, 0);
            refresh();
        },
        [this, userId, was](const RestClient::Error &error) {
            m_busy.remove(userId);
            wlog(QStringLiteral("friends"), QStringLiteral("removing relationship (type %1) failed: HTTP %2 %3")
                                                .arg(was).arg(error.httpStatus).arg(error.message));
            const QString what = was == 3   ? QStringLiteral("ignore that request")
                                 : was == 4 ? QStringLiteral("cancel that request")
                                 : was == 2 ? QStringLiteral("unblock them")
                                            : QStringLiteral("do that");
            emit statusMessage(QStringLiteral("Could not %1: %2")
                                   .arg(what, error.message.isEmpty() ? QStringLiteral("no reason given")
                                                                      : error.message.left(160)));
        });
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
