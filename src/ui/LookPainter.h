#pragma once

#include "core/Cosmetics.h"
#include "core/MessageStore.h"
#include "ui/FrameClock.h"

#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QUrl>

class AnimatedImage;
class QPainter;

// Draws what Discord dresses people in, the way Discord's own client draws
// it (web build 643186: its components and stylesheets, read for this):
//
//   - nameplates, the picture and colour wash behind a person's row in the
//     member list, the direct message list, the friends list and your own
//     panel;
//   - avatar decorations, the frame round a picture;
//   - server tags, the small chip of a server's badge and four letters;
//   - display name styles, the font and effect of a name (solid, gradient,
//     neon, toon, pop, glow, prism, gummy);
//   - gradient role colours, in servers with enhanced role colours.
//
// One instance for the whole window. Every picture it needs is fetched
// through MediaCache and the fonts from discord.com; `changed` says when
// something new has arrived and the views should paint again.
class LookPainter : public QObject
{
    Q_OBJECT

public:
    static LookPainter &instance();

    void setStore(MessageStore *store) { m_store = store; }
    PersonLook look(const QString &userId, const QString &guildId = QString()) const;

    // Discord's places for a nameplate. Each washes and fades it its own way.
    enum class Placement { MemberList, Channel, FriendsList, Account };

    // The nameplate behind a row. `contentRight` is where the row's own
    // content ends, from the row's left: the picture there shows faint and
    // comes up to full strength over the next 50 pixels, as in Discord.
    void paintNameplate(QPainter *painter, const QRect &row, const Cosmetics::Nameplate &plate, int contentRight,
                        Placement placement, bool hovered, bool selected, qreal radius = 7);

    // The frame round a picture drawn in `avatar`. Moving while `animate`.
    void paintDecoration(QPainter *painter, const QRect &avatar, const Cosmetics::Decoration &decoration,
                         bool animate);

    // A server tag's chip: badge and tag, 16 pixels tall. `left` is its left
    // edge and vertical middle. Returns the width it took.
    int tagWidth(const Cosmetics::GuildTag &tag, int badgeSize = 14) const;
    int paintTag(QPainter *painter, const QPoint &left, const Cosmetics::GuildTag &tag, int badgeSize = 14);

    // How a name is drawn where it appears.
    enum class EffectMode {
        FontOnly,   // a server: the person's font, their role's colour
        Plain,      // Discord's PLAIN: the font, in ordinary text colour
        Static,     // the effect, holding still
        Animated    // the effect, moving
    };
    struct Name
    {
        QString text;
        QFont font;                       // size and weight of the place it sits in
        Cosmetics::NameStyle style;
        EffectMode mode = EffectMode::FontOnly;
        quint32 roleColour = 0;
        Cosmetics::RoleColours roleColours;
        bool roleGradient = false;
        bool animateRole = false;         // a gradient role slides while hovered
        QColor fallback;                  // the colour with no role and no effect
    };

    // The font a style asks for, sized like `base`. `base` itself until it
    // has arrived.
    QFont fontFor(const QFont &base, int fontId) const;
    int nameWidth(const Name &name) const;
    // Paints within `box`, vertically centred, eliding to fit. Returns the
    // width drawn.
    int paintName(QPainter *painter, const QRect &box, const Name &name);
    // The same name as a picture, for places that cannot paint, at the
    // device pixel ratio given, with `pad` pixels round it for an effect's
    // outline or glow to fit in.
    QImage renderName(const Name &name, qreal ratio, int pad = 6);
    QImage renderTag(const Cosmetics::GuildTag &tag, int badgeSize, qreal ratio);

    // Whether something drawn with these needs another frame soon: an
    // animation that is playing.
    static bool moving(const Name &name);

    // The frame clock's time (ui/FrameClock.h), held still through a paint,
    // so every row and every piece of a split paint shows the same moment.
    qint64 now() const { return FrameClock::now(); }

    // A name the way Discord writes it into a member row or a message: the
    // look's font and colours, in the mode that place uses.
    Name nameFor(const QString &text, const QFont &font, const PersonLook &look, EffectMode mode,
                 const QColor &fallback) const;

signals:
    void changed();

private:
    LookPainter();

    struct Font
    {
        const char *family;
        const char *file;       // discord.com/assets/<file>
        qreal letterSpacing;    // em
        int weight;             // the one weight it is made in
    };
    static const Font *fontInfo(int fontId);
    void loadFont(int fontId) const;
    void installFont(int fontId, const QByteArray &woff2);

    QImage picture(const QUrl &url) const;
    QImage frame(const QUrl &url);

    // A style's colours, made readable on Singularity's dark background the
    // way Discord's client does it.
    QList<QColor> styleColours(const Cosmetics::NameStyle &style) const;

    void paintEffect(QPainter *painter, const QPointF &baseline, const QFont &font, const Name &name,
                     const QString &text);

    MessageStore *m_store = nullptr;

    mutable QNetworkAccessManager m_network;
    mutable QSet<int> m_fontsAsked;
    QHash<int, QString> m_families;              // font id -> installed family

    QHash<QString, QPointer<AnimatedImage>> m_players;   // url -> its player
};
