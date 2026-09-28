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

    // Gateway activities. Empty when the switch is off.
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
    };

    bool m_shown = true;
    bool m_detect = true;
    QList<Saved> m_saved;
    QList<Running> m_running;
    QJsonArray m_activities;
    QHash<QString, Known> m_catalog;          // exe basename, lower case
    QHash<QString, qint64> m_started;         // exe -> when we first saw it
    QString m_runningKey;
    QTimer m_timer;
    bool m_catalogAsked = false;
};
