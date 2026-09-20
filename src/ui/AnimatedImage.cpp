#include "ui/AnimatedImage.h"

#include <QImageReader>
#include <QMovie>

AnimatedImage::AnimatedImage(QObject *parent)
    : QObject(parent)
{
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
}

void AnimatedImage::clear()
{
    teardown();
    m_bytes.clear();
    m_still = QImage();
    emit frameChanged();
}

bool AnimatedImage::setData(const QByteArray &bytes)
{
    teardown();
    m_bytes = bytes;
    m_still = QImage();

    if (m_bytes.isEmpty())
        return false;

    // Ask the image plugins whether this format actually moves. GIF and
    // animated WebP do; PNG does not, because Qt ships no APNG reader.
    m_buffer.setBuffer(&m_bytes);
    m_buffer.open(QIODevice::ReadOnly);

    QImageReader reader(&m_buffer);
    const bool animated = reader.supportsAnimation() && reader.imageCount() > 1;
    m_buffer.seek(0);

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
    return m_still;
}

bool AnimatedImage::isNull() const
{
    return m_movie ? m_movie->currentImage().isNull() : m_still.isNull();
}

void AnimatedImage::setPlaying(bool playing)
{
    if (!m_movie)
        return;
    m_movie->setPaused(!playing);
}
