#pragma once

#include <QFrame>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QUrl>

#include <functional>

class QLabel;
class QListWidget;
class RestClient;

// Discord's "Customize your badges" card on My Account (2026): your badges in
// the order your profile shows them, then the hidden ones, crossed out. Drag a
// badge to move it; click one to hide it, or to show it again. Like every other
// profile edit, the change waits for Save Changes and then goes to Discord
// (PATCH /users/@me/badges/settings), so every client shows the same.
//
// Discord's rules, read from its web client: Staff always comes first and
// stays there; Staff and Nitro cannot be hidden; a badge you hide goes to the
// end, and one you show again joins the shown ones last. A profile shows six
// badges and a "+N" for the rest.
class BadgeCustomizer : public QFrame
{
    Q_OBJECT

public:
    explicit BadgeCustomizer(RestClient *rest, QWidget *parent = nullptr);

    // Asks Discord for your badges, dropping any edit.
    void load();
    // Fills the card from Discord's answer (GET /users/@me/badges).
    void applyDirectory(const QJsonObject &payload);
    // Back to what Discord has.
    void reset();
    bool isDirty() const;

    // Sends the edit. `done` hears whether Discord took it; nothing to send
    // counts as taken.
    void save(std::function<void(bool ok, const QString &problem)> done);

    // Your badges as your profile would show them with the edit in.
    QList<QUrl> shownIcons() const;
    QStringList shownNames() const;

    // How many a profile shows before "+N", Discord's number.
    static constexpr int ProfileShows = 6;

signals:
    // What the card shows changed: an edit, Discord's answer, or a picture
    // arriving.
    void changed();
    void viewBadgesRequested();

protected:
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuild();
    void readOrderFromGrid();
    void fitGridHeight();
    void toggleHidden(int badgeId);
    QPixmap tilePicture(int badgeId, bool hidden) const;

    RestClient *m_rest = nullptr;
    QListWidget *m_grid = nullptr;
    QLabel *m_status = nullptr;

    QHash<int, QJsonObject> m_badges;   // the badges you own, by id
    QList<int> m_fixed;                 // Staff: first, always
    QList<int> m_savedShown;            // as Discord has them
    QSet<int> m_savedHidden;
    QList<int> m_shown;                 // with the edit in
    QList<int> m_hidden;                // hidden, in the order they were hidden
    QSet<QString> m_iconUrls;

    bool m_loaded = false;
    bool m_loading = false;
    bool m_rebuilding = false;
    int m_loadRequest = 0;
};
