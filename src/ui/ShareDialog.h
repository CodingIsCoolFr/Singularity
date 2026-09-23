#pragma once

#include "core/ScreenCapture.h"
#include "core/ShareAudio.h"

#include <QDialog>
#include <QList>
#include <QString>

class QComboBox;
class QListWidget;
class QPushButton;

// Picks what to share, and how well.
//
// Deliberately short. The moment somebody reaches for this they are already
// mid-conversation with people waiting, so it asks the two things that cannot
// be guessed and nothing else.
class ShareDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ShareDialog(QWidget *parent = nullptr);

    QString monitorId() const;
    int width() const;
    int height() const;
    int frameRate() const;
    int bitrate() const;

    // Whose sound goes out with the picture. Remembered for next time,
    // except for a single program, which may not be running then.
    ShareAudio::Source soundSource() const;
    quint32 soundProcessId() const;
    QString soundName() const;

    // False when Windows reported nothing that can be captured at all, which
    // is worth saying rather than showing an empty list.
    bool hasScreens() const { return !m_monitors.isEmpty() || !m_windows.isEmpty(); }

private:
    void showWindows(bool windows);
    const QList<ScreenCapture::Monitor> &currentList() const;

    QList<ScreenCapture::Monitor> m_monitors;
    QList<ScreenCapture::Monitor> m_windows;
    bool m_showingWindows = false;
    QPushButton *m_screensTab = nullptr;
    QPushButton *m_windowsTab = nullptr;
    QPushButton *m_goLive = nullptr;
    int m_savedSound = 1;
    QListWidget *m_list = nullptr;
    QComboBox *m_quality = nullptr;
    QList<ShareAudio::App> m_apps;
    QComboBox *m_sound = nullptr;
};
