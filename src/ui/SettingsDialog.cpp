#include "ui/SettingsDialog.h"

#include "core/AppConfig.h"
#include "core/GameActivity.h"
#include "core/Logger.h"
#include "core/MessageStore.h"
#include "core/RestClient.h"
#include "plugin/PluginHost.h"
#include "ui/AnimatedImage.h"
#include "ui/AudioMeter.h"
#include "ui/BadgeCustomizer.h"
#include "ui/BadgesDialog.h"
#include "ui/CaptchaDialog.h"
#include "ui/FrameClock.h"
#include "ui/LookPainter.h"
#include "ui/MediaCache.h"
#include "ui/ProfilePreview.h"
#include "ui/Sounds.h"
#include "ui/Speech.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QAudioDevice>
#include <QCameraDevice>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSignalBlocker>

#include <algorithm>
#include <functional>
#include <QMovie>
#include <QPainter>
#include <QPainterPath>
#include <QRadioButton>
#include <QStandardPaths>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMediaDevices>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QLabel *pageTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    // Light rather than bold, and larger. A heading earns its place by size
    // and the space around it; making it heavy as well shouts.
    label->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 300; color: %1; "
                                        "letter-spacing: -0.5px; margin-bottom: 4px;")
                             .arg(QLatin1String(Theme::TextPrimary)));
    return label;
}

QLabel *groupTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 700; "
                                        "letter-spacing: 0.8px; margin-top: 22px;")
                             .arg(QLatin1String(Theme::TextMuted)));
    return label;
}

QLabel *hint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                             .arg(QLatin1String(Theme::TextFaint)));
    return label;
}

QSlider *makeSlider(int minimum, int maximum, int value, QWidget *parent)
{
    auto *slider = new QSlider(Qt::Horizontal, parent);
    slider->setRange(minimum, maximum);
    slider->setValue(value);
    return slider;
}

// Puts a chosen picture somewhere it will survive.
//
// Remembering the path it was picked from is the obvious thing and is wrong:
// a wallpaper chosen out of a downloads folder is one tidy-up away from
// vanishing, and the background would revert with nothing on screen saying
// why. A copy in the program's own folder is a few megabytes and never moves.
QString copyBackgroundFile(const QString &source)
{
    const QString folder =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/backgrounds");

    QDir().mkpath(folder);

    const QFileInfo info(source);
    const QString target = folder + QStringLiteral("/background.") + info.suffix().toLower();

    // Only one is kept. Someone changing their mind five times should not
    // leave five files behind that nothing will ever delete.
    const QDir directory(folder);
    const QStringList old = directory.entryList(QStringList{QStringLiteral("background.*")},
                                                QDir::Files);
    for (const QString &name : old)
        QFile::remove(directory.filePath(name));

    if (!QFile::copy(source, target))
        return {};

    return target;
}

} // namespace

SettingsDialog::SettingsDialog(MessageStore *store, RestClient *rest, PluginHost *plugins,
                               const QString &selfUserId, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
    , m_rest(rest)
    , m_plugins(plugins)
    , m_selfUserId(selfUserId)
    , m_meter(new AudioMeter(this))
{
    setWindowTitle(QStringLiteral("Settings"));
    setAccessibleName(QStringLiteral("Settings"));
    resize(980, 680);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- section list -----------------------------------------------------
    auto *nav = new QWidget(this);
    nav->setObjectName(QStringLiteral("SettingsNav"));
    nav->setFixedWidth(210);

    auto *navLayout = new QVBoxLayout(nav);
    navLayout->setContentsMargins(0, 16, 0, 16);

    m_sections = new QListWidget(nav);
    m_sections->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navLayout->addWidget(m_sections, 1);
    root->addWidget(nav);

    // --- pages ------------------------------------------------------------
    // The pages, and under them the save bar Profiles uses.
    auto *pageColumn = new QVBoxLayout;
    pageColumn->setContentsMargins(0, 0, 0, 0);
    pageColumn->setSpacing(0);
    m_pages = new QStackedWidget(this);
    pageColumn->addWidget(m_pages, 1);
    pageColumn->addWidget(buildSaveBar());
    root->addLayout(pageColumn, 1);

    addSection(QStringLiteral("My Account"), [this]() { return buildAccountPage(); });
    m_profilesRow = m_sections->count();
    addSection(QStringLiteral("Profiles"), [this]() { return buildProfilesPage(); });
    addSection(QStringLiteral("Voice & Video"), [this]() { return buildVoicePage(); });
    addSection(QStringLiteral("Activity"), [this]() { return buildActivityPage(); });
    addSection(QStringLiteral("Notifications"), [this]() { return buildNotificationsPage(); });
    addSection(QStringLiteral("Accessibility"), [this]() { return buildAccessibilityPage(); });
    addSection(QStringLiteral("Appearance"), [this]() { return buildAppearancePage(); });
    addSection(QStringLiteral("Plugins"), [this]() { return buildPluginsPage(); });
    addSection(QStringLiteral("Advanced"), [this]() { return buildAdvancedPage(); });

    connect(m_sections, &QListWidget::currentRowChanged, this, [this](int row) {
        ensurePage(row);
        m_pages->setCurrentIndex(row);
        // The microphone is only held open while its page is in front.
        if (m_sections->item(row) && m_sections->item(row)->text() != QLatin1String("Voice & Video"))
            stopMicTest();
    });
    m_sections->setCurrentRow(0);

    setStyleSheet(QStringLiteral(R"(
#SettingsNav {
    background-color: %1;
    border-right: 1px solid %2;
}
#SettingsNav QListWidget {
    background: transparent;
    border: none;
    outline: none;
}
#SettingsNav QListWidget::item {
    color: %3;
    padding: 9px 16px;
    margin: 1px 10px;
    border-radius: 6px;
}
#SettingsNav QListWidget::item:hover { background-color: %4; color: %5; }
#SettingsNav QListWidget::item:selected {
    background-color: %6;
    color: %5;
    font-weight: 600;
}
QScrollArea, QStackedWidget { background: %8; border: none; }
QScrollArea > QWidget > QWidget { background: %8; }

QPushButton {
    background-color: %6;
    color: %5;
    border: 1px solid %2;
    border-radius: 8px;
    padding: 7px 14px;
    min-height: 18px;
}
QPushButton:hover { background-color: %4; }
QPushButton:pressed { background-color: %6; }
QPushButton:disabled { color: %9; }

QSpinBox, QComboBox, QLineEdit, QPlainTextEdit {
    background-color: %6;
    color: %5;
    border: 1px solid %2;
    border-radius: 8px;
    padding: 6px 10px;
    min-height: 22px;
    selection-background-color: %7;
}
QSpinBox:focus, QComboBox:focus, QLineEdit:focus, QPlainTextEdit:focus { border-color: %7; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView {
    background-color: %8;
    color: %5;
    border: 1px solid %2;
    selection-background-color: %4;
    outline: none;
}

QCheckBox, QRadioButton {
    spacing: 8px;
    color: %5;
    background: transparent;
}
QCheckBox::indicator, QRadioButton::indicator {
    width: 16px;
    height: 16px;
}

QSlider::groove:horizontal {
    height: 5px;
    background: %6;
    border-radius: 2px;
}
QSlider::sub-page:horizontal {
    background: %7;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    background: %5;
    width: 14px;
    margin: -5px 0;
    border-radius: 7px;
}
)")
                      .arg(QLatin1String(Theme::SurfaceSidebar), QLatin1String(Theme::Border),
                           QLatin1String(Theme::TextMuted), QLatin1String(Theme::SurfaceHover),
                           QLatin1String(Theme::TextPrimary), QLatin1String(Theme::SurfaceInput),
                           QLatin1String(Theme::Accent), QLatin1String(Theme::SurfaceChat),
                           QLatin1String(Theme::TextFaint)));
}

SettingsDialog::~SettingsDialog()
{
    stopMicTest();
}

void SettingsDialog::addSection(const QString &title, std::function<QWidget *()> builder)
{
    m_sections->addItem(title);

    // Every page scrolls, so a small window never clips the controls.
    auto *scroll = new QScrollArea(m_pages);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    m_pages->addWidget(scroll);
    m_pageBuilders.append(std::move(builder));
}

void SettingsDialog::ensurePage(int row)
{
    if (row < 0 || row >= m_pageBuilders.size())
        return;
    auto *scroll = qobject_cast<QScrollArea *>(m_pages->widget(row));
    if (!scroll || scroll->widget())
        return;

    QElapsedTimer clock;
    clock.start();
    scroll->setWidget(m_pageBuilders[row]());
    wlog(QStringLiteral("ui"),
         QStringLiteral("settings page \"%1\" built in %2 ms")
             .arg(m_sections->item(row) ? m_sections->item(row)->text() : QString::number(row))
             .arg(clock.elapsed()));
}

// ---------------------------------------------------------------------------
// My Account
// ---------------------------------------------------------------------------

namespace {

// Discord's own words for a refused change. A form error carries them per
// field ({"errors":{"avatar":{"_errors":[{"message":...}]}}}); the top line
// ("Invalid Form Body") says nothing useful by itself.
QString discordReason(const RestClient::Error &error)
{
    std::function<QString(const QJsonObject &)> first = [&first](const QJsonObject &node) -> QString {
        const QJsonArray list = node.value(QStringLiteral("_errors")).toArray();
        if (!list.isEmpty())
            return list.first().toObject().value(QStringLiteral("message")).toString();
        for (auto it = node.constBegin(); it != node.constEnd(); ++it) {
            if (it.value().isObject()) {
                const QString found = first(it.value().toObject());
                if (!found.isEmpty())
                    return found;
            }
        }
        return {};
    };
    const QString field = first(error.body.value(QStringLiteral("errors")).toObject());
    if (!field.isEmpty())
        return field;
    if (error.body.contains(QStringLiteral("captcha_key")))
        return QStringLiteral("Discord asked for a captcha, and it was not finished.");
    const QString message = error.body.value(QStringLiteral("message")).toString();
    if (error.isRateLimit())
        return QStringLiteral("Discord says to slow down. Try again in a little while.");
    return message.isEmpty() ? QStringLiteral("HTTP %1").arg(error.httpStatus) : message;
}

// --- Display name styles, as Discord's editor offers them ----------------------

struct StyleChoice
{
    int id;
    const char *name;
};

// Discord's fonts, in its editor's order; 11 is its ordinary one.
constexpr StyleChoice StyleFonts[] = {
    {11, "Default"},  {3, "Sakura"},   {4, "Jellybean"},    {6, "Modern"},     {7, "Medieval"},  {8, "8Bit"},
    {10, "Vampyre"},  {12, "Tempo"},   {13, "Monkey Bars"}, {14, "Mainframe"}, {15, "Headbang"}, {16, "Journal"},
};

constexpr StyleChoice StyleEffects[] = {
    {Cosmetics::Solid, "Solid"}, {Cosmetics::Gradient, "Gradient"}, {Cosmetics::Neon, "Neon"},
    {Cosmetics::Toon, "Toon"},   {Cosmetics::Pop, "Pop"},           {Cosmetics::Glow, "Glow"},
    {Cosmetics::Prism, "Prism"}, {Cosmetics::Gummy, "Gummy"},
};

// How many colours each effect takes; none for no style.
int styleColourCount(int effect)
{
    switch (effect) {
    case 0: return 0;
    case Cosmetics::Gradient:
    case Cosmetics::Glow: return 2;
    case Cosmetics::Gummy: return 4;
    case Cosmetics::Prism: return 5;
    default: return 1;
    }
}

// Discord's ready-made palettes for each effect, its default first.
QList<QList<quint32>> stylePresets(int effect)
{
    switch (effect) {
    case 0: return {};
    case Cosmetics::Gradient:
    case Cosmetics::Glow:
        return {{0x2aaea6, 0xffc490}, {0xe45eff, 0x267de0}, {0xefb4aa, 0xe46ce2},
                {0xdf4232, 0xbd8fff}, {0x26b164, 0x90eaff}, {0xf5983c, 0xe55ae7}};
    case Cosmetics::Gummy:
        return {{0xe9a9d5, 0xa9ddf0, 0xb9a7ee, 0xb9eacb}, {0xff6fc2, 0xf54fa8, 0xff9bd6, 0xd96ab7},
                {0xffb57a, 0xff936f, 0xff7c78, 0xf06b8a}, {0xc4ed65, 0x98de60, 0x6ccd63, 0x45bc66},
                {0x7cb8ff, 0x8ee0d0, 0x9d94f5, 0x66cff0}, {0xd77bff, 0xb56bff, 0xe45ce5, 0x8b6cff}};
    case Cosmetics::Prism:
        return {{0xfe9242, 0x35ed7e, 0xffe047, 0xff4cd2, 0x5865f2}, {0xff3d3d, 0xff8a2a, 0xffd447, 0xff4fa3, 0x7b61ff},
                {0x7a0906, 0xc81e12, 0xf45a18, 0xff9500, 0xffd447}, {0x7ee7d8, 0x7ccbff, 0x7f8cff, 0xb56dff, 0xf38ad8},
                {0x35d65f, 0x16d8b2, 0x20c5f5, 0x2b7cff, 0x6b4dff}, {0xa7e8a1, 0xd9f08c, 0xffe08a, 0xffc4a8, 0xf3b6d9}};
    default:
        return {{0x24e36d}, {0x1c98eb}, {0xc32aff}, {0xda0149}, {0xf93722}, {0xc6a615}};
    }
}

// Colours side by side in a rounded square, for a button.
QIcon swatchIcon(const QList<quint32> &colours, int size = 20)
{
    const qreal ratio = 2.0;
    QPixmap pixmap(QSize(size, size) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box(0.5, 0.5, size - 1, size - 1);
    QPainterPath round;
    round.addRoundedRect(box, 5, 5);
    painter.setClipPath(round);
    const int count = qMax<int>(1, colours.size());
    for (int i = 0; i < colours.size(); ++i)
        painter.fillRect(QRectF(box.left() + box.width() * i / count, box.top(), box.width() / count + 0.5, box.height()),
                         QColor::fromRgb(QRgb(colours.at(i))));
    painter.setClipping(false);
    painter.setPen(QColor(Theme::Border));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(round);
    return QIcon(pixmap);
}

QString skuOf(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(qint64(value.toDouble()));
    return {};
}

// A nameplate as Discord's picker shows it: your picture and your name, in
// its style, on the plate. The one under the pointer moves.
class NameplateDelegate : public QStyledItemDelegate
{
public:
    NameplateDelegate(QListWidget *list, std::function<QPixmap()> face, std::function<LookPainter::Name()> name)
        : QStyledItemDelegate(list)
        , m_list(list)
        , m_face(std::move(face))
        , m_name(std::move(name))
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {240, 50}; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool picked = option.state & QStyle::State_Selected;
        const QRect row = option.rect.adjusted(4, 3, -4, -3);
        if (hovered || picked) {
            painter->setPen(picked ? QPen(QColor(Theme::Accent), 2) : QPen(Qt::NoPen));
            painter->setBrush(QColor(Theme::SurfaceHover));
            painter->drawRoundedRect(QRectF(row).adjusted(1, 1, -1, -1), 7, 7);
        }

        const QVariantMap data = index.data(Qt::UserRole).toMap();
        Cosmetics::Nameplate plate;
        plate.skuId = data.value(QStringLiteral("sku")).toString();
        plate.palette = data.value(QStringLiteral("palette")).toString();
        LookPainter &looks = LookPainter::instance();
        LookPainter::Name name = m_name();
        QFont font = option.font;
        font.setPixelSize(15);
        font.setWeight(QFont::DemiBold);
        name.font = font;
        name.mode = hovered ? LookPainter::EffectMode::Animated : LookPainter::EffectMode::Static;
        const int nameLeft = 52;
        const int nameRoom = row.width() - nameLeft - 8;
        const int contentRight = nameLeft + qMin(looks.nameWidth(name), nameRoom);
        looks.paintNameplate(painter, row, plate, contentRight, LookPainter::Placement::MemberList, hovered, false);

        const QPixmap face = m_face();
        if (!face.isNull())
            painter->drawPixmap(QRect(row.left() + 10, row.center().y() - 16, 32, 32), face);
        looks.paintName(painter, QRect(row.left() + nameLeft, row.top(), nameRoom, row.height()), name);
        if (!plate.isSet()) {
            QFont small = option.font;
            small.setPixelSize(12);
            painter->setFont(small);
            painter->setPen(QColor(Theme::TextMuted));
            painter->drawText(row.adjusted(0, 0, -12, 0), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("None"));
        }
        painter->restore();

        // The row under the pointer plays: its next frame on the clock.
        if (hovered && (plate.isSet() || LookPainter::moving(name)))
            FrameClock::scheduleRepaint(m_list->viewport(), 33, option.rect);
    }

private:
    QListWidget *m_list;
    std::function<QPixmap()> m_face;
    std::function<LookPainter::Name()> m_name;
};

} // namespace

QWidget *SettingsDialog::buildAccountPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(34, 28, 34, 28);
    layout->setSpacing(8);

    layout->addWidget(pageTitle(QStringLiteral("My Account"), page));

    // The same card as Profiles shows, as Discord puts at the top of this page.
    m_accountCard = new ProfilePreview(page);
    // Tall enough for the 5:2 banner with the name and handle underneath it.
    m_accountCard->setFixedSize(440, 258);
    layout->addWidget(m_accountCard);

    auto *editProfile = new QPushButton(QStringLiteral("Edit User Profile"), page);
    editProfile->setMaximumWidth(220);
    connect(editProfile, &QPushButton::clicked, this, [this]() { m_sections->setCurrentRow(m_profilesRow); });
    layout->addWidget(editProfile);

    // Discord's "Customize your badges" card. The card above shows the order
    // as it is being changed; Save Changes sends it.
    m_badgeCustomizer = new BadgeCustomizer(m_rest, page);
    m_badgeCustomizer->setMaximumWidth(440);
    connect(m_badgeCustomizer, &BadgeCustomizer::changed, this, [this]() {
        refreshCardBadges();
        if (m_displayName)
            updateSaveBar();
    });
    connect(m_badgeCustomizer, &BadgeCustomizer::viewBadgesRequested, this, [this]() {
        const QString name = m_loaded.globalName.isEmpty() ? m_loaded.username : m_loaded.globalName;
        BadgesDialog::open(m_rest, m_selfUserId, name, true, 0, this);
    });
    layout->addSpacing(8);
    layout->addWidget(m_badgeCustomizer);

    // What Discord said about the last change: saved, or why not.
    m_accountStatus = new QLabel(page);
    m_accountStatus->setWordWrap(true);
    m_accountStatus->hide();
    layout->addWidget(m_accountStatus);

    layout->addWidget(groupTitle(QStringLiteral("USERNAME"), page));
    m_username = new QLineEdit(page);
    m_username->setMaxLength(32);
    m_username->setMaximumWidth(300);
    m_username->setPlaceholderText(QStringLiteral("lowercase letters, numbers, _ and ."));
    auto *saveUsername = new QPushButton(QStringLiteral("Change"), page);
    auto *usernameRow = new QHBoxLayout;
    usernameRow->addWidget(m_username);
    usernameRow->addWidget(saveUsername);
    usernameRow->addStretch(1);
    layout->addLayout(usernameRow);
    layout->addWidget(hint(QStringLiteral("Discord asks for your password to change this, and only allows it "
                                          "a couple of times an hour."),
                           page));
    connect(saveUsername, &QPushButton::clicked, this, [this]() {
        const QString wanted = m_username->text().trimmed();
        if (wanted.isEmpty() || wanted == m_loaded.username)
            return;
        bool ok = false;
        const QString password = QInputDialog::getText(this, QStringLiteral("Change username"),
                                                       QStringLiteral("Your Discord password:"),
                                                       QLineEdit::Password, QString(), &ok);
        if (!ok || password.isEmpty())
            return;
        setAccountStatus(true, QStringLiteral("Saving…"));
        editUser(QJsonObject{{QStringLiteral("username"), wanted}, {QStringLiteral("password"), password}},
                 [this](bool saved, const QString &problem) {
                     setAccountStatus(saved, saved ? QStringLiteral("Username saved.")
                                                   : QStringLiteral("Username was not changed: %1").arg(problem));
                     if (saved)
                         loadProfile(true);
                 });
    });

    layout->addWidget(groupTitle(QStringLiteral("ACCOUNT"), page));

    auto *copyId = new QPushButton(QStringLiteral("Copy my user ID"), page);
    copyId->setMaximumWidth(220);
    connect(copyId, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_selfUserId);
    });
    layout->addWidget(copyId);

    auto *logOut = new QPushButton(QStringLiteral("Log out"), page);
    logOut->setMaximumWidth(220);
    connect(logOut, &QPushButton::clicked, this, [this]() {
        emit logOutRequested();
        accept();
    });
    layout->addWidget(logOut);

    layout->addWidget(hint(QStringLiteral("Logging out clears the saved token from this machine. "
                                          "Changing your password or email is not built yet: do those on "
                                          "discord.com."),
                           page));

    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildProfilesPage()
{
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(34, 28, 34, 28);
    outer->setSpacing(8);
    outer->addWidget(pageTitle(QStringLiteral("Profiles"), page));

    m_profileStatus = new QLabel(page);
    m_profileStatus->setWordWrap(true);
    m_profileStatus->hide();
    outer->addWidget(m_profileStatus);

    // Editors on the left, the live card on the right, as in Discord.
    auto *columns = new QHBoxLayout;
    columns->setSpacing(28);
    auto *left = new QVBoxLayout;
    left->setSpacing(8);
    columns->addLayout(left, 1);

    auto *right = new QVBoxLayout;
    right->addWidget(groupTitle(QStringLiteral("PREVIEW"), page));
    m_preview = new ProfilePreview(page);
    m_preview->setFixedWidth(300);
    right->addWidget(m_preview);
    right->addStretch(1);
    columns->addLayout(right);
    outer->addLayout(columns);

    const auto buttonRow = [](std::initializer_list<QPushButton *> buttons) {
        auto *line = new QHBoxLayout;
        line->setSpacing(8);
        for (QPushButton *button : buttons)
            line->addWidget(button);
        line->addStretch(1);
        return line;
    };

    // --- Display name and pronouns ------------------------------------------
    left->addWidget(groupTitle(QStringLiteral("DISPLAY NAME"), page));
    m_displayName = new QLineEdit(page);
    m_displayName->setMaxLength(32);
    m_displayName->setPlaceholderText(QStringLiteral("Empty shows your username"));
    left->addWidget(m_displayName);

    addNameStyleEditor(page, left);

    left->addWidget(groupTitle(QStringLiteral("PRONOUNS"), page));
    m_pronouns = new QLineEdit(page);
    m_pronouns->setMaxLength(40);
    m_pronouns->setPlaceholderText(QStringLiteral("Add your pronouns"));
    left->addWidget(m_pronouns);

    // --- Avatar -----------------------------------------------------------------
    left->addWidget(groupTitle(QStringLiteral("AVATAR"), page));
    auto *changeAvatar = new QPushButton(QStringLiteral("Change Avatar"), page);
    auto *removeAvatar = new QPushButton(QStringLiteral("Remove Avatar"), page);
    left->addLayout(buttonRow({changeAvatar, removeAvatar}));
    left->addWidget(hint(QStringLiteral("PNG, JPG, WebP or GIF, up to 10 MB. A moving GIF needs Nitro."), page));
    connect(changeAvatar, &QPushButton::clicked, this, [this]() {
        QByteArray bytes;
        const QString data = pickPicture(QStringLiteral("Choose an avatar"), &bytes);
        if (data.isEmpty())
            return;
        m_pending.avatarTouched = true;
        m_pending.avatarData = data;
        m_pending.avatarBytes = bytes;
        profileEdited();
    });
    connect(removeAvatar, &QPushButton::clicked, this, [this]() {
        m_pending.avatarTouched = true;
        m_pending.avatarData.clear();
        m_pending.avatarBytes.clear();
        profileEdited();
    });

    // --- Decoration -------------------------------------------------------------
    left->addWidget(groupTitle(QStringLiteral("AVATAR DECORATION"), page));
    m_decorations = new QListWidget(page);
    m_decorations->setViewMode(QListView::IconMode);
    m_decorations->setIconSize(QSize(72, 72));
    m_decorations->setGridSize(QSize(96, 108));
    m_decorations->setResizeMode(QListView::Adjust);
    m_decorations->setMovement(QListView::Static);
    m_decorations->setWordWrap(true);
    m_decorations->setFixedHeight(236);
    m_decorations->setStyleSheet(QStringLiteral(
        "QListWidget { background: %1; border: 1px solid %2; border-radius: 8px; padding: 6px; }"
        "QListWidget::item { color: %3; border-radius: 8px; padding: 4px; }"
        "QListWidget::item:selected { background: %4; color: %5; }"
        "QListWidget::item:hover { background: %4; }")
                                     .arg(QLatin1String(Theme::SurfaceInput), QLatin1String(Theme::Border),
                                          QLatin1String(Theme::TextMuted), QLatin1String(Theme::SurfaceHover),
                                          QLatin1String(Theme::TextPrimary)));
    left->addWidget(m_decorations);
    m_decorationHint = hint(QStringLiteral("The decorations you own from the Discord shop."), page);
    left->addWidget(m_decorationHint);
    connect(m_decorations, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (!item || m_fillingDecorations)
            return;
        m_pending.decorationTouched = true;
        m_pending.decoration = item->data(Qt::UserRole).toMap();
        profileEdited();
    });

    // The playing tile: frames painted onto it as they come.
    m_tileAnimation = new AnimatedImage(this);
    connect(m_tileAnimation, &AnimatedImage::frameChanged, this, [this]() {
        if (QListWidgetItem *item = m_decorations->item(m_animatedTileRow))
            paintDecorationTile(item, m_tileAnimation->currentFrame());
    });
    m_decorations->setMouseTracking(true);
    m_decorations->viewport()->installEventFilter(this);
    connect(m_decorations, &QListWidget::itemEntered, this, [this](QListWidgetItem *item) {
        m_decorationHover = m_decorations->row(item);
        animateDecorationTile(m_decorationHover);
    });

    addNameplatePicker(page, left);
    addEffectPicker(page, left);

    // --- Banner -------------------------------------------------------------------
    left->addWidget(groupTitle(QStringLiteral("PROFILE BANNER"), page));
    m_accentButton = new QPushButton(QStringLiteral("Banner Colour"), page);
    auto *changeBanner = new QPushButton(QStringLiteral("Change Banner"), page);
    auto *removeBanner = new QPushButton(QStringLiteral("Remove Banner"), page);
    left->addLayout(buttonRow({m_accentButton, changeBanner, removeBanner}));
    left->addWidget(hint(QStringLiteral("The colour shows when there is no banner picture. A banner picture needs "
                                        "Nitro."),
                         page));
    connect(m_accentButton, &QPushButton::clicked, this, [this]() {
        const int now = m_pending.accentTouched ? m_pending.accent : m_loaded.accent;
        const QColor chosen = QColorDialog::getColor(now >= 0 ? QColor::fromRgb(QRgb(now)) : QColor(Qt::black), this,
                                                     QStringLiteral("Banner colour"));
        if (!chosen.isValid())
            return;
        m_pending.accentTouched = true;
        m_pending.accent = int(chosen.rgb() & 0xFFFFFF);
        profileEdited();
    });
    connect(changeBanner, &QPushButton::clicked, this, [this]() {
        QByteArray bytes;
        const QString data = pickPicture(QStringLiteral("Choose a banner"), &bytes);
        if (data.isEmpty())
            return;
        m_pending.bannerTouched = true;
        m_pending.bannerData = data;
        m_pending.bannerBytes = bytes;
        profileEdited();
    });
    connect(removeBanner, &QPushButton::clicked, this, [this]() {
        m_pending.bannerTouched = true;
        m_pending.bannerData.clear();
        m_pending.bannerBytes.clear();
        profileEdited();
    });

    addThemeEditor(page, left);

    // --- About me -------------------------------------------------------------------
    m_bioTitle = groupTitle(QStringLiteral("ABOUT ME"), page);
    left->addWidget(m_bioTitle);
    m_bio = new QPlainTextEdit(page);
    m_bio->setFixedHeight(110);
    m_bio->setPlaceholderText(QStringLiteral("Up to 190 characters. Markdown and emoji work, as in Discord."));
    left->addWidget(m_bio);

    connect(m_displayName, &QLineEdit::textEdited, this, [this]() { profileEdited(); });
    connect(m_pronouns, &QLineEdit::textEdited, this, [this]() { profileEdited(); });
    connect(m_bio, &QPlainTextEdit::textChanged, this, [this]() {
        if (!m_fillingProfile)
            profileEdited();
    });

    addTagPicker(page, left);

    // --- Per server (saved as soon as pressed) -----------------------------------------
    left->addWidget(groupTitle(QStringLiteral("SERVER PROFILE"), page));
    m_serverPick = new QComboBox(page);
    left->addWidget(m_serverPick);
    m_nickname = new QLineEdit(page);
    m_nickname->setMaxLength(32);
    m_nickname->setPlaceholderText(QStringLiteral("Nickname in this server. Empty removes it."));
    auto *saveNick = new QPushButton(QStringLiteral("Save Nickname"), page);
    auto *nickRow = new QHBoxLayout;
    nickRow->addWidget(m_nickname, 1);
    nickRow->addWidget(saveNick);
    left->addLayout(nickRow);
    auto *serverAvatar = new QPushButton(QStringLiteral("Server Avatar"), page);
    auto *resetServerAvatar = new QPushButton(QStringLiteral("Reset Server Avatar"), page);
    left->addLayout(buttonRow({serverAvatar, resetServerAvatar}));
    left->addWidget(hint(QStringLiteral("A nickname needs the server's Change Nickname permission. A different "
                                        "avatar per server needs Nitro. These save straight away."),
                         page));
    connect(m_serverPick, &QComboBox::currentIndexChanged, this, [this](int) { loadServerNickname(); });
    connect(saveNick, &QPushButton::clicked, this, [this]() {
        const QString nick = m_nickname->text().trimmed();
        editServer(QJsonObject{{QStringLiteral("nick"), nick.isEmpty() ? QJsonValue() : QJsonValue(nick)}},
                   nick.isEmpty() ? QStringLiteral("Nickname removed") : QStringLiteral("Nickname"));
    });
    connect(serverAvatar, &QPushButton::clicked, this, [this]() {
        QByteArray bytes;
        const QString data = pickPicture(QStringLiteral("Choose a server avatar"), &bytes);
        if (!data.isEmpty())
            editServer(QJsonObject{{QStringLiteral("avatar"), data}}, QStringLiteral("Server avatar"));
    });
    connect(resetServerAvatar, &QPushButton::clicked, this, [this]() {
        editServer(QJsonObject{{QStringLiteral("avatar"), QJsonValue()}}, QStringLiteral("Server avatar reset"));
    });

    left->addStretch(1);

    // Pictures arrive after the page is drawn; redraw what depends on them.
    //
    // Only those. Every picture the window downloaded used to redraw both
    // previews and every decoration tile, a third of a second each time, so
    // the whole settings window stuttered for as long as anything anywhere was
    // loading - minutes, on a busy server (2026-10-21). A decoration now
    // repaints its own tile; your avatar or banner, or the decoration you
    // wear, redraws the previews, once per burst.
    m_profileArtTimer = new QTimer(this);
    m_profileArtTimer->setSingleShot(true);
    m_profileArtTimer->setInterval(80);
    connect(m_profileArtTimer, &QTimer::timeout, this, [this]() {
        if (isVisible())
            refreshProfileArt();
    });
    connect(&MediaCache::instance(), &MediaCache::ready, this, [this](const QUrl &url) {
        if (!isVisible() || !m_decorations)
            return;
        // An effect's thumbnail, for its tile.
        for (int row = 0; m_effects && row < m_effects->count(); ++row) {
            QListWidgetItem *item = m_effects->item(row);
            const QString thumbnail = item->data(Qt::UserRole).toMap().value(QStringLiteral("thumbnail")).toString();
            if (!thumbnail.isEmpty() && QUrl(thumbnail) == url)
                item->setIcon(effectTileIcon(thumbnail));
        }
        const QString path = url.path();
        if (path.startsWith(QLatin1String("/avatar-decoration-presets/"))) {
            for (int row = 0; row < m_decorations->count(); ++row) {
                QListWidgetItem *item = m_decorations->item(row);
                const QString asset = item->data(Qt::UserRole).toMap().value(QStringLiteral("asset")).toString();
                if (MediaCache::decorationUrl(asset) == url)
                    paintDecorationTile(item, MediaCache::instance().image(url));
            }
            animateDecorationTile(m_decorationHover >= 0 ? m_decorationHover : m_decorations->currentRow());

            const QString worn = m_pending.decorationTouched
                ? m_pending.decoration.value(QStringLiteral("asset")).toString()
                : m_loaded.decorationAsset;
            if (MediaCache::decorationUrl(worn) != url)
                return;
        } else {
            const bool yours = !m_selfUserId.isEmpty()
                && (path.startsWith(QStringLiteral("/avatars/%1/").arg(m_selfUserId))
                    || path.startsWith(QStringLiteral("/banners/%1/").arg(m_selfUserId)));
            if (!yours && !path.startsWith(QLatin1String("/embed/avatars/")))
                return;
        }
        if (!m_profileArtTimer->isActive())
            m_profileArtTimer->start();
    });
    return page;
}

void SettingsDialog::addNameStyleEditor(QWidget *page, QVBoxLayout *left)
{
    left->addWidget(groupTitle(QStringLiteral("DISPLAY NAME STYLE"), page));
    auto *choose = new QHBoxLayout;
    choose->setSpacing(8);
    m_nameFont = new QComboBox(page);
    m_nameFont->setToolTip(QStringLiteral("Font"));
    for (const StyleChoice &font : StyleFonts)
        m_nameFont->addItem(QString::fromLatin1(font.name), font.id);
    m_nameEffect = new QComboBox(page);
    m_nameEffect->setToolTip(QStringLiteral("Effect"));
    m_nameEffect->addItem(QStringLiteral("No style"), 0);
    for (const StyleChoice &effect : StyleEffects)
        m_nameEffect->addItem(QString::fromLatin1(effect.name), effect.id);
    choose->addWidget(m_nameFont, 1);
    choose->addWidget(m_nameEffect, 1);
    left->addLayout(choose);

    // The colours the effect takes, one button each, then Discord's palettes.
    auto *colours = new QHBoxLayout;
    colours->setSpacing(6);
    for (int i = 0; i < 5; ++i) {
        auto *button = new QPushButton(page);
        button->setFixedSize(34, 30);
        button->setIconSize(QSize(20, 20));
        button->setToolTip(QStringLiteral("Colour %1").arg(i + 1));
        colours->addWidget(button);
        m_nameColours.append(button);
        connect(button, &QPushButton::clicked, this, [this, i]() {
            Cosmetics::NameStyle style = shownNameStyle();
            const QList<quint32> fallback = stylePresets(style.effectId).value(0);
            const quint32 now = style.colors.value(i, fallback.value(i, 0xffffff));
            const QColor chosen = QColorDialog::getColor(QColor::fromRgb(QRgb(now)), this, QStringLiteral("Name colour"));
            if (!chosen.isValid())
                return;
            while (style.colors.size() <= i)
                style.colors.append(fallback.value(style.colors.size(), 0xffffff));
            style.colors[i] = quint32(chosen.rgb() & 0xffffff);
            setPendingNameStyle(style);
        });
    }
    colours->addSpacing(12);
    for (int i = 0; i < 6; ++i) {
        auto *preset = new QPushButton(page);
        preset->setFixedSize(30, 30);
        preset->setIconSize(QSize(20, 20));
        preset->setFlat(true);
        preset->setToolTip(QStringLiteral("One of Discord's colour sets"));
        colours->addWidget(preset);
        m_namePresets.append(preset);
        connect(preset, &QPushButton::clicked, this, [this, i]() {
            Cosmetics::NameStyle style = shownNameStyle();
            const QList<QList<quint32>> presets = stylePresets(style.effectId);
            if (i >= presets.size())
                return;
            style.colors = presets.at(i);
            setPendingNameStyle(style);
        });
    }
    colours->addStretch(1);
    left->addLayout(colours);

    m_removeNameStyle = new QPushButton(QStringLiteral("Remove Style"), page);
    auto *removeRow = new QHBoxLayout;
    removeRow->addWidget(m_removeNameStyle);
    removeRow->addStretch(1);
    left->addLayout(removeRow);
    left->addWidget(hint(QStringLiteral("The font and effect your name has everywhere on Discord. Needs Nitro."), page));

    connect(m_nameFont, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_fillingLooks)
            return;
        Cosmetics::NameStyle style = shownNameStyle();
        style.fontId = m_nameFont->currentData().toInt();
        // A font goes with an effect: plain, in the first colour, to start.
        if (style.effectId == 0)
            style.effectId = Cosmetics::Solid;
        setPendingNameStyle(style);
    });
    connect(m_nameEffect, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_fillingLooks)
            return;
        const int effect = m_nameEffect->currentData().toInt();
        if (effect == 0) {
            setPendingNameStyle({});
            return;
        }
        Cosmetics::NameStyle style = shownNameStyle();
        // Another effect with as many colours keeps them; otherwise its default.
        if (styleColourCount(effect) != styleColourCount(style.effectId))
            style.colors = stylePresets(effect).value(0);
        style.effectId = effect;
        setPendingNameStyle(style);
    });
    connect(m_removeNameStyle, &QPushButton::clicked, this, [this]() { setPendingNameStyle({}); });

    // Each font's name in the font itself, as each arrives.
    connect(&LookPainter::instance(), &LookPainter::changed, m_nameFont, [this]() {
        for (int i = 0; i < m_nameFont->count(); ++i)
            m_nameFont->setItemData(i, LookPainter::instance().fontFor(m_nameFont->font(), m_nameFont->itemData(i).toInt()),
                                    Qt::FontRole);
    });
}

void SettingsDialog::addNameplatePicker(QWidget *page, QVBoxLayout *left)
{
    left->addWidget(groupTitle(QStringLiteral("NAMEPLATE"), page));
    m_nameplates = new QListWidget(page);
    m_nameplates->setFixedHeight(166);
    m_nameplates->setMouseTracking(true);
    m_nameplates->setStyleSheet(QStringLiteral("QListWidget { background: %1; border: 1px solid %2; border-radius: 8px; "
                                               "padding: 4px; outline: none; }")
                                    .arg(QLatin1String(Theme::SurfaceInput), QLatin1String(Theme::Border)));
    m_nameplates->setItemDelegate(new NameplateDelegate(
        m_nameplates, [this]() { return m_tileFace; },
        [this]() {
            LookPainter::Name name;
            const QString shown = m_displayName ? m_displayName->text().trimmed() : QString();
            name.text = shown.isEmpty() ? m_loaded.username : shown;
            name.style = shownNameStyle();
            name.fallback = QColor(Theme::TextPrimary);
            return name;
        }));
    left->addWidget(m_nameplates);
    m_nameplateHint = hint(QStringLiteral("The nameplates you own, behind your name in member lists."), page);
    left->addWidget(m_nameplateHint);
    connect(m_nameplates, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (!item || m_fillingLooks)
            return;
        m_pending.nameplateTouched = true;
        m_pending.nameplate = item->data(Qt::UserRole).toMap();
        profileEdited();
    });
    // Nameplate pictures and name fonts arriving.
    connect(&LookPainter::instance(), &LookPainter::changed, m_nameplates->viewport(), qOverload<>(&QWidget::update));
}

void SettingsDialog::addEffectPicker(QWidget *page, QVBoxLayout *left)
{
    left->addWidget(groupTitle(QStringLiteral("PROFILE EFFECT"), page));
    m_effects = new QListWidget(page);
    m_effects->setViewMode(QListView::IconMode);
    m_effects->setIconSize(QSize(72, 72));
    m_effects->setGridSize(QSize(96, 108));
    m_effects->setResizeMode(QListView::Adjust);
    m_effects->setMovement(QListView::Static);
    m_effects->setWordWrap(true);
    m_effects->setFixedHeight(130);
    m_effects->setStyleSheet(m_decorations->styleSheet());
    left->addWidget(m_effects);
    m_effectHint = hint(QStringLiteral("The profile effects you own. The preview plays the one picked."), page);
    left->addWidget(m_effectHint);
    connect(m_effects, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (!item || m_fillingLooks)
            return;
        m_pending.effectTouched = true;
        m_pending.effectSku = item->data(Qt::UserRole).toMap().value(QStringLiteral("sku")).toString();
        profileEdited();
    });
}

void SettingsDialog::addThemeEditor(QWidget *page, QVBoxLayout *left)
{
    left->addWidget(groupTitle(QStringLiteral("PROFILE THEME"), page));
    m_themePrimaryButton = new QPushButton(QStringLiteral("Primary"), page);
    m_themeAccentButton = new QPushButton(QStringLiteral("Accent"), page);
    auto *reset = new QPushButton(QStringLiteral("Reset Theme"), page);
    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    row->addWidget(m_themePrimaryButton);
    row->addWidget(m_themeAccentButton);
    row->addWidget(reset);
    row->addStretch(1);
    left->addLayout(row);
    left->addWidget(hint(QStringLiteral("Your profile shades from the primary colour at the top to the accent at the "
                                        "bottom. Needs Nitro."),
                         page));
    const auto pick = [this](bool primary) {
        const auto [top, bottom] = shownTheme();
        const int now = primary ? top : bottom;
        const QColor chosen = QColorDialog::getColor(now >= 0 ? QColor::fromRgb(QRgb(now)) : QColor(Theme::Accent), this,
                                                     primary ? QStringLiteral("Primary colour")
                                                             : QStringLiteral("Accent colour"));
        if (!chosen.isValid())
            return;
        const int picked = int(chosen.rgb() & 0xffffff);
        // A theme is always two colours; the other starts as this one.
        if (primary)
            setPendingTheme(picked, bottom >= 0 ? bottom : picked);
        else
            setPendingTheme(top >= 0 ? top : picked, picked);
    };
    connect(m_themePrimaryButton, &QPushButton::clicked, this, [pick]() { pick(true); });
    connect(m_themeAccentButton, &QPushButton::clicked, this, [pick]() { pick(false); });
    connect(reset, &QPushButton::clicked, this, [this]() { setPendingTheme(-1, -1); });
}

void SettingsDialog::addTagPicker(QWidget *page, QVBoxLayout *left)
{
    left->addWidget(groupTitle(QStringLiteral("SERVER TAG"), page));
    m_tagPick = new QComboBox(page);
    m_tagPick->setIconSize(QSize(64, 18));
    m_tagPick->addItem(QStringLiteral("None"), QString());
    left->addWidget(m_tagPick);
    left->addWidget(hint(QStringLiteral("A server's tag, worn beside your name. The servers you are in that have made "
                                        "one are listed."),
                         page));
    connect(m_tagPick, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_fillingLooks)
            return;
        m_pending.tagTouched = true;
        m_pending.tagGuildId = m_tagPick->currentData().toString();
        profileEdited();
    });
}

Cosmetics::NameStyle SettingsDialog::shownNameStyle() const
{
    return m_pending.nameStyleTouched ? m_pending.nameStyle : m_loaded.nameStyle;
}

QString SettingsDialog::shownNameplateSku() const
{
    return m_pending.nameplateTouched ? m_pending.nameplate.value(QStringLiteral("sku")).toString()
                                      : m_loaded.nameplate.skuId;
}

QString SettingsDialog::shownEffectSku() const
{
    return m_pending.effectTouched ? m_pending.effectSku : m_loaded.effectSku;
}

QString SettingsDialog::shownTagGuildId() const
{
    return m_pending.tagTouched ? m_pending.tagGuildId : m_loaded.tag.guildId;
}

Cosmetics::GuildTag SettingsDialog::shownTag() const
{
    const QString id = shownTagGuildId();
    if (id.isEmpty())
        return {};
    if (id == m_loaded.tag.guildId && m_loaded.tag.isSet())
        return m_loaded.tag;
    return m_serverTags.value(id);
}

QPair<int, int> SettingsDialog::shownTheme() const
{
    return m_pending.themeTouched ? qMakePair(m_pending.themePrimary, m_pending.themeAccent)
                                  : qMakePair(m_loaded.themePrimary, m_loaded.themeAccent);
}

void SettingsDialog::setPendingNameStyle(const Cosmetics::NameStyle &style)
{
    // Whole, the way Discord keeps one: a font, an effect, and as many
    // colours as the effect takes.
    Cosmetics::NameStyle whole;
    if (style.isSet()) {
        whole = style;
        if (whole.fontId == 0)
            whole.fontId = StyleFonts[0].id;
        if (whole.effectId == 0)
            whole.effectId = Cosmetics::Solid;
        const int count = styleColourCount(whole.effectId);
        const QList<quint32> fallback = stylePresets(whole.effectId).value(0);
        while (whole.colors.size() < count)
            whole.colors.append(fallback.value(whole.colors.size(), 0xffffff));
        while (whole.colors.size() > count)
            whole.colors.removeLast();
    }
    m_pending.nameStyleTouched = true;
    m_pending.nameStyle = whole;
    fillLookEditors();
    profileEdited();
}

void SettingsDialog::setPendingTheme(int primary, int accent)
{
    m_pending.themeTouched = true;
    m_pending.themePrimary = primary;
    m_pending.themeAccent = accent;
    profileEdited();
}

void SettingsDialog::fillLookEditors()
{
    if (!m_nameFont)
        return;
    m_fillingLooks = true;

    const Cosmetics::NameStyle style = shownNameStyle();
    LookPainter &looks = LookPainter::instance();
    for (int i = 0; i < m_nameFont->count(); ++i)
        m_nameFont->setItemData(i, looks.fontFor(m_nameFont->font(), m_nameFont->itemData(i).toInt()), Qt::FontRole);
    m_nameFont->setCurrentIndex(qMax(0, m_nameFont->findData(style.fontId ? style.fontId : StyleFonts[0].id)));
    const int effect = style.isSet() ? (style.effectId ? style.effectId : int(Cosmetics::Solid)) : 0;
    m_nameEffect->setCurrentIndex(qMax(0, m_nameEffect->findData(effect)));
    const int count = styleColourCount(effect);
    const QList<QList<quint32>> presets = stylePresets(effect);
    for (int i = 0; i < m_nameColours.size(); ++i) {
        m_nameColours.at(i)->setVisible(i < count);
        if (i < count)
            m_nameColours.at(i)->setIcon(swatchIcon({style.colors.value(i, presets.value(0).value(i, 0xffffff))}));
    }
    for (int i = 0; i < m_namePresets.size(); ++i) {
        m_namePresets.at(i)->setVisible(i < presets.size());
        if (i < presets.size())
            m_namePresets.at(i)->setIcon(swatchIcon(presets.at(i)));
    }
    m_removeNameStyle->setEnabled(style.isSet());

    const auto pickRow = [](QListWidget *list, const QString &sku) {
        for (int row = 0; list && row < list->count(); ++row) {
            if (list->item(row)->data(Qt::UserRole).toMap().value(QStringLiteral("sku")).toString() == sku) {
                list->setCurrentRow(row);
                return;
            }
        }
    };
    pickRow(m_nameplates, shownNameplateSku());
    pickRow(m_effects, shownEffectSku());
    m_tagPick->setCurrentIndex(qMax(0, m_tagPick->findData(shownTagGuildId())));

    m_fillingLooks = false;
}

void SettingsDialog::loadServerTags()
{
    if (!m_tagPick || !m_rest || !m_store)
        return;
    // The tag you wear is known from your profile; the rest are asked of
    // each server that can have one.
    if (m_loaded.tag.isSet())
        m_serverTags.insert(m_loaded.tag.guildId, m_loaded.tag);
    for (const GuildInfo &guild : m_store->guilds()) {
        if (!guild.features.contains(QStringLiteral("GUILD_TAGS")) || m_serverTags.contains(guild.id))
            continue;
        m_serverTags.insert(guild.id, {});
        const QString id = guild.id;
        m_rest->fetchGuildProfile(
            id,
            [this, id](const QJsonObject &answer) {
                Cosmetics::GuildTag tag;
                tag.guildId = id;
                tag.tag = answer.value(QStringLiteral("tag")).toString();
                tag.badge = answer.value(QStringLiteral("badge")).toString();
                m_serverTags.insert(id, tag);
                fillTagPick();
            },
            [](const RestClient::Error &) {});
    }
    fillTagPick();
}

void SettingsDialog::fillTagPick()
{
    if (!m_tagPick)
        return;
    QList<Cosmetics::GuildTag> tags;
    for (const Cosmetics::GuildTag &tag : std::as_const(m_serverTags)) {
        if (tag.isSet())
            tags.append(tag);
    }
    const auto serverName = [this](const QString &id) {
        const GuildInfo guild = m_store->guild(id);
        return guild.name.isEmpty() ? id : guild.name;
    };
    std::sort(tags.begin(), tags.end(), [&serverName](const Cosmetics::GuildTag &a, const Cosmetics::GuildTag &b) {
        return serverName(a.guildId).compare(serverName(b.guildId), Qt::CaseInsensitive) < 0;
    });

    m_fillingLooks = true;
    m_tagPick->clear();
    m_tagPick->addItem(QStringLiteral("None"), QString());
    const qreal ratio = qMax<qreal>(1.0, devicePixelRatioF());
    for (const Cosmetics::GuildTag &tag : std::as_const(tags)) {
        QPixmap chip = QPixmap::fromImage(LookPainter::instance().renderTag(tag, 14, ratio));
        chip.setDevicePixelRatio(ratio);
        m_tagPick->addItem(QIcon(chip), serverName(tag.guildId), tag.guildId);
    }
    m_tagPick->setCurrentIndex(qMax(0, m_tagPick->findData(shownTagGuildId())));
    m_fillingLooks = false;
}

void SettingsDialog::showEffectDetails(const QString &sku)
{
    if (sku.isEmpty() || m_effectDetails.contains(sku) || m_effectsAsked.contains(sku) || !m_rest)
        return;
    m_effectsAsked.insert(sku);
    m_rest->fetchCollectiblesProduct(
        sku,
        [this, sku](const QJsonObject &product) {
            const Cosmetics::ProfileEffect effect = Cosmetics::profileEffect(product);
            if (!effect.isSet())
                return;
            m_effectDetails.insert(sku, effect);
            refreshProfileArt();
        },
        [this, sku](const RestClient::Error &error) {
            m_effectsAsked.remove(sku);
            wlog(QStringLiteral("account"), QStringLiteral("could not load profile effect %1: HTTP %2 %3")
                                                .arg(sku)
                                                .arg(error.httpStatus)
                                                .arg(error.message));
        });
}

QIcon SettingsDialog::effectTileIcon(const QString &thumbnail) const
{
    const QImage picture = thumbnail.isEmpty() ? QImage() : MediaCache::instance().image(QUrl(thumbnail));
    if (picture.isNull())
        return {};
    QPixmap tile(72, 72);
    tile.fill(Qt::transparent);
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QImage fitted = picture.scaled(QSize(72, 72), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    painter.drawImage(QPoint((72 - fitted.width()) / 2, (72 - fitted.height()) / 2), fitted);
    painter.end();
    return QIcon(tile);
}

QWidget *SettingsDialog::buildSaveBar()
{
    // Discord's "Careful - you have unsaved changes!" bar, under the pages.
    m_saveBar = new QFrame(this);
    m_saveBar->setObjectName(QStringLiteral("SaveBar"));
    m_saveBar->setStyleSheet(QStringLiteral("#SaveBar { background: %1; border-top: 1px solid %2; }")
                                 .arg(QLatin1String(Theme::SurfaceSidebar), QLatin1String(Theme::Border)));
    auto *bar = new QHBoxLayout(m_saveBar);
    bar->setContentsMargins(24, 10, 24, 10);
    auto *careful = new QLabel(QStringLiteral("Careful — you have unsaved changes!"), m_saveBar);
    careful->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(QLatin1String(Theme::TextPrimary)));
    auto *reset = new QPushButton(QStringLiteral("Reset"), m_saveBar);
    reset->setFlat(true);
    m_saveButton = new QPushButton(QStringLiteral("Save Changes"), m_saveBar);
    m_saveButton->setStyleSheet(QStringLiteral("QPushButton { background-color: %1; color: #ffffff; border: none; "
                                               "border-radius: 4px; padding: 7px 16px; font-weight: 600; }"
                                               "QPushButton:disabled { background-color: %2; }")
                                    .arg(QLatin1String(Theme::Green), QLatin1String(Theme::SurfaceHover)));
    bar->addWidget(careful, 1);
    bar->addWidget(reset);
    bar->addWidget(m_saveButton);
    m_saveBar->hide();

    connect(reset, &QPushButton::clicked, this, [this]() {
        m_pending = PendingProfile{};
        m_userEdited = false;
        if (m_badgeCustomizer)
            m_badgeCustomizer->reset();
        fillProfileFields();
        refreshProfileArt();
        updateSaveBar();
    });
    connect(m_saveButton, &QPushButton::clicked, this, &SettingsDialog::saveProfile);
    return m_saveBar;
}

void SettingsDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // Paint the window first. Filling Profiles and talking to Discord here
    // used to hitch the first frame, with nothing in the log to name it.
    QTimer::singleShot(0, this, [this]() { loadProfile(true); });
}

bool SettingsDialog::eventFilter(QObject *watched, QEvent *event)
{
    // The pointer left the decoration grid: the picked tile plays again.
    if (m_decorations && watched == m_decorations->viewport() && event->type() == QEvent::Leave) {
        m_decorationHover = -1;
        animateDecorationTile(m_decorations->currentRow());
    }
    return QDialog::eventFilter(watched, event);
}

void SettingsDialog::setStatus(QLabel *label, bool ok, const QString &text)
{
    if (!label)
        return;
    label->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;")
                             .arg(ok ? QLatin1String(Theme::Green) : QStringLiteral("#f23f43")));
    label->setText(text);
    label->show();
}

void SettingsDialog::setProfileStatus(bool ok, const QString &text)
{
    setStatus(m_profileStatus, ok, text);
}

void SettingsDialog::setAccountStatus(bool ok, const QString &text)
{
    setStatus(m_accountStatus, ok, text);
}

bool SettingsDialog::profileDirty() const
{
    if (m_badgeCustomizer && m_badgeCustomizer->isDirty())
        return true;
    if (!m_displayName)
        return false;
    return m_displayName->text().trimmed() != m_loaded.globalName
        || m_pronouns->text().trimmed() != m_loaded.pronouns || m_bio->toPlainText() != m_loaded.bio
        || m_pending.avatarTouched || m_pending.bannerTouched || m_pending.accentTouched
        || m_pending.decorationTouched || m_pending.nameStyleTouched || m_pending.nameplateTouched
        || m_pending.effectTouched || m_pending.themeTouched || m_pending.tagTouched;
}

void SettingsDialog::updateSaveBar()
{
    if (!m_saveBar)
        return;
    m_saveBar->setVisible(profileDirty());
    const int length = m_bio->toPlainText().size();
    m_bioTitle->setText(QStringLiteral("ABOUT ME  (%1/190)").arg(length));
    m_saveButton->setEnabled(length <= 190);
}

void SettingsDialog::profileEdited()
{
    m_userEdited = true;
    refreshProfileArt();
    updateSaveBar();
}

void SettingsDialog::fillProfileFields()
{
    m_fillingProfile = true;
    m_displayName->setText(m_loaded.globalName);
    m_pronouns->setText(m_loaded.pronouns);
    m_bio->setPlainText(m_loaded.bio);
    m_username->setText(m_loaded.username);
    m_fillingProfile = false;

    // The decoration you have on now is the one picked in the grid.
    m_fillingDecorations = true;
    for (int row = 0; row < m_decorations->count(); ++row) {
        const QString sku = m_decorations->item(row)->data(Qt::UserRole).toMap().value(QStringLiteral("sku")).toString();
        if (sku == m_loaded.decorationSku) {
            m_decorations->setCurrentRow(row);
            break;
        }
    }
    m_fillingDecorations = false;

    fillLookEditors();
}

void SettingsDialog::refreshProfileArt()
{
    if (!m_preview)
        return;
    MediaCache &cache = MediaCache::instance();

    // Avatar: a picture just chosen, the default after Remove, or yours now.
    QByteArray avatarBytes;
    QImage avatar;
    if (m_pending.avatarTouched && !m_pending.avatarBytes.isEmpty()) {
        avatarBytes = m_pending.avatarBytes;
        avatar = QImage::fromData(avatarBytes);
    } else {
        const QUrl url = MediaCache::avatarUrl(m_selfUserId, m_pending.avatarTouched ? QString() : m_loaded.avatarHash,
                                               160);
        avatar = url.isEmpty() ? QImage() : cache.image(url);
        avatarBytes = cache.animationData(url);
    }

    // Decoration: the one picked in the grid, or the one on now.
    const QString asset = m_pending.decorationTouched
        ? m_pending.decoration.value(QStringLiteral("asset")).toString()
        : m_loaded.decorationAsset;
    const QUrl decorationUrl = MediaCache::decorationUrl(asset);
    const QImage decoration = decorationUrl.isEmpty() ? QImage() : cache.image(decorationUrl);

    // Banner.
    QByteArray bannerBytes;
    QImage banner;
    if (m_pending.bannerTouched) {
        bannerBytes = m_pending.bannerBytes;
        banner = QImage::fromData(bannerBytes);
    } else {
        const QUrl url = MediaCache::bannerUrl(m_selfUserId, m_loaded.bannerHash, 600);
        banner = url.isEmpty() ? QImage() : cache.image(url);
        bannerBytes = cache.animationData(url);
    }
    const int accent = m_pending.accentTouched ? m_pending.accent : m_loaded.accent;
    const QColor bannerColour = accent >= 0 ? QColor::fromRgb(QRgb(accent)) : QColor();

    const QString shown = m_displayName->text().trimmed();
    const Cosmetics::NameStyle nameStyle = shownNameStyle();
    const Cosmetics::GuildTag tag = shownTag();
    const auto [themeTop, themeBottom] = shownTheme();
    const QColor themePrimary = themeTop >= 0 ? QColor::fromRgb(QRgb(themeTop)) : QColor();
    const QColor themeAccent = themeBottom >= 0 ? QColor::fromRgb(QRgb(themeBottom)) : QColor();
    for (ProfilePreview *card : {m_preview, m_accountCard}) {
        if (!card)
            continue;
        card->setNameStyle(nameStyle);
        card->setGuildTag(tag);
        card->setTheme(themePrimary, themeAccent);
        card->setAvatar(avatarBytes, avatar);
        card->setDecoration(cache.animationData(decorationUrl), decoration);
        card->setBanner(bannerBytes, banner);
        card->setBannerColour(bannerColour);
        card->setNames(shown, m_loaded.username);
        card->setPronouns(m_pronouns->text().trimmed());
        card->setBio(card == m_preview ? m_bio->toPlainText() : QString());
    }
    // The chosen colour as a small square on the button. A thick left border
    // did this before and, on a rounded button, looked like a smudge.
    if (accent >= 0) {
        QPixmap swatch(16, 16);
        swatch.fill(Qt::transparent);
        QPainter paint(&swatch);
        paint.setRenderHint(QPainter::Antialiasing, true);
        paint.setPen(QColor(Theme::Border));
        paint.setBrush(bannerColour);
        paint.drawRoundedRect(QRectF(0.5, 0.5, 15, 15), 4, 4);
        paint.end();
        m_accentButton->setIcon(swatch);
    } else {
        m_accentButton->setIcon(QIcon());
    }

    // The profile effect plays on the Profiles card. It is made for a card
    // that shape; the wide one on My Account goes without, as Discord's does.
    const QString effectSku = shownEffectSku();
    showEffectDetails(effectSku);
    m_preview->setProfileEffect(m_effectDetails.value(effectSku));
    if (m_themePrimaryButton) {
        m_themePrimaryButton->setIcon(themeTop >= 0 ? swatchIcon({quint32(themeTop)}, 16) : QIcon());
        m_themeAccentButton->setIcon(themeBottom >= 0 ? swatchIcon({quint32(themeBottom)}, 16) : QIcon());
    }
    // The nameplates show your name in its style, picture and all.
    if (m_nameplates)
        m_nameplates->viewport()->update();

    // Each decoration tile shows it around your own face, as Discord's does.
    m_tileFace = avatar.isNull() ? MediaCache::initialsAvatar(m_loaded.username, 54)
                                 : MediaCache::circular(avatar, 54);
    for (int row = 0; row < m_decorations->count(); ++row) {
        QListWidgetItem *item = m_decorations->item(row);
        const QString tileAsset = item->data(Qt::UserRole).toMap().value(QStringLiteral("asset")).toString();
        const QUrl url = MediaCache::decorationUrl(tileAsset);
        paintDecorationTile(item, url.isEmpty() ? QImage() : cache.image(url));
    }

    // One tile moves at a time - the one under the pointer, or else the one
    // picked - as in Discord. Playing all of them would hold every frame of
    // every decoration in memory at once.
    animateDecorationTile(m_decorationHover >= 0 ? m_decorationHover : m_decorations->currentRow());

    refreshCardBadges();
}

void SettingsDialog::refreshCardBadges()
{
    if (!m_badgeCustomizer)
        return;
    QList<QImage> icons;
    for (const QUrl &url : m_badgeCustomizer->shownIcons())
        icons.append(url.isEmpty() ? QImage() : MediaCache::instance().image(url));
    for (ProfilePreview *card : {m_preview, m_accountCard}) {
        if (card)
            card->setBadges(icons);
    }
}

void SettingsDialog::paintDecorationTile(QListWidgetItem *item, const QImage &frame)
{
    QPixmap tile(72, 72);
    tile.fill(Qt::transparent);
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawPixmap(9, 9, m_tileFace);
    if (!frame.isNull())
        painter.drawImage(QRect(0, 0, 72, 72), frame);
    painter.end();
    item->setIcon(tile);
}

void SettingsDialog::animateDecorationTile(int row)
{
    QListWidgetItem *item = m_decorations->item(row);
    const QString asset = item ? item->data(Qt::UserRole).toMap().value(QStringLiteral("asset")).toString()
                               : QString();
    const QUrl url = MediaCache::decorationUrl(asset);
    const QByteArray bytes = url.isEmpty() ? QByteArray() : MediaCache::instance().animationData(url);
    if (row == m_animatedTileRow && bytes == m_animatedTileBytes)
        return;

    // The tile that stops goes back to its first frame.
    if (QListWidgetItem *previous = m_decorations->item(m_animatedTileRow)) {
        const QString oldAsset = previous->data(Qt::UserRole).toMap().value(QStringLiteral("asset")).toString();
        const QUrl oldUrl = MediaCache::decorationUrl(oldAsset);
        paintDecorationTile(previous, oldUrl.isEmpty() ? QImage() : MediaCache::instance().image(oldUrl));
    }

    m_animatedTileRow = row;
    m_animatedTileBytes = bytes;
    if (bytes.isEmpty())
        m_tileAnimation->clear();
    else
        m_tileAnimation->setData(bytes);
}

void SettingsDialog::loadProfile(bool force)
{
    ensurePage(m_profilesRow);
    if (!m_rest || m_selfUserId.isEmpty() || !m_displayName)
        return;
    if (!force && profileDirty())
        return;

    // The servers, by name, for nicknames.
    const QString keepServer = m_serverPick->currentData().toString();
    QList<GuildInfo> guilds = m_store->guilds();
    std::sort(guilds.begin(), guilds.end(), [](const GuildInfo &a, const GuildInfo &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    {
        const QSignalBlocker quiet(m_serverPick);
        m_serverPick->clear();
        for (const GuildInfo &guild : guilds)
            m_serverPick->addItem(guild.name, guild.id);
        const int keep = m_serverPick->findData(keepServer);
        m_serverPick->setCurrentIndex(keep >= 0 ? keep : 0);
    }
    loadServerNickname();

    // Start from what the store already knows, so the card is right at once.
    const UserInfo known = m_store->user(m_selfUserId);
    if (m_loaded.username.isEmpty()) {
        m_loaded.username = known.username;
        m_loaded.globalName = known.displayName() == known.username ? QString() : known.displayName();
        m_loaded.avatarHash = known.avatarHash;
    }
    m_pending = PendingProfile{};
    m_userEdited = false;
    if (m_badgeCustomizer)
        m_badgeCustomizer->load();
    fillProfileFields();
    refreshProfileArt();
    updateSaveBar();

    // The profile as Discord has it now.
    m_rest->fetchUserProfile(
        m_selfUserId, QString(),
        [this](const QJsonObject &answer) {
            const QJsonObject user = answer.value(QStringLiteral("user")).toObject();
            const QJsonObject profile = answer.value(QStringLiteral("user_profile")).toObject();
            const QJsonObject decoration = user.value(QStringLiteral("avatar_decoration_data")).toObject();

            m_loaded.username = user.value(QStringLiteral("username")).toString();
            m_loaded.globalName = user.value(QStringLiteral("global_name")).toString();
            m_loaded.avatarHash = user.value(QStringLiteral("avatar")).toString();
            m_loaded.bannerHash = profile.value(QStringLiteral("banner")).toString();
            if (m_loaded.bannerHash.isEmpty())
                m_loaded.bannerHash = user.value(QStringLiteral("banner")).toString();
            m_loaded.pronouns = profile.value(QStringLiteral("pronouns")).toString();
            m_loaded.bio = profile.value(QStringLiteral("bio")).toString();
            const QJsonValue accent = profile.value(QStringLiteral("accent_color"));
            m_loaded.accent = accent.isDouble() ? accent.toInt() : -1;
            m_loaded.decorationSku = decoration.value(QStringLiteral("sku_id")).toString();
            m_loaded.decorationAsset = decoration.value(QStringLiteral("asset")).toString();
            m_loaded.nameStyle = Cosmetics::nameStyle(user.value(QStringLiteral("display_name_styles")));
            m_loaded.nameplate = Cosmetics::nameplate(user.value(QStringLiteral("collectibles")));
            m_loaded.tag = Cosmetics::guildTag(user.value(QStringLiteral("primary_guild")));
            m_loaded.effectSku = Cosmetics::profileEffectSku(profile);
            m_loaded.collectibles = profile.value(QStringLiteral("collectibles")).toArray();
            const QJsonArray theme = profile.value(QStringLiteral("theme_colors")).toArray();
            m_loaded.themePrimary = theme.size() == 2 ? theme.at(0).toInt() : -1;
            m_loaded.themeAccent = theme.size() == 2 ? theme.at(1).toInt() : -1;
            loadServerTags();

            if (!m_userEdited) {
                fillProfileFields();
                refreshProfileArt();
                updateSaveBar();
            }
        },
        [](const RestClient::Error &error) {
            wlog(QStringLiteral("account"), QStringLiteral("could not load your profile: HTTP %1 %2")
                                                .arg(error.httpStatus).arg(error.message));
        });

    loadServerTags();

    // The decorations, nameplates and profile effects you own, each in its
    // picker.
    m_rest->fetchCollectibles(
        [this](const QJsonArray &products) {
            m_fillingLooks = true;
            m_nameplates->clear();
            auto *noPlate = new QListWidgetItem(QStringLiteral("None"), m_nameplates);
            noPlate->setData(Qt::UserRole, QVariantMap{});
            m_effects->clear();
            auto *noEffect = new QListWidgetItem(QStringLiteral("None"), m_effects);
            noEffect->setData(Qt::UserRole, QVariantMap{});
            int plates = 0;
            int effects = 0;
            for (const QJsonValue &value : products) {
                const QJsonObject product = value.toObject();
                const int type = product.value(QStringLiteral("type")).toInt();
                if (type != 1 && type != 2)
                    continue;
                const QString sku = skuOf(product.value(QStringLiteral("sku_id")));
                QJsonObject item;
                for (const QJsonValue &part : product.value(QStringLiteral("items")).toArray()) {
                    if (part.toObject().value(QStringLiteral("type")).toInt() == type) {
                        item = part.toObject();
                        break;
                    }
                }
                const QString name = product.value(QStringLiteral("name")).toString();
                if (type == 2) {
                    auto *row = new QListWidgetItem(name, m_nameplates);
                    row->setToolTip(name);
                    row->setData(Qt::UserRole, QVariantMap{{QStringLiteral("sku"), sku},
                                                           {QStringLiteral("palette"),
                                                            item.value(QStringLiteral("palette")).toString()},
                                                           {QStringLiteral("label"), name}});
                    ++plates;
                } else {
                    const QString thumbnail = item.value(QStringLiteral("thumbnailPreviewSrc")).toString();
                    auto *tile = new QListWidgetItem(effectTileIcon(thumbnail), name, m_effects);
                    tile->setToolTip(product.value(QStringLiteral("summary")).toString());
                    tile->setData(Qt::UserRole, QVariantMap{{QStringLiteral("sku"), sku},
                                                            {QStringLiteral("thumbnail"), thumbnail}});
                    // The purchase may already say what the effect is made of.
                    const Cosmetics::ProfileEffect effect = Cosmetics::profileEffect(product);
                    if (effect.isSet())
                        m_effectDetails.insert(sku, effect);
                    ++effects;
                }
            }
            m_fillingLooks = false;
            m_nameplateHint->setText(plates == 0 ? QStringLiteral("You do not own any nameplates. They come from the "
                                                                  "Discord shop.")
                                                 : QStringLiteral("The %1 nameplate%2 you own, behind your name in "
                                                                  "member lists.")
                                                       .arg(plates)
                                                       .arg(plates == 1 ? QString() : QStringLiteral("s")));
            m_effectHint->setText(effects == 0 ? QStringLiteral("You do not own any profile effects. They come from "
                                                                "the Discord shop.")
                                               : QStringLiteral("The %1 profile effect%2 you own. The preview plays "
                                                                "the one picked.")
                                                     .arg(effects)
                                                     .arg(effects == 1 ? QString() : QStringLiteral("s")));

            m_fillingDecorations = true;
            m_decorations->clear();
            auto *none = new QListWidgetItem(QStringLiteral("None"), m_decorations);
            none->setData(Qt::UserRole, QVariantMap{});
            int owned = 0;
            for (const QJsonValue &value : products) {
                const QJsonObject product = value.toObject();
                if (product.value(QStringLiteral("type")).toInt() != 0)
                    continue;
                const QString sku = product.value(QStringLiteral("sku_id")).toString();
                QString id;
                QString asset;
                for (const QJsonValue &part : product.value(QStringLiteral("items")).toArray()) {
                    const QJsonObject item = part.toObject();
                    if (item.value(QStringLiteral("type")).toInt() == 0) {
                        id = item.value(QStringLiteral("id")).toString();
                        asset = item.value(QStringLiteral("asset")).toString();
                        break;
                    }
                }
                auto *tile = new QListWidgetItem(product.value(QStringLiteral("name")).toString(), m_decorations);
                tile->setToolTip(product.value(QStringLiteral("summary")).toString());
                tile->setData(Qt::UserRole, QVariantMap{{QStringLiteral("id"), id.isEmpty() ? sku : id},
                                                        {QStringLiteral("sku"), sku},
                                                        {QStringLiteral("asset"), asset}});
                ++owned;
            }
            m_fillingDecorations = false;
            m_decorationHint->setText(owned == 0
                                          ? QStringLiteral("You do not own any decorations. They come from the "
                                                           "Discord shop.")
                                          : QStringLiteral("The %1 decoration%2 you own. Pick one, then Save Changes.")
                                                .arg(owned)
                                                .arg(owned == 1 ? QString() : QStringLiteral("s")));
            fillProfileFields();
            refreshProfileArt();
        },
        [this](const RestClient::Error &error) {
            wlog(QStringLiteral("account"), QStringLiteral("could not list your decorations: HTTP %1 %2")
                                                .arg(error.httpStatus).arg(error.message));
            m_decorationHint->setText(QStringLiteral("Could not load your decorations: %1").arg(error.message));
        });
}

void SettingsDialog::loadServerNickname()
{
    const QString guildId = m_serverPick ? m_serverPick->currentData().toString() : QString();
    if (guildId.isEmpty() || !m_rest)
        return;
    m_nickname->clear();
    m_nickname->setPlaceholderText(QStringLiteral("Loading…"));
    m_rest->fetchUserProfile(
        m_selfUserId, guildId,
        [this, guildId](const QJsonObject &answer) {
            if (m_serverPick->currentData().toString() != guildId)
                return;
            m_nickname->setText(answer.value(QStringLiteral("guild_member")).toObject()
                                    .value(QStringLiteral("nick")).toString());
            m_nickname->setPlaceholderText(QStringLiteral("Nickname in this server. Empty removes it."));
        },
        [this](const RestClient::Error &) {
            m_nickname->setPlaceholderText(QStringLiteral("Nickname in this server. Empty removes it."));
        });
}

void SettingsDialog::saveProfile()
{
    // The two halves go where Discord keeps them: names, avatar and
    // decoration on the account, the rest on the profile.
    QJsonObject userFields;
    QJsonObject profileFields;

    const QString name = m_displayName->text().trimmed();
    if (name != m_loaded.globalName)
        userFields.insert(QStringLiteral("global_name"), name.isEmpty() ? QJsonValue() : QJsonValue(name));
    if (m_pending.avatarTouched)
        userFields.insert(QStringLiteral("avatar"), m_pending.avatarData.isEmpty() ? QJsonValue()
                                                                                    : QJsonValue(m_pending.avatarData));
    if (m_pending.decorationTouched) {
        const QString id = m_pending.decoration.value(QStringLiteral("id")).toString();
        const QString sku = m_pending.decoration.value(QStringLiteral("sku")).toString();
        userFields.insert(QStringLiteral("avatar_decoration_id"), id.isEmpty() ? QJsonValue() : QJsonValue(id));
        userFields.insert(QStringLiteral("avatar_decoration_sku_id"), sku.isEmpty() ? QJsonValue() : QJsonValue(sku));
    }
    // Discord's account settings send these three together, or all null.
    if (m_pending.nameStyleTouched) {
        const Cosmetics::NameStyle &style = m_pending.nameStyle;
        if (style.isSet()) {
            QJsonArray colours;
            for (const quint32 colour : style.colors)
                colours.append(qint64(colour));
            userFields.insert(QStringLiteral("display_name_font_id"), style.fontId);
            userFields.insert(QStringLiteral("display_name_effect_id"), style.effectId);
            userFields.insert(QStringLiteral("display_name_colors"), colours);
        } else {
            userFields.insert(QStringLiteral("display_name_font_id"), QJsonValue());
            userFields.insert(QStringLiteral("display_name_effect_id"), QJsonValue());
            userFields.insert(QStringLiteral("display_name_colors"), QJsonValue());
        }
    }
    if (m_pending.nameplateTouched) {
        const QString sku = m_pending.nameplate.value(QStringLiteral("sku")).toString();
        userFields.insert(QStringLiteral("nameplate_sku_id"), sku.isEmpty() ? QJsonValue() : QJsonValue(sku));
    }
    if (m_pending.tagTouched)
        userFields.insert(QStringLiteral("primary_guild_id"),
                          m_pending.tagGuildId.isEmpty() ? QJsonValue() : QJsonValue(m_pending.tagGuildId));

    const QString pronouns = m_pronouns->text().trimmed();
    if (pronouns != m_loaded.pronouns)
        profileFields.insert(QStringLiteral("pronouns"), pronouns);
    if (m_bio->toPlainText() != m_loaded.bio)
        profileFields.insert(QStringLiteral("bio"), m_bio->toPlainText());
    if (m_pending.accentTouched)
        profileFields.insert(QStringLiteral("accent_color"), m_pending.accent);
    if (m_pending.bannerTouched)
        profileFields.insert(QStringLiteral("banner"), m_pending.bannerData.isEmpty() ? QJsonValue()
                                                                                       : QJsonValue(m_pending.bannerData));
    if (m_pending.themeTouched) {
        if (m_pending.themePrimary >= 0 && m_pending.themeAccent >= 0)
            profileFields.insert(QStringLiteral("theme_colors"),
                                 QJsonArray{m_pending.themePrimary, m_pending.themeAccent});
        else
            profileFields.insert(QStringLiteral("theme_colors"), QJsonValue());
    }
    // A profile's collectibles go as one list: whatever else it wears (a
    // frame) stays, and the effect is swapped, as Discord's editor does.
    if (m_pending.effectTouched) {
        QJsonArray skus;
        for (const QJsonValue &value : std::as_const(m_loaded.collectibles)) {
            const QJsonObject worn = value.toObject();
            const QString sku = skuOf(worn.value(QStringLiteral("sku_id")));
            if (worn.value(QStringLiteral("type")).toInt() != 1 && !sku.isEmpty())
                skus.append(sku);
        }
        if (!m_pending.effectSku.isEmpty())
            skus.append(m_pending.effectSku);
        profileFields.insert(QStringLiteral("collectibles_sku_ids"), skus);
    }

    m_saveButton->setEnabled(false);
    m_saveButton->setText(QStringLiteral("Saving…"));
    setProfileStatus(true, QStringLiteral("Saving…"));

    const auto finish = [this](bool ok, const QString &problem) {
        m_saveButton->setText(QStringLiteral("Save Changes"));
        m_saveButton->setEnabled(true);
        // Said on both pages: the save bar is under either.
        if (!ok) {
            setProfileStatus(false, QStringLiteral("Not saved: %1").arg(problem));
            setAccountStatus(false, QStringLiteral("Not saved: %1").arg(problem));
            return;
        }
        setProfileStatus(true, QStringLiteral("Saved."));
        setAccountStatus(true, QStringLiteral("Saved."));
        loadProfile(true);
    };

    // Badges last, the way Discord's own Save does them first: a separate
    // request either way, and the order does not matter to Discord.
    const auto saveBadges = [this, finish]() {
        if (!m_badgeCustomizer || !m_badgeCustomizer->isDirty()) {
            finish(true, QString());
            return;
        }
        m_badgeCustomizer->save([finish](bool ok, const QString &problem) {
            finish(ok, ok ? QString() : QStringLiteral("badges: %1").arg(problem));
        });
    };

    const auto saveProfilePart = [this, profileFields, saveBadges, finish]() {
        if (profileFields.isEmpty()) {
            saveBadges();
            return;
        }
        editProfileFields(profileFields, [saveBadges, finish](bool ok, const QString &problem) {
            if (!ok) {
                finish(false, problem);
                return;
            }
            saveBadges();
        });
    };

    if (userFields.isEmpty()) {
        saveProfilePart();
        return;
    }
    editUser(userFields, [saveProfilePart, finish](bool ok, const QString &problem) {
        if (!ok) {
            finish(false, problem);
            return;
        }
        saveProfilePart();
    });
}

bool SettingsDialog::solveCaptcha(const RestClient::Error &error, RestClient::CaptchaProof *proof, bool *asked)
{
    *asked = CaptchaDialog::isDemand(error.body);
    if (!*asked)
        return false;
    wlog(QStringLiteral("account"), QStringLiteral("Discord asks for a captcha before this change"));
    const QString token = CaptchaDialog::solve(this, error.body.value(QStringLiteral("captcha_sitekey")).toString(),
                                               error.body.value(QStringLiteral("captcha_rqdata")).toString());
    if (token.isEmpty())
        return false;
    proof->key = token;
    proof->rqtoken = error.body.value(QStringLiteral("captcha_rqtoken")).toString();
    proof->sessionId = error.body.value(QStringLiteral("captcha_session_id")).toString();
    return true;
}

void SettingsDialog::editProfileFields(const QJsonObject &fields, std::function<void(bool, const QString &)> done,
                                       const RestClient::CaptchaProof &captcha)
{
    m_rest->editCurrentProfile(
        fields, [done](const QJsonObject &) { done(true, QString()); },
        [this, fields, done, captcha](const RestClient::Error &error) {
            RestClient::CaptchaProof proof;
            bool asked = false;
            if (captcha.key.isEmpty() && solveCaptcha(error, &proof, &asked)) {
                editProfileFields(fields, done, proof);
                return;
            }
            wlog(QStringLiteral("account"), QStringLiteral("profile change refused: HTTP %1 %2")
                                                .arg(error.httpStatus).arg(error.message));
            done(false, asked ? QStringLiteral("the captcha was not finished.") : discordReason(error));
        },
        captcha);
}

void SettingsDialog::editUser(const QJsonObject &fields, std::function<void(bool, const QString &)> done,
                              const QString &mfaToken, const RestClient::CaptchaProof &captcha)
{
    m_rest->editCurrentUser(
        fields,
        [this, done](const QJsonObject &user) {
            // Some changes come back with a new sign-in token; the old one
            // no longer works after them.
            const QString token = user.value(QStringLiteral("token")).toString();
            if (!token.isEmpty())
                emit tokenReplaced(token);
            if (user.contains(QStringLiteral("id")))
                m_store->rememberUser(user);
            wlog(QStringLiteral("account"), QStringLiteral("account change saved"));
            done(true, QString());
        },
        [this, fields, done, mfaToken, captcha](const RestClient::Error &error) {
            // A captcha first, when Discord asks: solved by the person, then
            // the same change again with the answer.
            RestClient::CaptchaProof proof;
            bool asked = false;
            if (captcha.key.isEmpty() && solveCaptcha(error, &proof, &asked)) {
                editUser(fields, done, mfaToken, proof);
                return;
            }
            if (asked) {
                done(false, QStringLiteral("the captcha was not finished."));
                return;
            }

            // Two-factor accounts: Discord wants the code before a sensitive
            // change, then the same change again with proof.
            const QJsonObject mfa = error.body.value(QStringLiteral("mfa")).toObject();
            const QString ticket = mfa.value(QStringLiteral("ticket")).toString();
            if (error.body.value(QStringLiteral("code")).toInt() == 60003 && !ticket.isEmpty() && mfaToken.isEmpty()) {
                bool ok = false;
                const QString code = QInputDialog::getText(
                                         this, QStringLiteral("Two-factor code"),
                                         QStringLiteral("Discord wants your two-factor code for this.\n"
                                                        "The 6 digits from your authenticator app, or an 8 character "
                                                        "backup code:"),
                                         QLineEdit::Normal, QString(), &ok)
                                         .trimmed()
                                         .remove(QLatin1Char(' '))
                                         .remove(QLatin1Char('-'));
                if (!ok || code.isEmpty()) {
                    done(false, QStringLiteral("the two-factor code was not entered."));
                    return;
                }
                const QString type = code.size() == 8 ? QStringLiteral("backup") : QStringLiteral("totp");
                m_rest->finishMfa(
                    ticket, type, code,
                    [this, fields, done](const QJsonObject &answer) {
                        editUser(fields, done, answer.value(QStringLiteral("token")).toString());
                    },
                    [done](const RestClient::Error &problem) { done(false, discordReason(problem)); });
                return;
            }
            wlog(QStringLiteral("account"), QStringLiteral("account change refused: HTTP %1 %2")
                                                .arg(error.httpStatus).arg(error.message));
            done(false, discordReason(error));
        },
        mfaToken, captcha);
}

void SettingsDialog::editServer(const QJsonObject &fields, const QString &what,
                                const RestClient::CaptchaProof &captcha)
{
    const QString guildId = m_serverPick->currentData().toString();
    const QString server = m_serverPick->currentText();
    if (guildId.isEmpty())
        return;
    setProfileStatus(true, QStringLiteral("Saving…"));
    m_rest->editGuildMember(
        guildId, fields,
        [this, what, server](const QJsonObject &) {
            wlog(QStringLiteral("account"), QStringLiteral("%1 saved in %2").arg(what, server));
            setProfileStatus(true, QStringLiteral("%1 saved in %2.").arg(what, server));
        },
        [this, fields, what, server, captcha](const RestClient::Error &error) {
            RestClient::CaptchaProof proof;
            bool asked = false;
            if (captcha.key.isEmpty() && solveCaptcha(error, &proof, &asked)) {
                editServer(fields, what, proof);
                return;
            }
            wlog(QStringLiteral("account"), QStringLiteral("%1 in %2 refused: HTTP %3 %4")
                                                .arg(what, server).arg(error.httpStatus).arg(error.message));
            const QString reason = asked                    ? QStringLiteral("the captcha was not finished.")
                                   : error.httpStatus == 403 ? QStringLiteral("this server does not let you change that.")
                                                             : discordReason(error);
            setProfileStatus(false, QStringLiteral("%1 was not changed in %2: %3").arg(what, server, reason));
        },
        captcha);
}

QString SettingsDialog::pickPicture(const QString &title, QByteArray *bytesOut)
{
    const QString path = QFileDialog::getOpenFileName(
        this, title, QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        QStringLiteral("Pictures (*.png *.jpg *.jpeg *.gif *.webp)"));
    if (path.isEmpty())
        return {};

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setProfileStatus(false, QStringLiteral("Could not open that file."));
        return {};
    }
    if (file.size() > 10 * 1024 * 1024) {
        setProfileStatus(false, QStringLiteral("That picture is %1 MB; Discord takes up to 10 MB.")
                                    .arg(file.size() / (1024.0 * 1024.0), 0, 'f', 1));
        return {};
    }
    const QByteArray bytes = file.readAll();

    // The type from the file's own first bytes, not its name.
    QString mime;
    if (bytes.startsWith("\x89PNG"))
        mime = QStringLiteral("image/png");
    else if (bytes.startsWith("\xFF\xD8\xFF"))
        mime = QStringLiteral("image/jpeg");
    else if (bytes.startsWith("GIF8"))
        mime = QStringLiteral("image/gif");
    else if (bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP")
        mime = QStringLiteral("image/webp");
    if (mime.isEmpty()) {
        setProfileStatus(false, QStringLiteral("That is not a PNG, JPG, GIF or WebP picture."));
        return {};
    }

    if (bytesOut)
        *bytesOut = bytes;
    return QStringLiteral("data:%1;base64,%2").arg(mime, QString::fromLatin1(bytes.toBase64()));
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

QWidget *SettingsDialog::buildNotificationsPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(34, 28, 34, 28);
    layout->setSpacing(8);

    layout->addWidget(pageTitle(QStringLiteral("Notifications"), page));
    layout->addWidget(hint(QStringLiteral("A Windows notification for a new message while Singularity is not the "
                                          "window in front. Which chats count follows your Discord settings: every "
                                          "direct message, and in servers all messages, only @mentions or nothing, "
                                          "the way each server and channel is set. A muted server or channel still "
                                          "notifies when you are mentioned. Clicking one opens that chat."),
                           page));

    AppConfig &config = AppConfig::instance();
    const auto toggle = [&](const QString &key, const QString &label, bool fallback) {
        auto *box = new QCheckBox(label, page);
        box->setChecked(config.value(key, fallback).toBool());
        connect(box, &QCheckBox::toggled, this, [key](bool on) { AppConfig::instance().setValue(key, on); });
        layout->addWidget(box);
        return box;
    };

    layout->addWidget(groupTitle(QStringLiteral("DESKTOP"), page));
    toggle(QStringLiteral("notifications/desktop"), QStringLiteral("Show desktop notifications"), true);
    toggle(QStringLiteral("notifications/showText"), QStringLiteral("Show what the message says"), true);
    toggle(QStringLiteral("notifications/duringDnd"), QStringLiteral("Keep notifying while I am on Do Not Disturb"),
           true);
    layout->addWidget(hint(QStringLiteral("Discord itself goes quiet on Do Not Disturb. Turn the last one off to "
                                          "match it. Windows' own Focus mode can also hide them."),
                           page));

    auto *test = new QPushButton(QStringLiteral("Send a test notification"), page);
    test->setMaximumWidth(260);
    connect(test, &QPushButton::clicked, this, [this]() { emit testNotificationRequested(); });
    layout->addSpacing(8);
    layout->addWidget(test);

    // ---- Discord's sounds ---------------------------------------------------

    layout->addSpacing(16);
    layout->addWidget(groupTitle(QStringLiteral("SOUNDS"), page));
    layout->addWidget(hint(QStringLiteral("Discord's own sounds, chosen the way Discord chooses them. Each is "
                                          "fetched from Discord the first time it is needed and kept, and plays "
                                          "through the same speakers as your calls."),
                           page));
    toggle(QStringLiteral("sounds/enabled"), QStringLiteral("Play sounds"), true);

    auto *packRow = new QHBoxLayout;
    packRow->addWidget(new QLabel(QStringLiteral("Sound pack"), page));
    auto *pack = new QComboBox(page);
    const QString chosenPack = config.value(QStringLiteral("sounds/pack"), QStringLiteral("classic")).toString();
    for (const Sounds::Pack &each : Sounds::packs()) {
        pack->addItem(QLatin1String(each.label), QLatin1String(each.id));
        if (chosenPack == QLatin1String(each.id))
            pack->setCurrentIndex(pack->count() - 1);
    }
    connect(pack, &QComboBox::currentIndexChanged, this, [pack]() {
        AppConfig::instance().setValue(QStringLiteral("sounds/pack"), pack->currentData().toString());
        Sounds::instance().prefetch();
        Sounds::instance().play(QStringLiteral("user_join"));
    });
    packRow->addWidget(pack);
    packRow->addStretch(1);
    layout->addLayout(packRow);

    toggle(QStringLiteral("sounds/holidays"),
           QStringLiteral("Holiday sounds: Halloween and winter packs at their time of year"), true);
    QString season = QStringLiteral("Like Discord, they take over only while you are on Classic, and go back after: "
                                    "Halloween from 7 October to 2 November, Winter Holiday from 1 December to "
                                    "6 January. Discord gives a holiday's sounds only to the accounts in that "
                                    "holiday's experiment, and Singularity asks it which yours is.");
    // Discord's own answer for this account, once it has given one.
    const QString status = Sounds::instance().holidayStatus();
    const QString holiday = Sounds::instance().holidayNow();
    if (!status.isEmpty())
        season += QLatin1Char(' ') + status;
    else if (holiday == QLatin1String("halloween"))
        season += QStringLiteral(" It is Halloween now.");
    else if (holiday == QLatin1String("winter_holiday"))
        season += QStringLiteral(" It is the winter holiday now.");
    layout->addWidget(hint(season, page));

    auto *volumeRow = new QHBoxLayout;
    volumeRow->addWidget(new QLabel(QStringLiteral("Volume"), page));
    auto *volume = new QSlider(Qt::Horizontal, page);
    volume->setRange(0, 100);
    volume->setValue(config.value(QStringLiteral("sounds/volume"), 100).toInt());
    volume->setMaximumWidth(240);
    connect(volume, &QSlider::valueChanged, this,
            [](int value) { AppConfig::instance().setValue(QStringLiteral("sounds/volume"), value); });
    // Heard when let go, so the level can be judged without a sound per step.
    connect(volume, &QSlider::sliderReleased, this, []() { Sounds::instance().play(QStringLiteral("message1")); });
    volumeRow->addWidget(volume);
    volumeRow->addStretch(1);
    layout->addLayout(volumeRow);

    // One switch a sound, as in Discord's own settings.
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(4);
    int slot = 0;
    for (const Sounds::Switch &each : Sounds::switches()) {
        const QString name = QLatin1String(each.name);
        auto *box = new QCheckBox(QLatin1String(each.label), page);
        box->setChecked(!config.value(QStringLiteral("sounds/off/%1").arg(name), false).toBool());
        connect(box, &QCheckBox::toggled, this, [name](bool on) {
            AppConfig::instance().setValue(QStringLiteral("sounds/off/%1").arg(name), !on);
            if (on && name != QLatin1String("call_ringing") && name != QLatin1String("call_calling"))
                Sounds::instance().play(name);
        });
        grid->addWidget(box, slot / 3, slot % 3);
        ++slot;
    }
    layout->addLayout(grid);

    layout->addStretch(1);
    return page;
}

// ---------------------------------------------------------------------------
// Accessibility
// ---------------------------------------------------------------------------

QWidget *SettingsDialog::buildAccessibilityPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(34, 28, 34, 28);
    layout->setSpacing(8);

    layout->addWidget(pageTitle(QStringLiteral("Accessibility"), page));
    layout->addWidget(groupTitle(QStringLiteral("TEXT TO SPEECH"), page));
    layout->addWidget(hint(QStringLiteral("Right-click any message and choose Speak Message to hear it read aloud, "
                                          "as in Discord: who said it, then what they said, with mentions, emoji and "
                                          "links read as words. Stop Speaking in the same menu stops it."),
                           page));

    AppConfig &config = AppConfig::instance();

    // Where the natural voice is, and a way to fetch it.
    auto *status = new QLabel(page);
    status->setWordWrap(true);
    auto *fetch = new QPushButton(page);
    fetch->setMaximumWidth(280);
    const auto refresh = [status, fetch]() {
        const Speech &speech = Speech::instance();
        switch (speech.naturalState()) {
        case Speech::Natural::Missing:
            status->setText(QStringLiteral("The natural voice is not downloaded yet. It is about %1 MB, once, and "
                                           "runs on this PC; until then Windows' own voice reads.")
                                .arg(Speech::DownloadMegabytes));
            fetch->setText(QStringLiteral("Download the natural voice"));
            fetch->show();
            break;
        case Speech::Natural::Downloading:
            status->setText(QStringLiteral("Downloading the natural voice: %1%").arg(speech.downloadPercent()));
            fetch->hide();
            break;
        case Speech::Natural::Unpacking:
            status->setText(QStringLiteral("Unpacking the natural voice..."));
            fetch->hide();
            break;
        case Speech::Natural::Ready:
            status->setText(QStringLiteral("The natural voice is ready."));
            fetch->hide();
            break;
        case Speech::Natural::Failed:
            status->setText(QStringLiteral("The natural voice could not be set up: %1").arg(speech.failure()));
            fetch->setText(QStringLiteral("Try again"));
            fetch->show();
            break;
        }
    };
    refresh();
    connect(&Speech::instance(), &Speech::changed, status, refresh);
    connect(fetch, &QPushButton::clicked, this, []() { Speech::instance().downloadNatural(); });
    layout->addWidget(status);
    layout->addWidget(fetch);

    auto *natural = new QCheckBox(QStringLiteral("Use the natural voice once it is downloaded"), page);
    natural->setChecked(config.value(QStringLiteral("speech/natural"), true).toBool());
    connect(natural, &QCheckBox::toggled, this,
            [](bool on) { AppConfig::instance().setValue(QStringLiteral("speech/natural"), on); });
    layout->addWidget(natural);

    auto *voiceRow = new QHBoxLayout;
    voiceRow->addWidget(new QLabel(QStringLiteral("Voice"), page));
    auto *voice = new QComboBox(page);
    const QString chosen = config.value(QStringLiteral("speech/voice"), QStringLiteral("af_heart")).toString();
    for (const Speech::Voice &each : Speech::voices()) {
        voice->addItem(QLatin1String(each.label), QLatin1String(each.id));
        if (chosen == QLatin1String(each.id))
            voice->setCurrentIndex(voice->count() - 1);
    }
    connect(voice, &QComboBox::currentIndexChanged, this, [voice]() {
        AppConfig::instance().setValue(QStringLiteral("speech/voice"), voice->currentData().toString());
    });
    voiceRow->addWidget(voice);
    voiceRow->addStretch(1);
    layout->addLayout(voiceRow);
    layout->addWidget(hint(QStringLiteral("The natural voice's own voices. Windows' voice, when it is reading, is "
                                          "the first woman's voice Windows has."),
                           page));

    // Speed and volume, each shown as it moves.
    const auto slider = [&](const QString &label, const QString &key, int low, int high, int fallback) {
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(label, page));
        auto *bar = new QSlider(Qt::Horizontal, page);
        bar->setRange(low, high);
        bar->setValue(config.value(key, fallback).toInt());
        bar->setMaximumWidth(240);
        auto *shown = new QLabel(QStringLiteral("%1%").arg(bar->value()), page);
        connect(bar, &QSlider::valueChanged, this, [key, shown](int value) {
            AppConfig::instance().setValue(key, value);
            shown->setText(QStringLiteral("%1%").arg(value));
        });
        row->addWidget(bar);
        row->addWidget(shown);
        row->addStretch(1);
        layout->addLayout(row);
    };
    slider(QStringLiteral("Speed"), QStringLiteral("speech/speed"), 50, 200, 100);
    slider(QStringLiteral("Volume"), QStringLiteral("speech/volume"), 0, 100, 100);

    auto *hear = new QPushButton(QStringLiteral("Hear it"), page);
    hear->setMaximumWidth(160);
    connect(hear, &QPushButton::clicked, this, []() {
        Speech::instance().speak(QStringLiteral("This is how Singularity reads your messages to you."));
    });
    layout->addSpacing(4);
    layout->addWidget(hear);

    layout->addSpacing(12);
    auto *readNew = new QCheckBox(QStringLiteral("Read new messages aloud in the chat I am looking at"), page);
    readNew->setChecked(config.value(QStringLiteral("speech/readNew"), false).toBool());
    connect(readNew, &QCheckBox::toggled, this,
            [](bool on) { AppConfig::instance().setValue(QStringLiteral("speech/readNew"), on); });
    layout->addWidget(readNew);
    layout->addWidget(hint(QStringLiteral("Discord's text-to-speech for the current channel. Each message waits for "
                                          "the one before it, and none are read while you are deafened."),
                           page));

    layout->addStretch(1);
    return page;
}

// ---------------------------------------------------------------------------
// Voice & Video
// ---------------------------------------------------------------------------

QWidget *SettingsDialog::buildVoicePage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(34, 28, 34, 28);
    layout->setSpacing(8);

    layout->addWidget(pageTitle(QStringLiteral("Voice & Video"), page));

    auto *warning = new QLabel(
        QStringLiteral("Calls send and receive sound. The bar below is the level other people "
                       "hear, including the input slider."),
        page);
    warning->setWordWrap(true);
    warning->setStyleSheet(QStringLiteral("color: %1; background-color: %2; border: 1px solid %3; "
                                          "border-radius: 8px; padding: 10px;")
                               .arg(QLatin1String(Theme::LightGray), QLatin1String(Theme::SurfaceInput),
                                    QLatin1String(Theme::Border)));
    layout->addWidget(warning);

    // --- devices ----------------------------------------------------------
    layout->addWidget(groupTitle(QStringLiteral("DEVICES"), page));

    auto *form = new QFormLayout;
    form->setSpacing(8);

    m_inputDevice = new QComboBox(page);
    m_outputDevice = new QComboBox(page);
    m_cameraDevice = new QComboBox(page);
    form->addRow(QStringLiteral("Input device"), m_inputDevice);
    form->addRow(QStringLiteral("Output device"), m_outputDevice);
    form->addRow(QStringLiteral("Camera"), m_cameraDevice);
    layout->addLayout(form);

    refreshAudioDevices();
    refreshVideoDevices();

    connect(m_inputDevice, &QComboBox::currentIndexChanged, this, [this](int) {
        AppConfig::instance().setValue(QStringLiteral("voice/inputDevice"),
                                       m_inputDevice->currentData());
        // Follow the new device straight away if the test is running.
        if (m_meter->isRunning())
            startMicTest();
        emit voiceSettingsChanged();
    });
    connect(m_outputDevice, &QComboBox::currentIndexChanged, this, [this](int) {
        AppConfig::instance().setValue(QStringLiteral("voice/outputDevice"),
                                       m_outputDevice->currentData());
        emit voiceSettingsChanged();
    });
    connect(m_cameraDevice, &QComboBox::currentIndexChanged, this, [this](int) {
        AppConfig::instance().setValue(QStringLiteral("voice/cameraDevice"),
                                       m_cameraDevice->currentData());
        emit voiceSettingsChanged();
    });

    // Devices come and go when headsets are plugged in.
    auto *devices = new QMediaDevices(this);
    connect(devices, &QMediaDevices::audioInputsChanged, this, &SettingsDialog::refreshAudioDevices);
    connect(devices, &QMediaDevices::audioOutputsChanged, this, &SettingsDialog::refreshAudioDevices);
    connect(devices, &QMediaDevices::videoInputsChanged, this, &SettingsDialog::refreshVideoDevices);

    // --- levels -----------------------------------------------------------
    layout->addWidget(groupTitle(QStringLiteral("VOLUME"), page));

    AppConfig &config = AppConfig::instance();

    auto *levels = new QFormLayout;
    levels->setSpacing(8);

    m_inputVolume = makeSlider(0, 200, config.value(QStringLiteral("voice/inputVolume"), 100).toInt(), page);
    m_outputVolume = makeSlider(0, 200, config.value(QStringLiteral("voice/outputVolume"), 100).toInt(), page);
    m_streamVolume = makeSlider(0, 200, config.value(QStringLiteral("voice/streamVolume"), 80).toInt(), page);
    levels->addRow(QStringLiteral("Input volume"), m_inputVolume);
    levels->addRow(QStringLiteral("Output volume"), m_outputVolume);
    levels->addRow(QStringLiteral("Screen share volume"), m_streamVolume);
    layout->addLayout(levels);
    layout->addWidget(hint(QStringLiteral("Halfway on input is your microphone as it is. "
                                          "The rest of that slider, through to the end, is what makes "
                                          "a quiet mic loud enough on a call. The test bar includes it."),
                           page));

    connect(m_inputVolume, &QSlider::valueChanged, this, [this](int value) {
        AppConfig::instance().setValue(QStringLiteral("voice/inputVolume"), value);
        emit voiceSettingsChanged();
    });
    connect(m_outputVolume, &QSlider::valueChanged, this, [this](int value) {
        AppConfig::instance().setValue(QStringLiteral("voice/outputVolume"), value);
        emit voiceSettingsChanged();
    });
    connect(m_streamVolume, &QSlider::valueChanged, this, [this](int value) {
        AppConfig::instance().setValue(QStringLiteral("voice/streamVolume"), value);
        emit voiceSettingsChanged();
    });

    // --- microphone test --------------------------------------------------
    layout->addWidget(groupTitle(QStringLiteral("MIC TEST"), page));

    auto *testRow = new QHBoxLayout;
    testRow->setSpacing(10);

    m_micTestButton = new QPushButton(QStringLiteral("Let's check"), page);
    m_micTestButton->setFixedWidth(130);
    testRow->addWidget(m_micTestButton);

    m_levelBar = new LevelBar(page);
    testRow->addWidget(m_levelBar, 1);
    layout->addLayout(testRow);

    m_micHint = hint(QStringLiteral("Press the button and say something. The bar turns green when you "
                                    "are loud enough to count as speaking."),
                     page);
    layout->addWidget(m_micHint);

    connect(m_micTestButton, &QPushButton::clicked, this, [this]() {
        if (m_meter->isRunning())
            stopMicTest();
        else
            startMicTest();
    });
    connect(m_meter, &AudioMeter::levelChanged, this, [this](qreal level) {
        m_levelBar->setLevel(level);
    });

    // --- speaking threshold ----------------------------------------------
    layout->addWidget(groupTitle(QStringLiteral("INPUT SENSITIVITY"), page));

    m_sensitivity = makeSlider(0, 100, config.value(QStringLiteral("voice/sensitivity"), 15).toInt(), page);
    layout->addWidget(m_sensitivity);
    m_levelBar->setThreshold(m_sensitivity->value() / 100.0);

    connect(m_sensitivity, &QSlider::valueChanged, this, [this](int value) {
        AppConfig::instance().setValue(QStringLiteral("voice/sensitivity"), value);
        m_levelBar->setThreshold(value / 100.0);
        emit voiceSettingsChanged();
    });

    layout->addWidget(hint(QStringLiteral("The white mark on the bar is the line. Sound above it counts "
                                          "as you speaking."),
                           page));

    // --- switches ---------------------------------------------------------
    layout->addWidget(groupTitle(QStringLiteral("WHEN JOINING"), page));

    const auto addSwitch = [&](const QString &label, const QString &key, bool fallback) {
        auto *box = new QCheckBox(label, page);
        box->setChecked(config.value(key, fallback).toBool());
        connect(box, &QCheckBox::toggled, this, [key](bool on) {
            AppConfig::instance().setValue(key, on);
        });
        layout->addWidget(box);
        return box;
    };

    addSwitch(QStringLiteral("Join muted"), QStringLiteral("voice/joinMuted"), false);
    addSwitch(QStringLiteral("Join deafened"), QStringLiteral("voice/joinDeafened"), false);

    layout->addStretch(1);
    return page;
}

void SettingsDialog::refreshAudioDevices()
{
    if (!m_inputDevice || !m_outputDevice)
        return;
    const QByteArray savedInput =
        AppConfig::instance().value(QStringLiteral("voice/inputDevice")).toByteArray();
    const QByteArray savedOutput =
        AppConfig::instance().value(QStringLiteral("voice/outputDevice")).toByteArray();

    const auto fill = [](QComboBox *box, const QList<QAudioDevice> &devices,
                         const QAudioDevice &fallback, const QByteArray &saved) {
        QSignalBlocker blocker(box);
        box->clear();
        box->addItem(QStringLiteral("Default - %1").arg(fallback.description()), QByteArray());

        for (const QAudioDevice &device : devices)
            box->addItem(device.description(), device.id());

        const int index = box->findData(saved);
        box->setCurrentIndex(index >= 0 ? index : 0);
    };

    fill(m_inputDevice, QMediaDevices::audioInputs(), QMediaDevices::defaultAudioInput(), savedInput);
    fill(m_outputDevice, QMediaDevices::audioOutputs(), QMediaDevices::defaultAudioOutput(), savedOutput);
}

void SettingsDialog::refreshVideoDevices()
{
    if (!m_cameraDevice)
        return;

    const QByteArray saved =
        AppConfig::instance().value(QStringLiteral("voice/cameraDevice")).toByteArray();

    QSignalBlocker blocker(m_cameraDevice);
    m_cameraDevice->clear();

    const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    if (cameras.isEmpty()) {
        m_cameraDevice->addItem(QStringLiteral("No camera found"), QByteArray());
        m_cameraDevice->setEnabled(false);
        return;
    }

    m_cameraDevice->setEnabled(true);
    const QCameraDevice fallback = QMediaDevices::defaultVideoInput();
    m_cameraDevice->addItem(QStringLiteral("Default - %1").arg(fallback.description()), QByteArray());

    for (const QCameraDevice &camera : cameras)
        m_cameraDevice->addItem(camera.description(), camera.id());

    const int index = m_cameraDevice->findData(saved);
    m_cameraDevice->setCurrentIndex(index >= 0 ? index : 0);
}

void SettingsDialog::startMicTest()
{
    m_meter->start(m_inputDevice->currentData().toByteArray());
    if (!m_meter->isRunning()) {
        m_micHint->setText(QStringLiteral("No microphone could be opened. Check Windows sound settings."));
        return;
    }
    m_micTestButton->setText(QStringLiteral("Stop"));
    m_micHint->setText(QStringLiteral("Listening. Say something."));
}

void SettingsDialog::stopMicTest()
{
    if (!m_meter->isRunning())
        return;
    m_meter->stop();
    if (!m_micTestButton)
        return;
    m_micTestButton->setText(QStringLiteral("Let's check"));
    m_micHint->setText(QStringLiteral("Press the button and say something. The bar turns green when you "
                                      "are loud enough to count as speaking."));
}

// ---------------------------------------------------------------------------
// Appearance
// ---------------------------------------------------------------------------

QWidget *SettingsDialog::buildAppearancePage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(34, 28, 34, 28);
    layout->setSpacing(8);

    layout->addWidget(pageTitle(QStringLiteral("Appearance"), page));

    AppConfig &config = AppConfig::instance();

    layout->addWidget(groupTitle(QStringLiteral("THEME"), page));
    layout->addWidget(hint(QStringLiteral("One colour tints the black hole, the glass, and every accent at once."),
                           page));

    auto *chipRow = new QWidget(page);
    auto *chipLayout = new QHBoxLayout(chipRow);
    chipLayout->setContentsMargins(0, 4, 0, 10);
    chipLayout->setSpacing(8);

    const QString currentSeed = config.value(QStringLiteral("appearance/themeSeed"),
                                             QStringLiteral("#6ee7d8")).toString();

    int presetCount = 0;
    const Theme::Preset *presets = Theme::presets(&presetCount);

    auto isPresetHex = [presets, presetCount](const QString &hex) {
        for (int i = 0; i < presetCount; ++i) {
            if (QString::compare(hex, QLatin1String(presets[i].hex), Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };

    auto *customChip = new QPushButton(chipRow);
    customChip->setFixedSize(36, 36);
    customChip->setCursor(Qt::PointingHandCursor);
    customChip->setToolTip(QStringLiteral("Custom"));
    customChip->setVisible(false);

    auto restyleChips = [chipRow, customChip, isPresetHex](const QString &selectedHex) {
        const bool customActive = !selectedHex.isEmpty() && !isPresetHex(selectedHex);
        customChip->setVisible(customActive);
        customChip->setProperty("seed", customActive ? selectedHex : QString());

        const auto buttons = chipRow->findChildren<QPushButton *>();
        for (QPushButton *btn : buttons) {
            const QString hex = btn->property("seed").toString();
            if (hex.isEmpty())
                continue;
            const bool selected = QString::compare(selectedHex, hex, Qt::CaseInsensitive) == 0;
            btn->setStyleSheet(QStringLiteral(
                "QPushButton { background: %1; border: 2px solid %2; border-radius: 18px; padding: 0; min-width: 36px; }"
                "QPushButton:hover { border-color: #eef4fb; }")
                                   .arg(hex, selected ? QStringLiteral("#eef4fb")
                                                      : QStringLiteral("rgba(238, 244, 251, 70)")));
        }
    };

    auto applyHex = [this, restyleChips](const QString &hex) {
        AppConfig::instance().setValue(QStringLiteral("appearance/themeSeed"), hex);
        restyleChips(hex);
        emit appearanceChanged();
    };

    for (int i = 0; i < presetCount; ++i) {
        auto *chip = new QPushButton(chipRow);
        chip->setFixedSize(36, 36);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(QLatin1String(presets[i].name));
        chip->setProperty("seed", QLatin1String(presets[i].hex));
        chipLayout->addWidget(chip);
        connect(chip, &QPushButton::clicked, this, [chip, applyHex]() {
            applyHex(chip->property("seed").toString());
        });
    }

    chipLayout->addWidget(customChip);
    connect(customChip, &QPushButton::clicked, this, [customChip, applyHex]() {
        const QString hex = customChip->property("seed").toString();
        if (!hex.isEmpty())
            applyHex(hex);
    });

    auto *custom = new QPushButton(QStringLiteral("Custom…"), chipRow);
    connect(custom, &QPushButton::clicked, this, [this, applyHex]() {
        const QColor picked = QColorDialog::getColor(Theme::seedColor(), this, QStringLiteral("Theme colour"));
        if (!picked.isValid())
            return;
        applyHex(picked.name(QColor::HexRgb));
    });
    chipLayout->addWidget(custom);
    chipLayout->addStretch(1);
    layout->addWidget(chipRow);
    restyleChips(currentSeed);

    // -----------------------------------------------------------------
    // Text size and zoom: Discord's "Chat font scaling" and "Zoom level".
    // -----------------------------------------------------------------

    layout->addWidget(groupTitle(QStringLiteral("TEXT SIZE"), page));

    // A name on the left, the value on the right, the slider underneath.
    const auto sliderHeader = [page, layout](const QString &name) {
        auto *row = new QHBoxLayout;
        auto *label = new QLabel(name, page);
        auto *value = new QLabel(page);
        value->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));
        row->addWidget(label);
        row->addStretch(1);
        row->addWidget(value);
        layout->addLayout(row);
        return value;
    };

    auto *chatSizeValue = sliderHeader(QStringLiteral("Chat text size"));
    auto *chatSize = makeSlider(12, 24, config.value(QStringLiteral("appearance/fontSize"), 15).toInt(), page);
    layout->addWidget(chatSize);
    layout->addWidget(hint(QStringLiteral("Messages, names and emoji in the conversation. Takes effect when Settings closes."),
                           page));
    chatSizeValue->setText(QStringLiteral("%1 px").arg(chatSize->value()));

    // Moving the slider redraws the whole conversation, so the redraw waits
    // until the slider has been still for a moment rather than happening for
    // every step it passes through.
    auto *chatSizeSettle = new QTimer(page);
    chatSizeSettle->setSingleShot(true);
    chatSizeSettle->setInterval(150);
    connect(chatSizeSettle, &QTimer::timeout, this, [this, chatSize]() {
        AppConfig::instance().setValue(QStringLiteral("appearance/fontSize"), chatSize->value());
        emit appearanceChanged();
    });
    connect(chatSize, &QSlider::valueChanged, this, [chatSizeValue, chatSizeSettle](int value) {
        chatSizeValue->setText(QStringLiteral("%1 px").arg(value));
        chatSizeSettle->start();
    });

    // Zoom is Qt's scale factor, which is fixed once the first window exists:
    // it is saved here and read by the next copy, before it opens anything.
    const int startedZoom = qApp->property("singularityZoom").isValid()
                                ? qApp->property("singularityZoom").toInt() : 100;
    auto *zoomValue = sliderHeader(QStringLiteral("App zoom"));
    auto *zoom = makeSlider(8, 20, config.value(QStringLiteral("appearance/zoom"), 100).toInt() / 10, page);
    zoom->setPageStep(1);
    layout->addWidget(zoom);

    auto *zoomRow = new QHBoxLayout;
    auto *zoomHint = hint(QString(), page);
    auto *restart = new QPushButton(QStringLiteral("Restart now"), page);
    zoomRow->addWidget(zoomHint, 1);
    zoomRow->addWidget(restart);
    layout->addLayout(zoomRow);

    const auto showZoom = [zoom, zoomValue, zoomHint, restart, startedZoom]() {
        const int percent = zoom->value() * 10;
        zoomValue->setText(QStringLiteral("%1%").arg(percent));
        const bool waiting = percent != startedZoom;
        zoomHint->setText(waiting ? QStringLiteral("Everything in the window, larger or smaller. "
                                                   "Takes effect when Singularity restarts.")
                                  : QStringLiteral("Everything in the window, larger or smaller."));
        restart->setVisible(waiting);
    };
    showZoom();
    connect(zoom, &QSlider::valueChanged, this, [showZoom](int value) {
        AppConfig::instance().setValue(QStringLiteral("appearance/zoom"), value * 10);
        showZoom();
    });
    connect(restart, &QPushButton::clicked, this, &SettingsDialog::restartRequested);

    layout->addWidget(groupTitle(QStringLiteral("MESSAGES"), page));

    auto *form = new QFormLayout;
    form->setSpacing(8);

    auto *groupWindow = new QSpinBox(page);
    groupWindow->setRange(0, 60);
    groupWindow->setSuffix(QStringLiteral(" min"));
    groupWindow->setValue(config.value(QStringLiteral("appearance/groupMinutes"), 7).toInt());
    form->addRow(QStringLiteral("Join messages within"), groupWindow);

    auto *historyCount = new QSpinBox(page);
    historyCount->setRange(25, 100);
    historyCount->setSingleStep(25);
    historyCount->setValue(config.value(QStringLiteral("appearance/historyLimit"), 50).toInt());
    form->addRow(QStringLiteral("Messages to load"), historyCount);

    layout->addLayout(form);

    connect(groupWindow, &QSpinBox::valueChanged, this, [this](int value) {
        AppConfig::instance().setValue(QStringLiteral("appearance/groupMinutes"), value);
        emit appearanceChanged();
    });
    connect(historyCount, &QSpinBox::valueChanged, this, [](int value) {
        AppConfig::instance().setValue(QStringLiteral("appearance/historyLimit"), value);
    });

    layout->addWidget(hint(QStringLiteral("Zero minutes means every message keeps its own name and "
                                          "avatar. How many messages to load only applies to channels "
                                          "opened from now on."),
                           page));

    // -----------------------------------------------------------------
    // Background
    // -----------------------------------------------------------------

    layout->addWidget(groupTitle(QStringLiteral("BACKGROUND"), page));
    layout->addWidget(hint(QStringLiteral("The black hole is drawn live. A picture fills the window "
                                          "instead, keeping its own shape. The edges are cropped, "
                                          "never stretched. GIF, WebP, PNG or JPEG."),
                           page));

    auto *modeRow = new QWidget(page);
    auto *modeLayout = new QHBoxLayout(modeRow);
    modeLayout->setContentsMargins(0, 8, 0, 8);
    modeLayout->setSpacing(8);

    auto *useHole = new QPushButton(QStringLiteral("Black hole"), modeRow);
    auto *usePicture = new QPushButton(QStringLiteral("My picture"), modeRow);
    useHole->setCheckable(true);
    usePicture->setCheckable(true);
    useHole->setCursor(Qt::PointingHandCursor);
    usePicture->setCursor(Qt::PointingHandCursor);
    auto *modes = new QButtonGroup(modeRow);
    modes->setExclusive(true);
    modes->addButton(useHole);
    modes->addButton(usePicture);
    modeLayout->addWidget(useHole);
    modeLayout->addWidget(usePicture);
    modeLayout->addStretch(1);
    layout->addWidget(modeRow);

    // Wide, the same shape as the window, so a GIF is judged the way it will
    // actually sit rather than as a postage stamp beside two buttons.
    auto *preview = new QLabel(page);
    preview->setFixedSize(520, 220);
    preview->setAlignment(Qt::AlignCenter);
    preview->setStyleSheet(QStringLiteral("background: %1; border: 1px solid %2; border-radius: 12px; color: %3;")
                               .arg(QLatin1String(Theme::SurfaceInput), QLatin1String(Theme::Border),
                                    QLatin1String(Theme::TextFaint)));
    layout->addWidget(preview);

    auto *fileName = new QLabel(page);
    fileName->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                                .arg(QLatin1String(Theme::TextMuted)));
    layout->addWidget(fileName);

    auto *fileRow = new QWidget(page);
    auto *fileLayout = new QHBoxLayout(fileRow);
    fileLayout->setContentsMargins(0, 4, 0, 0);
    fileLayout->setSpacing(8);

    auto *chooseFile = new QPushButton(QStringLiteral("Choose a picture…"), fileRow);
    auto *clearFile = new QPushButton(QStringLiteral("Remove"), fileRow);
    fileLayout->addWidget(chooseFile);
    fileLayout->addWidget(clearFile);
    fileLayout->addStretch(1);
    layout->addWidget(fileRow);

    auto *dimHeader = new QHBoxLayout;
    auto *dimLabel = new QLabel(QStringLiteral("Dim"), page);
    dimLabel->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextPrimary)));
    auto *dimValue = new QLabel(page);
    dimValue->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));
    dimHeader->addWidget(dimLabel);
    dimHeader->addStretch(1);
    dimHeader->addWidget(dimValue);
    layout->addLayout(dimHeader);

    auto *dim = makeSlider(0, 80, config.value(QStringLiteral("appearance/backgroundDim"), 45).toInt(),
                           page);
    layout->addWidget(dim);

    layout->addWidget(hint(QStringLiteral("Dim lays a flat darkening over the whole picture, so a "
                                          "bright GIF stays even instead of turning into a spotlight. "
                                          "Higher is darker, and the conversation stays readable."),
                           page));

    // The preview, the mode buttons and the file buttons have to agree.
    auto refreshBackground = [preview, fileName, dimValue, useHole, usePicture, clearFile, dim]() {
        AppConfig &config = AppConfig::instance();
        const QString path = config.value(QStringLiteral("appearance/backgroundPath")).toString();
        const bool picture =
            config.value(QStringLiteral("appearance/backgroundMode")).toString()
            == QLatin1String("picture")
            && !path.isEmpty() && QFileInfo::exists(path);

        useHole->blockSignals(true);
        usePicture->blockSignals(true);
        useHole->setChecked(!picture);
        usePicture->setChecked(picture);
        useHole->blockSignals(false);
        usePicture->blockSignals(false);
        usePicture->setEnabled(!path.isEmpty() && QFileInfo::exists(path));
        clearFile->setEnabled(!path.isEmpty());
        dim->setEnabled(picture);
        dimValue->setText(QStringLiteral("%1%").arg(dim->value()));

        // Dark ink on the accent, because the accent can be a bright swatch
        // and the normal text colour is already light.
        const QString onStyle = QStringLiteral(
            "QPushButton { background: %1; color: #07090d; border: 1px solid %1; font-weight: 600; }")
                                    .arg(QLatin1String(Theme::Accent));
        const QString offStyle = QStringLiteral(
            "QPushButton { background: %1; color: %2; border: 1px solid %3; }")
                                     .arg(QLatin1String(Theme::SurfaceInput),
                                          QLatin1String(Theme::TextMuted),
                                          QLatin1String(Theme::Border));
        useHole->setStyleSheet(useHole->isChecked() ? onStyle : offStyle);
        usePicture->setStyleSheet(usePicture->isChecked() ? onStyle : offStyle);

        if (!picture && (path.isEmpty() || !QFileInfo::exists(path))) {
            preview->setPixmap(QPixmap());
            preview->setText(path.isEmpty() ? QStringLiteral("No picture chosen")
                                            : QStringLiteral("That file is no longer there"));
            fileName->setText(QStringLiteral("Black hole"));
            return;
        }

        if (!picture) {
            preview->setPixmap(QPixmap());
            preview->setText(QStringLiteral("Black hole"));
            fileName->setText(QStringLiteral("Drawn live, tinted by the theme colour."));
            return;
        }

        // First frame only. A second decoder running behind the settings
        // window is not what a thumbnail is for.
        QImage first(path);
        if (first.isNull()) {
            QMovie probe(path);
            probe.jumpToFrame(0);
            first = probe.currentImage();
        }

        if (first.isNull()) {
            preview->setPixmap(QPixmap());
            preview->setText(QStringLiteral("Could not read that file"));
            fileName->setText(QFileInfo(path).fileName());
            return;
        }

        QImage fitted = first.scaled(preview->size(), Qt::KeepAspectRatioByExpanding,
                                     Qt::SmoothTransformation);
        const int x = qMax(0, (fitted.width() - preview->width()) / 2);
        const int y = qMax(0, (fitted.height() - preview->height()) / 2);
        fitted = fitted.copy(x, y, preview->width(), preview->height());

        QPainter painter(&fitted);
        painter.fillRect(fitted.rect(), QColor(7, 9, 13, dim->value() * 255 / 100));
        painter.end();

        preview->setText(QString());
        preview->setPixmap(QPixmap::fromImage(fitted));

        const bool animated = QFileInfo(path).suffix().compare(QLatin1String("gif"), Qt::CaseInsensitive) == 0
            || QFileInfo(path).suffix().compare(QLatin1String("webp"), Qt::CaseInsensitive) == 0;
        fileName->setText(QStringLiteral("%1  ·  %2×%3%4")
                              .arg(QFileInfo(path).fileName())
                              .arg(first.width())
                              .arg(first.height())
                              .arg(animated ? QStringLiteral("  ·  animated") : QString()));
    };

    connect(useHole, &QPushButton::toggled, this, [this, refreshBackground](bool on) {
        if (!on)
            return;
        AppConfig::instance().setValue(QStringLiteral("appearance/backgroundMode"),
                                       QStringLiteral("hole"));
        refreshBackground();
        emit appearanceChanged();
    });

    connect(usePicture, &QPushButton::toggled, this, [this, refreshBackground](bool on) {
        if (!on)
            return;
        AppConfig::instance().setValue(QStringLiteral("appearance/backgroundMode"),
                                       QStringLiteral("picture"));
        refreshBackground();
        emit appearanceChanged();
    });

    connect(chooseFile, &QPushButton::clicked, this, [this, refreshBackground]() {
        const QString picked = QFileDialog::getOpenFileName(
            this, QStringLiteral("Choose a background"), QString(),
            QStringLiteral("Pictures (*.gif *.webp *.png *.jpg *.jpeg *.bmp)"));
        if (picked.isEmpty())
            return;

        // Copied into the program's own folder rather than remembered by
        // path. A wallpaper chosen out of a downloads folder is one tidy-up
        // away from vanishing, and the background would quietly revert with
        // no way to tell why.
        const QString stored = copyBackgroundFile(picked);
        if (stored.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Could not use that picture"),
                                 QStringLiteral("It could not be copied into Singularity's folder."));
            return;
        }

        AppConfig &config = AppConfig::instance();
        config.setValue(QStringLiteral("appearance/backgroundPath"), stored);
        config.setValue(QStringLiteral("appearance/backgroundMode"), QStringLiteral("picture"));
        refreshBackground();
        emit appearanceChanged();
    });

    connect(clearFile, &QPushButton::clicked, this, [this, refreshBackground]() {
        AppConfig &config = AppConfig::instance();
        config.setValue(QStringLiteral("appearance/backgroundPath"), QString());
        config.setValue(QStringLiteral("appearance/backgroundMode"), QStringLiteral("hole"));
        refreshBackground();
        emit appearanceChanged();
    });

    connect(dim, &QSlider::valueChanged, this, [this, refreshBackground](int value) {
        AppConfig::instance().setValue(QStringLiteral("appearance/backgroundDim"), value);
        refreshBackground();
        emit appearanceChanged();
    });

    refreshBackground();

    auto *playSky = new QCheckBox(QStringLiteral("Animate the background"), page);
    playSky->setChecked(config.value(QStringLiteral("appearance/animatedBackground"), true).toBool());
    playSky->setToolTip(QStringLiteral("Turning this off freezes the black hole, and stops an "
                                       "animated picture playing."));
    connect(playSky, &QCheckBox::toggled, this, [this](bool on) {
        AppConfig::instance().setValue(QStringLiteral("appearance/animatedBackground"), on);
        emit appearanceChanged();
    });
    layout->addWidget(playSky);

    layout->addWidget(groupTitle(QStringLiteral("PICTURES"), page));

    auto *playGifs = new QCheckBox(QStringLiteral("Play animated pictures in messages"), page);
    playGifs->setChecked(config.value(QStringLiteral("appearance/playAnimations"), true).toBool());
    connect(playGifs, &QCheckBox::toggled, this, [this](bool on) {
        AppConfig::instance().setValue(QStringLiteral("appearance/playAnimations"), on);
        emit appearanceChanged();
    });
    layout->addWidget(playGifs);

    layout->addStretch(1);
    return page;
}

// ---------------------------------------------------------------------------
// Plugins
// ---------------------------------------------------------------------------

QWidget *SettingsDialog::buildPluginsPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(34, 28, 34, 28);
    layout->setSpacing(10);

    layout->addWidget(pageTitle(QStringLiteral("Plugins"), page));
    layout->addWidget(hint(QStringLiteral("Plugins are built into Singularity itself, not loaded from files, "
                                          "so nothing outside this program can add code."),
                           page));

    const QList<Plugin *> plugins = m_plugins->plugins();
    for (Plugin *plugin : plugins) {
        auto *card = new QFrame(page);
        card->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 10px;")
                                .arg(QLatin1String(Theme::SurfaceSidebar)));

        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 12, 14, 12);
        cardLayout->setSpacing(4);

        auto *toggle = new QCheckBox(plugin->name(), card);
        toggle->setChecked(m_plugins->isEnabled(plugin->id()));
        toggle->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 14px;"));
        cardLayout->addWidget(toggle);

        auto *description = new QLabel(plugin->description(), card);
        description->setWordWrap(true);
        description->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                       .arg(QLatin1String(Theme::TextMuted)));
        cardLayout->addWidget(description);

        if (QWidget *settings = plugin->createSettingsWidget(card))
            cardLayout->addWidget(settings);

        const QString pluginId = plugin->id();
        connect(toggle, &QCheckBox::toggled, this, [this, pluginId](bool on) {
            m_plugins->setEnabled(pluginId, on);
        });

        layout->addWidget(card);
    }

    layout->addStretch(1);
    return page;
}

// ---------------------------------------------------------------------------
// Advanced
// ---------------------------------------------------------------------------

QWidget *SettingsDialog::buildAdvancedPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(34, 28, 34, 28);
    layout->setSpacing(8);

    layout->addWidget(pageTitle(QStringLiteral("Advanced"), page));

    AppConfig &config = AppConfig::instance();

    layout->addWidget(groupTitle(QStringLiteral("LOG"), page));

    auto *path = new QLabel(Logger::instance().filePath(), page);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setWordWrap(true);
    path->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                            .arg(QLatin1String(Theme::TextFaint)));
    layout->addWidget(path);

    auto *openFolder = new QPushButton(QStringLiteral("Open log folder"), page);
    openFolder->setMaximumWidth(220);
    connect(openFolder, &QPushButton::clicked, this, []() {
        const QFileInfo info(Logger::instance().filePath());
        QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
    });
    layout->addWidget(openFolder);

    layout->addWidget(groupTitle(QStringLiteral("SETTINGS FILE"), page));

    auto *settingsPath = new QLabel(page);
    settingsPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    settingsPath->setWordWrap(true);
    settingsPath->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                    .arg(QLatin1String(Theme::TextFaint)));
    settingsPath->setText(QStringLiteral("Your token and settings live beside the log."));
    layout->addWidget(settingsPath);

    layout->addWidget(groupTitle(QStringLiteral("CONNECTION"), page));

    auto *reconnectNote = hint(QStringLiteral("Singularity reconnects by itself with a growing wait, capped at "
                                              "one minute. Close codes are written to the log."),
                               page);
    layout->addWidget(reconnectNote);

    auto *devMode = new QCheckBox(QStringLiteral("Developer mode (show raw ids in menus)"), page);
    devMode->setChecked(config.value(QStringLiteral("advanced/developerMode"), false).toBool());
    connect(devMode, &QCheckBox::toggled, this, [](bool on) {
        AppConfig::instance().setValue(QStringLiteral("advanced/developerMode"), on);
    });
    layout->addWidget(devMode);

    layout->addStretch(1);
    return page;
}
