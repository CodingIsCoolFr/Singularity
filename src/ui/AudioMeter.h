#pragma once

#include <QAudioFormat>
#include <QIODevice>
#include <QObject>
#include <QWidget>

class QAudioSource;

// Listens to a microphone and reports how loud it is, from 0 to 1.
//
// This is the piece that makes the settings page honest: the device list and
// the level bar are reading a real microphone, not pretending.
class AudioMeter : public QObject
{
    Q_OBJECT

public:
    explicit AudioMeter(QObject *parent = nullptr);
    ~AudioMeter() override;

    // Empty id means "whatever Windows calls the default".
    void start(const QByteArray &deviceId);
    void stop();
    bool isRunning() const { return m_source != nullptr; }

signals:
    // Loudness of the last chunk, already smoothed a little so the bar does
    // not flicker.
    void levelChanged(qreal level);

private:
    class Sink;

    QAudioSource *m_source = nullptr;
    Sink *m_sink = nullptr;
    qreal m_smoothed = 0.0;
};

// A simple horizontal bar that fills up with the microphone level, with a
// marker showing where the speaking threshold sits.
class LevelBar : public QWidget
{
    Q_OBJECT

public:
    explicit LevelBar(QWidget *parent = nullptr);

    void setLevel(qreal level);
    void setThreshold(qreal threshold);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    qreal m_level = 0.0;
    qreal m_threshold = 0.15;
};
