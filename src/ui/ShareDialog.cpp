#include "ui/ShareDialog.h"

#include "core/AppConfig.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QStyle>
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
    setWindowTitle(QStringLiteral("Share"));
    setModal(true);
    setMinimumWidth(460);

    m_monitors = ScreenCapture::monitors();
    m_windows = ScreenCapture::windows();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);

    // Discord's two tabs: a whole screen, or one application's window.
    auto *headingRow = new QHBoxLayout;
    headingRow->setSpacing(14);
    auto *heading = new QLabel(QStringLiteral("What to share?"), this);
    heading->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 500;"));
    headingRow->addWidget(heading);
    headingRow->addStretch(1);
    const auto makeTab = [this, headingRow](const QString &text) {
        auto *tab = new QPushButton(text, this);
        tab->setObjectName(QStringLiteral("TabButton"));
        tab->setFlat(true);
        tab->setCursor(Qt::PointingHandCursor);
        headingRow->addWidget(tab);
        return tab;
    };
    m_screensTab = makeTab(QStringLiteral("Screens"));
    m_windowsTab = makeTab(QStringLiteral("Windows"));
    layout->addLayout(headingRow);

    m_list = new QListWidget(this);
    m_list->setFixedHeight(6 * 34 + 8);
    layout->addWidget(m_list);

    auto *qualityLabel = new QLabel(QStringLiteral("How good?"), this);
    qualityLabel->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 500;"));
    layout->addWidget(qualityLabel);

    m_quality = new QComboBox(this);
    // UTF-8, not Latin-1: the labels hold a dash, and reading its three bytes
    // as three Latin-1 letters is what put "â" and two boxes in this list.
    for (const Quality &quality : kQualities)
        m_quality->addItem(QString::fromUtf8(quality.label));
    m_quality->setCurrentIndex(0);
    layout->addWidget(m_quality);

    auto *soundLabel = new QLabel(QStringLiteral("Which sound?"), this);
    soundLabel->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 500;"));
    layout->addWidget(soundLabel);

    // Like Discord: the whole computer, or one program and nothing else.
    // Item data is -1 for none, -2 for everything, otherwise an index into
    // m_apps.
    m_apps = ShareAudio::apps();
    m_sound = new QComboBox(this);
    m_sound->addItem(QStringLiteral("No sound"), -1);
    m_sound->addItem(QStringLiteral("Everything on this computer (not the call)"), -2);
    if (!m_apps.isEmpty())
        m_sound->insertSeparator(m_sound->count());
    for (int i = 0; i < m_apps.size(); ++i) {
        const ShareAudio::App &app = m_apps.at(i);
        const QString title = app.title.size() > 48 ? app.title.left(47) + QChar(0x2026) : app.title;
        m_sound->addItem(app.exeName.isEmpty() ? QStringLiteral("Only %1").arg(title)
                                               : QStringLiteral("Only %1  ·  %2").arg(title, app.exeName),
                         i);
    }

    const QString saved =
        AppConfig::instance().value(QStringLiteral("share/sound"), QStringLiteral("all")).toString();
    int pick = 1;
    if (saved == QLatin1String("none")) {
        pick = 0;
    } else if (saved.startsWith(QLatin1String("app:"))) {
        const QString exe = saved.mid(4);
        for (int i = 0; i < m_apps.size(); ++i) {
            if (m_apps.at(i).exeName.compare(exe, Qt::CaseInsensitive) == 0) {
                pick = m_sound->findData(i);
                break;
            }
        }
    }
    m_sound->setCurrentIndex(pick);
    m_savedSound = pick;
    layout->addWidget(m_sound);

    // Said once, plainly, rather than discovered afterwards.
    auto *note = new QLabel(
        QStringLiteral("Your screen and its sound are sent encrypted, and only to the people "
                       "in this call. The call itself is never sent back to them."),
        this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: %1; font-size: 12.5px;")
                            .arg(QLatin1String(Theme::TextMuted)));
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_goLive = buttons->button(QDialogButtonBox::Ok);
    m_goLive->setText(QStringLiteral("Go live"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(this, &QDialog::accepted, this, [this]() {
        // A window share picks its own program's sound, so it says nothing
        // about what a screen share should default to next time.
        if (m_showingWindows)
            return;
        QString remember = QStringLiteral("all");
        if (soundSource() == ShareAudio::Source::Nothing)
            remember = QStringLiteral("none");
        else if (soundSource() == ShareAudio::Source::OneApp)
            remember = QStringLiteral("app:") + m_apps.at(m_sound->currentData().toInt()).exeName;
        AppConfig::instance().setValue(QStringLiteral("share/sound"), remember);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Double clicking a screen is the obvious way to pick it.
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        if (!currentList().isEmpty())
            accept();
    });

    connect(m_screensTab, &QPushButton::clicked, this, [this]() { showWindows(false); });
    connect(m_windowsTab, &QPushButton::clicked, this, [this]() { showWindows(true); });

    // Picking a window picks its sound too, as Discord does. The sound box
    // can still be changed afterwards.
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!m_showingWindows || row < 0 || row >= m_windows.size())
            return;
        for (int i = 0; i < m_apps.size(); ++i) {
            if (m_apps.at(i).processId == m_windows.at(row).processId) {
                m_sound->setCurrentIndex(m_sound->findData(i));
                return;
            }
        }
    });

    showWindows(false);
}

const QList<ScreenCapture::Monitor> &ShareDialog::currentList() const
{
    return m_showingWindows ? m_windows : m_monitors;
}

void ShareDialog::showWindows(bool windows)
{
    m_showingWindows = windows;

    for (QPushButton *tab : {m_screensTab, m_windowsTab}) {
        tab->setProperty("active", tab == (windows ? m_windowsTab : m_screensTab));
        tab->style()->unpolish(tab);
        tab->style()->polish(tab);
    }

    m_list->clear();
    const QList<ScreenCapture::Monitor> &list = currentList();
    for (const ScreenCapture::Monitor &source : list)
        m_list->addItem(source.name);

    if (list.isEmpty()) {
        m_list->addItem(windows ? QStringLiteral("No windows are open that can be shared.")
                                : QStringLiteral("Windows reported no screen that can be shared."));
        m_list->setEnabled(false);
    } else {
        m_list->setEnabled(true);
    }
    m_goLive->setEnabled(!list.isEmpty());

    // Back on screens, the sound goes back to what screen shares use.
    if (!windows)
        m_sound->setCurrentIndex(m_savedSound);
    if (!list.isEmpty())
        m_list->setCurrentRow(0);
}

QString ShareDialog::monitorId() const
{
    const QList<ScreenCapture::Monitor> &list = currentList();
    const int row = m_list->currentRow();
    if (row < 0 || row >= list.size())
        return {};
    return list.at(row).id;
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

ShareAudio::Source ShareDialog::soundSource() const
{
    const int data = m_sound->currentData().toInt();
    if (data == -2)
        return ShareAudio::Source::EverythingButUs;
    if (data >= 0 && data < m_apps.size())
        return ShareAudio::Source::OneApp;
    return ShareAudio::Source::Nothing;
}

quint32 ShareDialog::soundProcessId() const
{
    const int data = m_sound->currentData().toInt();
    return data >= 0 && data < m_apps.size() ? m_apps.at(data).processId : 0;
}

QString ShareDialog::soundName() const
{
    const int data = m_sound->currentData().toInt();
    if (data >= 0 && data < m_apps.size())
        return m_apps.at(data).exeName.isEmpty() ? m_apps.at(data).title : m_apps.at(data).exeName;
    return data == -2 ? QStringLiteral("all sound") : QString();
}
