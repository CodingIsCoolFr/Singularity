#include "core/Cosmetics.h"

#include <QJsonArray>

namespace Cosmetics {

namespace {

// Ids arrive as strings or, now and then, as numbers.
QString idOf(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(qint64(value.toDouble()));
    return {};
}

} // namespace

Decoration decoration(const QJsonValue &value)
{
    const QJsonObject data = value.toObject();
    Decoration out;
    out.asset = data.value(QStringLiteral("asset")).toString();
    out.skuId = idOf(data.value(QStringLiteral("sku_id")));
    return out;
}

Nameplate nameplate(const QJsonValue &collectibles)
{
    // Discord's own check: a nameplate without a label and a palette is not
    // one it draws.
    const QJsonObject plate = collectibles.toObject().value(QStringLiteral("nameplate")).toObject();
    Nameplate out;
    const QString sku = idOf(plate.value(QStringLiteral("sku_id")));
    if (sku.isEmpty() || !plate.value(QStringLiteral("palette")).isString())
        return out;
    out.skuId = sku;
    out.asset = plate.value(QStringLiteral("asset")).toString();
    out.label = plate.value(QStringLiteral("label")).toString();
    out.palette = plate.value(QStringLiteral("palette")).toString();
    return out;
}

GuildTag guildTag(const QJsonValue &primaryGuild)
{
    const QJsonObject guild = primaryGuild.toObject();
    GuildTag out;
    if (!guild.value(QStringLiteral("identity_enabled")).toBool())
        return out;
    out.guildId = idOf(guild.value(QStringLiteral("identity_guild_id")));
    out.tag = guild.value(QStringLiteral("tag")).toString();
    out.badge = guild.value(QStringLiteral("badge")).toString();
    return out;
}

NameStyle nameStyle(const QJsonValue &value)
{
    const QJsonObject styles = value.toObject();
    NameStyle out;
    out.fontId = styles.value(QStringLiteral("font_id")).toInt();
    out.effectId = styles.value(QStringLiteral("effect_id")).toInt();
    for (const QJsonValue &colour : styles.value(QStringLiteral("colors")).toArray())
        out.colors.append(quint32(qint64(colour.toDouble())) & 0xffffff);
    return out;
}

RoleColours roleColours(const QJsonObject &role)
{
    RoleColours out;
    const QJsonObject colors = role.value(QStringLiteral("colors")).toObject();
    out.primary = quint32(qint64(colors.contains(QStringLiteral("primary_color"))
                                     ? colors.value(QStringLiteral("primary_color")).toDouble()
                                     : role.value(QStringLiteral("color")).toDouble()));
    const QJsonValue secondary = colors.value(QStringLiteral("secondary_color"));
    const QJsonValue tertiary = colors.value(QStringLiteral("tertiary_color"));
    out.hasSecondary = secondary.isDouble();
    out.hasTertiary = tertiary.isDouble();
    out.secondary = quint32(qint64(secondary.toDouble()));
    out.tertiary = quint32(qint64(tertiary.toDouble()));
    return out;
}

} // namespace Cosmetics
