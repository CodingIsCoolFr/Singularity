#pragma once

#include "core/Cosmetics.h"
#include "core/RestClient.h"

#include <QDialog>
#include <QHash>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QPair>
#include <QPixmap>
#include <QSet>
#include <QVariantMap>

#include <functional>

#include <QList>

class AudioMeter;
class ProfilePreview;
class QFrame;
class LevelBar;
class MessageStore;
class PluginHost;
class GameActivity;

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QStackedWidget;
class QVBoxLayout;

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

    void setGames(GameActivity *games) { m_games = games; }

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

    // The Singularity card switch on the Activity page. The voice-panel
    // button uses the same path.
    void activityShareChanged(bool on);

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
    void refreshCardBadges();   // both cards' badges, the unsaved order included
    void paintDecorationTile(class QListWidgetItem *item, const QImage &frame);
    void animateDecorationTile(int row);
    void profileEdited();
    bool profileDirty() const;
    void updateSaveBar();
    void saveProfile();

    // Discord's Edit Profile beyond the picture and the words: the name's
    // font, effect and colours, the nameplate, the profile effect, the
    // profile theme and the server tag. Picked here, shown on both cards and
    // in the pickers at once, sent with Save Changes.
    void addNameStyleEditor(QWidget *page, QVBoxLayout *left);
    void addNameplatePicker(QWidget *page, QVBoxLayout *left);
    void addEffectPicker(QWidget *page, QVBoxLayout *left);
    void addThemeEditor(QWidget *page, QVBoxLayout *left);
    void addTagPicker(QWidget *page, QVBoxLayout *left);
    void fillLookEditors();
    void fillTagPick();
    void loadServerTags();
    void showEffectDetails(const QString &sku);
    QIcon effectTileIcon(const QString &thumbnail) const;
    void setPendingNameStyle(const Cosmetics::NameStyle &style);
    void setPendingTheme(int primary, int accent);
    // What is picked now: the unsaved choice, or what Discord has.
    Cosmetics::NameStyle shownNameStyle() const;
    QString shownNameplateSku() const;
    QString shownEffectSku() const;
    QString shownTagGuildId() const;
    Cosmetics::GuildTag shownTag() const;
    QPair<int, int> shownTheme() const;
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
    QWidget *buildActivityPage();
    QWidget *buildNotificationsPage();
    // Speak Message: the natural voice, which voice, its speed and volume, and
    // reading new messages aloud.
    QWidget *buildAccessibilityPage();
    QWidget *buildAppearancePage();
    QWidget *buildPluginsPage();
    QWidget *buildAdvancedPage();

    // Pages other than My Account are built the first time they are shown.
    // Building Voice, Appearance and Plugins up front enumerated devices,
    // decoded the wallpaper GIF and created every plugin pane before the
    // window had painted, which is the hitch opening Settings used to be.
    void addSection(const QString &title, std::function<QWidget *()> builder);
    void ensurePage(int row);
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
    QList<std::function<QWidget *()>> m_pageBuilders;

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
        Cosmetics::NameStyle nameStyle;
        Cosmetics::Nameplate nameplate;
        Cosmetics::GuildTag tag;          // the server tag worn, if any
        QString effectSku;
        QJsonArray collectibles;          // user_profile.collectibles: {sku_id, type}
        int themePrimary = -1;            // theme_colors, -1 for none
        int themeAccent = -1;
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
        bool nameStyleTouched = false;
        Cosmetics::NameStyle nameStyle;   // not set = none
        bool nameplateTouched = false;
        QVariantMap nameplate;     // {sku, palette, label}; empty = none
        bool effectTouched = false;
        QString effectSku;         // empty = none
        bool themeTouched = false;
        int themePrimary = -1;     // both -1 = none
        int themeAccent = -1;
        bool tagTouched = false;
        QString tagGuildId;        // empty = none
    };
    LoadedProfile m_loaded;
    PendingProfile m_pending;
    bool m_userEdited = false;
    bool m_fillingProfile = false;
    bool m_fillingDecorations = false;
    int m_profilesRow = 1;

    ProfilePreview *m_accountCard = nullptr;
    // "Customize your badges", on My Account. Its edits wait for Save Changes
    // with the rest of the profile, as in Discord.
    class BadgeCustomizer *m_badgeCustomizer = nullptr;
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

    // The look editors.
    QComboBox *m_nameFont = nullptr;
    QComboBox *m_nameEffect = nullptr;
    QList<QPushButton *> m_nameColours;     // one per colour the effect takes
    QList<QPushButton *> m_namePresets;     // Discord's palettes for the effect
    QPushButton *m_removeNameStyle = nullptr;
    QListWidget *m_nameplates = nullptr;
    QLabel *m_nameplateHint = nullptr;
    QListWidget *m_effects = nullptr;
    QLabel *m_effectHint = nullptr;
    QPushButton *m_themePrimaryButton = nullptr;
    QPushButton *m_themeAccentButton = nullptr;
    QComboBox *m_tagPick = nullptr;
    bool m_fillingLooks = false;
    QHash<QString, Cosmetics::ProfileEffect> m_effectDetails;   // sku -> what it is made of
    QSet<QString> m_effectsAsked;
    QHash<QString, Cosmetics::GuildTag> m_serverTags;           // guild id -> its tag ({} while asked)

    // The one decoration tile that plays: hovered, or else the picked one.
    class AnimatedImage *m_tileAnimation = nullptr;
    QPixmap m_tileFace;
    int m_animatedTileRow = -1;
    QByteArray m_animatedTileBytes;
    int m_decorationHover = -1;

    // Your own pictures arriving redraw the previews once per burst.
    class QTimer *m_profileArtTimer = nullptr;

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
    GameActivity *m_games = nullptr;
};
