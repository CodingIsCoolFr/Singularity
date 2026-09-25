#pragma once

#include <QDialog>
#include <QJsonObject>

#include <functional>

class AudioMeter;
class LevelBar;
class MessageStore;
class PluginHost;
class RestClient;

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
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

    // Raised whenever a voice setting moves.
    //
    // Writing the value to the config file is not enough: a call already in
    // progress read those numbers when it started and never looks again, so
    // without this the sliders do nothing until the next time you join.
    void voiceSettingsChanged();

    // Discord answered an account change with a new sign-in token (it does
    // after some changes). The old one stops working, so the window swaps it.
    void tokenReplaced(const QString &token);

    // "Send a test notification" on the Notifications page.
    void testNotificationRequested();

protected:
    void showEvent(QShowEvent *event) override;

private:
    // My Account: what is on Discord now, loaded each time the window opens.
    void loadProfile();
    void loadServerNickname();
    void setProfileStatus(bool ok, const QString &text);
    void setAvatarPreview(const QImage &picture);

    // One change each. Every answer, good or bad, is written under the title.
    // editUser handles Discord's "two factor is required" by asking for the
    // code and sending the same change again.
    void editUser(const QJsonObject &fields, const QString &what, const QString &mfaToken = {});
    void editProfile(const QJsonObject &fields, const QString &what);
    void editServer(const QJsonObject &fields, const QString &what);

    // Asks for a picture and hands it back as a data URI, or empty.
    QString pickPicture(const QString &title, QImage *preview);

    QWidget *buildAccountPage();
    QWidget *buildVoicePage();
    QWidget *buildNotificationsPage();
    QWidget *buildAppearancePage();
    QWidget *buildPluginsPage();
    QWidget *buildAdvancedPage();

    void addSection(const QString &title, QWidget *page);
    void startMicTest();
    void stopMicTest();
    void refreshAudioDevices();
    void refreshVideoDevices();

    MessageStore *m_store = nullptr;
    RestClient *m_rest = nullptr;
    PluginHost *m_plugins = nullptr;
    QString m_selfUserId;

    QListWidget *m_sections = nullptr;
    QStackedWidget *m_pages = nullptr;

    // My Account page.
    QLabel *m_avatarPreview = nullptr;
    QLabel *m_nameLabel = nullptr;
    QLabel *m_handleLabel = nullptr;
    QLabel *m_profileStatus = nullptr;
    QLineEdit *m_displayName = nullptr;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_pronouns = nullptr;
    QPlainTextEdit *m_bio = nullptr;
    QPushButton *m_accentButton = nullptr;
    int m_accentColour = -1;
    QComboBox *m_decorations = nullptr;
    QString m_currentDecorationSku;
    QComboBox *m_serverPick = nullptr;
    QLineEdit *m_nickname = nullptr;

    // Voice page.
    QComboBox *m_inputDevice = nullptr;
    QComboBox *m_outputDevice = nullptr;
    QComboBox *m_cameraDevice = nullptr;
    QSlider *m_inputVolume = nullptr;
    QSlider *m_outputVolume = nullptr;
    QSlider *m_streamVolume = nullptr;
    QSlider *m_sensitivity = nullptr;
    QPushButton *m_micTestButton = nullptr;
    LevelBar *m_levelBar = nullptr;
    QLabel *m_micHint = nullptr;
    AudioMeter *m_meter = nullptr;
};
