#include "plugin/builtin/PresenceHintsPlugin.h"

#include "core/RestClient.h"

#include <QCheckBox>
#include <QJsonArray>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

void PresenceHintsPlugin::onLoad(PluginContext *context)
{
    Plugin::onLoad(context);
    m_noted.clear();
    m_namesAsked.clear();
}

void PresenceHintsPlugin::onUnload()
{
    // Switching the plugin off clears every hint, so nothing lingers.
    if (context() && context()->store()) {
        for (const QString &userId : m_noted)
            context()->store()->forgetActivity(userId);
    }
    m_noted.clear();
    m_namesAsked.clear();
    Plugin::onUnload();
}

void PresenceHintsPlugin::note(const QString &userId, const QString &why)
{
    if (userId.isEmpty() || !context() || !context()->store())
        return;

    MessageStore *store = context()->store();

    // Someone Discord already reports as online needs no hint.
    if (store->presence(userId).isOnline()) {
        if (m_noted.remove(userId))
            store->forgetActivity(userId);
        return;
    }

    // Marks are kept for everyone, so the ring works wherever you look.
    store->noteActivity(userId);

    const bool firstSighting = !m_noted.contains(userId);
    m_noted.insert(userId);
    if (!firstSighting)
        return;

    announce(userId, why);
}

void PresenceHintsPlugin::announce(const QString &userId, const QString &why)
{
    MessageStore *store = context()->store();

    // A busy server puts dozens of strangers through here. Saying something
    // about every one of them is noise, so by default only friends get a line.
    const bool friendsOnly = context()->setting(QStringLiteral("friendsOnly"), true).toBool();
    if (friendsOnly && !store->user(userId).isFriend())
        return;

    const QString name = store->userName(userId);
    if (!name.isEmpty()) {
        context()->log(QStringLiteral("%1 looks active (%2) while showing offline").arg(name, why));
        return;
    }

    // No name yet. Ask Discord for one rather than printing a bare number,
    // and only once per person.
    if (m_namesAsked.contains(userId) || !context()->rest())
        return;
    m_namesAsked.insert(userId);

    const QString reason = why;
    context()->rest()->fetchUser(
        userId,
        [this, userId, reason](const QJsonObject &user) {
            if (!context() || !context()->store())
                return;
            context()->store()->rememberUser(user);

            const QString resolved = context()->store()->userName(userId);
            context()->log(QStringLiteral("%1 looks active (%2) while showing offline")
                               .arg(resolved.isEmpty() ? userId : resolved, reason));
        },
        [](const RestClient::Error &) {
            // Not worth a fuss. The mark still stands, only the name is missing.
        });
}

void PresenceHintsPlugin::onGatewayEvent(const QString &eventType, const QJsonObject &data)
{
    if (!context() || !context()->store())
        return;

    // Typing is the strongest signal. It fires from a live client and Discord
    // sends it even for someone hiding their status.
    if (eventType == QLatin1String("TYPING_START")) {
        note(data.value(QStringLiteral("user_id")).toString(), QStringLiteral("typing"));
        return;
    }

    if (eventType == QLatin1String("MESSAGE_CREATE")) {
        const QJsonObject author = data.value(QStringLiteral("author")).toObject();
        if (!author.value(QStringLiteral("bot")).toBool())
            note(author.value(QStringLiteral("id")).toString(), QStringLiteral("posted"));
        return;
    }

    if (eventType == QLatin1String("VOICE_STATE_UPDATE")) {
        // Only joining counts. Leaving says nothing about being there now.
        if (!data.value(QStringLiteral("channel_id")).toString().isEmpty())
            note(data.value(QStringLiteral("user_id")).toString(), QStringLiteral("in voice"));
        return;
    }

    // Everyone already sitting in a voice channel when we connected. Without
    // this, someone who joined the call before Wisp started is missed, which
    // is the most obvious case of all.
    if (eventType == QLatin1String("READY_SUPPLEMENTAL")) {
        const QJsonArray guilds = data.value(QStringLiteral("guilds")).toArray();
        for (const QJsonValue &guildValue : guilds) {
            const QJsonArray states =
                guildValue.toObject().value(QStringLiteral("voice_states")).toArray();
            for (const QJsonValue &stateValue : states) {
                note(stateValue.toObject().value(QStringLiteral("user_id")).toString(),
                     QStringLiteral("in voice"));
            }
        }
        return;
    }

    // A real presence puts the truth back in charge.
    if (eventType == QLatin1String("PRESENCE_UPDATE")) {
        const QString userId = data.value(QStringLiteral("user"))
                                   .toObject()
                                   .value(QStringLiteral("id"))
                                   .toString();
        const QString status = data.value(QStringLiteral("status")).toString();
        if (!userId.isEmpty() && !status.isEmpty() && status != QLatin1String("offline")) {
            if (m_noted.remove(userId))
                context()->store()->forgetActivity(userId);
        }
    }
}

QWidget *PresenceHintsPlugin::createSettingsWidget(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(6);

    auto *friendsOnly = new QCheckBox(QStringLiteral("Only mention friends at the bottom"), page);
    friendsOnly->setChecked(context() ? context()->setting(QStringLiteral("friendsOnly"), true).toBool()
                                      : true);
    friendsOnly->setToolTip(QStringLiteral("Everyone is still watched. This only controls the "
                                           "notices along the bottom of the window."));
    QObject::connect(friendsOnly, &QCheckBox::toggled, page, [this](bool on) {
        if (context())
            context()->setSetting(QStringLiteral("friendsOnly"), on);
    });
    layout->addWidget(friendsOnly);

    auto *minutes = new QSpinBox(page);
    minutes->setRange(1, 120);
    minutes->setSuffix(QStringLiteral(" min"));
    minutes->setValue(context() ? context()->setting(QStringLiteral("window"), 10).toInt() : 10);
    minutes->setPrefix(QStringLiteral("Count as active for "));
    minutes->setMaximumWidth(220);
    layout->addWidget(minutes);

    QObject::connect(minutes, &QSpinBox::valueChanged, page, [this](int value) {
        if (context())
            context()->setSetting(QStringLiteral("window"), value);
    });

    auto *note = new QLabel(
        QStringLiteral("A hollow green ring means \"looks active\", a filled one means Discord "
                       "actually said so. This can only notice someone who does something."),
        page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: #75736b; font-size: 11px;"));
    layout->addWidget(note);

    return page;
}
