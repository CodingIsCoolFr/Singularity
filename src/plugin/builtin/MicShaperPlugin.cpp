#include "plugin/builtin/MicShaperPlugin.h"

#include "core/Logger.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

namespace {

constexpr double Pi = 3.14159265358979323846;

QSlider *makeSlider(int low, int high, int value, QWidget *parent)
{
    auto *slider = new QSlider(Qt::Horizontal, parent);
    slider->setRange(low, high);
    slider->setValue(value);
    return slider;
}

QLabel *hint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                             .arg(QLatin1String(Theme::TextFaint)));
    return label;
}

} // namespace

void MicShaperPlugin::onLoad(PluginContext *context)
{
    Plugin::onLoad(context);
    reloadSettings();
    resetState();
}

void MicShaperPlugin::resetState()
{
    m_hpLastIn[0] = m_hpLastIn[1] = 0.0;
    m_hpLastOut[0] = m_hpLastOut[1] = 0.0;
    m_gateOpen = false;
    m_gateHeldFrames = 0;
    m_gateGain = 0.0;
    m_compressorGain = 1.0;
}

void MicShaperPlugin::reloadSettings()
{
    if (!context())
        return;

    const auto get = [this](const char *key, const QVariant &fallback) {
        return context()->setting(QLatin1String(key), fallback);
    };

    m_highPassOn = get("highPassOn", true).toBool();
    m_highPassHz = get("highPassHz", 90).toInt();

    m_gateOn = get("gateOn", true).toBool();
    // The slider is 0 to 100 and means "how loud before the gate opens".
    // Squared so the low end, where speech actually sits, has room to move.
    {
        const double raw = get("gateLevel", 12).toInt() / 100.0;
        m_gateOpenLevel = raw * raw * 0.5;
    }
    m_gateHoldMs = get("gateHoldMs", 180).toInt();

    m_compressorOn = get("compressorOn", true).toBool();
    m_threshold = get("threshold", 18).toInt() / 100.0;
    m_ratio = qMax(1.0, get("ratio", 4).toInt() * 1.0);

    m_makeup = get("makeup", 100).toInt() / 100.0;
    m_pan = qBound(-100, get("pan", 0).toInt(), 100);
    m_forceMono = get("forceMono", false).toBool();

    // One pole high-pass. The coefficient is how much of the previous output
    // carries forward, worked out from the cutoff and the sample rate.
    const double cutoff = qBound(20, m_highPassHz, 400);
    const double rc = 1.0 / (2.0 * Pi * cutoff);
    const double dt = 1.0 / 48000.0;
    m_hpCoefficient = rc / (rc + dt);
}

void MicShaperPlugin::onMicrophoneFrame(qint16 *samples, int frames, int channels, int sampleRate)
{
    Q_UNUSED(sampleRate)

    if (channels < 1 || frames < 1)
        return;

    const int used = qMin(channels, 2);

    // ---- 1. High-pass -------------------------------------------------
    //
    // Takes out everything below the cutoff: desk thumps, footsteps, the hum
    // of a computer fan, the low rumble a hand makes on a boom arm. None of it
    // carries speech, all of it makes a gate misfire and a compressor duck.
    if (m_highPassOn) {
        for (int channel = 0; channel < used; ++channel) {
            double lastIn = m_hpLastIn[channel];
            double lastOut = m_hpLastOut[channel];

            for (int frame = 0; frame < frames; ++frame) {
                const int index = frame * channels + channel;
                const double in = samples[index];
                const double out = m_hpCoefficient * (lastOut + in - lastIn);
                lastIn = in;
                lastOut = out;
                samples[index] = static_cast<qint16>(qBound(-32768.0, out, 32767.0));
            }

            m_hpLastIn[channel] = lastIn;
            m_hpLastOut[channel] = lastOut;
        }
    }

    // How loud this slice is, measured once and used by both the gate and the
    // compressor below.
    double sum = 0.0;
    const int total = frames * channels;
    for (int i = 0; i < total; ++i) {
        const double value = samples[i] / 32768.0;
        sum += value * value;
    }
    const double level = std::sqrt(sum / total);

    // ---- 2. Noise gate ------------------------------------------------
    //
    // Silence between sentences stays silent, so a fan or a keyboard is not
    // broadcast while nobody is talking.
    //
    // The hold is the part that matters. A gate that shuts the instant you
    // stop chops the ends off words and makes every pause sound like a dropped
    // connection, so once open it stays open for a while after you go quiet.
    // The gain also slides rather than switching, because a hard cut is heard
    // as a click.
    if (m_gateOn) {
        const int holdFrames = (m_gateHoldMs / 20) + 1;   // slices, at 20 ms each

        if (level >= m_gateOpenLevel) {
            m_gateOpen = true;
            m_gateHeldFrames = holdFrames;
        } else if (m_gateHeldFrames > 0) {
            --m_gateHeldFrames;
        } else {
            m_gateOpen = false;
        }

        const double target = m_gateOpen ? 1.0 : 0.0;
        const double step = 1.0 / 4.0;   // fully open or shut in about 80 ms
        if (m_gateGain < target)
            m_gateGain = qMin(target, m_gateGain + step);
        else if (m_gateGain > target)
            m_gateGain = qMax(target, m_gateGain - step);

        if (m_gateGain < 0.999) {
            for (int i = 0; i < total; ++i)
                samples[i] = static_cast<qint16>(samples[i] * m_gateGain);
        }
    }

    // ---- 3. Compressor ------------------------------------------------
    //
    // Holds the loud parts down so the quiet parts can come up, which is why
    // radio voices sound even whether the speaker leans in or sits back.
    //
    // Gain falls quickly and recovers slowly, matching how a real compressor
    // behaves: catching a sudden shout has to be immediate, but letting go
    // quickly would make the room noise swell between words.
    if (m_compressorOn && level > 0.0) {
        double wanted = 1.0;
        if (level > m_threshold) {
            const double over = level / m_threshold;
            wanted = std::pow(over, (1.0 / m_ratio) - 1.0);
        }

        const double rate = wanted < m_compressorGain ? 0.4 : 0.05;
        m_compressorGain += (wanted - m_compressorGain) * rate;

        if (std::fabs(m_compressorGain - 1.0) > 0.001) {
            for (int i = 0; i < total; ++i) {
                samples[i] = static_cast<qint16>(
                    qBound(-32768.0, samples[i] * m_compressorGain, 32767.0));
            }
        }
    }

    // ---- 4. Makeup gain -----------------------------------------------
    if (std::fabs(m_makeup - 1.0) > 0.001) {
        for (int i = 0; i < total; ++i)
            samples[i] = static_cast<qint16>(qBound(-32768.0, samples[i] * m_makeup, 32767.0));
    }

    // ---- 5. Placing ---------------------------------------------------
    //
    // The part the official client cannot do at all, because it throws the
    // second channel away before encoding. Wisp sends two, so this really does
    // put your voice in one ear of everybody listening.
    if (channels < 2)
        return;

    if (m_forceMono || m_pan != 0) {
        // Collapse to one signal first. Most microphones fill only one channel
        // or fill both with the same thing, and panning a half empty stereo
        // pair would place silence rather than a voice.
        for (int frame = 0; frame < frames; ++frame) {
            const int left = frame * channels;
            const int right = left + 1;
            const double mono = (static_cast<double>(samples[left]) + samples[right]) * 0.5;
            samples[left] = static_cast<qint16>(mono);
            samples[right] = static_cast<qint16>(mono);
        }
    }

    if (m_pan == 0)
        return;

    // Constant power: both sides at once would be louder in the middle than at
    // either edge, so the two gains are taken off a quarter circle instead of
    // a straight line and the total loudness stays put as you move.
    const double position = (m_pan + 100) / 200.0;   // 0 is left, 1 is right
    const double leftGain = std::cos(position * Pi / 2.0);
    const double rightGain = std::sin(position * Pi / 2.0);

    for (int frame = 0; frame < frames; ++frame) {
        const int left = frame * channels;
        const int right = left + 1;
        samples[left] = static_cast<qint16>(samples[left] * leftGain);
        samples[right] = static_cast<qint16>(samples[right] * rightGain);
    }
}

QWidget *MicShaperPlugin::createSettingsWidget(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    const auto save = [this](const char *key, const QVariant &value) {
        if (context())
            context()->setSetting(QLatin1String(key), value);
        reloadSettings();
    };

    // ---- placing -------------------------------------------------------
    auto *pan = makeSlider(-100, 100, m_pan, page);
    auto *panLabel = new QLabel(page);

    const auto describePan = [panLabel](int value) {
        if (value == 0)
            panLabel->setText(QStringLiteral("Centre, both ears"));
        else if (value <= -99)
            panLabel->setText(QStringLiteral("Hard left, left ear only"));
        else if (value >= 99)
            panLabel->setText(QStringLiteral("Hard right, right ear only"));
        else
            panLabel->setText(QStringLiteral("%1% to the %2")
                                  .arg(std::abs(value))
                                  .arg(value < 0 ? QStringLiteral("left") : QStringLiteral("right")));
    };
    describePan(m_pan);
    panLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                .arg(QLatin1String(Theme::TextMuted)));

    auto *placing = new QFormLayout;
    placing->addRow(QStringLiteral("Where you sit"), pan);
    layout->addLayout(placing);
    layout->addWidget(panLabel);
    layout->addWidget(hint(QStringLiteral(
        "The official client cannot do this. It folds your microphone down to one channel before "
        "sending, so there is no left or right left to place. Wisp sends two, which is what lets "
        "your voice actually arrive in one ear."), page));

    auto *mono = new QCheckBox(QStringLiteral("Force mono first"), page);
    mono->setChecked(m_forceMono);
    layout->addWidget(mono);
    layout->addWidget(hint(QStringLiteral(
        "For a microphone that only fills one channel, which otherwise sounds like it is already "
        "stuck in one ear."), page));

    // ---- cleanup -------------------------------------------------------
    auto *highPass = new QCheckBox(QStringLiteral("Cut low rumble"), page);
    highPass->setChecked(m_highPassOn);
    layout->addWidget(highPass);

    auto *cutoff = makeSlider(40, 200, m_highPassHz, page);
    auto *cutoffForm = new QFormLayout;
    cutoffForm->addRow(QStringLiteral("Cut below"), cutoff);
    layout->addLayout(cutoffForm);
    layout->addWidget(hint(QStringLiteral(
        "Removes desk thumps, footsteps and fan hum. None of it carries speech, and all of it "
        "makes the gate and the compressor misbehave, so it goes first."), page));

    auto *gate = new QCheckBox(QStringLiteral("Silence the gaps"), page);
    gate->setChecked(m_gateOn);
    layout->addWidget(gate);

    auto *gateLevel = makeSlider(0, 100,
                                 context() ? context()->setting(QStringLiteral("gateLevel"), 12).toInt() : 12,
                                 page);
    auto *gateForm = new QFormLayout;
    gateForm->addRow(QStringLiteral("Opens above"), gateLevel);
    layout->addLayout(gateForm);
    layout->addWidget(hint(QStringLiteral(
        "Keeps your microphone shut between sentences. It stays open for a moment after you stop, "
        "so the ends of words are not chopped off."), page));

    auto *compressor = new QCheckBox(QStringLiteral("Even out my volume"), page);
    compressor->setChecked(m_compressorOn);
    layout->addWidget(compressor);

    auto *makeup = makeSlider(50, 300, static_cast<int>(m_makeup * 100), page);
    auto *makeupForm = new QFormLayout;
    makeupForm->addRow(QStringLiteral("Then boost by"), makeup);
    layout->addLayout(makeupForm);
    layout->addWidget(hint(QStringLiteral(
        "Holds the loud parts down so the quiet parts can come up, which is why radio voices sound "
        "even whether the speaker leans in or sits back. The boost puts back what it took."), page));

    layout->addStretch(1);

    QObject::connect(pan, &QSlider::valueChanged, page, [save, describePan](int value) {
        describePan(value);
        save("pan", value);
    });
    QObject::connect(mono, &QCheckBox::toggled, page, [save](bool on) { save("forceMono", on); });
    QObject::connect(highPass, &QCheckBox::toggled, page, [save](bool on) { save("highPassOn", on); });
    QObject::connect(cutoff, &QSlider::valueChanged, page, [save](int value) { save("highPassHz", value); });
    QObject::connect(gate, &QCheckBox::toggled, page, [save](bool on) { save("gateOn", on); });
    QObject::connect(gateLevel, &QSlider::valueChanged, page, [save](int value) { save("gateLevel", value); });
    QObject::connect(compressor, &QCheckBox::toggled, page, [save](bool on) { save("compressorOn", on); });
    QObject::connect(makeup, &QSlider::valueChanged, page, [save](int value) { save("makeup", value); });

    return page;
}
