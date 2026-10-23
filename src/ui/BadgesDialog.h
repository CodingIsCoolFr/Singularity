#pragma once

#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QPointer>
#include <QUrl>

class FlowLayout;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QPushButton;
class QHBoxLayout;
class QVBoxLayout;
class QWidget;
class RestClient;

// Discord's badges window (2026): every badge a person has, the ones you can
// still earn, and for each one its tier, when it was unlocked and how far it
// is to the next tier - Account Age, Game Time, Streaming and Game Variety
// level up; Nitro, Server Boosting, Quests, Orbs and the rest are as they
// always were. For yourself, "Customize your badges" sets the order they show
// in on your profile and which ones are hidden, saved to Discord so every
// client shows the same.
//
// Everything comes from Discord's badge directory (RestClient::fetchUserBadges)
// and the pictures from Discord's own servers.
class BadgesDialog : public QDialog
{
    Q_OBJECT

public:
    // Opens the window for one person, on `initialBadgeId` when it is not 0.
    // A badge clicked on a profile that badgeIdFromProfile does not know - one
    // Discord has added since - is opened on by its picture, `iconHash`.
    // Closes itself when done.
    static void open(RestClient *rest, const QString &userId, const QString &name, bool isSelf, int initialBadgeId,
                     QWidget *parent, const QString &iconHash = QString());

    // The directory's number for the id a badge has on a profile ("premium",
    // "guild_booster_lvl3", "account_age", ...), or 0 for one it does not know.
    static int badgeIdFromProfile(const QString &profileBadgeId);

    // A profile badge's picture.
    static QUrl profileBadgeIconUrl(const QString &iconHash);

    // The picture of a badge from Discord's directory, at the tier it shows:
    // the one you have, or the first one you could get.
    static QUrl directoryIconUrl(const QJsonObject &badge);
    // Its name, with the tier's after it ("Account Age - Sapling").
    static QString directoryName(const QJsonObject &badge);

    // Discord's badge numbers that are used by name here.
    enum : int {
        PremiumTenure = 1,
        AprilFools2026 = 15,
        AccountAge = 18,
        Streaming = 19,
        GameTime = 20,
        GameVariety = 21,
        Staff = 22,
    };

private:
    BadgesDialog(RestClient *rest, const QString &userId, const QString &name, bool isSelf, int initialBadgeId,
                 QWidget *parent);
    QString m_initialIcon;   // the picture to open on, for a badge known only by it

    // Asks Discord for the directory. `notice` replaces the loading line once
    // it is in, so "Saved" survives the reload a save causes.
    void load(const QString &notice = QString());
    // When the directory is refused: the badges on the profile instead.
    void loadFromProfile(int directoryStatus);
    void rebuildGrids();
    void select(int badgeId);
    void showDetail();
    void fetchProgress(int badgeId);
    void refreshIcons();
    void openCustomize();
    void saveCustomization(const QList<int> &order, const QList<int> &hidden);

    QListWidget *makeGrid();
    void fillGrid(QListWidget *grid, const QList<int> &badgeIds);

    RestClient *m_rest = nullptr;
    QString m_userId;
    QString m_name;
    bool m_isSelf = false;
    int m_selected = 0;
    bool m_loaded = false;

    QList<int> m_order;                 // as Discord listed them, which is your profile's order
    QHash<int, QJsonObject> m_badges;   // badge id -> the directory's entry, with progress once fetched
    QList<int> m_progressAsked;

    QLabel *m_status = nullptr;
    QLabel *m_ownedTitle = nullptr;
    QListWidget *m_ownedGrid = nullptr;
    QLabel *m_earnableTitle = nullptr;
    QListWidget *m_earnableGrid = nullptr;
    QPushButton *m_customize = nullptr;

    // The detail card.
    QWidget *m_detail = nullptr;
    QLabel *m_beta = nullptr;
    QLabel *m_bigIcon = nullptr;
    QLabel *m_eyebrow = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    QLabel *m_description = nullptr;
    QWidget *m_progressCard = nullptr;
    QLabel *m_progressTitle = nullptr;
    QLabel *m_progressFrom = nullptr;
    QLabel *m_progressNext = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QLabel *m_progressText = nullptr;
    QLabel *m_progressTo = nullptr;
    QLabel *m_instructions = nullptr;
    // Wraps: Account Age alone has nine tiers, and in one row they pushed the
    // whole card wider than the window, cutting off its text on the right.
    QWidget *m_tierHost = nullptr;
    FlowLayout *m_tierRow = nullptr;

    // Pictures on screen that may still be downloading. Drawn again when the
    // cache says one has landed; a label rebuilt since is simply gone.
    struct PendingIcon
    {
        QPointer<QLabel> label;
        QUrl url;
        int size = 0;
        bool dim = false;
    };
    QList<PendingIcon> m_pendingIcons;
    void setIcon(QLabel *label, const QUrl &url, int size, bool dim);
};
