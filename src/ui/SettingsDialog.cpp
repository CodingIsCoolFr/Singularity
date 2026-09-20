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
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFormLayout>
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
    label->setStyleSheet(QStringLiteral("font-size: 19px; font-weight: 700; color: %1;")
                             .arg(QLatin1String(Theme::TextPrimary)));
    return label;
}

QLabel *groupTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 700; "
                                        "letter-spacing: 0.6px; margin-top: 10px;")
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
    resize(900, 640);

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
    background-color: %4;
    color: %5;
    font-weight: 600;
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
                           QLatin1String(Theme::Accent)));
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

    layout->addWidget(groupTitle(QStringLiteral("PICTURES"), page));

    auto *playGifs = new QCheckBox(QStringLiteral("Play animated pictures"), page);
    playGifs->setChecked(config.value(QStringLiteral("appearance/playAnimations"), true).toBool());
    connect(playGifs, &QCheckBox::toggled, this, [this](bool on) {
        AppConfig::instance().setValue(QStringLiteral("appearance/playAnimations"), on);
        emit appearanceChanged();
    });
    layout->addWidget(playGifs);

    auto *playSky = new QCheckBox(QStringLiteral("Spin the black hole"), page);
    playSky->setChecked(config.value(QStringLiteral("appearance/animatedBackground"), true).toBool());
    connect(playSky, &QCheckBox::toggled, this, [this](bool on) {
        AppConfig::instance().setValue(QStringLiteral("appearance/animatedBackground"), on);
        emit appearanceChanged();
    });
    layout->addWidget(playSky);

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
