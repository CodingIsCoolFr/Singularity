#pragma once

#include <QDialog>

class AudioMeter;
class LevelBar;
class MessageStore;
class PluginHost;
class RestClient;

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QStackedWidget;

// The settings window, laid out like Discord's: a list of sections on the
// left, one page at a time on the right.
//
// Every control here either changes something real or says plainly that it is
// waiting on voice audio. Nothing is a decoration.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    SettingsDialog(MessageStore *store, RestClient *rest, PluginHost *plugins,
                   const QString &selfUserId, QWidget *parent = nullptr);
    ~SettingsDialog() override;

signals:
    // Raised when a change needs the message list redrawn.
    void appearanceChanged();
    void logOutRequested();

private:
    QWidget *buildAccountPage();
    QWidget *buildVoicePage();
    QWidget *buildAppearancePage();
    QWidget *buildPluginsPage();
    QWidget *buildAdvancedPage();

    void addSection(const QString &title, QWidget *page);
    void startMicTest();
    void stopMicTest();
    void refreshAudioDevices();

    MessageStore *m_store = nullptr;
    RestClient *m_rest = nullptr;
    PluginHost *m_plugins = nullptr;
    QString m_selfUserId;

    QListWidget *m_sections = nullptr;
    QStackedWidget *m_pages = nullptr;

    // Voice page.
    QComboBox *m_inputDevice = nullptr;
    QComboBox *m_outputDevice = nullptr;
    QSlider *m_inputVolume = nullptr;
    QSlider *m_outputVolume = nullptr;
    QSlider *m_sensitivity = nullptr;
    QPushButton *m_micTestButton = nullptr;
    LevelBar *m_levelBar = nullptr;
    QLabel *m_micHint = nullptr;
    AudioMeter *m_meter = nullptr;
};
