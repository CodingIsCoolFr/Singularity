#include "ui/ProfilePreview.h"

#include "ui/AnimatedImage.h"
#include "ui/MediaCache.h"
#include "ui/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>

namespace {

constexpr int Radius = 12;
constexpr int BannerHeight = 106;
constexpr int AvatarSize = 88;    // the frame; the face is 76% of it with a decoration
constexpr int AvatarLeft = 16;

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

void ProfilePreview::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QColor card(Theme::SurfaceSidebar);
    const QRectF whole = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    QPainterPath shape;
    shape.addRoundedRect(whole, Radius, Radius);
    painter.fillPath(shape, card);
    painter.setPen(QColor(Theme::Border));
    painter.drawPath(shape);
    painter.setClipPath(shape);

    // --- banner: the picture if there is one, otherwise the colour.
    const QRect banner(0, 0, width(), BannerHeight);
    painter.fillRect(banner, m_bannerColour.isValid() ? m_bannerColour : QColor(Theme::SurfaceRail));
    QImage bannerFrame = m_banner->currentFrame();
    if (bannerFrame.isNull())
        bannerFrame = m_bannerStill;
    if (!bannerFrame.isNull()) {
        const QImage scaled = bannerFrame.scaled(banner.size(), Qt::KeepAspectRatioByExpanding,
                                                 Qt::SmoothTransformation);
        painter.drawImage(QPoint((banner.width() - scaled.width()) / 2, (banner.height() - scaled.height()) / 2),
                          scaled);
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
    painter.setBrush(card);
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

    // --- the words.
    int y = frame.bottom() + 14;
    const int left = 16;
    const int textWidth = width() - 2 * left;

    QFont nameFont = font();
    nameFont.setPixelSize(20);
    nameFont.setWeight(QFont::Bold);
    painter.setFont(nameFont);
    painter.setPen(QColor(Theme::TextPrimary));
    const QString shownName = m_displayName.isEmpty() ? m_username : m_displayName;
    painter.drawText(QRect(left, y, textWidth, 26), Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(nameFont).elidedText(shownName, Qt::ElideRight, textWidth));
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
