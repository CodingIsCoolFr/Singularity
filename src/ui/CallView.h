#pragma once

#include <QFrame>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPoint>
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

    // The call keeps running. This only hides the stage, which is what
    // happens on the friends page: the controls stay, the grid does not
    // sit on top of the list.
    void setStageSuppressed(bool suppressed);

    void setSpeaking(const QSet<QString> &userIds);
    void setFocusedUser(const QString &userId, Surface surface = Surface::Camera);
    QString focusedUser() const { return m_focusedUser; }

    void setFrame(const QString &userId, const QImage &image, Surface surface);
    void dropFrames(const QString &userId, Surface surface);

    void refresh();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // Which cameras are on screen, and how big. The voice connection asks
    // Discord only for those, and for a small copy when the tile is small.
    // A camera missing from the list is not downloaded at all.
    void videoViewsChanged(const QHash<QString, int> &pixelsByUser);

    void profileRequested(const QString &userId);
    void volumeMenuRequested(const QString &userId, const QPoint &globalPos);
    void watchAttempted(const QString &userId);
    void focusRequested(const QString &userId, CallView::Surface surface);
    void visibilityChanged(bool visible);

    // What sits on the big tile, each time that changes. Empty when the stage
    // is hidden. Your own share uses it to send a full sized picture only
    // while you are actually looking at it.
    void stageChanged(const QString &userId, CallView::Surface surface);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

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
        bool visible = false;
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
    void paintStripControls(QPainter &painter) const;
    void scrollStrip(int pixels);
    void publishViews();
    bool videoOnly() const;

    MessageStore *m_store = nullptr;

    // The row under the stage. It scrolls sideways instead of squeezing
    // everyone in: forty people in a 150 pixel strip is forty slivers.
    QRect m_stripRect;
    int m_stripOffset = 0;
    int m_stripMaxOffset = 0;
    int m_stripStep = 0;
    QRect m_leftArrow;
    QRect m_rightArrow;

    // Big calls show only the people with video, the way Discord does.
    // -1 follows the size of the call; 0 and 1 are the user's own choice.
    int m_videoOnlyChoice = -1;
    int m_peopleInCall = 0;
    int m_peopleWithVideo = 0;
    mutable QRect m_filterChip;   // placed while painting, where its text is measured

    QHash<QString, int> m_lastViews;
    bool m_viewsPublished = false;
    QString m_lastStageUser;
    Surface m_lastStageSurface = Surface::Camera;
    QString m_channelId;
    bool m_stageSuppressed = false;
    QString m_focusedUser;
    Surface m_focusedSurface = Surface::Camera;
    QString m_hoverUserId;
    Surface m_hoverSurface = Surface::Camera;
    QList<Tile> m_tiles;
    QSet<QString> m_speaking;
    QHash<QString, QImage> m_cameraFrames;
    QHash<QString, QImage> m_shareFrames;
};
