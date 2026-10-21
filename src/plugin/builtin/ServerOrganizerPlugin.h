#pragma once

#include "plugin/Plugin.h"
#include "plugin/builtin/ServerCategories.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>

// Sorts the server rail into folders by what each server is: Social, Friends,
// Gaming, VR, Anime & Art, Music, Tech, Money & Crypto, Apps & Support,
// Communities, Other. ServerCategories does the deciding, from what the
// client knows plus two things fetched for it: Discord's list of games (the
// one the "Playing" status uses, already on disk) and each server's profile
// (GET /guilds/{id}/profile: its traits, linked games and game activity),
// read one at a time and kept for a week.
//
// Turning it on sorts everything at once. After that it only places servers
// it has not seen - one you just joined, or joined on another device - so a
// server you moved yourself stays where you put it. "Organize now" sorts
// everything again, and remembers your moves: a server you dragged from one
// of its folders into another is kept there from then on. Undo puts the rail
// back the way it was before the last sort.
//
// The folders it makes are ordinary Discord folders, saved to Discord like
// any other change, so the official app and your phone show them too.
// Folders you made yourself are left alone unless you say otherwise.
class ServerOrganizerPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("server-organizer"); }
    QString name() const override { return QStringLiteral("Server organizer"); }
    QString description() const override
    {
        return QStringLiteral("Sorts your servers into folders by what they are - Social, Gaming, VR, "
                              "Anime & Art, Music, Tech and more - as soon as you turn it on, and puts "
                              "each server you join into its folder. It reads each server's Discord "
                              "profile for the games it plays and the tags its admins gave it, knows "
                              "every game Discord does by name, and counts a server where people are "
                              "always talking in voice as Social. Folders you made yourself are left "
                              "alone, and Undo puts everything back.");
    }
    bool enabledByDefault() const override { return false; }

    void onLoad(PluginContext *context) override;
    void onUnload() override;
    void onServerRail(const QStringList &joinedGuildIds) override;
    QWidget *createSettingsWidget(QWidget *parent) override;

    // For the settings panel.
    struct Row
    {
        QString guildId;
        QString name;
        QString autoCategory;   // what it would be left to itself
        QString reason;
        QString chosen;         // what you picked for it, empty for automatic
    };
    bool running() const;
    QList<Row> rows();
    void organizeAll();
    void undo();
    bool canUndo() const;
    void choose(const QString &guildId, const QString &categoryId);
    QString status() const { return m_status; }

    // The panel's two switches: "sortNew" and "leaveMine", both on unless
    // turned off.
    bool option(const QString &key) const { return settingValue(key, true).toBool(); }
    void setOption(const QString &key, bool on) { setSettingValue(key, on); }

private:
    ServerCategories::Verdict verdictFor(const QString &guildId);
    void sampleVoice();
    void organizeNow();
    void sortNewServers();
    void refreshPanel();

    // Discord's games, by application id, from the list the "Playing" status
    // is matched against. Read again while it is missing or changes.
    void loadGameCatalog();
    QHash<QString, QString> m_gameNames;
    qint64 m_catalogStamp = -1;

    // Server profiles, fetched one at a time - a second apart, waiting out
    // any rate limit - and kept for a week (a day when Discord refused).
    // `then` runs once every one asked for is in.
    void ensureProfiles(const QStringList &guildIds, std::function<void()> then);
    void fetchNextProfile();
    QHash<QString, QJsonObject> m_profiles;   // guild id -> {traits, games, names, activity, description, at}
    QStringList m_profileQueue;
    int m_profileTotal = 0;
    QList<std::function<void()>> m_afterProfiles;
    bool m_fetchingProfiles = false;

    QString accountKey(const QString &what) const;
    QJsonObject loadObject(const QString &key) const;
    void saveObject(const QString &key, const QJsonObject &object);

    static QJsonArray railToJson(const QList<RailLayout::Entry> &entries);
    static QList<RailLayout::Entry> railFromJson(const QJsonArray &array);

    // `log` false for progress lines, which would be forty in a row.
    void setStatus(const QString &text, bool log = true);

    std::unique_ptr<QTimer> m_voiceTimer;
    QHash<QString, double> m_usualVoice;   // guild id -> people usually in voice
    QString m_status;
    QPointer<QWidget> m_panel;
};
