#pragma once

#include "plugin/Plugin.h"

#include <QString>

// Reshapes your microphone before it is sent.
//
// The headline is stereo, and it is the one thing the official client cannot
// do at all: Discord folds your microphone down to mono before encoding it, so
// there is no left or right left to place. Wisp already encodes two channels,
// which is what let Ripcord do this, so the pan control here genuinely puts
// your voice in one ear of everybody listening.
//
// The rest is an ordinary broadcast chain, in the order a studio would use it,
// because each stage depends on the one before:
//
//   high-pass  ->  gate  ->  compressor  ->  makeup  ->  pan
//
// Rumble is removed first so it cannot hold the gate open. The gate runs
// before the compressor so the compressor is not busy lifting silence up to
// speaking level. Makeup gain restores what the compressor took. Placing
// happens last, on the finished sound.
//
// Everything here is written out by hand in fixed arithmetic with no
// allocation, because it runs fifty times a second inside the audio path.
class MicShaperPlugin : public Plugin
{
public:
    QString id() const override { return QStringLiteral("mic-shaper"); }
    QString name() const override { return QStringLiteral("Mic shaper"); }
    QString description() const override
    {
        return QStringLiteral("Stereo placement, rumble filter, noise gate and compressor for your "
                              "microphone.");
    }
    bool enabledByDefault() const override { return false; }

    void onLoad(PluginContext *context) override;
    void onMicrophoneFrame(qint16 *samples, int frames, int channels, int sampleRate) override;
    QWidget *createSettingsWidget(QWidget *parent) override;

private:
    void reloadSettings();
    void resetState();

    // --- settings, copied out of the config so the audio path never reads it
    bool m_highPassOn = true;
    int m_highPassHz = 90;

    bool m_gateOn = true;
    double m_gateOpenLevel = 0.012;   // amplitude, not decibels
    int m_gateHoldMs = 180;

    bool m_compressorOn = true;
    double m_threshold = 0.18;
    double m_ratio = 4.0;

    double m_makeup = 1.0;   // 1.0 is unchanged
    int m_pan = 0;           // -100 hard left, 0 centre, +100 hard right
    bool m_forceMono = false;

    // --- running state, carried between slices ---------------------------

    // One pole high-pass, per channel: keeps the last input and output so the
    // filter does not restart at every slice boundary, which would click.
    double m_hpLastIn[2] = {0.0, 0.0};
    double m_hpLastOut[2] = {0.0, 0.0};
    double m_hpCoefficient = 0.0;

    // Gate.
    bool m_gateOpen = false;
    int m_gateHeldFrames = 0;
    double m_gateGain = 0.0;

    // Compressor. Gain moves smoothly rather than jumping, or every syllable
    // would be heard being grabbed.
    double m_compressorGain = 1.0;
};
