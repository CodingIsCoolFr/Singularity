#include "plugin/builtin/ServerOrganizerPlugin.h"

#include "core/Logger.h"
#include "core/RestClient.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace {

// How often voice is looked at, and how far each look moves the average. A
// server needs people in voice across a couple of hours, not one busy
// moment, to read as a hangout.
constexpr int VoiceSampleMs = 5 * 60 * 1000;
constexpr double VoiceWeight = 0.2;

// After signing in, long enough for READY_SUPPLEMENTAL, which is where most
// voice states arrive, so a new server is judged with them.
constexpr int SettleMs = 4000;

// Server profiles: one request a second, the way a person opening them would,
// kept a week, and a day when Discord would not give one.
constexpr int ProfileGapMs = 1100;
constexpr qint64 ProfileKeepSecs = 7 * 24 * 3600;
constexpr qint64 ProfileRetrySecs = 24 * 3600;

// Takes a server out of wherever it sits. A folder left empty goes too, as
// it does in Discord.
void takeOut(QList<RailLayout::Entry> &entries, const QString &guildId)
{
    for (qsizetype row = entries.size() - 1; row >= 0; --row) {
        RailLayout::Entry &entry = entries[row];
        if (entry.isFolder) {
            entry.folder.guildIds.removeAll(guildId);
            if (entry.folder.guildIds.isEmpty())
                entries.removeAt(row);
        } else if (entry.guildId == guildId) {
            entries.removeAt(row);
        }
    }
}

// Puts a server at the end of the organizer's folder for `category`, making
// that folder at the end of the rail when there is none yet.
void putInFolder(QList<RailLayout::Entry> &entries, QJsonObject &folders, const QString &guildId,
                 const QString &category)
{
    const QString folderId = folders.value(category).toString();
    if (!folderId.isEmpty()) {
        for (RailLayout::Entry &entry : entries) {
            if (entry.isFolder && entry.folder.id == folderId) {
                entry.folder.guildIds.append(guildId);
                return;
            }
        }
    }

    const ServerCategories::Category *info = ServerCategories::find(category);
    RailLayout::Entry entry;
    entry.isFolder = true;
    entry.folder.id = RailLayout::newFolderId();
    entry.folder.name = info ? info->name : category;
    entry.folder.hasColor = info != nullptr;
    entry.folder.color = info ? info->color : 0;
    entry.folder.open = false;
    entry.folder.guildIds = QStringList{guildId};
    entries.append(entry);
    folders.insert(category, entry.folder.id);
}

QString categoryName(const QString &id)
{
    const ServerCategories::Category *category = ServerCategories::find(id);
    return category ? category->name : id;
}

class OrganizerPanel : public QWidget
{
public:
    OrganizerPanel(ServerOrganizerPlugin *plugin, QWidget *parent);
    void refresh();

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        refresh();
    }

private:
    ServerOrganizerPlugin *m_plugin = nullptr;
    QPushButton *m_organize = nullptr;
    QPushButton *m_undo = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_off = nullptr;
    QTableWidget *m_table = nullptr;
};

OrganizerPanel::OrganizerPanel(ServerOrganizerPlugin *plugin, QWidget *parent)
    : QWidget(parent)
    , m_plugin(plugin)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    const auto addSwitch = [&](const QString &key, const QString &label) {
        auto *box = new QCheckBox(label, this);
        box->setChecked(plugin->option(key));
        connect(box, &QCheckBox::toggled, this, [plugin, key](bool on) { plugin->setOption(key, on); });
        layout->addWidget(box);
    };
    addSwitch(QStringLiteral("sortNew"), QStringLiteral("Put servers you join into their folder"));
    addSwitch(QStringLiteral("leaveMine"), QStringLiteral("Leave folders you made yourself alone"));

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    m_organize = new QPushButton(QStringLiteral("Organize now"), this);
    m_undo = new QPushButton(QStringLiteral("Undo"), this);
    m_undo->setToolTip(QStringLiteral("Put the servers back the way they were before the last sort"));
    buttons->addWidget(m_organize);
    buttons->addWidget(m_undo);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    connect(m_organize, &QPushButton::clicked, this, [plugin]() { plugin->organizeAll(); });
    connect(m_undo, &QPushButton::clicked, this, [plugin]() { plugin->undo(); });

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));
    layout->addWidget(m_status);

    m_off = new QLabel(QStringLiteral("Turn the organizer on and it sorts your servers straight away. "
                                      "Each server and the folder it goes in are listed here then."),
                       this);
    m_off->setWordWrap(true);
    m_off->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::TextMuted)));
    layout->addWidget(m_off);

    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Server"), QStringLiteral("Folder")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->hide();
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setFixedHeight(300);
    layout->addWidget(m_table);

    auto *note = new QLabel(
        QStringLiteral("Pick a folder for a server and it moves there straight away, and stays there "
                       "whenever the organizer sorts again. Dragging a server into another of its "
                       "folders counts the same. Social is partly judged by how many people are "
                       "usually in voice, which the organizer keeps counting while it is on, so its "
                       "guesses get better over a day or two."),
        this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(QLatin1String(Theme::TextFaint)));
    layout->addWidget(note);
}

void OrganizerPanel::refresh()
{
    const bool on = m_plugin->running();
    m_organize->setEnabled(on);
    m_undo->setEnabled(on && m_plugin->canUndo());
    m_status->setText(m_plugin->status());
    m_status->setVisible(!m_plugin->status().isEmpty());
    m_off->setVisible(!on);
    m_table->setVisible(on);
    if (!on)
        return;

    const QList<ServerOrganizerPlugin::Row> rows = m_plugin->rows();
    m_table->setRowCount(int(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        const ServerOrganizerPlugin::Row &row = rows.at(i);

        auto *name = new QTableWidgetItem(row.name.isEmpty() ? row.guildId : row.name);
        const QString automatic = categoryName(row.autoCategory);
        name->setToolTip(row.reason.isEmpty() ? automatic
                                              : QStringLiteral("%1: %2").arg(automatic, row.reason));
        m_table->setItem(i, 0, name);

        auto *folder = new QComboBox;
        folder->addItem(QStringLiteral("Automatic (%1)").arg(automatic), QString());
        for (const ServerCategories::Category &category : ServerCategories::all())
            folder->addItem(category.name, category.id);
        const int current = row.chosen.isEmpty() ? 0 : folder->findData(row.chosen);
        folder->setCurrentIndex(current < 0 ? 0 : current);

        const QString guildId = row.guildId;
        connect(folder, &QComboBox::currentIndexChanged, this, [this, folder, guildId]() {
            m_plugin->choose(guildId, folder->currentData().toString());
        });
        m_table->setCellWidget(i, 1, folder);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ServerOrganizerPlugin::onLoad(PluginContext *context)
{
    Plugin::onLoad(context);

    // Kept between runs, so a server's usual crowd is not forgotten.
    m_usualVoice.clear();
    const QJsonObject saved = loadObject(QStringLiteral("voice"));
    for (auto it = saved.constBegin(); it != saved.constEnd(); ++it)
        m_usualVoice.insert(it.key(), it.value().toDouble());

    m_profiles.clear();
    const QJsonObject profiles = loadObject(QStringLiteral("profiles"));
    for (auto it = profiles.constBegin(); it != profiles.constEnd(); ++it)
        m_profiles.insert(it.key(), it.value().toObject());
    loadGameCatalog();

    m_voiceTimer = std::make_unique<QTimer>();
    m_voiceTimer->setInterval(VoiceSampleMs);
    QObject::connect(m_voiceTimer.get(), &QTimer::timeout, [this]() { sampleVoice(); });
    m_voiceTimer->start();

    // Switched on while connected: sort now. At start-up the rail is not
    // ready yet, and onServerRail takes over once it is.
    if (context->serverRailReady()) {
        QTimer::singleShot(0, [this]() {
            if (running())
                organizeAll();
        });
    }
    refreshPanel();
}

void ServerOrganizerPlugin::onUnload()
{
    m_voiceTimer.reset();
    m_profileQueue.clear();
    m_afterProfiles.clear();
    m_fetchingProfiles = false;
    Plugin::onUnload();
    refreshPanel();
}

void ServerOrganizerPlugin::onServerRail(const QStringList &joinedGuildIds)
{
    // A server just joined has its voice states with it. At sign-in they
    // come a moment after the rail.
    QTimer::singleShot(joinedGuildIds.isEmpty() ? SettleMs : 0, [this]() {
        if (!running())
            return;
        sampleVoice();
        sortNewServers();
        refreshPanel();
    });
}

bool ServerOrganizerPlugin::running() const
{
    return context() && context()->store() && context()->serverRailReady();
}

// ---------------------------------------------------------------------------
// Deciding
// ---------------------------------------------------------------------------

ServerCategories::Verdict ServerOrganizerPlugin::verdictFor(const QString &guildId)
{
    if (ServerCategories::knownGameCount() == 0)
        loadGameCatalog();

    MessageStore *store = context()->store();
    const GuildInfo guild = store->guild(guildId);
    const QJsonObject profile = m_profiles.value(guildId);

    ServerCategories::Facts facts;
    facts.name = guild.name;
    facts.description = guild.description.isEmpty() ? profile.value(QStringLiteral("description")).toString()
                                                    : guild.description;
    facts.features = guild.features;
    for (const QJsonValue &trait : profile.value(QStringLiteral("traits")).toArray())
        facts.traits.append(trait.toString());
    // Games by id, named from Discord's list; the profile's own names for any
    // the list does not have; and one that is neither is still a game.
    const QJsonObject names = profile.value(QStringLiteral("names")).toObject();
    for (const QJsonValue &id : profile.value(QStringLiteral("games")).toArray()) {
        QString name = m_gameNames.value(id.toString());
        if (name.isEmpty())
            name = names.value(id.toString()).toString();
        facts.linkedGames.append(name.isEmpty() ? QStringLiteral("a game") : name);
    }
    facts.gameActivity = profile.value(QStringLiteral("activity")).toInt();
    facts.memberCount = std::max(guild.memberCount, store->memberList(guildId).memberCount);
    for (const QString &channelId : guild.channelIds) {
        const ChannelInfo channel = store->channel(channelId);
        if (channel.isCategory()) {
            facts.categoryNames.append(channel.name);
        } else {
            facts.channelNames.append(channel.name);
            ++facts.channelCount;
        }
    }
    for (const RoleInfo &role : guild.roles)
        facts.roleNames.append(role.name);
    facts.usualVoice = m_usualVoice.value(guildId);
    return ServerCategories::classify(facts);
}

void ServerOrganizerPlugin::sampleVoice()
{
    if (!context() || !context()->store())
        return;
    const QList<GuildInfo> guilds = context()->store()->guilds();
    if (guilds.isEmpty())
        return;   // not signed in yet: nothing to count, and zeros would drag every average down

    const QHash<QString, int> now = context()->store()->voiceCountsByGuild();
    QJsonObject saved;
    for (const GuildInfo &guild : guilds) {
        const double count = now.value(guild.id);
        const auto known = m_usualVoice.constFind(guild.id);
        const double usual = known == m_usualVoice.constEnd() ? count : *known + VoiceWeight * (count - *known);
        m_usualVoice.insert(guild.id, usual);
        saved.insert(guild.id, std::round(usual * 100.0) / 100.0);
    }
    saveObject(QStringLiteral("voice"), saved);
}

QList<ServerOrganizerPlugin::Row> ServerOrganizerPlugin::rows()
{
    if (!running())
        return {};

    const QJsonObject chosen = loadObject(QStringLiteral("overrides"));
    QList<Row> out;
    for (const RailLayout::Entry &entry : context()->serverRail()) {
        const QStringList ids = entry.isFolder ? entry.folder.guildIds : QStringList{entry.guildId};
        for (const QString &guildId : ids) {
            const ServerCategories::Verdict verdict = verdictFor(guildId);
            Row row;
            row.guildId = guildId;
            row.name = context()->store()->guild(guildId).name;
            row.autoCategory = verdict.categoryId;
            row.reason = verdict.reason;
            row.chosen = chosen.value(guildId).toString();
            out.append(row);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Sorting
// ---------------------------------------------------------------------------

void ServerOrganizerPlugin::organizeAll()
{
    if (!running()) {
        setStatus(QStringLiteral("Not connected yet. Try again once your servers have loaded."));
        return;
    }
    // Every server's profile first, so the sort sees the games and tags.
    QStringList guildIds;
    for (const GuildInfo &guild : context()->store()->guilds())
        guildIds.append(guild.id);
    ensureProfiles(guildIds, [this]() {
        if (running())
            organizeNow();
    });
}

void ServerOrganizerPlugin::organizeNow()
{
    if (!running())
        return;
    sampleVoice();

    const QList<RailLayout::Entry> current = context()->serverRail();
    QJsonObject folders = loadObject(accountKey(QStringLiteral("folders")));
    QJsonObject placed = loadObject(accountKey(QStringLiteral("placed")));
    QJsonObject chosen = loadObject(QStringLiteral("overrides"));
    const bool leaveMine = option(QStringLiteral("leaveMine"));

    // The folders this made that are still on the rail, by id.
    QHash<QString, QString> ours;
    for (auto it = folders.constBegin(); it != folders.constEnd(); ++it)
        ours.insert(it.value().toString(), it.key());

    QList<RailLayout::Entry> arranged;              // your own folders first, as they are
    QHash<QString, QStringList> members;            // category -> servers, in rail order
    QHash<QString, RailLayout::Folder> existing;    // category -> its folder, kept with your name and colour
    int keptServers = 0;
    int remembered = 0;

    const auto assign = [&](const QString &guildId, const QString &sittingIn) {
        QString category = chosen.value(guildId).toString();
        // In one of its folders, but not the one it was put in: moved there
        // by hand. That is an answer, and it sticks.
        if (category.isEmpty() && !sittingIn.isEmpty() && placed.contains(guildId)
            && placed.value(guildId).toString() != sittingIn) {
            category = sittingIn;
            chosen.insert(guildId, category);
            ++remembered;
        }
        if (!ServerCategories::find(category))
            category = verdictFor(guildId).categoryId;
        members[category].append(guildId);
        placed.insert(guildId, category);
    };

    for (const RailLayout::Entry &entry : current) {
        if (!entry.isFolder) {
            assign(entry.guildId, QString());
            continue;
        }
        const QString category = ours.value(entry.folder.id);
        if (!category.isEmpty()) {
            existing.insert(category, entry.folder);
            for (const QString &guildId : entry.folder.guildIds)
                assign(guildId, category);
        } else if (leaveMine) {
            arranged.append(entry);
            for (const QString &guildId : entry.folder.guildIds)
                placed.insert(guildId, QString());
            keptServers += int(entry.folder.guildIds.size());
        } else {
            for (const QString &guildId : entry.folder.guildIds)
                assign(guildId, QString());
        }
    }

    QJsonObject madeFolders;
    QStringList summary;
    int sorted = 0;
    for (const ServerCategories::Category &category : ServerCategories::all()) {
        const QStringList ids = members.value(category.id);
        if (ids.isEmpty())
            continue;
        RailLayout::Entry entry;
        entry.isFolder = true;
        if (existing.contains(category.id)) {
            entry.folder = existing.value(category.id);
        } else {
            entry.folder.id = RailLayout::newFolderId();
            entry.folder.name = category.name;
            entry.folder.hasColor = true;
            entry.folder.color = category.color;
            entry.folder.open = false;
        }
        entry.folder.guildIds = ids;
        arranged.append(entry);
        madeFolders.insert(category.id, entry.folder.id);
        summary << QStringLiteral("%1 %2").arg(ids.size()).arg(category.name);
        sorted += int(ids.size());
    }

    saveObject(accountKey(QStringLiteral("folders")), madeFolders);
    saveObject(accountKey(QStringLiteral("placed")), placed);
    saveObject(QStringLiteral("overrides"), chosen);

    QString text = sorted > 0 ? QStringLiteral("Sorted %1 servers: %2.").arg(sorted).arg(summary.join(QStringLiteral(", ")))
                              : QStringLiteral("Nothing to sort.");
    if (keptServers > 0)
        text += QStringLiteral(" Left %1 in folders you made; untick \"Leave folders you made yourself "
                               "alone\" to sort those too.")
                    .arg(keptServers);
    if (remembered > 0)
        text += QStringLiteral(" Kept %1 where you moved them.").arg(remembered);

    if (RailLayout::sameArrangement(current, arranged)) {
        setStatus(QStringLiteral("Already sorted. ") + text);
        return;
    }

    setSettingValue(accountKey(QStringLiteral("undo")),
                    QString::fromUtf8(QJsonDocument(railToJson(current)).toJson(QJsonDocument::Compact)));
    context()->setServerRail(arranged, QStringLiteral("sorted %1 servers into %2 folders")
                                           .arg(sorted)
                                           .arg(summary.size()));
    setStatus(text);
}

void ServerOrganizerPlugin::sortNewServers()
{
    if (!running())
        return;

    QList<RailLayout::Entry> entries = context()->serverRail();
    QJsonObject folders = loadObject(accountKey(QStringLiteral("folders")));
    QJsonObject placed = loadObject(accountKey(QStringLiteral("placed")));
    const QJsonObject chosen = loadObject(QStringLiteral("overrides"));

    QHash<QString, QString> ours;
    for (auto it = folders.constBegin(); it != folders.constEnd(); ++it)
        ours.insert(it.value().toString(), it.key());

    // A server already in a folder - put there by you, or on another device
    // - stays there. Only loose ones it has never seen are sorted.
    QStringList fresh;
    for (const RailLayout::Entry &entry : std::as_const(entries)) {
        if (!entry.isFolder) {
            if (!placed.contains(entry.guildId))
                fresh.append(entry.guildId);
            continue;
        }
        for (const QString &guildId : entry.folder.guildIds) {
            if (!placed.contains(guildId))
                placed.insert(guildId, ours.value(entry.folder.id));
        }
    }

    // With sorting of new servers off they are still remembered, so turning
    // it back on later does not sweep up servers you have had for months.
    if (fresh.isEmpty() || !option(QStringLiteral("sortNew"))) {
        for (const QString &guildId : std::as_const(fresh))
            placed.insert(guildId, QString());
        saveObject(accountKey(QStringLiteral("placed")), placed);
        return;
    }

    // Their profiles first; this runs again once they are in, and finds
    // them there.
    for (const QString &guildId : std::as_const(fresh)) {
        if (!m_profiles.contains(guildId)) {
            ensureProfiles(fresh, [this]() { sortNewServers(); });
            return;
        }
    }

    QStringList moves;
    for (const QString &guildId : std::as_const(fresh)) {
        QString category = chosen.value(guildId).toString();
        if (!ServerCategories::find(category))
            category = verdictFor(guildId).categoryId;
        takeOut(entries, guildId);
        putInFolder(entries, folders, guildId, category);
        placed.insert(guildId, category);
        moves << QStringLiteral("%1 to %2").arg(context()->store()->guild(guildId).name, categoryName(category));
    }

    saveObject(accountKey(QStringLiteral("folders")), folders);
    saveObject(accountKey(QStringLiteral("placed")), placed);
    context()->setServerRail(entries, QStringLiteral("put %1 new server(s) in folders").arg(fresh.size()));
    setStatus(QStringLiteral("Put %1.").arg(moves.join(QStringLiteral(", "))));
}

void ServerOrganizerPlugin::choose(const QString &guildId, const QString &categoryId)
{
    QJsonObject chosen = loadObject(QStringLiteral("overrides"));
    if (categoryId.isEmpty())
        chosen.remove(guildId);
    else
        chosen.insert(guildId, categoryId);
    saveObject(QStringLiteral("overrides"), chosen);
    if (!running())
        return;

    const QString category = categoryId.isEmpty() ? verdictFor(guildId).categoryId : categoryId;
    QList<RailLayout::Entry> entries = context()->serverRail();
    QJsonObject folders = loadObject(accountKey(QStringLiteral("folders")));
    QJsonObject placed = loadObject(accountKey(QStringLiteral("placed")));

    // Already there: nothing to move.
    const QString folderId = folders.value(category).toString();
    for (const RailLayout::Entry &entry : std::as_const(entries)) {
        if (entry.isFolder && !folderId.isEmpty() && entry.folder.id == folderId
            && entry.folder.guildIds.contains(guildId)) {
            placed.insert(guildId, category);
            saveObject(accountKey(QStringLiteral("placed")), placed);
            refreshPanel();
            return;
        }
    }

    takeOut(entries, guildId);
    putInFolder(entries, folders, guildId, category);
    placed.insert(guildId, category);
    saveObject(accountKey(QStringLiteral("folders")), folders);
    saveObject(accountKey(QStringLiteral("placed")), placed);

    const QString name = context()->store()->guild(guildId).name;
    context()->setServerRail(entries, QStringLiteral("moved %1 to %2").arg(name, categoryName(category)));
    setStatus(QStringLiteral("Moved %1 to %2.").arg(name, categoryName(category)));
}

void ServerOrganizerPlugin::undo()
{
    if (!running())
        return;
    const QString key = accountKey(QStringLiteral("undo"));
    const QJsonArray saved = QJsonDocument::fromJson(settingValue(key).toString().toUtf8()).array();
    if (saved.isEmpty()) {
        setStatus(QStringLiteral("Nothing to undo."));
        return;
    }
    // One step: an undo is not itself undone.
    setSettingValue(key, QString());
    context()->setServerRail(railFromJson(saved), QStringLiteral("put the rail back as it was before sorting"));
    setStatus(QStringLiteral("Put your servers back the way they were before the last sort."));
}

bool ServerOrganizerPlugin::canUndo() const
{
    return running() && !settingValue(accountKey(QStringLiteral("undo"))).toString().isEmpty();
}

// ---------------------------------------------------------------------------
// What is fetched for the deciding
// ---------------------------------------------------------------------------

void ServerOrganizerPlugin::loadGameCatalog()
{
    // The file the "Playing" status is matched against: {"game.exe": {"n":
    // name, "id": application id}}, written by GameActivity a moment after
    // start and refreshed by it now and then.
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/detectable-games.json");
    const QFileInfo info(path);
    const qint64 stamp = info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0;
    if (stamp == m_catalogStamp)
        return;
    m_catalogStamp = stamp;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject catalog = QJsonDocument::fromJson(file.readAll()).object();
    m_gameNames.clear();
    QStringList names;
    for (auto it = catalog.constBegin(); it != catalog.constEnd(); ++it) {
        const QJsonObject game = it.value().toObject();
        const QString name = game.value(QStringLiteral("n")).toString();
        const QString id = game.value(QStringLiteral("id")).toString();
        if (name.isEmpty())
            continue;
        names.append(name);
        if (!id.isEmpty())
            m_gameNames.insert(id, name);
    }
    ServerCategories::setKnownGames(names);
    wlog(QStringLiteral("organizer"), QStringLiteral("knows %1 games by name").arg(ServerCategories::knownGameCount()));
}

void ServerOrganizerPlugin::ensureProfiles(const QStringList &guildIds, std::function<void()> then)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const QString &guildId : guildIds) {
        const QJsonObject profile = m_profiles.value(guildId);
        const qint64 at = qint64(profile.value(QStringLiteral("at")).toDouble());
        const qint64 keep = profile.value(QStringLiteral("failed")).toBool() ? ProfileRetrySecs : ProfileKeepSecs;
        if (!profile.isEmpty() && now - at < keep)
            continue;
        if (!m_profileQueue.contains(guildId))
            m_profileQueue.append(guildId);
    }
    m_afterProfiles.append(std::move(then));
    if (m_fetchingProfiles)
        return;   // the run under way picks these up and then calls everyone back
    m_profileTotal = int(m_profileQueue.size());
    if (m_profileTotal == 0) {
        const QList<std::function<void()>> waiting = std::exchange(m_afterProfiles, {});
        for (const auto &callback : waiting)
            callback();
        return;
    }
    m_fetchingProfiles = true;
    fetchNextProfile();
}

void ServerOrganizerPlugin::fetchNextProfile()
{
    if (!running()) {
        m_fetchingProfiles = false;
        m_profileQueue.clear();
        m_afterProfiles.clear();
        return;
    }
    if (m_profileQueue.isEmpty()) {
        m_fetchingProfiles = false;
        QJsonObject saved;
        for (auto it = m_profiles.constBegin(); it != m_profiles.constEnd(); ++it)
            saved.insert(it.key(), it.value());
        saveObject(QStringLiteral("profiles"), saved);
        const QList<std::function<void()>> waiting = std::exchange(m_afterProfiles, {});
        for (const auto &callback : waiting)
            callback();
        return;
    }

    const QString guildId = m_profileQueue.first();
    const int done = m_profileTotal - int(m_profileQueue.size());
    setStatus(QStringLiteral("Reading your servers' profiles for the games they play and their tags (%1 of %2)...")
                  .arg(done + 1)
                  .arg(std::max(m_profileTotal, done + 1)),
              false);

    const auto next = [this](int waitMs) {
        QTimer::singleShot(waitMs, [this]() { fetchNextProfile(); });
    };
    context()->rest()->fetchGuildProfile(
        guildId,
        [this, guildId, next](const QJsonObject &payload) {
            QJsonObject profile;
            QJsonArray traits;
            for (const QJsonValue &value : payload.value(QStringLiteral("traits")).toArray()) {
                const QString label = value.toObject().value(QStringLiteral("label")).toString().trimmed();
                if (!label.isEmpty())
                    traits.append(label);
            }
            QJsonArray games;
            for (const QJsonValue &value : payload.value(QStringLiteral("game_application_ids")).toArray())
                games.append(value.isString() ? value.toString() : QString::number(qint64(value.toDouble())));
            // Some answers carry the games themselves too; their names help
            // with a game Discord's list does not have.
            QJsonObject names;
            for (const QJsonValue &value : payload.value(QStringLiteral("games")).toArray()) {
                const QJsonObject game = value.toObject();
                const QString id = game.value(QStringLiteral("id")).toString();
                const QString name = game.value(QStringLiteral("name")).toString();
                if (!id.isEmpty() && !name.isEmpty())
                    names.insert(id, name);
            }
            int activity = 0;
            const QJsonObject played = payload.value(QStringLiteral("game_activity")).toObject();
            for (auto it = played.constBegin(); it != played.constEnd(); ++it)
                activity = std::max(activity, it.value().toObject().value(QStringLiteral("activity_level")).toInt());
            profile.insert(QStringLiteral("traits"), traits);
            profile.insert(QStringLiteral("games"), games);
            if (!names.isEmpty())
                profile.insert(QStringLiteral("names"), names);
            profile.insert(QStringLiteral("activity"), activity);
            const QString description = payload.value(QStringLiteral("description")).toString();
            if (!description.isEmpty())
                profile.insert(QStringLiteral("description"), description);
            profile.insert(QStringLiteral("at"), double(QDateTime::currentSecsSinceEpoch()));
            m_profiles.insert(guildId, profile);
            m_profileQueue.removeAll(guildId);
            next(ProfileGapMs);
        },
        [this, guildId, next](const RestClient::Error &error) {
            if (error.httpStatus == 429) {
                // Waited out and asked again.
                const double retry = error.body.value(QStringLiteral("retry_after")).toDouble(5.0);
                wlog(QStringLiteral("organizer"), QStringLiteral("profiles rate limited, waiting %1 s").arg(retry));
                next(int(std::clamp(retry, 1.0, 120.0) * 1000) + 250);
                return;
            }
            // No profile to be had - a server without one, or one Discord
            // keeps to itself. Remembered, so it is not asked for again today.
            m_profiles.insert(guildId, QJsonObject{{QStringLiteral("failed"), true},
                                                   {QStringLiteral("at"), double(QDateTime::currentSecsSinceEpoch())}});
            m_profileQueue.removeAll(guildId);
            next(ProfileGapMs);
        });
}

// ---------------------------------------------------------------------------
// Saved state
// ---------------------------------------------------------------------------

// Folders and what was sorted belong to one account; the overrides and the
// voice counts are about servers, whoever is looking at them.
QString ServerOrganizerPlugin::accountKey(const QString &what) const
{
    QString self = context() && context()->store() ? context()->store()->selfUserId() : QString();
    if (self.isEmpty())
        self = QStringLiteral("unknown");
    return what + QLatin1Char('/') + self;
}

QJsonObject ServerOrganizerPlugin::loadObject(const QString &key) const
{
    return QJsonDocument::fromJson(settingValue(key).toString().toUtf8()).object();
}

void ServerOrganizerPlugin::saveObject(const QString &key, const QJsonObject &object)
{
    setSettingValue(key, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

QJsonArray ServerOrganizerPlugin::railToJson(const QList<RailLayout::Entry> &entries)
{
    QJsonArray out;
    for (const RailLayout::Entry &entry : entries) {
        if (!entry.isFolder) {
            out.append(QJsonObject{{QStringLiteral("guild"), entry.guildId}});
            continue;
        }
        QJsonObject folder{
            {QStringLiteral("folder"), entry.folder.id},
            {QStringLiteral("name"), entry.folder.name},
            {QStringLiteral("open"), entry.folder.open},
            {QStringLiteral("guilds"), QJsonArray::fromStringList(entry.folder.guildIds)},
        };
        if (entry.folder.hasColor)
            folder.insert(QStringLiteral("color"), double(entry.folder.color));
        out.append(folder);
    }
    return out;
}

QList<RailLayout::Entry> ServerOrganizerPlugin::railFromJson(const QJsonArray &array)
{
    QList<RailLayout::Entry> out;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        RailLayout::Entry entry;
        if (object.contains(QStringLiteral("guild"))) {
            entry.guildId = object.value(QStringLiteral("guild")).toString();
            if (!entry.guildId.isEmpty())
                out.append(entry);
            continue;
        }
        entry.isFolder = true;
        entry.folder.id = object.value(QStringLiteral("folder")).toString();
        entry.folder.name = object.value(QStringLiteral("name")).toString();
        entry.folder.open = object.value(QStringLiteral("open")).toBool();
        for (const QJsonValue &guild : object.value(QStringLiteral("guilds")).toArray())
            entry.folder.guildIds.append(guild.toString());
        if (object.contains(QStringLiteral("color"))) {
            entry.folder.hasColor = true;
            entry.folder.color = quint64(object.value(QStringLiteral("color")).toDouble());
        }
        if (!entry.folder.id.isEmpty() && !entry.folder.guildIds.isEmpty())
            out.append(entry);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Settings panel
// ---------------------------------------------------------------------------

void ServerOrganizerPlugin::setStatus(const QString &text, bool log)
{
    m_status = text;
    if (log)
        wlog(QStringLiteral("organizer"), text);
    refreshPanel();
}

void ServerOrganizerPlugin::refreshPanel()
{
    // Later, never inside the click or the menu change that got here: the
    // refresh rebuilds the list, and with it the box that was just changed.
    if (!m_panel)
        return;
    QTimer::singleShot(0, m_panel.data(), [panel = m_panel]() {
        if (panel)
            static_cast<OrganizerPanel *>(panel.data())->refresh();
    });
}

QWidget *ServerOrganizerPlugin::createSettingsWidget(QWidget *parent)
{
    auto *panel = new OrganizerPanel(this, parent);
    m_panel = panel;
    return panel;
}
