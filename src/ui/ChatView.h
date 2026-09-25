#pragma once

#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QSet>
#include <QTextBrowser>
#include <QTimer>

class QMediaPlayer;

class AnimatedImage;

// The message view.
//
// A plain QTextBrowser never fetches remote pictures, so avatars, emoji and
// image attachments would stay blank. This subclass routes those requests
// through MediaCache, rounds avatars into circles, shrinks large pictures so
// one photo cannot take over the column, and drives animated pictures frame by
// frame, which QTextBrowser will not do on its own.
class ChatView : public QTextBrowser
{
    Q_OBJECT

public:
    explicit ChatView(QWidget *parent = nullptr);
    ~ChatView() override;

    void clearImageCache();

    // Turning this off freezes every moving picture on the first frame.
    void setAnimationsEnabled(bool enabled);

    static bool isAllowedImageHost(const QUrl &url);

    // How much of the last layout went on shrinking and rounding pictures,
    // and how many had to be done from scratch. Reading it clears it.
    //
    // Forty messages took 57 ms to lay out where a hundred plain ones take 14,
    // so the cost is not per message, and guessing which part it is has been
    // wrong twice already.
    qint64 takePictureWorkMs();
    int takePicturesPrepared();

protected:
    QVariant loadResource(int type, const QUrl &name) override;

private:
    void pumpAnimations();
    void adoptAnimation(const QUrl &url);
    void adoptVideo(const QUrl &url);
    void showVideoFrame(const QUrl &url, const QImage &frame);
    QPixmap prepare(const QUrl &url) const;
    QPixmap scaleForDocument(const QUrl &url, const QImage &source) const;
    QSize boxFor(const QUrl &url) const;

    void flushArrivals();

    QSet<QString> m_wanted;
    QHash<QString, AnimatedImage *> m_animations;

    // gifv cards are mp4s. One player each, muted and looping.
    QHash<QString, QMediaPlayer *> m_videos;

    // When each video last had a frame turned into a picture, so a 60 fps
    // clip is not copied off the graphics card sixty times a second.
    QHash<QString, qint64> m_videoFrameAt;

    // Videos whose frames could not be copied. They stay a still picture
    // for the rest of this run instead of being started again.
    QSet<QString> m_videosGivenUp;

    // The size each picture settled on, so later frames never change the page
    // height and make the view jump.
    mutable QHash<QString, QSize> m_frameSize;

    // Pictures already shrunk, rounded and turned into pixmaps.
    //
    // The text engine asks for every picture again on every layout pass, and
    // answering meant a smooth rescale and, for avatars, a fresh circular mask
    // each time. A hundred messages is a hundred of those, and that - not the
    // markup - was most of the wait when a channel opened.
    mutable QHash<QString, QPixmap> m_prepared;
    mutable qint64 m_preparedBytes = 0;

    mutable qint64 m_pictureWorkNs = 0;
    mutable int m_picturesPrepared = 0;

    // Pictures that landed since the last redraw. They are handled in one
    // batch, because each one arriving on its own used to lay the whole
    // conversation out again.
    QSet<QString> m_arrived;
    QTimer m_arrivalTimer;

    QTimer m_animationTimer;
    bool m_animationsEnabled = true;
};
