#include "plugin/builtin/PlatformIndicatorsPlugin.h"

#include "ui/Theme.h"

#include <QBuffer>
#include <QCheckBox>
#include <QGridLayout>
#include <QImageReader>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

namespace {

// The icons, Vencord's own paths. Each is drawn in a square of the size
// asked; the phone a little smaller than the rest, as Vencord sizes it.
struct Shape
{
    const char *platform;
    qreal viewX, viewY, viewWidth, viewHeight;
    qreal scale;
    const char *path;
};

const Shape &shapeOf(const QString &platform)
{
    static const Shape shapes[] = {
        {"desktop", 0, 0, 24, 24, 1.0,
         "M4 2.5c-1.103 0-2 .897-2 2v11c0 1.104.897 2 2 2h7v2H7v2h10v-2h-4v-2h7c1.103 0 2-.896 2-2v-11c0-1.103-.897-2-2-2H4Zm16 2v9H4v-9h16Z"},
        {"web", 0, 0, 24, 24, 1.0,
         "M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2Zm-1 17.93c-3.95-.49-7-3.85-7-7.93 "
         "0-.62.08-1.21.21-1.79L9 15v1c0 1.1.9 2 2 2v1.93Zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3c0-.55-.45-1-1-1H8v-2h2c.55 "
         "0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39Z"},
        {"mobile", 0, 0, 1000, 1500, 0.85,
         "M 187 0 L 813 0 C 916.277 0 1000 83.723 1000 187 L 1000 1313 C 1000 1416.277 916.277 1500 813 1500 L 187 1500 "
         "C 83.723 1500 0 1416.277 0 1313 L 0 187 C 0 83.723 83.723 0 187 0 Z M 125 1000 L 875 1000 L 875 250 L 125 250 Z "
         "M 500 1125 C 430.964 1125 375 1180.964 375 1250 C 375 1319.036 430.964 1375 500 1375 C 569.036 1375 625 1319.036 "
         "625 1250 C 625 1180.964 569.036 1125 500 1125 Z"},
        {"embedded", 0, 0, 24, 24, 1.0,
         "M3.06 20.4q-1.53 0-2.37-1.065T.06 16.74l1.26-9q.27-1.8 1.605-2.97T6.06 3.6h11.88q1.8 0 3.135 1.17t1.605 2.97l1.26 "
         "9q.21 1.53-.63 2.595T20.94 20.4q-.63 0-1.17-.225T18.78 19.5l-2.7-2.7H7.92l-2.7 2.7q-.45.45-.99.675t-1.17.225Zm14.94"
         "-7.2q.51 0 .855-.345T19.2 12q0-.51-.345-.855T18 10.8q-.51 0-.855.345T16.8 12q0 .51.345 .855T18 13.2Zm-2.4-3.6q.51 "
         "0 .855-.345T16.8 8.4q0-.51-.345-.855T15.6 7.2q-.51 0-.855.345T14.4 8.4q0 .51.345 .855T15.6 9.6ZM6.9 13.2h1.8v-2.1h2"
         ".1v-1.8h-2.1v-2.1h-1.8v2.1h-2.1v1.8h2.1v2.1Z"},
        {"vr", 0, 4, 24, 16, 1.0,
         "M8.46 8.64a1 1 0 0 1 1 1c0 .44-.3.8-.72.92l-.11.07c-.08.06-.2.19-.2.41a.99.99 0 0 1-.98.86h-.06a1 1 0 0 1-.94"
         "-1.05l.02-.32c.05-1.06.92-1.9 1.99-1.9ZM15.55 5a5.5 5.5 0 0 1 5.15 3.67h.3a2 2 0 0 1 2 2v3.18a2 2 0 0 1-2 1.99h-.2A4"
         ".54 4.54 0 0 1 16.55 19a4.45 4.45 0 0 1-3.6-1.83 1.2 1.2 0 0 0-1.9 0 4.44 4.44 0 0 1-3.9 1.82 4.54 4.54 0 0 1-3.94"
         "-3.15H3a2 2 0 0 1-2-2v-3.18c0-1.1.9-1.99 2-1.99h.3A5.5 5.5 0 0 1 8.46 5h7.09Zm-7.1 2C6.6 7 5.06 8.5 4.97 10.41l-.02"
         ".66v3.18c0 1.43 1.05 2.66 2.34 2.74.85.06 1.63-.32 2.14-1.01a3.2 3.2 0 0 1 2.57-1.3c1 0 1.97.48 2.57 1.3.5.69 1.3 "
         "1.08 2.14 1.01 1.3-.08 2.34-1.31 2.34-2.74l-.02-3.84a3.54 3.54 0 0 0-3.49-3.43H8.45Z"},
    };
    for (const Shape &shape : shapes) {
        if (platform == QLatin1String(shape.platform))
            return shape;
    }
    return shapes[0];
}

// Discord's status colours, the ones the status dots use.
QColor statusColour(const QString &status)
{
    if (status == QLatin1String("online"))
        return QColor(Theme::Green);
    if (status == QLatin1String("idle"))
        return QColor(Theme::Yellow);
    if (status == QLatin1String("dnd"))
        return QColor(Theme::Red);
    if (status == QLatin1String("streaming"))
        return QColor(0x59, 0x36, 0x95);
    return QColor(Theme::TextFaint);
}

QString statusWord(const QString &status)
{
    if (status == QLatin1String("online"))
        return QStringLiteral("Online");
    if (status == QLatin1String("idle"))
        return QStringLiteral("Idle");
    if (status == QLatin1String("dnd"))
        return QStringLiteral("Do Not Disturb");
    if (status == QLatin1String("streaming"))
        return QStringLiteral("Streaming");
    if (status == QLatin1String("invisible"))
        return QStringLiteral("Invisible");
    return QStringLiteral("Offline");
}

// Online wins over idle, idle over anything else.
int rank(const QString &status)
{
    if (status == QLatin1String("online"))
        return 3;
    if (status == QLatin1String("idle"))
        return 2;
    if (status == QLatin1String("dnd"))
        return 1;
    return 0;
}

// The places, each with its own switch, as the two plugins offer them.
struct Place
{
    NamePlace place;
    const char *label;
};

const QList<Place> &placesShown()
{
    static const QList<Place> places{
        {NamePlace::MemberList, "In the member list"},
        {NamePlace::DirectMessages, "In direct messages"},
        {NamePlace::FriendsList, "In the friends list"},
        {NamePlace::Chat, "Next to names in chat"},
        {NamePlace::ProfileBadges, "Next to profile badges"},
        {NamePlace::Avatar, "On pictures, in place of the status dot"},
    };
    return places;
}

} // namespace

const QList<PlatformIndicatorsPlugin::Platform> &PlatformIndicatorsPlugin::platforms()
{
    static const QList<Platform> list{
        {"desktop", "Desktop"}, {"mobile", "Mobile"}, {"web", "Web"}, {"embedded", "Console"}, {"vr", "VR"},
    };
    return list;
}

QString PlatformIndicatorsPlugin::placeKey(NamePlace place)
{
    switch (place) {
    case NamePlace::MemberList: return QStringLiteral("memberList");
    case NamePlace::DirectMessages: return QStringLiteral("directMessages");
    case NamePlace::FriendsList: return QStringLiteral("friendsList");
    case NamePlace::Chat: return QStringLiteral("chat");
    case NamePlace::ProfileBadges: return QStringLiteral("badges");
    case NamePlace::Avatar: return QStringLiteral("avatars");
    }
    return {};
}

QString PlatformIndicatorsPlugin::columnKey(NamePlace place)
{
    // BetterDiscord's three columns: next to names, next to badges, on
    // pictures.
    if (place == NamePlace::ProfileBadges)
        return QStringLiteral("badgeIcons");
    if (place == NamePlace::Avatar)
        return QStringLiteral("avatarIcons");
    return QStringLiteral("nameIcons");
}

void PlatformIndicatorsPlugin::loadOptions()
{
    m_options.clear();
    for (const Place &place : placesShown())
        m_options.insert(placeKey(place.place), settingValue(placeKey(place.place), true).toBool());
    m_options.insert(QStringLiteral("ignoreBots"), settingValue(QStringLiteral("ignoreBots"), true).toBool());
    for (NamePlace column : {NamePlace::Chat, NamePlace::ProfileBadges, NamePlace::Avatar}) {
        for (const Platform &platform : platforms()) {
            const QString key = columnKey(column) + QLatin1Char('/') + QLatin1String(platform.id);
            m_options.insert(key, settingValue(key, true).toBool());
        }
    }
}

void PlatformIndicatorsPlugin::onLoad(PluginContext *context)
{
    Plugin::onLoad(context);
    loadOptions();
    m_icons.clear();
    if (context->store())
        m_selfId = context->store()->selfUserId();
}

void PlatformIndicatorsPlugin::onUnload()
{
    m_ownClients.clear();
    m_icons.clear();
    Plugin::onUnload();
}

void PlatformIndicatorsPlugin::takeSessions(const QJsonArray &sessions)
{
    QHash<QString, QString> clients;
    for (const QJsonValue &value : sessions) {
        const QJsonObject session = value.toObject();
        const QString client = session.value(QStringLiteral("client_info")).toObject().value(QStringLiteral("client")).toString();
        const QString status = session.value(QStringLiteral("status")).toString();
        // "unknown" is Discord's summary of every session at once.
        if (client.isEmpty() || client == QLatin1String("unknown") || status.isEmpty())
            continue;
        if (!clients.contains(client) || rank(status) > rank(clients.value(client)))
            clients.insert(client, status);
    }
    if (clients == m_ownClients)
        return;
    m_ownClients = clients;
    if (context())
        context()->refreshNames(m_selfId);
}

void PlatformIndicatorsPlugin::onGatewayEvent(const QString &eventType, const QJsonObject &data)
{
    if (!context())
        return;
    if (eventType == QLatin1String("READY")) {
        m_selfId = data.value(QStringLiteral("user")).toObject().value(QStringLiteral("id")).toString();
        takeSessions(data.value(QStringLiteral("sessions")).toArray());
        context()->refreshNames();
    } else if (eventType == QLatin1String("SESSIONS_REPLACE")) {
        // A list rather than an object; the gateway hands it on as "list".
        takeSessions(data.value(QStringLiteral("list")).toArray());
    } else if (eventType == QLatin1String("PRESENCE_UPDATE")) {
        const QString userId = data.value(QStringLiteral("user")).toObject().value(QStringLiteral("id")).toString();
        if (!userId.isEmpty())
            context()->refreshNames(userId);
    } else if (eventType == QLatin1String("READY_SUPPLEMENTAL") || eventType == QLatin1String("PRESENCES_REPLACE")) {
        context()->refreshNames();
    }
}

QImage PlatformIndicatorsPlugin::icon(const QString &platform, const QString &status, int size, qreal ratio)
{
    const QColor colour = statusColour(status);
    const QString key = QStringLiteral("%1 %2 %3 %4").arg(platform, colour.name()).arg(size).arg(ratio);
    const auto cached = m_icons.constFind(key);
    if (cached != m_icons.constEnd())
        return cached.value();

    // Drawn by Qt's SVG reader, its own aspect kept, then centred in a square
    // so a row of them is evenly spaced.
    const Shape &shape = shapeOf(platform);
    const int pixels = qMax(1, qRound(size * ratio));
    const qreal fit = pixels * shape.scale / qMax(shape.viewWidth, shape.viewHeight);
    const QSize drawn(qMax(1, qRound(shape.viewWidth * fit)), qMax(1, qRound(shape.viewHeight * fit)));
    const QByteArray svg = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"%1 %2 %3 %4\" "
                                          "width=\"%5\" height=\"%6\"><path fill=\"%7\" d=\"%8\"/></svg>")
                               .arg(shape.viewX)
                               .arg(shape.viewY)
                               .arg(shape.viewWidth)
                               .arg(shape.viewHeight)
                               .arg(drawn.width())
                               .arg(drawn.height())
                               .arg(colour.name(), QLatin1String(shape.path))
                               .toUtf8();
    QBuffer buffer;
    buffer.setData(svg);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "svg");
    reader.setScaledSize(drawn);
    const QImage art = reader.read();

    QImage image(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    if (!art.isNull()) {
        QPainter painter(&image);
        painter.drawImage((pixels - art.width()) / 2, (pixels - art.height()) / 2, art);
    }
    image.setDevicePixelRatio(ratio);

    if (m_icons.size() > 256)
        m_icons.clear();
    m_icons.insert(key, image);
    return image;
}

QList<NameIcon> PlatformIndicatorsPlugin::nameIcons(const QString &userId, NamePlace place, int size, qreal ratio)
{
    if (!context() || !context()->store() || userId.isEmpty() || !m_options.value(placeKey(place), true))
        return {};
    MessageStore *store = context()->store();
    if (m_options.value(QStringLiteral("ignoreBots"), true) && store->user(userId).bot)
        return {};

    // Your own devices are your sessions. Until Discord has listed them -
    // switched on after signing in - this copy counts as a desktop.
    const QString self = m_selfId.isEmpty() ? store->selfUserId() : m_selfId;
    QHash<QString, QString> clients;
    if (userId == self) {
        clients = m_ownClients;
        if (clients.isEmpty() && store->presence(self).isOnline())
            clients.insert(QStringLiteral("desktop"), store->presence(self).status);
    } else {
        clients = store->presence(userId).clientStatus;
    }
    if (clients.isEmpty())
        return {};

    const QString column = columnKey(place) + QLatin1Char('/');
    QList<NameIcon> out;
    for (const Platform &platform : platforms()) {
        const QString key = QLatin1String(platform.id);
        const QString status = clients.value(key);
        if (status.isEmpty() || !m_options.value(column + key, true))
            continue;
        NameIcon item;
        item.id = key + QLatin1Char(':') + status;
        item.image = icon(key, status, size, ratio);
        item.tooltip = QStringLiteral("%1: %2").arg(QLatin1String(platform.name), statusWord(status));
        out.append(item);
        // A picture's corner has room for one.
        if (place == NamePlace::Avatar)
            break;
    }
    return out;
}

QWidget *PlatformIndicatorsPlugin::createSettingsWidget(QWidget *parent)
{
    loadOptions();

    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(6);

    const auto toggle = [this](const QString &key, bool on) {
        setSettingValue(key, on);
        m_options.insert(key, on);
        if (context())
            context()->refreshNames();
    };

    for (const Place &place : placesShown()) {
        const QString key = placeKey(place.place);
        auto *box = new QCheckBox(QString::fromLatin1(place.label), page);
        box->setChecked(m_options.value(key, true));
        QObject::connect(box, &QCheckBox::toggled, page, [toggle, key](bool on) { toggle(key, on); });
        layout->addWidget(box);
    }

    auto *bots = new QCheckBox(QStringLiteral("Ignore bots"), page);
    bots->setToolTip(QStringLiteral("A bot's status is nearly always the web one anyway."));
    bots->setChecked(m_options.value(QStringLiteral("ignoreBots"), true));
    QObject::connect(bots, &QCheckBox::toggled, page,
                     [toggle](bool on) { toggle(QStringLiteral("ignoreBots"), on); });
    layout->addWidget(bots);

    // Which platforms show where, BetterDiscord's table.
    auto *heading = new QLabel(QStringLiteral("Which devices show where"), page);
    heading->setStyleSheet(QStringLiteral("font-weight: 600; margin-top: 8px;"));
    layout->addWidget(heading);

    auto *table = new QGridLayout;
    table->setHorizontalSpacing(18);
    table->setVerticalSpacing(4);
    const QList<QPair<NamePlace, QString>> columns{
        {NamePlace::Chat, QStringLiteral("Next to names")},
        {NamePlace::ProfileBadges, QStringLiteral("Next to badges")},
        {NamePlace::Avatar, QStringLiteral("On pictures")},
    };
    for (int c = 0; c < columns.size(); ++c) {
        auto *title = new QLabel(columns.at(c).second, page);
        title->setStyleSheet(QStringLiteral("color: #75736b; font-size: 11px;"));
        table->addWidget(title, 0, c + 1, Qt::AlignCenter);
    }
    for (int r = 0; r < platforms().size(); ++r) {
        const Platform &platform = platforms().at(r);
        auto *row = new QWidget(page);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        auto *picture = new QLabel(row);
        picture->setPixmap(QPixmap::fromImage(icon(QLatin1String(platform.id), QStringLiteral("online"), 18,
                                                   page->devicePixelRatioF())));
        rowLayout->addWidget(picture);
        rowLayout->addWidget(new QLabel(QString::fromLatin1(platform.name), row), 1);
        table->addWidget(row, r + 1, 0);
        for (int c = 0; c < columns.size(); ++c) {
            const QString key = columnKey(columns.at(c).first) + QLatin1Char('/') + QLatin1String(platform.id);
            auto *box = new QCheckBox(page);
            box->setChecked(m_options.value(key, true));
            box->setToolTip(QStringLiteral("%1: %2").arg(QString::fromLatin1(platform.name), columns.at(c).second));
            QObject::connect(box, &QCheckBox::toggled, page, [toggle, key](bool on) { toggle(key, on); });
            table->addWidget(box, r + 1, c + 1, Qt::AlignCenter);
        }
    }
    layout->addLayout(table);

    auto *note = new QLabel(QStringLiteral("Each icon is coloured by the person's status on that device: green "
                                           "online, yellow idle, red do not disturb. Discord only says which "
                                           "devices someone is on while they are not invisible."),
                            page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: #75736b; font-size: 11px;"));
    layout->addWidget(note);

    return page;
}
