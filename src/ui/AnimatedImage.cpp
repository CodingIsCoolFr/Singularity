#include "ui/AnimatedImage.h"

#include "ui/ApngDecoder.h"

#include <QImageReader>
#include <QMovie>

AnimatedImage::AnimatedImage(QObject *parent)
    : QObject(parent)
{
    m_apngTimer.setSingleShot(true);
    connect(&m_apngTimer, &QTimer::timeout, this, [this]() {
        if (m_apngFrames.size() < 2)
            return;
        m_apngIndex = (m_apngIndex + 1) % int(m_apngFrames.size());
        m_apngTimer.start(m_apngDelays.value(m_apngIndex, 100));
        emit frameChanged();
    });
}

AnimatedImage::~AnimatedImage()
{
    teardown();
}

void AnimatedImage::teardown()
{
    if (m_movie) {
        m_movie->stop();
        delete m_movie;
        m_movie = nullptr;
    }
    if (m_buffer.isOpen())
        m_buffer.close();
    m_apngTimer.stop();
    m_apngFrames.clear();
    m_apngDelays.clear();
    m_apngIndex = 0;
}

void AnimatedImage::clear()
{
    teardown();
    m_bytes.clear();
    m_still = QImage();
    emit frameChanged();
}

bool AnimatedImage::isAnimatedData(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return false;
    if (Apng::isAnimated(bytes))
        return true;

    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);

    QImageReader reader(&buffer);
    const QByteArray format = reader.format().toLower();
    if (format != "gif" && format != "webp")
        return false;

    const int counted = reader.imageCount();
    if (counted > 1)
        return true;
    if (counted == 1)
        return false;

    // A GIF's count is 0 until something actually steps to the next frame.
    if (reader.read().isNull())
        return false;
    return reader.jumpToNextImage();
}

bool AnimatedImage::setData(const QByteArray &bytes)
{
    teardown();
    m_bytes = bytes;
    m_still = QImage();

    if (m_bytes.isEmpty())
        return false;

    // Animated PNG: decoded here, since Qt's own readers only see frame one.
    if (Apng::isAnimated(m_bytes)) {
        const QList<Apng::Frame> frames = Apng::decode(m_bytes);
        if (frames.size() > 1) {
            for (const Apng::Frame &frame : frames) {
                m_apngFrames.append(frame.image);
                m_apngDelays.append(frame.delayMs);
            }
            m_apngTimer.start(m_apngDelays.first());
            emit frameChanged();
            return true;
        }
    }

    // Ask the image plugins whether this format actually moves. GIF and
    // animated WebP do; PNG does not, because Qt ships no APNG reader.
    const bool animated = isAnimatedData(m_bytes);
    m_buffer.setBuffer(&m_bytes);
    m_buffer.open(QIODevice::ReadOnly);

    if (animated) {
        m_movie = new QMovie(&m_buffer, QByteArray(), this);
        if (m_movie->isValid()) {
            m_movie->setCacheMode(QMovie::CacheAll);
            connect(m_movie, &QMovie::frameChanged, this, [this](int) { emit frameChanged(); });
            m_movie->start();
            emit frameChanged();
            return true;
        }
        // Not usable after all, so fall through to a still picture.
        delete m_movie;
        m_movie = nullptr;
        m_buffer.seek(0);
    }

    m_still.loadFromData(m_bytes);
    m_buffer.close();

    emit frameChanged();
    return !m_still.isNull();
}

QImage AnimatedImage::currentFrame() const
{
    if (m_movie)
        return m_movie->currentImage();
    if (!m_apngFrames.isEmpty())
        return m_apngFrames.at(m_apngIndex);
    return m_still;
}

bool AnimatedImage::isNull() const
{
    if (!m_apngFrames.isEmpty())
        return false;
    return m_movie ? m_movie->currentImage().isNull() : m_still.isNull();
}

void AnimatedImage::setPlaying(bool playing)
{
    if (m_apngFrames.size() > 1) {
        if (playing && !m_apngTimer.isActive())
            m_apngTimer.start(m_apngDelays.value(m_apngIndex, 100));
        else if (!playing)
            m_apngTimer.stop();
        return;
    }
    if (!m_movie)
        return;
    m_movie->setPaused(!playing);
}
