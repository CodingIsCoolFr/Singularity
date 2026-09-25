#pragma once

#include "core/RestClient.h"

#include <QDialog>
#include <QJsonObject>
#include <QPixmap>
#include <QVariantMap>

#include <functional>

class AudioMeter;
class ProfilePreview;
class QFrame;
class LevelBar;
class MessageStore;
class PluginHost;

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

    // "Restart now" under App zoom. The zoom is read once, before the first
    // window exists, so it only takes effect in a new copy.
    void restartRequested();

protected:
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // My Account and Profiles: what is on Discord now, loaded each time the
    // window opens (force) or when nothing has been edited since.
    QWidget *buildProfilesPage();
    QWidget *buildSaveBar();
    void loadProfile(bool force);
    void loadServerNickname();
    void fillProfileFields();
    void refreshProfileArt();   // both cards and the decoration tiles
    void paintDecorationTile(class QListWidgetItem *item, const QImage &frame);
    void animateDecorationTile(int row);
    void profileEdited();
    bool profileDirty() const;
    void updateSaveBar();
    void saveProfile();
    void setStatus(QLabel *label, bool ok, const QString &text);
    void setProfileStatus(bool ok, const QString &text);
    void setAccountStatus(bool ok, const QString &text);

    // PATCH /users/@me, handling Discord's "two factor is required" by asking
    // for the code and sending the same change again.
    void editUser(const QJsonObject &fields, std::function<void(bool ok, const QString &problem)> done,
                  const QString &mfaToken = {}, const RestClient::CaptchaProof &captcha = {});
    void editProfileFields(const QJsonObject &fields, std::function<void(bool ok, const QString &problem)> done,
                           const RestClient::CaptchaProof &captcha = {});
    void editServer(const QJsonObject &fields, const QString &what, const RestClient::CaptchaProof &captcha = {});

    // Discord's captcha, shown for the person to solve when an answer asks
    // for one. True with `proof` filled when it was solved; false when there
    // was no demand (`*asked` false) or it was closed unsolved (`*asked` true).
    bool solveCaptcha(const RestClient::Error &error, RestClient::CaptchaProof *proof, bool *asked);

    // Asks for a picture and hands it back as a data URI, or empty.
    QString pickPicture(const QString &title, QByteArray *bytes);

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

    // My Account and Profiles pages.
    struct LoadedProfile
    {
        QString username;
        QString globalName;
        QString avatarHash;
        QString bannerHash;
        QString pronouns;
        QString bio;
        QString decorationSku;
        QString decorationAsset;
        int accent = -1;
    };
    // Changes picked but not saved. Text fields are compared to LoadedProfile
    // directly; pictures, colour and decoration are held here.
    struct PendingProfile
    {
        bool avatarTouched = false;
        QString avatarData;        // data URI; empty with avatarTouched = remove
        QByteArray avatarBytes;
        bool bannerTouched = false;
        QString bannerData;
        QByteArray bannerBytes;
        bool accentTouched = false;
        int accent = -1;
        bool decorationTouched = false;
        QVariantMap decoration;    // {id, sku, asset}; empty = none
    };
    LoadedProfile m_loaded;
    PendingProfile m_pending;
    bool m_userEdited = false;
    bool m_fillingProfile = false;
    bool m_fillingDecorations = false;
    int m_profilesRow = 1;

    ProfilePreview *m_accountCard = nullptr;
    ProfilePreview *m_preview = nullptr;
    QLabel *m_accountStatus = nullptr;
    QLabel *m_profileStatus = nullptr;
    QLabel *m_bioTitle = nullptr;
    QLabel *m_decorationHint = nullptr;
    QLineEdit *m_displayName = nullptr;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_pronouns = nullptr;
    QPlainTextEdit *m_bio = nullptr;
    QPushButton *m_accentButton = nullptr;
    QListWidget *m_decorations = nullptr;
    QComboBox *m_serverPick = nullptr;
    QLineEdit *m_nickname = nullptr;
    QFrame *m_saveBar = nullptr;
    QPushButton *m_saveButton = nullptr;

    // The one decoration tile that plays: hovered, or else the picked one.
    class AnimatedImage *m_tileAnimation = nullptr;
    QPixmap m_tileFace;
    int m_animatedTileRow = -1;
    QByteArray m_animatedTileBytes;
    int m_decorationHover = -1;

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
