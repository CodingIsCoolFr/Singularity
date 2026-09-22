#include "ui/SettingsDialog.h"

#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/MessageStore.h"
#include "plugin/PluginHost.h"
#include "ui/AudioMeter.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QAudioDevice>
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
#include <QMessageBox>
#include <QMovie>
#include <QPainter>
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
    m_pages = new QStackedWidget(this);
    root->addWidget(m_pages, 1);

    addSection(QStringLiteral("My Account"), buildAccountPage());
    addSection(QStringLiteral("Voice & Video"), buildVoicePage());
    addSection(QStringLiteral("Appearance"), buildAppearancePage());
    addSection(QStringLiteral("Plugins"), buildPluginsPage());
    addSection(QStringLiteral("Advanced"), buildAdvancedPage());

    connect(m_sections, &QListWidget::currentRowChanged, this, [this](int row) {
        m_pages->setCurrentIndex(row);
        // The microphone is only held open while its page is in front.
        if (row != 1)
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

QSpinBox, QComboBox, QLineEdit {
    background-color: %6;
    color: %5;
    border: 1px solid %2;
    border-radius: 8px;
    padding: 6px 10px;
    min-height: 22px;
    selection-background-color: %7;
}
QSpinBox:focus, QComboBox:focus, QLineEdit:focus { border-color: %7; }
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

void SettingsDialog::addSection(const QString &title, QWidget *page)
{
    m_sections->addItem(title);

    // Every page scrolls, so a small window never clips the controls.
    auto *scroll = new QScrollArea(m_pages);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(page);
    m_pages->addWidget(scroll);
}

// ---------------------------------------------------------------------------
// My Account
// ---------------------------------------------------------------------------

QWidget *SettingsDialog::buildAccountPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(34, 28, 34, 28);
    layout->setSpacing(8);

    layout->addWidget(pageTitle(QStringLiteral("My Account"), page));

    const UserInfo info = m_store->user(m_selfUserId);

    auto *row = new QHBoxLayout;
    row->setSpacing(14);

    auto *avatar = new QLabel(page);
    avatar->setFixedSize(72, 72);
    const QUrl url = MediaCache::avatarUrl(m_selfUserId, info.avatarHash, 160);
    const QImage picture = url.isEmpty() ? QImage() : MediaCache::instance().image(url);
    avatar->setPixmap(picture.isNull() ? MediaCache::initialsAvatar(info.displayName(), 72)
                                       : MediaCache::circular(picture, 72));
    row->addWidget(avatar);

    auto *names = new QVBoxLayout;
    names->setSpacing(2);

    auto *display = new QLabel(info.displayName().isEmpty() ? QStringLiteral("Signed in")
                                                            : info.displayName(),
                               page);
    display->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600; color: %1;")
                               .arg(QLatin1String(Theme::TextPrimary)));
    names->addWidget(display);

    auto *handle = new QLabel(info.username.isEmpty() ? m_selfUserId
                                                      : QStringLiteral("@%1").arg(info.username),
                              page);
    handle->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));
    handle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    names->addWidget(handle);

    names->addStretch(1);
    row->addLayout(names, 1);
    layout->addLayout(row);

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
                                          "Changing your name, avatar, password or email is not built "
                                          "yet: do those on discord.com."),
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
        QStringLiteral("Singularity can join a voice channel, so other people see you there, but it cannot "
                       "send or receive sound yet. The devices and levels below are real and are "
                       "saved, and the microphone test works now."),
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
    form->addRow(QStringLiteral("Input device"), m_inputDevice);
    form->addRow(QStringLiteral("Output device"), m_outputDevice);
    layout->addLayout(form);

    refreshAudioDevices();

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

    // Devices come and go when headsets are plugged in.
    auto *devices = new QMediaDevices(this);
    connect(devices, &QMediaDevices::audioInputsChanged, this, &SettingsDialog::refreshAudioDevices);
    connect(devices, &QMediaDevices::audioOutputsChanged, this, &SettingsDialog::refreshAudioDevices);

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

    layout->addWidget(groupTitle(QStringLiteral("MESSAGES"), page));

    auto *form = new QFormLayout;
    form->setSpacing(8);

    auto *fontSize = new QSpinBox(page);
    fontSize->setRange(11, 20);
    fontSize->setSuffix(QStringLiteral(" px"));
    fontSize->setValue(config.value(QStringLiteral("appearance/fontSize"), 14).toInt());
    form->addRow(QStringLiteral("Message text size"), fontSize);

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

    connect(fontSize, &QSpinBox::valueChanged, this, [this](int value) {
        AppConfig::instance().setValue(QStringLiteral("appearance/fontSize"), value);
        emit appearanceChanged();
    });
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
