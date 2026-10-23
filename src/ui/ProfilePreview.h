#pragma once

#include "core/Cosmetics.h"

#include <QColor>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QWidget>

class AnimatedImage;
class ProfileEffectView;

// The card on the right of Settings > Profiles, the way Discord shows
// "Preview": banner (picture or colour), the round avatar with its decoration
// around it, display name in its style with the server tag beside it,
// username, pronouns and About Me, on the profile's theme colours with its
// profile effect over it. Every setter redraws at once, so it follows each
// keystroke and each pick before anything is saved. Moving GIF avatars and
// banners play.
class ProfilePreview : public QWidget
{
    Q_OBJECT

public:
    explicit ProfilePreview(QWidget *parent = nullptr);

    // `bytes` wins when it is an animation; `still` is the fallback frame.
    void setAvatar(const QByteArray &bytes, const QImage &still);
    // `bytes` is the animated PNG when there is one; `still` otherwise.
    void setDecoration(const QByteArray &bytes, const QImage &still);
    void setBanner(const QByteArray &bytes, const QImage &still);
    void setBannerColour(const QColor &colour);
    void setNames(const QString &displayName, const QString &username);
    void setPronouns(const QString &pronouns);
    void setBio(const QString &bio);
    // Your badges in the order your profile shows them, beside the name:
    // Discord's six, then "+N" for the rest.
    void setBadges(const QList<QImage> &icons);
    // The name's font, effect and colours, and the server tag worn beside it.
    void setNameStyle(const Cosmetics::NameStyle &style);
    void setGuildTag(const Cosmetics::GuildTag &tag);
    // A profile theme: the card shades from `primary` at the top to `accent`
    // at the bottom. Invalid colours for none.
    void setTheme(const QColor &primary, const QColor &accent);
    // Played over the card, as on a profile. An empty one clears it.
    void setProfileEffect(const Cosmetics::ProfileEffect &effect);

    QSize sizeHint() const override { return {300, 420}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    AnimatedImage *m_avatar = nullptr;
    AnimatedImage *m_banner = nullptr;
    AnimatedImage *m_decoration = nullptr;
    QImage m_avatarStill;
    QImage m_bannerStill;
    QImage m_decorationStill;
    QByteArray m_decorationBytes;
    QByteArray m_avatarBytes;
    QByteArray m_bannerBytes;
    QColor m_bannerColour;
    QString m_displayName;
    QString m_username;
    QString m_pronouns;
    QString m_bio;
    QList<QPixmap> m_badgeIcons;   // already at size, the first six
    int m_moreBadges = 0;
    Cosmetics::NameStyle m_nameStyle;
    Cosmetics::GuildTag m_tag;
    QColor m_themePrimary;
    QColor m_themeAccent;
    ProfileEffectView *m_effect = nullptr;
    QString m_effectSku;
};
