#include "ui/ShareDialog.h"

#include "ui/Theme.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// Width, height, frames, bits per second.
//
// The ceiling is 2.5 Mbit because that is roughly where Discord throttles a
// stream from an account without Nitro. Asking for more does not arrive as
// more; it arrives as the same picture with packets thrown away, which looks
// worse than simply having asked for less.
struct Quality {
    const char *label;
    int width;
    int height;
    int fps;
    int bitrate;
};

constexpr Quality kQualities[] = {
    {"1080p, 30 frames — sharpest text", 1920, 1080, 30, 2500000},
    {"1080p, 60 frames — smoothest motion", 1920, 1080, 60, 2500000},
    {"720p, 30 frames — kindest to a slow connection", 1280, 720, 30, 1500000},
    {"720p, 60 frames", 1280, 720, 60, 2000000},
    {"480p, 30 frames — when nothing else holds", 854, 480, 30, 800000},
};

} // namespace

ShareDialog::ShareDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Share your screen"));
    setModal(true);
    setMinimumWidth(420);

    m_monitors = ScreenCapture::monitors();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);

    auto *heading = new QLabel(QStringLiteral("Which screen?"), this);
    heading->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 500;"));
    layout->addWidget(heading);

    m_list = new QListWidget(this);
    m_list->setFixedHeight(m_monitors.isEmpty() ? 60 : qMin(4, m_monitors.size()) * 34 + 8);
    for (const ScreenCapture::Monitor &monitor : std::as_const(m_monitors))
        m_list->addItem(monitor.name);

    if (m_monitors.isEmpty()) {
        m_list->addItem(QStringLiteral("Windows reported no screen that can be shared."));
        m_list->setEnabled(false);
    } else {
        m_list->setCurrentRow(0);
    }
    layout->addWidget(m_list);

    auto *qualityLabel = new QLabel(QStringLiteral("How good?"), this);
    qualityLabel->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 500;"));
    layout->addWidget(qualityLabel);

    m_quality = new QComboBox(this);
    for (const Quality &quality : kQualities)
        m_quality->addItem(QString::fromLatin1(quality.label));
    m_quality->setCurrentIndex(0);
    layout->addWidget(m_quality);

    // Said once, plainly, rather than discovered afterwards. Discord's own
    // client is no different and does not mention it either.
    auto *note = new QLabel(
        QStringLiteral("Your screen is sent encrypted, and only to the people in this call. "
                       "Sound from the screen is not shared — only your microphone."),
        this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: %1; font-size: 12.5px;")
                            .arg(QLatin1String(Theme::TextMuted)));
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Go live"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(!m_monitors.isEmpty());
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Double clicking a screen is the obvious way to pick it.
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        if (!m_monitors.isEmpty())
            accept();
    });
}

QString ShareDialog::monitorId() const
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_monitors.size())
        return {};
    return m_monitors.at(row).id;
}

int ShareDialog::width() const
{
    return kQualities[qBound(0, m_quality->currentIndex(), int(std::size(kQualities)) - 1)].width;
}

int ShareDialog::height() const
{
    return kQualities[qBound(0, m_quality->currentIndex(), int(std::size(kQualities)) - 1)].height;
}

int ShareDialog::frameRate() const
{
    return kQualities[qBound(0, m_quality->currentIndex(), int(std::size(kQualities)) - 1)].fps;
}

int ShareDialog::bitrate() const
{
    return kQualities[qBound(0, m_quality->currentIndex(), int(std::size(kQualities)) - 1)].bitrate;
}
