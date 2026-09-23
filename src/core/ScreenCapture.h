#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGIOutputDuplication;
class WindowCapture;

// Grabs the contents of a monitor, the way Windows itself wants it grabbed.
//
// This uses Desktop Duplication, which is the API the compositor already
// feeds. The obvious alternatives are worse in ways that matter here:
//
//   Repeatedly asking Qt for a screenshot copies the whole desktop through
//   the CPU every frame, and on a 4K screen at thirty frames a second that
//   alone is most of a core.
//
//   BitBlt from the screen device context is the old way, and on a machine
//   with a compositor it can miss hardware overlays entirely - which is to
//   say video players and many games come out black.
//
// Desktop Duplication hands back the frame the compositor just presented, in
// video memory, and tells us which rectangles changed. It also does something
// none of the others do: when nothing on screen has moved, it says so rather
// than handing over an identical picture. A still desktop then costs nothing.
class ScreenCapture
{
public:
    // Something that can be shared: a whole monitor, or one window.
    struct Monitor {
        QString id;        // "adapter:output", or "window:<hwnd>" for a window
        QString name;      // what to show a person
        int width = 0;
        int height = 0;
        bool primary = false;
        quint32 processId = 0;   // windows only: whose it is, to match its sound
    };

    ScreenCapture() = default;
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture &) = delete;
    ScreenCapture &operator=(const ScreenCapture &) = delete;

    // What there is to share. Safe to call without starting anything.
    static QList<Monitor> monitors();

    // Every window someone could pick, top of the stack first. A window is
    // captured through WindowCapture rather than Desktop Duplication; start()
    // tells the two apart by the id.
    static QList<Monitor> windows();
    static bool isWindowId(const QString &id) { return id.startsWith(QLatin1String("window:")); }

    bool start(const QString &monitorId);
    void stop();
    bool isRunning() const { return m_duplication != nullptr || m_window != nullptr; }
    bool isWindow() const { return m_window != nullptr; }

    int width() const { return m_width; }
    int height() const { return m_height; }

    enum class Result {
        Frame,      // `data` holds a new picture
        NoChange,   // nothing moved; the previous picture still stands
        Lost,       // the duplication died and was rebuilt; expect a new size
        Failed,
    };

    // Waits up to `timeoutMs` for the next picture.
    //
    // On Frame, `data` points at tightly packed BGRA rows that stay valid
    // until the next call. Nothing is copied out of video memory that does not
    // have to be.
    Result grab(int timeoutMs, const uchar **data, int *stride);

private:
    bool build(const QString &monitorId);
    void release();

    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_context = nullptr;
    IDXGIOutputDuplication *m_duplication = nullptr;
    ID3D11Texture2D *m_staging = nullptr;

    // Set instead of the four above when a single window is being shared.
    WindowCapture *m_window = nullptr;

    QString m_monitorId;
    int m_width = 0;
    int m_height = 0;

    // The frame that is still mapped. Kept as a plain pointer and a stride so
    // this header does not have to drag in the Windows and Direct3D ones.
    const uchar *m_lastData = nullptr;
    int m_lastStride = 0;
    bool m_mapped = false;
};
