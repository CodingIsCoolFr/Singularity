#include "ui/ProfilePreview.h"

#include "ui/AnimatedImage.h"
#include "ui/FrameClock.h"
#include "ui/LookPainter.h"
#include "ui/MediaCache.h"
#include "ui/ProfileArt.h"
#include "ui/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>

namespace {

constexpr int Radius = 12;
constexpr int BannerHeight = 106;
constexpr int AvatarSize = 88;    // the frame; the face is 76% of it with a decoration
constexpr int AvatarLeft = 16;

// Discord's profile shows six badges and a count of the rest.
constexpr int BadgesShown = 6;
constexpr int BadgeIcon = 20;
constexpr int BadgeGap = 4;
constexpr int BadgePad = 6;

// Over a profile theme Discord lays this much black, so the words stay
// readable on any two colours.
constexpr qreal ThemeShade = 0.6;

QColor mixed(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

} // namespace

ProfilePreview::ProfilePreview(QWidget *parent)
    : QWidget(parent)
    , m_avatar(new AnimatedImage(this))
    , m_banner(new AnimatedImage(this))
    , m_decoration(new AnimatedImage(this))
{
    setMinimumSize(300, 380);
    connect(m_avatar, &AnimatedImage::frameChanged, this, [this]() { update(); });
    connect(m_banner, &AnimatedImage::frameChanged, this, [this]() { update(); });
    connect(m_decoration, &AnimatedImage::frameChanged, this, [this]() { update(); });
    // A name font arriving, or a tag's badge.
    connect(&LookPainter::instance(), &LookPainter::changed, this, [this]() { update(); });
}

void ProfilePreview::setAvatar(const QByteArray &bytes, const QImage &still)
{
    m_avatarStill = still;
    if (bytes == m_avatarBytes && !bytes.isEmpty())
        return update();
    m_avatarBytes = bytes;
    if (bytes.isEmpty() || !AnimatedImage::isAnimatedData(bytes))
        m_avatar->clear();
    else
        m_avatar->setData(bytes);
    update();
}

void ProfilePreview::setDecoration(const QByteArray &bytes, const QImage &still)
{
    m_decorationStill = still;
    // Decoding an animation is not free; the same bytes again (every picture
    // that lands redraws the card) keep the one already playing.
    if (bytes == m_decorationBytes)
        return update();
    m_decorationBytes = bytes;
    if (bytes.isEmpty() || !AnimatedImage::isAnimatedData(bytes))
        m_decoration->clear();
    else
        m_decoration->setData(bytes);
    update();
}

void ProfilePreview::setBanner(const QByteArray &bytes, const QImage &still)
{
    m_bannerStill = still;
    if (bytes == m_bannerBytes && !bytes.isEmpty())
        return update();
    m_bannerBytes = bytes;
    if (bytes.isEmpty() || !AnimatedImage::isAnimatedData(bytes))
        m_banner->clear();
    else
        m_banner->setData(bytes);
    update();
}

void ProfilePreview::setBannerColour(const QColor &colour)
{
    m_bannerColour = colour;
    update();
}

void ProfilePreview::setNames(const QString &displayName, const QString &username)
{
    m_displayName = displayName;
    m_username = username;
    update();
}

void ProfilePreview::setPronouns(const QString &pronouns)
{
    m_pronouns = pronouns;
    update();
}

void ProfilePreview::setBio(const QString &bio)
{
    m_bio = bio;
    update();
}

void ProfilePreview::setBadges(const QList<QImage> &icons)
{
    // Scaled once here rather than on every paint: the card repaints for each
    // frame of a moving banner.
    const qreal ratio = qMax<qreal>(1.0, devicePixelRatioF());
    m_badgeIcons.clear();
    for (int i = 0; i < qMin<int>(icons.size(), BadgesShown); ++i) {
        QPixmap picture;
        if (!icons.at(i).isNull()) {
            picture = QPixmap::fromImage(icons.at(i).scaled(QSize(BadgeIcon, BadgeIcon) * ratio, Qt::KeepAspectRatio,
                                                            Qt::SmoothTransformation));
            picture.setDevicePixelRatio(ratio);
        }
        m_badgeIcons.append(picture);
    }
    m_moreBadges = qMax(0, int(icons.size()) - BadgesShown);
    update();
}

void ProfilePreview::setNameStyle(const Cosmetics::NameStyle &style)
{
    m_nameStyle = style;
    update();
}

void ProfilePreview::setGuildTag(const Cosmetics::GuildTag &tag)
{
    m_tag = tag;
    update();
}

void ProfilePreview::setTheme(const QColor &primary, const QColor &accent)
{
    m_themePrimary = primary;
    m_themeAccent = accent;
    update();
}

void ProfilePreview::setProfileEffect(const Cosmetics::ProfileEffect &effect)
{
    // The same one again (the card is refreshed for every pick) keeps
    // playing rather than starting over.
    if (effect.isSet() && effect.skuId == m_effectSku)
        return;
    m_effectSku = effect.isSet() ? effect.skuId : QString();
    if (!effect.isSet() && !m_effect)
        return;
    if (!m_effect) {
        m_effect = new ProfileEffectView(this);
        m_effect->show();
    }
    m_effect->setEffect(effect);
}

void ProfilePreview::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QColor card(Theme::SurfaceSidebar);
    const QRectF whole = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    QPainterPath shape;
    shape.addRoundedRect(whole, Radius, Radius);
    // A theme: its two colours top to bottom, darkened for the words.
    const bool themed = m_themePrimary.isValid() && m_themeAccent.isValid();
    const auto themeAt = [this](qreal y) {
        const qreal t = height() > 0 ? qBound<qreal>(0.0, y / height(), 1.0) : 0.0;
        return mixed(mixed(m_themePrimary, m_themeAccent, t), QColor(Qt::black), ThemeShade);
    };
    if (themed) {
        QLinearGradient shade(0, 0, 0, height());
        shade.setColorAt(0, themeAt(0));
        shade.setColorAt(1, themeAt(height()));
        painter.fillPath(shape, shade);
    } else {
        painter.fillPath(shape, card);
    }
    painter.setPen(QColor(Theme::Border));
    painter.drawPath(shape);
    painter.setClipPath(shape);

    // --- banner: the picture if there is one, otherwise the colour.
    //
    // A picture gets Discord's banner shape, five wide by two tall, so on the
    // wide My Account card most of it shows. It used to be squeezed into a
    // 106-pixel strip and drawn past it unclipped, which ran it underneath
    // the name. A plain colour keeps the short strip.
    QImage bannerFrame = m_banner->currentFrame();
    if (bannerFrame.isNull())
        bannerFrame = m_bannerStill;
    const int bannerHeight = bannerFrame.isNull() ? BannerHeight : qMax(BannerHeight, width() * 2 / 5);
    const QRect banner(0, 0, width(), bannerHeight);
    painter.fillRect(banner, m_bannerColour.isValid() ? m_bannerColour : QColor(Theme::SurfaceRail));
    if (!bannerFrame.isNull()) {
        const QImage scaled = bannerFrame.scaled(banner.size(), Qt::KeepAspectRatioByExpanding,
                                                 Qt::SmoothTransformation);
        painter.save();
        painter.setClipRect(banner, Qt::IntersectClip);
        painter.drawImage(QPoint((banner.width() - scaled.width()) / 2, (banner.height() - scaled.height()) / 2),
                          scaled);
        painter.restore();
    }

    // --- avatar, sitting half over the banner in a ring of the card colour.
    const QRect frame(AvatarLeft, BannerHeight - AvatarSize / 2, AvatarSize, AvatarSize);
    QImage decoration = m_decoration->currentFrame();
    if (decoration.isNull())
        decoration = m_decorationStill;
    const bool framed = !decoration.isNull();
    const int face = framed ? qRound(AvatarSize * 0.76) : AvatarSize - 12;
    const QRect faceRect(frame.center().x() - face / 2 + 1, frame.center().y() - face / 2 + 1, face, face);

    painter.setPen(Qt::NoPen);
    painter.setBrush(themed ? themeAt(faceRect.center().y()) : card);
    painter.drawEllipse(faceRect.adjusted(-6, -6, 6, 6));

    QImage avatarFrame = m_avatar->currentFrame();
    if (avatarFrame.isNull())
        avatarFrame = m_avatarStill;
    const QString fallbackName = m_displayName.isEmpty() ? m_username : m_displayName;
    painter.drawPixmap(faceRect.topLeft(), avatarFrame.isNull()
                                               ? MediaCache::initialsAvatar(fallbackName, face)
                                               : MediaCache::circular(avatarFrame, face));
    if (framed)
        painter.drawImage(frame, decoration.scaled(frame.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

    // --- the words, under the avatar and under the banner, whichever is lower.
    int y = qMax(frame.bottom() + 14, banner.bottom() + 12);
    const int left = 16;
    const int textWidth = width() - 2 * left;

    // --- badges, on the right of the name line, in a dark pill.
    int nameWidth = textWidth;
    if (!m_badgeIcons.isEmpty()) {
        QFont count = font();
        count.setPixelSize(12);
        count.setWeight(QFont::DemiBold);
        const QString more = m_moreBadges > 0 ? QStringLiteral("+%1").arg(m_moreBadges) : QString();
        const int moreWidth = more.isEmpty() ? 0 : QFontMetrics(count).horizontalAdvance(more) + BadgeGap;
        const int shown = int(m_badgeIcons.size());
        const int stripWidth = 2 * BadgePad + shown * BadgeIcon + (shown - 1) * BadgeGap + moreWidth;
        const QRectF strip(width() - left - stripWidth, y - 1, stripWidth, 28);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(Theme::SurfaceRail));
        painter.drawRoundedRect(strip, 8, 8);
        qreal x = strip.left() + BadgePad;
        for (const QPixmap &icon : std::as_const(m_badgeIcons)) {
            if (!icon.isNull()) {
                const QSizeF size = QSizeF(icon.size()) / icon.devicePixelRatio();
                painter.drawPixmap(QPointF(x + (BadgeIcon - size.width()) / 2.0,
                                           strip.center().y() - size.height() / 2.0),
                                   icon);
            }
            x += BadgeIcon + BadgeGap;
        }
        if (!more.isEmpty()) {
            painter.setFont(count);
            painter.setPen(QColor(Theme::TextPrimary));
            painter.drawText(QRectF(x, strip.top(), moreWidth, strip.height()), Qt::AlignLeft | Qt::AlignVCenter,
                             more);
        }
        nameWidth = qMax(60, textWidth - stripWidth - 10);
    }

    // The name in its style, always moving, and the server tag after it, as
    // a profile shows them.
    QFont nameFont = font();
    nameFont.setPixelSize(20);
    nameFont.setWeight(QFont::Bold);
    LookPainter &looks = LookPainter::instance();
    LookPainter::Name name;
    name.text = m_displayName.isEmpty() ? m_username : m_displayName;
    name.font = nameFont;
    name.style = m_nameStyle;
    name.mode = LookPainter::EffectMode::Animated;
    name.fallback = QColor(Theme::TextPrimary);
    const int tagSpace = m_tag.isSet() ? looks.tagWidth(m_tag, 16) + 8 : 0;
    const QRect nameBox(left, y, qMax(40, nameWidth - tagSpace), 26);
    const int drawn = looks.paintName(&painter, nameBox, name);
    if (tagSpace)
        looks.paintTag(&painter, QPoint(left + drawn + 8, nameBox.center().y() + 1), m_tag, 16);
    if (LookPainter::moving(name))
        FrameClock::scheduleRepaint(this, 33, nameBox.adjusted(-6, -6, 6, 6));
    y += 26;

    QFont small = font();
    small.setPixelSize(13);
    painter.setFont(small);
    painter.setPen(QColor(Theme::TextMuted));
    QString handle = m_username.isEmpty() ? QString() : m_username;
    if (!m_pronouns.isEmpty())
        handle += (handle.isEmpty() ? QString() : QStringLiteral("  •  ")) + m_pronouns;
    painter.drawText(QRect(left, y, textWidth, 20), Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(small).elidedText(handle, Qt::ElideRight, textWidth));
    y += 30;

    if (!m_bio.trimmed().isEmpty()) {
        QFont label = font();
        label.setPixelSize(11);
        label.setWeight(QFont::Bold);
        painter.setFont(label);
        painter.setPen(QColor(Theme::TextMuted));
        painter.drawText(QRect(left, y, textWidth, 16), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("ABOUT ME"));
        y += 20;

        QFont body = font();
        body.setPixelSize(13);
        painter.setFont(body);
        painter.setPen(QColor(Theme::TextPrimary));
        painter.drawText(QRect(left, y, textWidth, height() - y - 12), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                         m_bio.trimmed());
    }
}
