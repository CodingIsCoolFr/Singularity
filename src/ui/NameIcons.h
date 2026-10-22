#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QRect>
#include <QString>

#include <functional>

class QPainter;

// Where a person's name is shown, for the pictures plugins add beside it.
// Avatar is the corner of their picture, where the status dot goes.
enum class NamePlace { MemberList, DirectMessages, FriendsList, Chat, ProfileBadges, Avatar };

// One small picture beside a name, and what it says when pointed at. `id`
// tells two apart; the chat asks for them by it. The image is in device
// pixels with its device pixel ratio set.
struct NameIcon
{
    QString id;
    QImage image;
    QString tooltip;
};

// The pictures plugins add beside people's names, for the parts of the
// window that draw names themselves: the list rows, the chat and the profile
// window. The main window points it at the plugin host; everything else
// only asks here, so none of it needs to know where they come from.
class NameIcons : public QObject
{
    Q_OBJECT

public:
    static NameIcons &instance();

    using Provider = std::function<QList<NameIcon>(const QString &userId, NamePlace place, int size, qreal ratio)>;
    void setProvider(Provider provider) { m_provider = std::move(provider); }

    // `size` is how tall they may be, in pixels, at `ratio` device pixels
    // per pixel.
    QList<NameIcon> icons(const QString &userId, NamePlace place, int size, qreal ratio) const;

    // How wide they are drawn, `gap` apart. 0 for none.
    static int width(const QList<NameIcon> &icons, int gap = 2);
    // Draws them left to right from `left`, centred on `middle`. Returns
    // where each one went, for its tooltip.
    static QList<QRect> paint(QPainter *painter, int left, int middle, const QList<NameIcon> &icons, int gap = 2);

    void announceChange(const QString &userId) { emit changed(userId); }

signals:
    // Whose pictures changed; empty for everyone's.
    void changed(const QString &userId);

private:
    NameIcons() = default;
    Provider m_provider;
};
