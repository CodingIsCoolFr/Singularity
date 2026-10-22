#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>

// What Discord dresses a person in beyond their name and picture: the avatar
// decoration, the nameplate behind their row, the server tag beside their
// name, and the font and effect of the name itself. Each comes on the user,
// for the whole account, and the first three may come on a server member as
// well, for that server only; the member's wins there.
//
// Read from Discord's own shapes. A field that is missing from a payload says
// nothing (partial user objects turn up all over the gateway); a field that
// is there and null clears it.
namespace Cosmetics {

// avatar_decoration_data: { asset, sku_id, expires_at }
struct Decoration
{
    QString asset;
    QString skuId;
    bool isSet() const { return !asset.isEmpty(); }
};

// collectibles.nameplate: { sku_id, asset, label, palette, expires_at }
struct Nameplate
{
    QString skuId;
    QString asset;
    QString label;
    QString palette;   // "crimson", "berry", ... the colour washed behind it
    bool isSet() const { return !skuId.isEmpty(); }
};

// primary_guild: { identity_guild_id, identity_enabled, tag, badge }. Only an
// enabled one is kept: a disabled tag is shown nowhere.
struct GuildTag
{
    QString guildId;
    QString tag;
    QString badge;
    bool isSet() const { return !guildId.isEmpty() && !tag.isEmpty(); }
};

// display_name_styles: { font_id, effect_id, colors }. Discord's ids:
// fonts 1 to 17 (11 is its ordinary font), effects 1 solid, 2 gradient,
// 3 neon, 4 toon, 5 pop, 6 glow, 7 prism, 8 gummy.
struct NameStyle
{
    int fontId = 0;
    int effectId = 0;
    QList<quint32> colors;
    bool isSet() const { return fontId != 0 || effectId != 0; }
};

enum Effect { Solid = 1, Gradient = 2, Neon = 3, Toon = 4, Pop = 5, Glow = 6, Prism = 7, Gummy = 8 };

// A role's colors: { primary_color, secondary_color, tertiary_color }. Two
// make a gradient, three the holographic style. Drawn as a gradient only in
// a server with ENHANCED_ROLE_COLORS.
struct RoleColours
{
    quint32 primary = 0;
    quint32 secondary = 0;
    quint32 tertiary = 0;
    bool hasSecondary = false;
    bool hasTertiary = false;
};

Decoration decoration(const QJsonValue &value);
Nameplate nameplate(const QJsonValue &collectibles);
GuildTag guildTag(const QJsonValue &primaryGuild);
NameStyle nameStyle(const QJsonValue &value);
RoleColours roleColours(const QJsonObject &role);

} // namespace Cosmetics
