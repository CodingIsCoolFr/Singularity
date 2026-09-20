#include "plugin/builtin/MicShaperPlugin.h"

#include "core/Logger.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <functional>

namespace {

constexpr double Pi = 3.14159265358979323846;

QLabel *hint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; line-height: 150%;")
                             .arg(QLatin1String(Theme::TextFaint)));
    return label;
}

// Where your voice sits, as something you move rather than a number.
//
// A slider labelled "pan" tells you nothing about what you will hear. Two ears
// with a dot between them tells you immediately, and the ears fill in as the
// dot passes so the effect is visible before anyone has to join a call to
// check it.
class StereoPad : public QWidget
{
public:
    explicit StereoPad(int pan, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_pan(qBound(-100, pan, 100))
    {
        setFixedHeight(120);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Drag to move your voice. Double click to centre it."));
    }

    int pan() const { return m_pan; }
    std::function<void(int)> onMoved;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF area = rect().adjusted(0.5, 0.5, -0.5, -0.5);

        painter.setPen(QPen(QColor(Theme::Border), 1));
        painter.setBrush(QColor(Theme::SurfaceChat));
        painter.drawRoundedRect(area, 12, 12);

        const double centreY = area.center().y();
        const double left = area.left() + 52;
        const double right = area.right() - 52;
        const double position = left + (m_pan + 100) / 200.0 * (right - left);

        // How much of your voice reaches each side, the same quarter circle
        // the sound itself uses, so the picture cannot disagree with the ears.
        const double share = (m_pan + 100) / 200.0;
        const double leftShare = std::cos(share * Pi / 2.0);
        const double rightShare = std::sin(share * Pi / 2.0);

        drawEar(painter, QPointF(area.left() + 28, centreY), leftShare, QStringLiteral("L"));
        drawEar(painter, QPointF(area.right() - 28, centreY), rightShare, QStringLiteral("R"));

        // The track the voice slides along.
        // Braces, not brackets, or this reads as a function declaration.
        QPen track{QColor(Theme::SurfaceHover)};
        track.setWidthF(3);
        track.setCapStyle(Qt::RoundCap);
        painter.setPen(track);
        painter.drawLine(QPointF(left, centreY), QPointF(right, centreY));

        // The middle, marked so centre is findable by feel.
        painter.setPen(QPen(QColor(Theme::TextFaint), 1, Qt::DotLine));
        painter.drawLine(QPointF(area.center().x(), centreY - 16),
                         QPointF(area.center().x(), centreY + 16));

        // You.
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(Theme::Accent));
        painter.drawEllipse(QPointF(position, centreY), 11, 11);

        painter.setPen(QColor(Theme::Dark));
        QFont face = font();
        face.setPixelSize(10);
        face.setWeight(QFont::Bold);
        painter.setFont(face);
        painter.drawText(QRectF(position - 11, centreY - 11, 22, 22), Qt::AlignCenter,
                         QStringLiteral("You"));

        // What it means, in words, under the track.
        painter.setPen(QColor(Theme::TextMuted));
        QFont caption = font();
        caption.setPixelSize(11);
        painter.setFont(caption);
        painter.drawText(QRectF(area.left(), centreY + 26, area.width(), 20),
                         Qt::AlignCenter, describe());
    }

    void mousePressEvent(QMouseEvent *event) override { moveTo(event->position().x()); }
    void mouseMoveEvent(QMouseEvent *event) override { moveTo(event->position().x()); }
    void mouseDoubleClickEvent(QMouseEvent *) override { apply(0); }

private:
    QString describe() const
    {
        if (m_pan == 0)
            return QStringLiteral("Both ears");
        if (m_pan <= -95)
            return QStringLiteral("Left ear only");
        if (m_pan >= 95)
            return QStringLiteral("Right ear only");
        return QStringLiteral("Mostly %1").arg(m_pan < 0 ? QStringLiteral("left")
                                                         : QStringLiteral("right"));
    }

    void drawEar(QPainter &painter, const QPointF &centre, double share, const QString &letter) const
    {
        // The ring fills as more of your voice arrives on that side.
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(Theme::SurfaceHover), 2));
        painter.drawEllipse(centre, 17, 17);

        QPen filled{QColor(Theme::Accent)};
        filled.setWidthF(2.5);
        filled.setCapStyle(Qt::RoundCap);
        painter.setPen(filled);
        painter.drawArc(QRectF(centre.x() - 17, centre.y() - 17, 34, 34), 90 * 16,
                        -static_cast<int>(share * 360 * 16));

        painter.setPen(QColor(share > 0.25 ? Theme::TextPrimary : Theme::TextFaint));
        QFont mark = painter.font();
        mark.setPixelSize(12);
        mark.setWeight(QFont::DemiBold);
        painter.setFont(mark);
        painter.drawText(QRectF(centre.x() - 17, centre.y() - 17, 34, 34), Qt::AlignCenter, letter);
    }

    void moveTo(double x)
    {
        const double left = rect().left() + 52;
        const double right = rect().right() - 52;
        if (right <= left)
            return;

        const double share = qBound(0.0, (x - left) / (right - left), 1.0);
        int wanted = static_cast<int>(std::lround(share * 200.0)) - 100;

        // Centre is a place people want to get back to, and hitting one exact
        // pixel with a mouse is luck. Anything near the middle counts as the
        // middle; the same for the two ends, so "left ear only" is reachable
        // without pressing against the edge of the widget.
        if (std::abs(wanted) <= 4)
            wanted = 0;
        else if (wanted <= -96)
            wanted = -100;
        else if (wanted >= 96)
            wanted = 100;

        apply(wanted);
    }

    void apply(int pan)
    {
        const int wanted = qBound(-100, pan, 100);
        if (wanted == m_pan)
            return;
        m_pan = wanted;
        update();
        if (onMoved)
            onMoved(m_pan);
    }

    int m_pan = 0;
};

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
    // Straight to the config file, not through the context. The settings page
    // is usable while the plugin is switched off, and the context does not
    // exist then.
    const auto get = [this](const char *key, const QVariant &fallback) {
        return settingValue(QLatin1String(key), fallback);
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

    // Proof, roughly twice a second.
    //
    // "It does not seem to work" is impossible to answer from the outside:
    // the sound has already left by the time anyone can say so. These two
    // numbers say whether this ran at all, and whether the two channels really
    // are different afterwards. If they are equal, nothing was placed.
    if (++m_reportCounter >= 100) {
        m_reportCounter = 0;

        double leftSum = 0.0;
        double rightSum = 0.0;
        for (int frame = 0; frame < frames; ++frame) {
            const double l = samples[frame * channels] / 32768.0;
            const double r = samples[frame * channels + 1] / 32768.0;
            leftSum += l * l;
            rightSum += r * r;
        }

        wlog(QStringLiteral("mic"),
             QStringLiteral("shaped: left %1%, right %2% (pan %3, channels %4)")
                 .arg(std::sqrt(leftSum / frames) * 100.0, 0, 'f', 1)
                 .arg(std::sqrt(rightSum / frames) * 100.0, 0, 'f', 1)
                 .arg(m_pan)
                 .arg(channels));
    }
}

QWidget *MicShaperPlugin::createSettingsWidget(QWidget *parent)
{
    // The page can be opened while the plugin is switched off, and nothing has
    // loaded its saved values in that case. Without this the controls show the
    // built-in defaults rather than what you last chose.
    reloadSettings();

    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // Saves whether the plugin is running or not, then applies at once, so a
    // slider moved during a call is heard immediately.
    const auto save = [this](const char *key, const QVariant &value) {
        setSettingValue(QLatin1String(key), value);
        reloadSettings();
    };

    layout->setSpacing(14);

    // ---- where your voice sits -----------------------------------------
    //
    // One thing to drag, and it shows what it does. Eleven controls were the
    // honest way to describe the chain and the wrong way to offer it.
    auto *heading = new QLabel(QStringLiteral("Where your voice sits"), page);
    heading->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 600;")
                               .arg(QLatin1String(Theme::TextPrimary)));
    layout->addWidget(heading);

    auto *pad = new StereoPad(m_pan, page);
    pad->onMoved = [save](int value) { save("pan", value); };
    layout->addWidget(pad);

    layout->addWidget(hint(QStringLiteral(
        "Drag to move. Double click to put yourself back in the middle.\n\n"
        "The official client cannot do this at all: it folds your microphone down to one channel "
        "before sending, so there is no left or right left to place. Wisp sends two, which is what "
        "lets your voice actually arrive in one ear."), page));

    // ---- the one switch ------------------------------------------------
    auto *cleanUp = new QCheckBox(QStringLiteral("Clean up my voice"), page);
    cleanUp->setChecked(m_highPassOn || m_gateOn || m_compressorOn);
    layout->addWidget(cleanUp);

    layout->addWidget(hint(QStringLiteral(
        "Cuts rumble from desks and fans, keeps the microphone shut between sentences, and evens "
        "out how loud you are so leaning back does not make you disappear. Set the way a broadcast "
        "desk would be, so there is nothing to tune."), page));

    layout->addStretch(1);

    QObject::connect(cleanUp, &QCheckBox::toggled, page, [save](bool on) {
        // One switch, three stages. They are useless apart: a gate with no
        // rumble filter opens on a desk thump, and a compressor with no gate
        // spends its time lifting room noise to speaking level.
        save("highPassOn", on);
        save("gateOn", on);
        save("compressorOn", on);
    });

    return page;
}
