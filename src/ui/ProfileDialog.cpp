#include "ui/CaptchaDialog.h"
#include "ui/ProfileDialog.h"

#include "core/Logger.h"
#include "core/RestClient.h"
#include "ui/AnimatedImage.h"
#include "ui/FlowLayout.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

namespace {

constexpr int DialogWidth = 940;
constexpr int DialogHeight = 700;
constexpr int LeftCardWidth = 400;
constexpr int BannerHeight = 150;
constexpr int AvatarPixels = 112;
constexpr int CornerRadius = 14;

// Status colours, matching the real client closely enough to read at a glance.
constexpr auto StatusOnline = "#23a55a";
constexpr auto StatusIdle = "#f0b232";
constexpr auto StatusDnd = "#f23f43";
constexpr auto StatusOffline = "#80848e";

QString statusColour(const QString &status)
{
    if (status == QLatin1String("online"))
        return QLatin1String(StatusOnline);
    if (status == QLatin1String("idle"))
        return QLatin1String(StatusIdle);
    if (status == QLatin1String("dnd"))
        return QLatin1String(StatusDnd);
    return QLatin1String(StatusOffline);
}

QString statusWord(const QString &status)
{
    if (status == QLatin1String("online"))
        return QStringLiteral("Online");
    if (status == QLatin1String("idle"))
        return QStringLiteral("Away");
    if (status == QLatin1String("dnd"))
        return QStringLiteral("Do not disturb");
    if (status == QLatin1String("hint"))
        return QStringLiteral("Shows offline, but looks active");
    return QStringLiteral("Offline");
}

// Every badge Discord grants through the public flags bitfield. Icons are not
// public art, so short words are used instead.
QStringList badgeNames(int flags)
{
    struct Badge { int bit; const char *label; };
    static const Badge kBadges[]{
        {0, "Staff"},          {1, "Partner"},         {2, "HypeSquad"},
        {3, "Bug Hunter"},     {6, "Bravery"},         {7, "Brilliance"},
        {8, "Balance"},        {9, "Early Supporter"}, {14, "Bug Hunter 2"},
        {17, "Dev"},           {18, "Moderator"},      {22, "Active Dev"},
    };

    QStringList result;
    for (const Badge &badge : kBadges) {
        if (flags & (1 << badge.bit))
            result << QLatin1String(badge.label);
    }
    return result;
}

QLabel *sectionTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 700; "
                                        "letter-spacing: 0.6px; background: transparent;")
                             .arg(QLatin1String(Theme::TextMuted)));
    return label;
}

// A widget that paints the banner itself. Setting a palette brush is not
// enough, because the widget has no final size until the layout has run.
class BannerWidget : public QWidget
{
public:
    explicit BannerWidget(QWidget *parent = nullptr)
        : QWidget(parent)
        , m_source(new AnimatedImage(this))
    {
        connect(m_source, &AnimatedImage::frameChanged, this, [this]() { update(); });
    }

    // `bytes` wins when the picture moves, `still` is the fallback frame.
    void setSource(const QByteArray &bytes, const QImage &still, const QColor &fallback)
    {
        m_fallback = fallback;
        m_still = still;
        if (bytes.isEmpty())
            m_source->clear();
        else
            m_source->setData(bytes);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        // Rounded at the top only, because it sits on the card's top edge.
        QPainterPath clip;
        clip.addRoundedRect(QRectF(0, 0, width(), height() + 12), 12, 12);
        painter.setClipPath(clip);

        painter.fillRect(rect(), m_fallback.isValid() ? m_fallback : QColor(Theme::SurfaceRail));

        QImage frame = m_source->currentFrame();
        if (frame.isNull())
            frame = m_still;
        if (frame.isNull())
            return;

        const QImage scaled = frame.scaled(size(), Qt::KeepAspectRatioByExpanding,
                                           Qt::SmoothTransformation);
        painter.drawImage(QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2), scaled);
    }

private:
    AnimatedImage *m_source = nullptr;
    QImage m_still;
    QColor m_fallback;
};

// Paints the round avatar, the decoration frame around it, and the status
// bubble on its corner. Both the avatar and the frame can be animations, so
// this is a live widget rather than a one-off pixmap.
class AvatarView : public QWidget
{
public:
    explicit AvatarView(int size, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_avatar(new AnimatedImage(this))
        , m_decoration(new AnimatedImage(this))
    {
        setFixedSize(size, size);
        connect(m_avatar, &AnimatedImage::frameChanged, this, [this]() { update(); });
        connect(m_decoration, &AnimatedImage::frameChanged, this, [this]() { update(); });
    }

    void setAvatar(const QByteArray &bytes, const QImage &still, const QString &fallbackName)
    {
        m_still = still;
        m_fallbackName = fallbackName;
        if (bytes.isEmpty())
            m_avatar->clear();
        else
            m_avatar->setData(bytes);
        update();
    }

    void setDecoration(const QByteArray &bytes, const QImage &still)
    {
        m_decorationStill = still;
        if (bytes.isEmpty())
            m_decoration->clear();
        else
            m_decoration->setData(bytes);
        update();
    }

    void setStatus(const QString &status)
    {
        m_status = status;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const int size = width();

        QImage decoration = m_decoration->currentFrame();
        if (decoration.isNull())
            decoration = m_decorationStill;

        // With a decoration the avatar shrinks so the frame has room around it.
        const bool framed = !decoration.isNull();
        const int inner = framed ? qRound(size * 0.76) : size;
        const int offset = (size - inner) / 2;

        QImage face = m_avatar->currentFrame();
        if (face.isNull())
            face = m_still;

        const QPixmap circle = face.isNull() ? MediaCache::initialsAvatar(m_fallbackName, inner)
                                             : MediaCache::circular(face, inner);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(offset, offset, circle);

        if (framed) {
            painter.drawImage(QRect(0, 0, size, size),
                              decoration.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }

        // --- status bubble, hugging the avatar circle rather than the frame.
        const int bubble = qMax(18, inner / 4);
        const int ring = qMax(3, size / 34);
        const QRect area(offset + inner - bubble, offset + inner - bubble, bubble, bubble);

        // A ring in the card colour separates the bubble from the picture.
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(Theme::SurfaceSidebar));
        painter.drawEllipse(area.adjusted(-ring, -ring, ring, ring));

        // A guess is drawn hollow, so it never passes for a reported status.
        if (m_status == QLatin1String("hint")) {
            QPen ring{QColor(Theme::Green)};
            ring.setWidthF(qMax(2.0, bubble / 7.0));
            painter.setPen(ring);
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QRectF(area).adjusted(1.5, 1.5, -1.5, -1.5));
            return;
        }

        painter.setBrush(QColor(statusColour(m_status)));
        painter.drawEllipse(area);

        // Do not disturb gets a bar, away gets a crescent, offline a hole.
        painter.setBrush(QColor(Theme::SurfaceSidebar));
        if (m_status == QLatin1String("dnd")) {
            const int barHeight = qMax(2, bubble / 5);
            painter.drawRoundedRect(QRect(area.left() + bubble / 4, area.center().y() - barHeight / 2,
                                          bubble / 2, barHeight),
                                    barHeight / 2.0, barHeight / 2.0);
        } else if (m_status == QLatin1String("idle")) {
            painter.drawEllipse(area.adjusted(-bubble / 5, -bubble / 5, -bubble / 2, -bubble / 2));
        } else if (m_status != QLatin1String("online")) {
            const int hole = bubble / 3;
            painter.drawEllipse(QRect(area.center().x() - hole / 2, area.center().y() - hole / 2,
                                      hole, hole));
        }
    }

private:
    AnimatedImage *m_avatar = nullptr;
    AnimatedImage *m_decoration = nullptr;
    QImage m_still;
    QImage m_decorationStill;
    QString m_fallbackName;
    QString m_status;
};

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ProfileDialog::ProfileDialog(MessageStore *store, RestClient *rest, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
    , m_store(store)
    , m_rest(rest)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);
    setFixedSize(DialogWidth, DialogHeight);

    // Everything lives inside one opaque rounded frame. A plain QWidget does
    // not paint a stylesheet background, which is why this must be a QFrame.
    auto *shell = new QFrame(this);
    shell->setObjectName(QStringLiteral("ProfileShell"));

    auto *shellWrap = new QVBoxLayout(this);
    shellWrap->setContentsMargins(10, 10, 10, 10);
    shellWrap->addWidget(shell);

    auto *shadow = new QGraphicsDropShadowEffect(shell);
    shadow->setBlurRadius(40);
    shadow->setOffset(0, 8);
    shadow->setColor(QColor(0, 0, 0, 200));
    shell->setGraphicsEffect(shadow);

    auto *root = new QHBoxLayout(shell);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(14);

    root->addWidget(buildLeftCard());
    root->addWidget(buildRightPane(), 1);

    // Close button, floating over the top right corner.
    auto *close = new QPushButton(QStringLiteral("✕"), this);
    close->setObjectName(QStringLiteral("ProfileClose"));
    close->setFixedSize(30, 30);
    close->setCursor(Qt::PointingHandCursor);
    close->move(DialogWidth - 46, 16);
    close->raise();
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    setStyleSheet(QStringLiteral(R"(
QLabel { background: transparent; }
#ProfileShell {
    background-color: %7;
    border: 1px solid %3;
    border-radius: 14px;
}
#ProfileClose {
    background-color: %1;
    color: %2;
    border: none;
    border-radius: 15px;
    font-size: 14px;
}
#ProfileClose:hover { background-color: %3; color: %4; }
#LeftCard {
    background-color: %5;
    border-radius: 12px;
}
#TabButton {
    background: transparent;
    border: none;
    border-bottom: 2px solid transparent;
    border-radius: 0;
    color: %2;
    padding: 8px 4px;
    font-weight: 600;
}
#TabButton:hover { color: %4; }
#TabButton[active="true"] {
    color: %4;
    border-bottom: 2px solid %6;
}
#Card {
    background-color: %5;
    border-radius: 10px;
}
QLineEdit#NoteEdit {
    background: transparent;
    border: none;
    border-radius: 4px;
    padding: 4px 2px;
    color: %2;
    font-style: italic;
}
QLineEdit#NoteEdit:focus { background-color: %1; font-style: normal; }
QScrollArea { background: transparent; border: none; }
)")
                      .arg(QLatin1String(Theme::SurfaceInput), QLatin1String(Theme::TextMuted),
                           QLatin1String(Theme::SurfaceHover), QLatin1String(Theme::TextPrimary),
                           QLatin1String(Theme::SurfaceSidebar), QLatin1String(Theme::Accent),
                           QLatin1String(Theme::SurfaceChat)));

    // Late downloads (avatar, banner, game art) repaint what is on screen.
    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &) {
        if (isVisible())
            refreshArtwork();
    });
    connect(m_store, &MessageStore::userChanged, this, [this](const QString &userId) {
        if (isVisible() && userId == m_userId) {
            applyLocalData();
            rebuildActivity();
        }
    });
}

QWidget *ProfileDialog::buildLeftCard()
{
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("LeftCard"));
    card->setFixedWidth(LeftCardWidth);

    auto *outer = new QVBoxLayout(card);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // --- banner -----------------------------------------------------------
    m_banner = new BannerWidget(card);
    m_banner->setFixedHeight(BannerHeight);
    outer->addWidget(m_banner);

    // --- scrolling body ---------------------------------------------------
    auto *scroll = new QScrollArea(card);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *body = new QWidget(scroll);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(22, 0, 22, 22);
    layout->setSpacing(6);

    // The avatar rides up over the banner.
    m_avatar = new AvatarView(AvatarPixels, body);
    auto *avatarRow = new QHBoxLayout;
    avatarRow->setContentsMargins(0, -52, 0, 0);
    avatarRow->addWidget(m_avatar);
    avatarRow->addStretch(1);
    layout->addLayout(avatarRow);

    m_displayName = new QLabel(body);
    m_displayName->setWordWrap(true);
    m_displayName->setStyleSheet(QStringLiteral("color: %1; font-size: 23px; font-weight: 700;")
                                     .arg(QLatin1String(Theme::TextPrimary)));
    layout->addWidget(m_displayName);

    // Handle and badges share one line.
    auto *handleRow = new QHBoxLayout;
    handleRow->setContentsMargins(0, 0, 0, 0);
    handleRow->setSpacing(8);

    m_handle = new QLabel(body);
    m_handle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_handle->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;")
                                .arg(QLatin1String(Theme::TextMuted)));
    handleRow->addWidget(m_handle);

    m_badgeHost = new QWidget(body);
    m_badgeFlow = new FlowLayout(m_badgeHost, 0, 4);
    handleRow->addWidget(m_badgeHost, 1);
    layout->addLayout(handleRow);

    // Only filled in by the Presence hints plugin, for someone Discord is
    // reporting as offline. Hidden the rest of the time.
    m_meta = new QLabel(body);
    m_meta->setWordWrap(true);
    m_meta->setVisible(false);
    m_meta->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; margin-top: 4px;")
                              .arg(QLatin1String(Theme::Green)));
    layout->addWidget(m_meta);

    // --- buttons ----------------------------------------------------------
    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 12, 0, 6);
    buttons->setSpacing(8);

    m_messageButton = new QPushButton(QStringLiteral("Message"), body);
    m_messageButton->setObjectName(QStringLiteral("PrimaryButton"));
    m_messageButton->setMinimumHeight(34);
    connect(m_messageButton, &QPushButton::clicked, this, &ProfileDialog::startDirectMessage);
    buttons->addWidget(m_messageButton, 1);

    m_friendButton = new QPushButton(body);
    m_friendButton->setMinimumHeight(34);
    connect(m_friendButton, &QPushButton::clicked, this, &ProfileDialog::toggleFriend);
    buttons->addWidget(m_friendButton);

    m_moreButton = new QPushButton(QStringLiteral("⋯"), body);
    m_moreButton->setFixedSize(38, 34);
    connect(m_moreButton, &QPushButton::clicked, this, &ProfileDialog::showMoreMenu);
    buttons->addWidget(m_moreButton);

    layout->addLayout(buttons);

    // --- about ------------------------------------------------------------
    m_bioSection = new QWidget(body);
    auto *bioLayout = new QVBoxLayout(m_bioSection);
    bioLayout->setContentsMargins(0, 8, 0, 0);
    bioLayout->setSpacing(4);
    bioLayout->addWidget(sectionTitle(QStringLiteral("About Me"), m_bioSection));
    m_bioLabel = new QLabel(m_bioSection);
    m_bioLabel->setWordWrap(true);
    m_bioLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;")
                                  .arg(QLatin1String(Theme::TextPrimary)));
    bioLayout->addWidget(m_bioLabel);
    layout->addWidget(m_bioSection);

    // --- member since -----------------------------------------------------
    auto *memberSection = new QWidget(body);
    auto *memberLayout = new QVBoxLayout(memberSection);
    memberLayout->setContentsMargins(0, 10, 0, 0);
    memberLayout->setSpacing(4);
    memberLayout->addWidget(sectionTitle(QStringLiteral("Member Since"), memberSection));
    m_memberSince = new QLabel(memberSection);
    m_memberSince->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;")
                                     .arg(QLatin1String(Theme::TextPrimary)));
    memberLayout->addWidget(m_memberSince);
    layout->addWidget(memberSection);

    // --- friends since ----------------------------------------------------
    m_friendsSinceSection = new QWidget(body);
    auto *friendsLayout = new QVBoxLayout(m_friendsSinceSection);
    friendsLayout->setContentsMargins(0, 10, 0, 0);
    friendsLayout->setSpacing(4);
    friendsLayout->addWidget(sectionTitle(QStringLiteral("Friends Since"), m_friendsSinceSection));
    m_friendsSince = new QLabel(m_friendsSinceSection);
    m_friendsSince->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;")
                                      .arg(QLatin1String(Theme::TextPrimary)));
    friendsLayout->addWidget(m_friendsSince);
    layout->addWidget(m_friendsSinceSection);

    // --- roles ------------------------------------------------------------
    m_rolesSection = new QWidget(body);
    auto *rolesLayout = new QVBoxLayout(m_rolesSection);
    rolesLayout->setContentsMargins(0, 10, 0, 0);
    rolesLayout->setSpacing(6);
    rolesLayout->addWidget(sectionTitle(QStringLiteral("Roles"), m_rolesSection));
    m_roleHost = new QWidget(m_rolesSection);
    m_roleFlow = new FlowLayout(m_roleHost, 0, 6);
    rolesLayout->addWidget(m_roleHost);
    layout->addWidget(m_rolesSection);

    // --- connections ------------------------------------------------------
    m_connectionsSection = new QWidget(body);
    m_connectionsLayout = new QVBoxLayout(m_connectionsSection);
    m_connectionsLayout->setContentsMargins(0, 10, 0, 0);
    m_connectionsLayout->setSpacing(4);
    m_connectionsLayout->addWidget(sectionTitle(QStringLiteral("Connections"), m_connectionsSection));
    layout->addWidget(m_connectionsSection);

    // --- private note -----------------------------------------------------
    auto *noteSection = new QWidget(body);
    auto *noteLayout = new QVBoxLayout(noteSection);
    noteLayout->setContentsMargins(0, 10, 0, 0);
    noteLayout->setSpacing(2);
    noteLayout->addWidget(sectionTitle(QStringLiteral("Note (only visible to you)"), noteSection));
    m_noteEdit = new QLineEdit(noteSection);
    m_noteEdit->setObjectName(QStringLiteral("NoteEdit"));
    m_noteEdit->setPlaceholderText(QStringLiteral("Click to add a note"));
    connect(m_noteEdit, &QLineEdit::editingFinished, this, &ProfileDialog::saveNote);
    noteLayout->addWidget(m_noteEdit);
    m_noteStatus = new QLabel(noteSection);
    m_noteStatus->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                    .arg(QLatin1String(Theme::TextFaint)));
    m_noteStatus->setVisible(false);
    noteLayout->addWidget(m_noteStatus);
    layout->addWidget(noteSection);

    layout->addStretch(1);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    return card;
}

QWidget *ProfileDialog::buildTabBar()
{
    auto *bar = new QWidget(this);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(18);

    const auto makeTab = [&](const QString &text, int index) {
        auto *button = new QPushButton(text, bar);
        button->setObjectName(QStringLiteral("TabButton"));
        button->setCursor(Qt::PointingHandCursor);
        button->setFlat(true);
        connect(button, &QPushButton::clicked, this, [this, index]() { setTab(index); });
        layout->addWidget(button);
        return button;
    };

    m_activityTab = makeTab(QStringLiteral("Activity"), 0);
    m_friendsTab = makeTab(QStringLiteral("Mutual Friends"), 1);
    m_serversTab = makeTab(QStringLiteral("Mutual Servers"), 2);
    layout->addStretch(1);

    return bar;
}

QWidget *ProfileDialog::buildRightPane()
{
    auto *pane = new QWidget(this);
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 26, 8, 0);
    layout->setSpacing(10);

    layout->addWidget(buildTabBar());

    m_tabs = new QStackedWidget(pane);

    // Each tab is a scrolling column of cards.
    const auto makePage = [this]() {
        auto *scroll = new QScrollArea(m_tabs);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        auto *page = new QWidget(scroll);
        auto *pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(4, 0, 10, 10);
        pageLayout->setSpacing(10);
        pageLayout->addStretch(1);

        scroll->setWidget(page);
        m_tabs->addWidget(scroll);
        return pageLayout;
    };

    m_activityLayout = makePage();
    m_mutualFriendsLayout = makePage();
    m_mutualServersLayout = makePage();

    layout->addWidget(m_tabs, 1);
    return pane;
}

// ---------------------------------------------------------------------------
// Painting and keys
// ---------------------------------------------------------------------------

void ProfileDialog::paintEvent(QPaintEvent *event)
{
    // The rounded shell frame paints the background now, so nothing is drawn
    // here. The window itself stays transparent so the corners are clean.
    QDialog::paintEvent(event);
}

void ProfileDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

// ---------------------------------------------------------------------------
// Showing someone
// ---------------------------------------------------------------------------

void ProfileDialog::showFor(const QString &userId, const QString &guildId, bool isSelf)
{
    if (userId.isEmpty())
        return;

    m_userId = userId;
    m_guildId = guildId;
    m_isSelf = isSelf;

    // Reset anything left over from the last person.
    m_profile = {};
    m_bannerHash.clear();
    m_decorationAsset.clear();
    m_accentColour = 0;
    m_mutualFriends = {};
    m_mutualGuilds = {};
    m_noteEdit->clear();
    m_noteStatus->setVisible(false);

    applyLocalData();
    rebuildActivity();
    rebuildMutuals();
    setTab(0);

    if (parentWidget())
        move(parentWidget()->geometry().center() - rect().center());

    show();
    raise();

    // Then ask Discord for everything the gateway did not give us.
    m_rest->fetchUserProfile(
        userId, guildId,
        [this, userId](const QJsonObject &payload) {
            if (m_userId != userId)
                return;
            applyProfilePayload(payload);
        },
        [this, userId](const RestClient::Error &error) {
            wlog(QStringLiteral("profile"), QStringLiteral("profile fetch failed for %1: HTTP %2 %3")
                                                .arg(userId).arg(error.httpStatus).arg(error.message));
        });

    m_rest->fetchNote(
        userId,
        [this, userId](const QJsonObject &payload) {
            if (m_userId != userId)
                return;
            m_noteEdit->setText(payload.value(QStringLiteral("note")).toString());
        },
        [](const RestClient::Error &) {
            // A missing note answers 404. That is normal and needs no fuss.
        });
}

void ProfileDialog::applyLocalData()
{
    const UserInfo info = m_store->user(m_userId);

    m_displayName->setText(info.displayName().isEmpty() ? QStringLiteral("Unknown") : info.displayName());
    m_handle->setText(info.username.isEmpty() ? QStringLiteral("id %1").arg(m_userId)
                                              : info.username);

    const QString hint = m_store->activityHint(m_userId);
    m_meta->setVisible(!hint.isEmpty());
    if (!hint.isEmpty())
        m_meta->setText(QStringLiteral("Shows offline, but %1").arg(hint));

    // Member Since: the Discord join date, plus the server join date if known.
    QStringList dates;
    const QDateTime created = info.createdAt();
    if (created.isValid())
        dates << created.toString(QStringLiteral("MMM d, yyyy"));

    const QJsonObject member = m_profile.value(QStringLiteral("guild_member")).toObject();
    const QDateTime joined =
        QDateTime::fromString(member.value(QStringLiteral("joined_at")).toString(), Qt::ISODateWithMs);
    if (joined.isValid())
        dates << joined.toLocalTime().toString(QStringLiteral("MMM d, yyyy"));
    m_memberSince->setText(dates.join(QStringLiteral("   •   ")));

    const bool haveFriendDate = info.friendsSince.isValid();
    m_friendsSinceSection->setVisible(haveFriendDate);
    if (haveFriendDate)
        m_friendsSince->setText(info.friendsSince.toString(QStringLiteral("MMM d, yyyy")));

    m_messageButton->setVisible(!m_isSelf);
    m_friendButton->setVisible(!m_isSelf);
    updateFriendButton();

    m_noteEdit->parentWidget()->setVisible(!m_isSelf);

    rebuildRoles();
    refreshArtwork();
}

void ProfileDialog::applyProfilePayload(const QJsonObject &payload)
{
    m_profile = payload;

    const QJsonObject user = payload.value(QStringLiteral("user")).toObject();
    m_store->rememberUser(user);

    // The profile block wins, because it holds the per-server overrides.
    const QJsonObject userProfile = payload.value(QStringLiteral("user_profile")).toObject();
    const QJsonObject memberProfile = payload.value(QStringLiteral("guild_member_profile")).toObject();

    m_bannerHash = memberProfile.value(QStringLiteral("banner")).toString();
    if (m_bannerHash.isEmpty())
        m_bannerHash = userProfile.value(QStringLiteral("banner")).toString();
    if (m_bannerHash.isEmpty())
        m_bannerHash = user.value(QStringLiteral("banner")).toString();

    m_accentColour = userProfile.value(QStringLiteral("accent_color")).toInt();
    if (m_accentColour == 0)
        m_accentColour = user.value(QStringLiteral("accent_color")).toInt();

    // The decoration can be hung off the user, the profile, or the per-server
    // member record, so try each in turn.
    const QJsonObject member = payload.value(QStringLiteral("guild_member")).toObject();
    for (const QJsonObject &source : {member, userProfile, user}) {
        const QString asset = source.value(QStringLiteral("avatar_decoration_data"))
                                  .toObject()
                                  .value(QStringLiteral("asset"))
                                  .toString();
        if (!asset.isEmpty()) {
            m_decorationAsset = asset;
            break;
        }
    }

    wlog(QStringLiteral("profile"),
         QStringLiteral("%1: banner=%2 decoration=%3 accent=%4")
             .arg(m_userId,
                  m_bannerHash.isEmpty() ? QStringLiteral("none") : m_bannerHash,
                  m_decorationAsset.isEmpty() ? QStringLiteral("none") : m_decorationAsset)
             .arg(m_accentColour));

    QString bio = memberProfile.value(QStringLiteral("bio")).toString();
    if (bio.isEmpty())
        bio = userProfile.value(QStringLiteral("bio")).toString();
    if (bio.isEmpty())
        bio = user.value(QStringLiteral("bio")).toString();

    m_bioSection->setVisible(!bio.isEmpty());
    m_bioLabel->setText(bio);

    rebuildBadges(user.value(QStringLiteral("public_flags")).toInt());
    rebuildConnections(payload.value(QStringLiteral("connected_accounts")).toArray());

    m_mutualFriends = payload.value(QStringLiteral("mutual_friends")).toArray();
    m_mutualGuilds = payload.value(QStringLiteral("mutual_guilds")).toArray();

    applyLocalData();
    rebuildMutuals();
}

// ---------------------------------------------------------------------------
// Pieces of the card
// ---------------------------------------------------------------------------

void ProfileDialog::refreshArtwork()
{
    const UserInfo info = m_store->user(m_userId);
    const PresenceInfo presence = m_store->presence(m_userId);

    MediaCache &cache = MediaCache::instance();
    auto *avatarView = static_cast<AvatarView *>(m_avatar);

    // Avatar. Asking for the image also starts the download.
    const QUrl avatarUrl = MediaCache::avatarUrl(m_userId, info.avatarHash, 160);
    const QImage avatar = avatarUrl.isEmpty() ? QImage() : cache.image(avatarUrl);
    avatarView->setAvatar(cache.animationData(avatarUrl), avatar, info.displayName());

    // Decoration frame.
    const QUrl decorationUrl = MediaCache::decorationUrl(m_decorationAsset);
    const QImage decoration = decorationUrl.isEmpty() ? QImage() : cache.image(decorationUrl);
    avatarView->setDecoration(cache.animationData(decorationUrl), decoration);

    const QString bubble = m_store->presenceBubble(m_userId);
    avatarView->setStatus(bubble);
    avatarView->setToolTip(statusWord(bubble));

    // Banner: the picture if there is one, otherwise the accent colour.
    const QUrl bannerUrl = MediaCache::bannerUrl(m_userId, m_bannerHash, 600);
    const QImage banner = bannerUrl.isEmpty() ? QImage() : cache.image(bannerUrl);
    const QColor fallback = m_accentColour != 0 ? QColor::fromRgb(static_cast<QRgb>(m_accentColour))
                                                : QColor(Theme::SurfaceRail);

    static_cast<BannerWidget *>(m_banner)->setSource(cache.animationData(bannerUrl), banner, fallback);
}

QWidget *ProfileDialog::makePill(const QString &text, const QColor &dot, QWidget *parent)
{
    // QFrame, not QWidget: only a frame paints a stylesheet background.
    auto *pill = new QFrame(parent);
    auto *layout = new QHBoxLayout(pill);
    layout->setContentsMargins(9, 4, 10, 4);
    layout->setSpacing(6);

    if (dot.isValid()) {
        auto *marker = new QLabel(pill);
        marker->setFixedSize(9, 9);
        marker->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 4px;").arg(dot.name()));
        layout->addWidget(marker);
    }

    auto *label = new QLabel(text, pill);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                             .arg(QLatin1String(Theme::TextPrimary)));
    layout->addWidget(label);

    pill->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 8px;")
                            .arg(QLatin1String(Theme::SurfaceInput)));
    return pill;
}

void ProfileDialog::rebuildBadges(int publicFlags)
{
    while (QLayoutItem *item = m_badgeFlow->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const QStringList names = badgeNames(publicFlags);
    for (const QString &name : names) {
        auto *badge = new QLabel(name, m_badgeHost);
        badge->setStyleSheet(QStringLiteral("background-color: %1; color: %2; border-radius: 6px; "
                                            "padding: 2px 7px; font-size: 10px; font-weight: 600;")
                                 .arg(QLatin1String(Theme::SurfaceInput), QLatin1String(Theme::Highlight)));
        m_badgeFlow->addWidget(badge);
    }

    m_badgeHost->setVisible(!names.isEmpty());
    m_badgeHost->updateGeometry();
}

void ProfileDialog::rebuildRoles()
{
    while (QLayoutItem *item = m_roleFlow->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const QJsonObject member = m_profile.value(QStringLiteral("guild_member")).toObject();
    QStringList roleIds;
    const QJsonArray rawRoles = member.value(QStringLiteral("roles")).toArray();
    for (const QJsonValue &value : rawRoles)
        roleIds << value.toString();

    const QList<RoleInfo> roles = m_store->resolveRoles(m_guildId, roleIds);
    for (const RoleInfo &role : roles) {
        const QColor dot = role.hasColour() ? QColor::fromRgb(static_cast<QRgb>(role.colour))
                                            : QColor(Theme::TextMuted);
        m_roleFlow->addWidget(makePill(role.name, dot, m_roleHost));
    }

    m_rolesSection->setVisible(!roles.isEmpty());
    m_roleHost->updateGeometry();
}

void ProfileDialog::rebuildConnections(const QJsonArray &connections)
{
    // Keep the title, drop the rows under it.
    while (m_connectionsLayout->count() > 1) {
        QLayoutItem *item = m_connectionsLayout->takeAt(1);
        delete item->widget();
        delete item;
    }

    for (const QJsonValue &value : connections) {
        const QJsonObject connection = value.toObject();
        const QString name = connection.value(QStringLiteral("name")).toString();
        const QString type = connection.value(QStringLiteral("type")).toString();
        if (name.isEmpty())
            continue;

        auto *row = new QLabel(QStringLiteral("%1  ·  %2").arg(type, name), m_connectionsSection);
        row->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                               .arg(QLatin1String(Theme::TextPrimary)));
        m_connectionsLayout->addWidget(row);
    }

    m_connectionsSection->setVisible(m_connectionsLayout->count() > 1);
}

// ---------------------------------------------------------------------------
// Right hand panel
// ---------------------------------------------------------------------------

QString ProfileDialog::relativeSince(qint64 startMs)
{
    if (startMs <= 0)
        return {};

    const qint64 seconds = QDateTime::currentMSecsSinceEpoch() / 1000 - startMs / 1000;
    if (seconds < 0)
        return {};

    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 rest = seconds % 60;

    if (hours > 0) {
        return QStringLiteral("%1:%2:%3 elapsed")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(rest, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2 elapsed").arg(minutes).arg(rest, 2, 10, QLatin1Char('0'));
}

void ProfileDialog::rebuildActivity()
{
    // Clear everything except the trailing stretch.
    while (m_activityLayout->count() > 1) {
        QLayoutItem *item = m_activityLayout->takeAt(0);
        delete item->widget();
        delete item;
    }

    const PresenceInfo presence = m_store->presence(m_userId);
    int shown = 0;

    const auto addCard = [this](const QString &heading, const QString &title, const QStringList &lines,
                                const QUrl &artwork) {
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("Card"));
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 12, 14, 14);
        cardLayout->setSpacing(10);

        auto *headingLabel = new QLabel(heading, card);
        headingLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 700;")
                                        .arg(QLatin1String(Theme::TextMuted)));
        cardLayout->addWidget(headingLabel);

        auto *row = new QHBoxLayout;
        row->setSpacing(12);

        auto *art = new QLabel(card);
        art->setFixedSize(60, 60);
        art->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 8px;")
                               .arg(QLatin1String(Theme::SurfaceHover)));
        if (!artwork.isEmpty()) {
            const QImage picture = MediaCache::instance().image(artwork);
            if (!picture.isNull()) {
                art->setPixmap(QPixmap::fromImage(
                    picture.scaled(60, 60, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)));
            }
        }
        row->addWidget(art);

        auto *text = new QVBoxLayout;
        text->setSpacing(2);

        auto *titleLabel = new QLabel(title, card);
        titleLabel->setWordWrap(true);
        titleLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; font-weight: 600;")
                                      .arg(QLatin1String(Theme::TextPrimary)));
        text->addWidget(titleLabel);

        for (const QString &line : lines) {
            if (line.isEmpty())
                continue;
            auto *lineLabel = new QLabel(line, card);
            lineLabel->setWordWrap(true);
            lineLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                                         .arg(QLatin1String(Theme::TextMuted)));
            text->addWidget(lineLabel);
        }

        text->addStretch(1);
        row->addLayout(text, 1);
        cardLayout->addLayout(row);

        m_activityLayout->insertWidget(m_activityLayout->count() - 1, card);
    };

    for (const ActivityInfo &activity : presence.activities) {
        if (activity.isCustomStatus()) {
            const QString text = activity.emoji.isEmpty()
                ? activity.state
                : QStringLiteral("%1  %2").arg(activity.emoji, activity.state);
            if (!text.trimmed().isEmpty()) {
                addCard(QStringLiteral("CUSTOM STATUS"), text, {}, {});
                ++shown;
            }
            continue;
        }

        QString heading = QStringLiteral("PLAYING");
        if (activity.type == 1)
            heading = QStringLiteral("STREAMING");
        else if (activity.type == 2)
            heading = QStringLiteral("LISTENING TO");
        else if (activity.type == 3)
            heading = QStringLiteral("WATCHING");
        else if (activity.type == 5)
            heading = QStringLiteral("COMPETING IN");

        addCard(heading, activity.name, {activity.details, activity.state, relativeSince(activity.startMs)},
                MediaCache::activityAssetUrl(activity.applicationId, activity.largeImage));
        ++shown;
    }

    // In voice.
    const VoiceStateInfo voice = m_store->voiceState(m_userId);
    if (!voice.channelId.isEmpty()) {
        const ChannelInfo channel = m_store->channel(voice.channelId);
        const GuildInfo guild = m_store->guild(voice.guildId);
        addCard(QStringLiteral("IN VOICE"),
                channel.name.isEmpty() ? QStringLiteral("Voice channel") : channel.name,
                {guild.name.isEmpty() ? QString() : QStringLiteral("in %1").arg(guild.name)},
                MediaCache::guildIconUrl(voice.guildId, guild.iconHash, 96));
        ++shown;
    }

    if (shown == 0) {
        auto *empty = new QLabel(QStringLiteral("Nothing right now."));
        empty->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-style: italic;")
                                 .arg(QLatin1String(Theme::TextFaint)));
        m_activityLayout->insertWidget(0, empty);
    }
}

void ProfileDialog::rebuildMutuals()
{
    const auto clear = [](QVBoxLayout *layout) {
        while (layout->count() > 1) {
            QLayoutItem *item = layout->takeAt(0);
            delete item->widget();
            delete item;
        }
    };
    clear(m_mutualFriendsLayout);
    clear(m_mutualServersLayout);

    // --- mutual friends ---------------------------------------------------
    for (const QJsonValue &value : m_mutualFriends) {
        const QJsonObject raw = value.toObject();
        m_store->rememberUser(raw);

        const QString id = raw.value(QStringLiteral("id")).toString();
        const UserInfo info = m_store->user(id);

        auto *row = new QFrame;
        row->setObjectName(QStringLiteral("Card"));
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(10, 8, 10, 8);
        rowLayout->setSpacing(10);

        auto *avatar = new QLabel(row);
        avatar->setFixedSize(32, 32);
        const QUrl url = MediaCache::avatarUrl(id, info.avatarHash, 80);
        const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
        avatar->setPixmap(picture.isNull() ? MediaCache::initialsAvatar(info.displayName(), 32)
                                           : MediaCache::circular(picture, 32));
        rowLayout->addWidget(avatar);

        auto *name = new QLabel(info.displayName(), row);
        name->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;")
                                .arg(QLatin1String(Theme::TextPrimary)));
        rowLayout->addWidget(name, 1);

        m_mutualFriendsLayout->insertWidget(m_mutualFriendsLayout->count() - 1, row);
    }

    // --- mutual servers ---------------------------------------------------
    for (const QJsonValue &value : m_mutualGuilds) {
        const QString guildId = value.toObject().value(QStringLiteral("id")).toString();
        const GuildInfo guild = m_store->guild(guildId);
        const QString name = guild.name.isEmpty() ? guildId : guild.name;

        auto *row = new QFrame;
        row->setObjectName(QStringLiteral("Card"));
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(10, 8, 10, 8);
        rowLayout->setSpacing(10);

        auto *icon = new QLabel(row);
        icon->setFixedSize(32, 32);
        const QUrl url = MediaCache::guildIconUrl(guildId, guild.iconHash, 96);
        const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
        icon->setPixmap(picture.isNull() ? MediaCache::initialsAvatar(name, 32)
                                         : MediaCache::circular(picture, 32));
        rowLayout->addWidget(icon);

        auto *label = new QLabel(name, row);
        label->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;")
                                 .arg(QLatin1String(Theme::TextPrimary)));
        rowLayout->addWidget(label, 1);

        m_mutualServersLayout->insertWidget(m_mutualServersLayout->count() - 1, row);
    }

    // Counts go in the tab names, the way the real client does it.
    m_friendsTab->setText(m_mutualFriends.isEmpty()
                              ? QStringLiteral("Mutual Friends")
                              : QStringLiteral("%1 Mutual Friends").arg(m_mutualFriends.size()));
    m_serversTab->setText(m_mutualGuilds.isEmpty()
                              ? QStringLiteral("Mutual Servers")
                              : QStringLiteral("%1 Mutual Servers").arg(m_mutualGuilds.size()));

    const bool hide = m_isSelf;
    m_friendsTab->setVisible(!hide);
    m_serversTab->setVisible(!hide);
}

void ProfileDialog::setTab(int index)
{
    m_tabs->setCurrentIndex(index);

    const QList<QPushButton *> tabs{m_activityTab, m_friendsTab, m_serversTab};
    for (int i = 0; i < tabs.size(); ++i) {
        tabs.at(i)->setProperty("active", i == index);
        // Qt only restyles when it is told the property changed.
        tabs.at(i)->style()->unpolish(tabs.at(i));
        tabs.at(i)->style()->polish(tabs.at(i));
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void ProfileDialog::updateFriendButton()
{
    const UserInfo info = m_store->user(m_userId);

    if (info.isBlocked())
        m_friendButton->setText(QStringLiteral("Unblock"));
    else if (info.isFriend())
        m_friendButton->setText(QStringLiteral("Remove friend"));
    else if (info.relationship == 4)
        m_friendButton->setText(QStringLiteral("Cancel request"));
    else if (info.relationship == 3)
        m_friendButton->setText(QStringLiteral("Accept request"));
    else
        m_friendButton->setText(QStringLiteral("Add friend"));
}

void ProfileDialog::toggleFriend()
{
    const UserInfo info = m_store->user(m_userId);
    const QString userId = m_userId;

    const auto onOk = [this, userId](int newType) {
        return [this, userId, newType](const QJsonObject &) {
            m_store->setRelationship(userId, newType);
            if (m_userId == userId)
                updateFriendButton();
        };
    };
    m_friendButton->setEnabled(false);
    const auto reenable = [this]() { m_friendButton->setEnabled(true); };

    const auto onError = [this, userId, info, onOk, reenable](const RestClient::Error &error) {
        const QJsonArray keys = error.body.value(QStringLiteral("captcha_key")).toArray();
        bool needsCheck = false;
        for (const QJsonValue &key : keys) {
            if (key.toString() == QLatin1String("captcha-required"))
                needsCheck = true;
        }
        if (needsCheck && !info.isFriend() && info.relationship != 4 && !info.isBlocked()) {
            const QString token = CaptchaDialog::solve(
                this, error.body.value(QStringLiteral("captcha_sitekey")).toString(),
                error.body.value(QStringLiteral("captcha_rqdata")).toString());
            if (token.isEmpty()) {
                setNote(QStringLiteral("Discord asked for a check, and it was not finished."), true);
                reenable();
                return;
            }
            RestClient::CaptchaProof proof;
            proof.key = token;
            proof.rqtoken = error.body.value(QStringLiteral("captcha_rqtoken")).toString();
            proof.sessionId = error.body.value(QStringLiteral("captcha_session_id")).toString();
            m_rest->addFriend(userId,
                              [onOk, reenable, info](const QJsonObject &o) {
                                  onOk(info.relationship == 3 ? 1 : 4)(o);
                                  reenable();
                              },
                              [this, reenable](const RestClient::Error &again) {
                                  wlog(QStringLiteral("profile"),
                                       QStringLiteral("relationship change failed: HTTP %1 %2")
                                           .arg(again.httpStatus)
                                           .arg(again.message));
                                  setNote(again.message.isEmpty() ? QStringLiteral("That did not work.")
                                                                  : again.message.left(180),
                                          true);
                                  reenable();
                              },
                              proof);
            return;
        }

        wlog(QStringLiteral("profile"), QStringLiteral("relationship change failed: HTTP %1 %2")
                                            .arg(error.httpStatus).arg(error.message));
        const QString shown = error.message.contains(QStringLiteral("captcha"))
            ? QStringLiteral("Discord asked for a check, and it was not finished.")
            : (error.message.isEmpty() ? QStringLiteral("That did not work.") : error.message.left(180));
        setNote(shown, true);
        reenable();
    };

    if (info.isFriend() || info.relationship == 4 || info.isBlocked()) {
        m_rest->removeRelationship(userId,
                                   [onOk, reenable](const QJsonObject &o) { onOk(0)(o); reenable(); },
                                   [onError, reenable](const RestClient::Error &e) { onError(e); reenable(); });
    } else {
        m_rest->addFriend(userId,
                          [onOk, reenable, info](const QJsonObject &o) {
                              onOk(info.relationship == 3 ? 1 : 4)(o);
                              reenable();
                          },
                          [onError](const RestClient::Error &e) { onError(e); });
    }
}

void ProfileDialog::showMoreMenu()
{
    QMenu menu(this);

    QAction *copyId = menu.addAction(QStringLiteral("Copy User ID"));

    QAction *block = nullptr;
    if (!m_isSelf) {
        menu.addSeparator();
        const UserInfo info = m_store->user(m_userId);
        block = menu.addAction(info.isBlocked() ? QStringLiteral("Unblock") : QStringLiteral("Block"));
    }

    QAction *chosen = menu.exec(m_moreButton->mapToGlobal(QPoint(0, m_moreButton->height() + 4)));
    if (!chosen)
        return;

    if (chosen == copyId) {
        QApplication::clipboard()->setText(m_userId);
        setNote(QStringLiteral("ID copied."));
        return;
    }

    if (chosen == block) {
        const QString userId = m_userId;
        const UserInfo info = m_store->user(userId);

        if (info.isBlocked()) {
            m_rest->removeRelationship(
                userId,
                [this, userId](const QJsonObject &) {
                    m_store->setRelationship(userId, 0);
                    if (m_userId == userId)
                        updateFriendButton();
                },
                [this](const RestClient::Error &e) { setNote(e.message, true); });
        } else {
            m_rest->blockUser(
                userId,
                [this, userId](const QJsonObject &) {
                    m_store->setRelationship(userId, 2);
                    if (m_userId == userId)
                        updateFriendButton();
                },
                [this](const RestClient::Error &e) { setNote(e.message, true); });
        }
    }
}

void ProfileDialog::startDirectMessage()
{
    const QString existing = m_store->directChannelWith(m_userId);
    if (!existing.isEmpty()) {
        emit openDirectMessage(existing);
        accept();
        return;
    }

    m_messageButton->setEnabled(false);
    m_rest->openDirectMessage(
        m_userId,
        [this](const QJsonObject &channel) {
            m_messageButton->setEnabled(true);
            const QString channelId = channel.value(QStringLiteral("id")).toString();
            if (channelId.isEmpty())
                return;
            m_store->ingestChannelObject(channel);
            emit openDirectMessage(channelId);
            accept();
        },
        [this](const RestClient::Error &error) {
            m_messageButton->setEnabled(true);
            wlog(QStringLiteral("profile"), QStringLiteral("open dm failed: HTTP %1 %2")
                                                .arg(error.httpStatus).arg(error.message));
            setNote(QStringLiteral("Could not open that chat."), true);
        });
}

void ProfileDialog::saveNote()
{
    const QString userId = m_userId;
    const QString text = m_noteEdit->text();

    m_rest->saveNote(
        userId, text,
        [this](const QJsonObject &) { setNote(QStringLiteral("Note saved.")); },
        [this](const RestClient::Error &error) {
            wlog(QStringLiteral("profile"), QStringLiteral("note save failed: HTTP %1 %2")
                                                .arg(error.httpStatus).arg(error.message));
            setNote(QStringLiteral("Could not save that note."), true);
        });
}

void ProfileDialog::setNote(const QString &text, bool isError)
{
    if (text.isEmpty()) {
        m_noteStatus->setVisible(false);
        return;
    }
    m_noteStatus->setText(text);
    m_noteStatus->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                    .arg(QLatin1String(isError ? Theme::Accent : Theme::TextFaint)));
    m_noteStatus->setVisible(true);
}
