#include "ui/AnimatedImage.h"

#include "ui/ApngDecoder.h"
#include "ui/FrameClock.h"

#include <QImageReader>
#include <QMovie>
#include <QPointer>
#include <QThreadPool>

namespace {

// Animated PNGs already taken apart, the most recently used last.
//
// Opening a profile decoded its avatar decoration, every frame of it, on the
// window's thread, and did it again whenever any other picture finished
// downloading. In a busy server that is dozens of times a minute, and the
// window froze for seconds at a time (2026-10-21, 07:09 and 07:41). Now each
// decoration is decoded once, off the window's thread, and the next card that
// shows it starts playing at once.
struct DecodedApng
{
    QByteArray bytes;
    QList<QImage> frames;
    QList<int> delays;
    qint64 cost = 0;
};
QList<DecodedApng> g_decoded;

// What the list may hold. A decoration at the size the cards ask for is 6 to
// 12 MB of frames; frames a card is showing are shared with it, not copied.
constexpr qint64 DecodedBudget = 32 * 1024 * 1024;

// Decodes under way, and the players waiting on each.
struct ApngJob
{
    QByteArray bytes;
    QList<QPointer<AnimatedImage>> waiting;
};
QList<ApngJob> g_jobs;

template <typename List>
qsizetype findBytes(const List &list, const QByteArray &bytes)
{
    for (qsizetype i = 0; i < list.size(); ++i) {
        const QByteArray &held = list.at(i).bytes;
        if (held.size() == bytes.size() && (held.constData() == bytes.constData() || held == bytes))
            return i;
    }
    return -1;
}

// Lives on the window's thread, so a finished decode is handed over there.
QObject &relay()
{
    static QObject object;
    return object;
}

} // namespace

AnimatedImage::AnimatedImage(QObject *parent)
    : QObject(parent)
{
}

int AnimatedImage::apngDelay(int index) const
{
    return qMax(10, m_apngDelays.value(index, 100));
}

void AnimatedImage::stepApng()
{
    const int count = int(m_apngFrames.size());
    if (count < 2 || !m_playing)
        return;
    // Every frame whose time has come, each due when the last one ended, so
    // a frame shows for its own delay and the animation never drifts. More
    // than a whole loop behind (the window was busy) it carries on from now.
    const qint64 now = FrameClock::now();
    int index = m_apngIndex;
    qint64 due = m_apngDueAt;
    for (int steps = 0; due <= now + 1 && steps < count; ++steps) {
        index = (index + 1) % count;
        due += apngDelay(index);
    }
    if (due <= now + 1)
        due = now + apngDelay(index);
    m_apngDueAt = due;
    FrameClock::wakeAt(this, m_apngDueAt, [this]() { stepApng(); });
    if (index != m_apngIndex) {
        m_apngIndex = index;
        emit frameChanged();
    }
}

void AnimatedImage::takeMovieFrame()
{
    if (!m_movie)
        return;
    m_movieFrame = m_movie->currentImage();
    emit frameChanged();
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
    FrameClock::cancel(this);
    m_movieFrame = QImage();
    m_apngLeft = -1;
    m_apngFrames.clear();
    m_apngDelays.clear();
    m_apngIndex = 0;
    m_apngPending = false;
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
    // The same picture again. Cards hand their pictures over again whenever
    // anything they show may have changed, and starting over each time threw
    // away a playing animation and decoded it from the beginning.
    if (!bytes.isEmpty() && bytes.size() == m_bytes.size()
        && (bytes.constData() == m_bytes.constData() || bytes == m_bytes)) {
        return !isNull();
    }

    teardown();
    m_bytes = bytes;
    m_still = QImage();
    m_playing = true;

    if (m_bytes.isEmpty())
        return false;

    // Animated PNG: Qt's own readers only see frame one, so ui/ApngDecoder
    // takes it apart - on a worker thread, because a decoration has around a
    // hundred frames and that took a few hundred milliseconds on the window's.
    if (Apng::isAnimated(m_bytes)) {
        const qsizetype done = findBytes(g_decoded, m_bytes);
        if (done >= 0) {
            g_decoded.move(done, g_decoded.size() - 1);
            const DecodedApng &entry = g_decoded.constLast();
            showApng(entry.frames, entry.delays);
            return true;
        }

        // Frame one stands in meanwhile, from Qt's ordinary PNG reader.
        m_still.loadFromData(m_bytes, "PNG");
        m_apngPending = true;

        const qsizetype job = findBytes(g_jobs, m_bytes);
        if (job >= 0) {
            g_jobs[job].waiting.append(this);
        } else {
            g_jobs.append(ApngJob{m_bytes, {QPointer<AnimatedImage>(this)}});
            relay();
            const QByteArray work = m_bytes;
            QThreadPool::globalInstance()->start([work]() {
                QList<QImage> frames;
                QList<int> delays;
                for (const Apng::Frame &frame : Apng::decode(work)) {
                    frames.append(frame.image);
                    delays.append(frame.delayMs);
                }
                QMetaObject::invokeMethod(
                    &relay(), [work, frames, delays]() { AnimatedImage::apngDecoded(work, frames, delays); },
                    Qt::QueuedConnection);
            });
        }
        emit frameChanged();
        return true;
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
            // QMovie reads frames on its own timer; each is shown from the
            // next tick, with everything else that changes then.
            connect(m_movie, &QMovie::frameChanged, this, [this](int) {
                if (!FrameClock::booked(this))
                    FrameClock::wakeAt(this, FrameClock::now() + 1, [this]() { takeMovieFrame(); });
            });
            m_movie->start();
            m_movieFrame = m_movie->currentImage();
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

void AnimatedImage::showApng(const QList<QImage> &frames, const QList<int> &delays)
{
    m_apngPending = false;
    m_apngFrames = frames;
    m_apngDelays = delays;
    m_apngIndex = 0;
    m_apngLeft = -1;
    m_apngDueAt = FrameClock::now() + apngDelay(0);
    if (m_playing && m_apngFrames.size() > 1)
        FrameClock::wakeAt(this, m_apngDueAt, [this]() { stepApng(); });
    emit frameChanged();
}

void AnimatedImage::apngDecoded(const QByteArray &bytes, const QList<QImage> &frames, const QList<int> &delays)
{
    const qsizetype job = findBytes(g_jobs, bytes);
    const QList<QPointer<AnimatedImage>> waiting = job >= 0 ? g_jobs.takeAt(job).waiting
                                                            : QList<QPointer<AnimatedImage>>();

    // A broken file comes back with no frames, and the first one stays up as
    // a still picture.
    const bool moves = frames.size() > 1;
    if (moves) {
        qint64 cost = 0;
        for (const QImage &frame : frames)
            cost += frame.sizeInBytes();
        g_decoded.append(DecodedApng{bytes, frames, delays, cost});

        qint64 total = 0;
        for (const DecodedApng &entry : g_decoded)
            total += entry.cost;
        while (total > DecodedBudget && g_decoded.size() > 1) {
            total -= g_decoded.constFirst().cost;
            g_decoded.removeFirst();
        }
    }

    for (const QPointer<AnimatedImage> &player : waiting) {
        // Gone, or showing something else by now.
        if (!player || !player->m_apngPending || player->m_bytes.size() != bytes.size()
            || player->m_bytes != bytes) {
            continue;
        }
        if (moves) {
            player->showApng(frames, delays);
        } else {
            player->m_apngPending = false;
            emit player->frameChanged();
        }
    }
}

QImage AnimatedImage::currentFrame() const
{
    if (m_movie)
        return m_movieFrame.isNull() ? m_movie->currentImage() : m_movieFrame;
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
    m_playing = playing;
    if (m_apngFrames.size() > 1) {
        // Asked for on every paint, so cheap when nothing changes. Paused,
        // the frame on show keeps the time it had left.
        const bool running = FrameClock::booked(this);
        if (playing && !running) {
            m_apngDueAt = FrameClock::now() + (m_apngLeft >= 0 ? m_apngLeft : apngDelay(m_apngIndex));
            m_apngLeft = -1;
            FrameClock::wakeAt(this, m_apngDueAt, [this]() { stepApng(); });
        } else if (!playing && running) {
            m_apngLeft = qMax<qint64>(0, m_apngDueAt - FrameClock::now());
            FrameClock::cancel(this);
        }
        return;
    }
    if (!m_movie)
        return;
    m_movie->setPaused(!playing);
}
