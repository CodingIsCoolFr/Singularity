#pragma once

#include <QObject>
#include <QRect>

#include <functional>

class QWidget;

// The clock every animation runs by.
//
// Everything that moves - profile effects, avatar decorations, nameplates,
// animated pictures, name styles, and whatever moves next - keeps two rules
// by going through here.
//
// One moment per picture. now() holds still while a window paints and while
// the clock ticks, so everything drawn for one moment agrees, however the
// paint is split up. Something with frames keeps the frame it chose at its
// last tick and paints that, never a frame worked out from the time of the
// paint. The profile effect once worked its frame out while painting: when
// an animated name under it repainted its own strip, the effect drew a newer
// frame in just that strip - a band across the name - and took that frame as
// shown, so the rest of the card never got it. Half the frames never reached
// the card.
//
// On time. Each animation books a wake for when its next frame is due, and
// one precise timer serves them all, on a sixty-a-second grid as a display
// is, so frames that change together are drawn together. A tick lets the
// window's frame pacer send those redraws at once instead of at its next
// beat. A 30 ms look on Windows' coarse timer and then the pacer's beat had
// held each 83 ms frame of a profile effect anywhere from 60 to 500 ms.
//
// So: take the time from now(), never from a clock of your own; book the
// next frame with wakeAt, or use a FramePulse where a QTimer would have
// stepped an animation; paint what the last tick chose. tools/test_frames.cpp
// holds all of this to account, and the release build runs it.
class FrameClock
{
public:
    // Animation time in milliseconds, held still through a paint or a tick.
    static qint64 now();

    // Calls `due` on the window's thread once now() reaches `atMs`, at the
    // first tick on or after it. One booking per owner: booking again moves
    // it. It goes when the owner does.
    static void wakeAt(QObject *owner, qint64 atMs, std::function<void()> due);
    static void cancel(const QObject *owner);
    static bool booked(const QObject *owner);

    // Repaints `area` of `widget` (all of it when empty) at the first tick
    // `inMs` from now, the next tick for 0: for something that moves all the
    // time, such as a name style, which paints and then asks again.
    static void scheduleRepaint(QWidget *widget, int inMs = 0, const QRect &area = QRect());

    // The grid the ticks fall on.
    static constexpr double TickMs = 1000.0 / 60.0;

    // Called by the application around every window redraw and paint.
    static void holdBegin();
    static void holdEnd();

    // For tests. Time stands at `ms` and moves only when told: setManualTime
    // moves it without ticking, advanceTo ticks every tick on the way. -1
    // goes back to the real clock.
    static void setManualTime(qint64 ms);
    static void advanceTo(qint64 ms);
};

// A steady beat on the frame clock, for an animation a QTimer would have
// stepped: calls `beat` every `everyMs`, rounded to the clock's ticks, from
// start() until stop(). Beats are counted from when it started, so they do
// not drift.
class FramePulse : public QObject
{
public:
    FramePulse(int everyMs, std::function<void()> beat, QObject *parent = nullptr);

    void start();
    void stop();
    bool isActive() const { return m_active; }
    void setInterval(int everyMs) { m_everyMs = qMax(1, everyMs); }
    int interval() const { return m_everyMs; }

private:
    void book();

    int m_everyMs;
    std::function<void()> m_beat;
    bool m_active = false;
    qint64 m_nextAt = 0;
};
