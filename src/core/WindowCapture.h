#pragma once

#include <QString>

#include <memory>

// Grabs the contents of one window, the way Discord's "Applications" tab does.
//
// This uses Windows.Graphics.Capture, the API behind the Snipping Tool and
// the Xbox Game Bar. Desktop Duplication (ScreenCapture) can only hand over a
// whole monitor, and cutting a window's rectangle out of that would also cut
// out whatever sits on top of it. Graphics Capture reads the window's own
// surface from the compositor, so it keeps working when the window is covered
// by another, and it sees games and video players that paint with the GPU.
//
// Needs Windows 10 1903 or later. On Windows 11 the yellow capture border is
// switched off where the system allows it.
//
// Same shape as ScreenCapture, so the share worker can hold either: grab()
// hands back tightly packed BGRA rows that stay valid until the next call.
class WindowCapture
{
public:
    WindowCapture();
    ~WindowCapture();

    WindowCapture(const WindowCapture &) = delete;
    WindowCapture &operator=(const WindowCapture &) = delete;

    // `window` is an HWND. Must be called on the thread that will grab.
    bool start(quintptr window);
    void stop();
    bool isRunning() const;

    int width() const;
    int height() const;

    enum class Result { Frame, NoChange, Lost, Failed };
    Result grab(const uchar **data, int *stride);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
