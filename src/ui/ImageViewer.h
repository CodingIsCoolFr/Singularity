#pragma once

#include <QDialog>
#include <QPointF>
#include <QUrl>

#include <functional>

class AnimatedImage;
class ClipPlayer;
class QAudioOutput;
class QLabel;
class QMediaPlayer;
class QNetworkAccessManager;
class QPushButton;
class QSlider;
class QStackedLayout;
class QVideoWidget;

// The big view you get when a picture in chat is clicked.
//
// Scroll to zoom around the pointer, drag to move, double click to flip
// between fitting the window and full size. Escape closes it.
//
// A video attachment plays here too, with its sound: play and pause, a bar to
// seek, the time and the volume. Space plays and pauses; the arrow keys skip
// five seconds.
class ImageViewer : public QDialog
{
    Q_OBJECT

public:
    explicit ImageViewer(QWidget *parent = nullptr);

    // `url` must be a Discord host, the same rule the message view follows.
    void showImage(const QUrl &url);

    // A video attachment, played with its sound. Same host rule.
    void showVideo(const QUrl &url);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    class Canvas;
    ClipPlayer *m_clip = nullptr;   // when what is shown is a clip, not a picture

    void updateZoomLabel();
    void saveAs();
    void saveVideo();
    void presentWindow();

    // Back to showing pictures: the player is stopped and its controls go.
    void stopVideo();
    bool playingVideo() const;
    void togglePlay();
    void updateTime();

    Canvas *m_canvas = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QLabel *m_nameLabel = nullptr;
    QWidget *m_pictureTools = nullptr;   // zoom and fit, hidden for a video
    QUrl m_url;

    // Made the first time a video is opened, so a viewer that only ever shows
    // pictures never starts the media machinery.
    QStackedLayout *m_stack = nullptr;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QVideoWidget *m_videoWidget = nullptr;
    QWidget *m_videoBar = nullptr;
    QPushButton *m_playButton = nullptr;
    QSlider *m_seek = nullptr;
    QLabel *m_timeLabel = nullptr;
    QSlider *m_volume = nullptr;
    QNetworkAccessManager *m_network = nullptr;
};
