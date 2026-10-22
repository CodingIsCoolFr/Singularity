#pragma once

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QList>
#include <QObject>

class QMovie;

// Holds one picture that may or may not move.
//
// QImage only ever decodes the first frame, which is why animated avatars and
// banners showed up frozen. This wraps QMovie when the bytes are an animation
// and falls back to a plain QImage when they are not, so callers just ask for
// `currentFrame()` and redraw whenever `frameChanged` fires.
//
// Frames change only on the frame clock's ticks (ui/FrameClock.h): an
// animated PNG is stepped by it, and a GIF's or WebP's next frame is taken up
// at the tick after QMovie reads it. So currentFrame() is the same picture
// for every paint between two ticks, however a window splits its painting.
class AnimatedImage : public QObject
{
    Q_OBJECT

public:
    explicit AnimatedImage(QObject *parent = nullptr);
    ~AnimatedImage() override;

    // Returns true if the bytes decoded into something drawable. The same
    // bytes again change nothing: what is already decoded keeps playing.
    bool setData(const QByteArray &bytes);

    // True when the file has more than one frame.
    //
    // QImageReader::imageCount() stays 0 for a GIF until the frames have been
    // walked, so "count > 1" treated every animated gif as a still.
    static bool isAnimatedData(const QByteArray &bytes);
    void clear();

    QImage currentFrame() const;
    bool isAnimated() const { return m_movie != nullptr || m_apngFrames.size() > 1 || m_apngPending; }
    bool isNull() const;

    // Animation is paused while nothing is on screen, so a hidden profile
    // window costs nothing.
    void setPlaying(bool playing);

signals:
    void frameChanged();

private:
    void teardown();
    void showApng(const QList<QImage> &frames, const QList<int> &delays);

    // Hands finished frames to every player still waiting on those bytes.
    static void apngDecoded(const QByteArray &bytes, const QList<QImage> &frames, const QList<int> &delays);

    QByteArray m_bytes;
    QBuffer m_buffer;
    QMovie *m_movie = nullptr;
    QImage m_still;

    // Animated PNG (Discord's avatar decorations), which QMovie cannot play:
    // decoded by Apng::decode off the window's thread and stepped by the
    // frame clock. Until the frames arrive the first one stands in, as a
    // still.
    void stepApng();
    int apngDelay(int index) const;
    QList<QImage> m_apngFrames;
    QList<int> m_apngDelays;
    int m_apngIndex = 0;
    qint64 m_apngDueAt = 0;     // frame clock time the next frame is due
    qint64 m_apngLeft = -1;     // while paused, how long the frame on show had left
    bool m_apngPending = false;
    bool m_playing = true;

    // A GIF's or WebP's frame as of the last tick.
    void takeMovieFrame();
    QImage m_movieFrame;
};
