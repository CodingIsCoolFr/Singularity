#include "ui/AudioMeter.h"

#include "core/Logger.h"
#include "ui/Theme.h"

#include <QAudioDevice>
#include <QAudioSource>
#include <QMediaDevices>
#include <QPainter>

#include <cmath>

// ---------------------------------------------------------------------------
// The device the microphone writes into
// ---------------------------------------------------------------------------

// Qt hands captured audio to a QIODevice. Rather than store it, this one
// measures each chunk and throws it away.
class AudioMeter::Sink : public QIODevice
{
public:
    explicit Sink(AudioMeter *owner)
        : QIODevice(owner)
        , m_owner(owner)
    {
        open(QIODevice::WriteOnly);
    }

protected:
    qint64 readData(char *, qint64) override { return 0; }

    qint64 writeData(const char *data, qint64 length) override
    {
        // 16 bit signed samples, which is the format asked for below.
        const auto *samples = reinterpret_cast<const qint16 *>(data);
        const qint64 count = length / static_cast<qint64>(sizeof(qint16));
        if (count <= 0)
            return length;

        // Root mean square is a fair measure of loudness, and steadier than
        // simply taking the biggest sample.
        double sum = 0.0;
        for (qint64 i = 0; i < count; ++i) {
            const double value = samples[i] / 32768.0;
            sum += value * value;
        }

        emit m_owner->levelChanged(std::sqrt(sum / static_cast<double>(count)));
        return length;
    }

private:
    AudioMeter *m_owner = nullptr;
};

// ---------------------------------------------------------------------------
// AudioMeter
// ---------------------------------------------------------------------------

AudioMeter::AudioMeter(QObject *parent)
    : QObject(parent)
{
}

AudioMeter::~AudioMeter()
{
    stop();
}

void AudioMeter::start(const QByteArray &deviceId)
{
    stop();

    QAudioDevice chosen = QMediaDevices::defaultAudioInput();
    if (!deviceId.isEmpty()) {
        const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
        for (const QAudioDevice &device : inputs) {
            if (device.id() == deviceId) {
                chosen = device;
                break;
            }
        }
    }

    if (chosen.isNull()) {
        wlog(QStringLiteral("audio"), QStringLiteral("no microphone available"));
        return;
    }

    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    if (!chosen.isFormatSupported(format)) {
        format = chosen.preferredFormat();
        wlog(QStringLiteral("audio"), QStringLiteral("falling back to the device's own format"));
    }

    m_sink = new Sink(this);
    m_source = new QAudioSource(chosen, format, this);
    m_source->start(m_sink);

    wlog(QStringLiteral("audio"), QStringLiteral("listening to %1").arg(chosen.description()));
}

void AudioMeter::stop()
{
    if (m_source) {
        m_source->stop();
        delete m_source;
        m_source = nullptr;
    }
    if (m_sink) {
        m_sink->close();
        delete m_sink;
        m_sink = nullptr;
    }
    m_smoothed = 0.0;
    emit levelChanged(0.0);
}

// ---------------------------------------------------------------------------
// LevelBar
// ---------------------------------------------------------------------------

LevelBar::LevelBar(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(10);
    setMinimumWidth(120);
}

void LevelBar::setLevel(qreal level)
{
    // Rises fast, falls slowly, so a quick word still registers.
    const qreal clamped = qBound(0.0, level * 3.0, 1.0);
    m_level = clamped > m_level ? clamped : m_level * 0.82 + clamped * 0.18;
    update();
}

void LevelBar::setThreshold(qreal threshold)
{
    m_threshold = qBound(0.0, threshold, 1.0);
    update();
}

void LevelBar::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    const QRectF track(0, 0, width(), height());
    painter.setBrush(QColor(Theme::SurfaceInput));
    painter.drawRoundedRect(track, height() / 2.0, height() / 2.0);

    if (m_level > 0.005) {
        // Green once loud enough to count as speaking, orange below it.
        const bool speaking = m_level >= m_threshold;
        painter.setBrush(QColor(speaking ? Theme::Green : Theme::Accent));

        QRectF filled = track;
        filled.setWidth(track.width() * m_level);
        painter.drawRoundedRect(filled, height() / 2.0, height() / 2.0);
    }

    // The mark showing where "loud enough" begins.
    painter.setBrush(QColor(Theme::TextPrimary));
    const qreal x = track.width() * m_threshold;
    painter.drawRect(QRectF(x - 1.0, 0, 2.0, height()));
}
