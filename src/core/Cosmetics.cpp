#include "core/Cosmetics.h"

#include <QJsonArray>

#include <algorithm>

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

// Discord's collectible item types.
constexpr int ProfileEffectType = 1;
constexpr int ProfileFrameType = 3;

QString collectibleSku(const QJsonObject &profile, int type)
{
    for (const QJsonValue &value : profile.value(QStringLiteral("collectibles")).toArray()) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("type")).toInt(-1) == type)
            return idOf(item.value(QStringLiteral("sku_id")));
    }
    return {};
}

QJsonObject productItem(const QJsonObject &product, int type)
{
    for (const QJsonValue &value : product.value(QStringLiteral("items")).toArray()) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("type")).toInt(-1) == type)
            return item;
    }
    return {};
}

int number(const QJsonObject &object, const QString &key)
{
    return qRound(object.value(key).toDouble());
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

QString profileEffectSku(const QJsonObject &profile)
{
    const QString sku = collectibleSku(profile, ProfileEffectType);
    if (!sku.isEmpty())
        return sku;
    // The field the API used before collectibles: { id, expires_at }, the id
    // being the SKU.
    const QJsonObject older = profile.value(QStringLiteral("profile_effect")).toObject();
    const QString skuId = idOf(older.value(QStringLiteral("sku_id")));
    return skuId.isEmpty() ? idOf(older.value(QStringLiteral("id"))) : skuId;
}

QString profileFrameSku(const QJsonObject &profile)
{
    return collectibleSku(profile, ProfileFrameType);
}

ProfileEffect profileEffect(const QJsonObject &product)
{
    const QJsonObject item = productItem(product, ProfileEffectType);
    ProfileEffect out;
    if (item.isEmpty())
        return out;
    out.skuId = idOf(item.value(QStringLiteral("sku_id")));
    if (out.skuId.isEmpty())
        out.skuId = idOf(product.value(QStringLiteral("sku_id")));
    out.label = item.value(QStringLiteral("accessibilityLabel")).toString();

    for (const QJsonValue &value : item.value(QStringLiteral("effects")).toArray()) {
        const QJsonObject effect = value.toObject();
        EffectLayer layer;
        layer.src = effect.value(QStringLiteral("src")).toString();
        for (const QJsonValue &source : effect.value(QStringLiteral("randomizedSources")).toArray()) {
            const QString src = source.toObject().value(QStringLiteral("src")).toString();
            if (!src.isEmpty())
                layer.randomSources.append(src);
        }
        if (layer.src.isEmpty() && layer.randomSources.isEmpty())
            continue;
        layer.loop = effect.value(QStringLiteral("loop")).toBool();
        layer.start = number(effect, QStringLiteral("start"));
        layer.duration = number(effect, QStringLiteral("duration"));
        layer.loopDelay = number(effect, QStringLiteral("loopDelay"));
        const QJsonObject position = effect.value(QStringLiteral("position")).toObject();
        layer.x = number(position, QStringLiteral("x"));
        layer.y = number(position, QStringLiteral("y"));
        layer.zIndex = number(effect, QStringLiteral("zIndex"));
        out.layers.append(layer);
    }
    std::stable_sort(out.layers.begin(), out.layers.end(),
                     [](const EffectLayer &a, const EffectLayer &b) { return a.zIndex < b.zIndex; });
    return out;
}

ProfileFrame profileFrame(const QJsonObject &product)
{
    const QJsonObject item = productItem(product, ProfileFrameType);
    ProfileFrame out;
    if (item.isEmpty())
        return out;
    out.skuId = idOf(item.value(QStringLiteral("sku_id")));
    if (out.skuId.isEmpty())
        out.skuId = idOf(product.value(QStringLiteral("sku_id")));
    out.label = item.value(QStringLiteral("label")).toString();
    out.innerWidth = number(item, QStringLiteral("inner_width"));
    out.overflowTop = number(item, QStringLiteral("overflow_top"));
    out.overflowBottom = number(item, QStringLiteral("overflow_bottom"));
    out.overflowHorizontal = number(item, QStringLiteral("overflow_horizontal"));

    for (const QJsonValue &value : item.value(QStringLiteral("layers")).toArray()) {
        const QJsonObject raw = value.toObject();
        FrameLayer layer;
        layer.id = idOf(raw.value(QStringLiteral("id")));
        layer.type = raw.value(QStringLiteral("type")).toString();
        layer.front = raw.value(QStringLiteral("order")).toString() != QLatin1String("back");
        layer.anchor = raw.value(QStringLiteral("anchor")).toString();
        layer.responsive = raw.value(QStringLiteral("responsive")).toBool();
        if (!layer.id.isEmpty())
            out.layers.append(layer);
    }
    return out;
}

} // namespace Cosmetics
