#pragma once

#include <QFrame>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

class MessageStore;

// The grid of tiles you see while sitting in a call.
//
// One tile per person, laid out the way the real client does it: a large round
// picture in the middle, the name along the bottom, a green edge while they
// are talking, and marks for muted, deafened and sharing a screen.
//
// Where a camera or a shared screen would be, the tile shows the person's
// picture and says so. Wisp carries the sound of a call but not the video, and
// a tile that quietly showed a photo would suggest the stream was simply
// blank. Saying it plainly is better than a black rectangle.
class CallView : public QFrame
{
    Q_OBJECT

public:
    explicit CallView(MessageStore *store, QWidget *parent = nullptr);

    // Which channel is being shown, and who is in it. Passing an empty channel
    // hides the view.
    void setChannel(const QString &channelId);
    QString channelId() const { return m_channelId; }

    void setSpeaking(const QSet<QString> &userIds);

    // Rebuilds from the store. Cheap enough to call whenever anything about
    // the call changes.
    void refresh();

signals:
    void profileRequested(const QString &userId);
    void watchAttempted(const QString &userId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Tile
    {
        QString userId;
        QString name;
        QRect box;
        bool streaming = false;
        bool video = false;
        bool muted = false;
        bool deafened = false;
        bool speaking = false;
    };

    void layoutTiles();

    MessageStore *m_store = nullptr;
    QString m_channelId;
    QList<Tile> m_tiles;
    QSet<QString> m_speaking;
};
