#pragma once

#include <QFrame>
#include <QHash>
#include <QImage>
#include <QList>
#include <QSet>
#include <QString>

class MessageStore;

// The stage you see while sitting in a call.
//
// A shared screen and a camera are different surfaces. They never share a
// tile: if somebody is on camera and sharing, both pictures sit on the stage
// at once. The featured tile is large; everyone else sits in a strip
// underneath. Click a tile to put that surface on the stage. Until a picture
// arrives the person's face stays, so a black rectangle is not mistaken for
// a working stream.
class CallView : public QFrame
{
    Q_OBJECT

public:
    enum class Surface { Camera, Share };
    Q_ENUM(Surface)

    explicit CallView(MessageStore *store, QWidget *parent = nullptr);

    void setChannel(const QString &channelId);
    QString channelId() const { return m_channelId; }

    void setSpeaking(const QSet<QString> &userIds);
    void setFocusedUser(const QString &userId, Surface surface = Surface::Camera);
    QString focusedUser() const { return m_focusedUser; }

    void setFrame(const QString &userId, const QImage &image, Surface surface);
    void dropFrames(const QString &userId, Surface surface);

    void refresh();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void profileRequested(const QString &userId);
    void watchAttempted(const QString &userId);
    void focusRequested(const QString &userId, CallView::Surface surface);
    void visibilityChanged(bool visible);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Tile
    {
        QString userId;
        QString name;
        QRect box;
        Surface surface = Surface::Camera;
        bool featured = false;
        bool streaming = false;
        bool video = false;
        bool muted = false;
        bool deafened = false;
        bool speaking = false;
    };

    struct Focus
    {
        QString userId;
        Surface surface = Surface::Camera;
    };

    void layoutTiles();
    Focus pickFocus() const;
    bool hasTile(const QString &userId, Surface surface) const;
    const QHash<QString, QImage> &framesFor(Surface surface) const;
    const Tile *tileAt(const QPoint &pos) const;
    void paintTile(QPainter &painter, const Tile &tile) const;
    void paintMarks(QPainter &painter, const Tile &tile) const;

    MessageStore *m_store = nullptr;
    QString m_channelId;
    QString m_focusedUser;
    Surface m_focusedSurface = Surface::Camera;
    QString m_hoverUserId;
    Surface m_hoverSurface = Surface::Camera;
    QList<Tile> m_tiles;
    QSet<QString> m_speaking;
    QHash<QString, QImage> m_cameraFrames;
    QHash<QString, QImage> m_shareFrames;
};
