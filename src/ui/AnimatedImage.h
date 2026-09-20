#pragma once

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QObject>

class QMovie;

// Holds one picture that may or may not move.
//
// QImage only ever decodes the first frame, which is why animated avatars and
// banners showed up frozen. This wraps QMovie when the bytes are an animation
// and falls back to a plain QImage when they are not, so callers just ask for
// `currentFrame()` and redraw whenever `frameChanged` fires.
class AnimatedImage : public QObject
{
    Q_OBJECT

public:
    explicit AnimatedImage(QObject *parent = nullptr);
    ~AnimatedImage() override;

    // Returns true if the bytes decoded into something drawable.
    bool setData(const QByteArray &bytes);
    void clear();

    QImage currentFrame() const;
    bool isAnimated() const { return m_movie != nullptr; }
    bool isNull() const;

    // Animation is paused while nothing is on screen, so a hidden profile
    // window costs nothing.
    void setPlaying(bool playing);

signals:
    void frameChanged();

private:
    void teardown();

    QByteArray m_bytes;
    QBuffer m_buffer;
    QMovie *m_movie = nullptr;
    QImage m_still;
};
