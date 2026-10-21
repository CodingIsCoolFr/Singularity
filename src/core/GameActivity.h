#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

class RestClient;

// Games other people should see, beside the Singularity card.
//
// A game Discord already knows is noticed by its program name. One the person
// adds is noticed the same way, or shown all the time if they left the program
// blank. Either can have its own line under the name.
class GameActivity : public QObject
{
    Q_OBJECT

public:
    struct Saved
    {
        QString name;
        QString exe;       // empty means show this even when nothing is running
        QString details;
        bool enabled = true;
    };

    struct Running
    {
        QString title;
        QString exe;
        bool known = false;
        quint32 pid = 0;
    };

    explicit GameActivity(QObject *parent = nullptr);

    void start(RestClient *rest);

    bool shown() const { return m_shown; }
    void setShown(bool on);
    bool detect() const { return m_detect; }
    void setDetect(bool on);

    void refresh();

    QList<Saved> saved() const { return m_saved; }
    void addSaved(const Saved &game);
    void updateSaved(int index, const Saved &game);
    void removeSaved(int index);

    // Programs with a window, for the Add list. Not the background services.
    QList<Running> running() const { return m_running; }

    // What is on the presence right now, as a short line each.
    QStringList showing() const;

    // True when this program should get a card: the switch is on, and they
    // added it, or automatic detection is on and Discord already knows it.
    bool wantsExe(const QString &exe) const;

    // Gateway activities. Empty when the switch is off. `_exe` is the program
    // file name, for the caller to match against a game's own activity and
    // then remove before sending.
    QJsonArray activities() const { return m_activities; }

signals:
    void changed();

private:
    void loadSaved();
    void storeSaved();
    void loadCatalog();
    void fetchCatalog(RestClient *rest);
    void applyCatalog(const QJsonArray &apps);
    void scan();
    void rebuild();

    struct Known
    {
        QString name;
        QString id;
        QString path;    // Discord's name for the program, folders and all, lower case with '/'
        QString args;    // what its command line must contain, lower case; usually empty
        bool exact = false;   // ">name" in Discord's list: the file name alone, any folder
    };

    // The game a running program is, by Discord's own rule: its full path
    // ends with the name in Discord's list, folders included. 192 games run a
    // "game.exe" and 34 an "hl2.exe"; the file name alone credited all of
    // them to whichever came first in the list. A few are told apart only by
    // their command line - Garry's Mod, Team Fortress 2, Left 4 Dead 2 and
    // Half-Life 2 by "-game ...", Minecraft by its main class in javaw.exe -
    // which is read from the process `pid` when one of those is the question.
    Known match(const QString &fullPath, quint32 pid = 0) const;

    bool m_shown = false;
    bool m_detect = true;
    QList<Saved> m_saved;
    QList<Running> m_running;
    QJsonArray m_activities;
    QHash<QString, QList<Known>> m_catalog;   // by file name, lower case
    QHash<QString, qint64> m_started;         // exe -> when we first saw it
    QString m_runningKey;
    QTimer m_timer;
    bool m_catalogAsked = false;
};
