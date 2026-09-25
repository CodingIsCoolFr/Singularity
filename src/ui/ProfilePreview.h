#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QWidget>

class AnimatedImage;

// The card on the right of Settings > Profiles, the way Discord shows
// "Preview": banner (picture or colour), the round avatar with its decoration
// around it, display name, username, pronouns and About Me. Every setter
// redraws at once, so it follows each keystroke and each pick before
// anything is saved. Moving GIF avatars and banners play.
class ProfilePreview : public QWidget
{
    Q_OBJECT

public:
    explicit ProfilePreview(QWidget *parent = nullptr);

    // `bytes` wins when it is an animation; `still` is the fallback frame.
    void setAvatar(const QByteArray &bytes, const QImage &still);
    void setDecoration(const QImage &decoration);
    void setBanner(const QByteArray &bytes, const QImage &still);
    void setBannerColour(const QColor &colour);
    void setNames(const QString &displayName, const QString &username);
    void setPronouns(const QString &pronouns);
    void setBio(const QString &bio);

    QSize sizeHint() const override { return {300, 420}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    AnimatedImage *m_avatar = nullptr;
    AnimatedImage *m_banner = nullptr;
    QImage m_avatarStill;
    QImage m_bannerStill;
    QImage m_decoration;
    QColor m_bannerColour;
    QString m_displayName;
    QString m_username;
    QString m_pronouns;
    QString m_bio;
};
