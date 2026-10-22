#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

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

// The two cosmetics worn on a profile itself rather than on a name. A profile
// names them by SKU only (user_profile.collectibles: { sku_id, type }, type 1
// an effect and 3 a frame); what they are made of is the shop's item for
// that SKU, GET /collectibles-products/{sku}, whose effect items keep
// Discord's camelCase names.

// One picture of a profile effect: an animation laid over the profile, shown
// from `start` for `duration` milliseconds after the effect begins and, when
// it loops, again every duration + loopDelay.
struct EffectLayer
{
    QString src;
    QStringList randomSources;   // one is picked, the same one for every layer
    bool loop = false;
    int start = 0;
    int duration = 0;
    int loopDelay = 0;
    int x = 0;                   // where it sits on the profile, in pixels
    int y = 0;
    int zIndex = 0;
};

struct ProfileEffect
{
    QString skuId;
    QString label;               // Discord's description of it, for a tooltip
    QList<EffectLayer> layers;   // lowest first
    bool isSet() const { return !layers.isEmpty(); }
};

// One picture of a profile frame. A "staple" is drawn whole, at the top or
// bottom of the card; a "rail" fills the card's height, its picture held
// against the anchor; a "border" is its picture repeated down the card. A
// responsive rail shows only on a card at least 480 tall for 300 across.
struct FrameLayer
{
    QString id;                  // collectibles-shop/<sku>/<id>/static
    QString type;
    bool front = true;           // over the card; otherwise behind it
    QString anchor;              // "top", "bottom" or "center"
    bool responsive = false;
};

// Every layer is as wide as the card plus overflowHorizontal each side, at a
// scale of card width / innerWidth, and the frame reaches overflowTop above
// the card and overflowBottom below it.
struct ProfileFrame
{
    QString skuId;
    QString label;
    int innerWidth = 0;
    int overflowTop = 0;
    int overflowBottom = 0;
    int overflowHorizontal = 0;
    QList<FrameLayer> layers;
    bool isSet() const { return innerWidth > 0 && !layers.isEmpty(); }
};

Decoration decoration(const QJsonValue &value);
Nameplate nameplate(const QJsonValue &collectibles);
GuildTag guildTag(const QJsonValue &primaryGuild);
NameStyle nameStyle(const QJsonValue &value);
RoleColours roleColours(const QJsonObject &role);

// The SKU of the effect or frame a profile block (user_profile,
// guild_member_profile) wears. Empty for none.
QString profileEffectSku(const QJsonObject &profile);
QString profileFrameSku(const QJsonObject &profile);

// The effect or frame a /collectibles-products/{sku} reply describes. Not
// set when the reply is for something else.
ProfileEffect profileEffect(const QJsonObject &product);
ProfileFrame profileFrame(const QJsonObject &product);

} // namespace Cosmetics
