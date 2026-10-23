#include "ui/FrameClock.h"

#include "ui/UiTime.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QRegion>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {

struct Booking
{
    QPointer<QObject> owner;
    qint64 atMs = 0;
    std::function<void()> due;
};

struct Repaint
{
    QPointer<QWidget> widget;
    qint64 atMs = 0;
    bool whole = false;
    QRegion area;   // pieces far apart stay apart
};

qint64 g_manual = -1;    // a test's time, or -1 for the real clock
int g_holds = 0;
qint64 g_heldAt = 0;     // now() while held
bool g_ticking = false;
QHash<const QObject *, Booking> g_bookings;
QHash<const QObject *, Repaint> g_repaints;
QTimer *g_timer = nullptr;
qint64 g_timerFor = -1;  // the tick the timer is set for

qint64 live()
{
    if (g_manual >= 0)
        return g_manual;
    static QElapsedTimer clock = [] {
        QElapsedTimer started;
        started.start();
        return started;
    }();
    return clock.elapsed();
}

// The first tick of the grid on or after `ms`, a millisecond early allowed:
// the grid's ticks fall between whole milliseconds.
qint64 tickFor(qint64 ms)
{
    return std::llround(std::ceil(double(ms - 1) / FrameClock::TickMs) * FrameClock::TickMs);
}

// The first tick strictly after `ms`.
qint64 tickAfter(qint64 ms)
{
    return std::llround((std::floor(double(ms) / FrameClock::TickMs) + 1) * FrameClock::TickMs);
}

// The tick the earliest booking or repaint falls on, -1 for none.
qint64 nextTick()
{
    qint64 earliest = -1;
    const auto consider = [&earliest](qint64 at) { earliest = earliest < 0 ? at : qMin(earliest, at); };
    for (const Booking &booking : std::as_const(g_bookings)) {
        if (booking.owner)
            consider(booking.atMs);
    }
    for (const Repaint &repaint : std::as_const(g_repaints)) {
        if (repaint.widget)
            consider(repaint.atMs);
    }
    return earliest < 0 ? -1 : tickFor(earliest);
}

void runTick();

void arm()
{
    // A test steps the clock itself, and nothing is armed while ticking:
    // the tick arms once when it is done.
    if (g_manual >= 0 || g_ticking)
        return;
    const qint64 tick = nextTick();
    if (tick < 0) {
        if (g_timer)
            g_timer->stop();
        g_timerFor = -1;
        return;
    }
    if (g_timer && g_timer->isActive() && g_timerFor == tick)
        return;
    if (!g_timer) {
        g_timer = new QTimer(QCoreApplication::instance());
        g_timer->setSingleShot(true);
        g_timer->setTimerType(Qt::PreciseTimer);
        QObject::connect(g_timer, &QTimer::timeout, g_timer, []() { runTick(); });
    }
    g_timerFor = tick;
    g_timer->start(int(qBound(qint64(0), tick - live(), qint64(60000))));
}

void runTick()
{
    g_timerFor = -1;
    g_ticking = true;
    FrameClock::holdBegin();
    const qint64 now = FrameClock::now();

    // Everything due by now, in the order it fell due. Taken out first: a
    // wake that books again is booked afresh.
    QList<Booking> due;
    for (auto it = g_bookings.begin(); it != g_bookings.end();) {
        if (!it->owner) {
            it = g_bookings.erase(it);
        } else if (it->atMs <= now + 1) {
            due.append(std::move(it.value()));
            it = g_bookings.erase(it);
        } else {
            ++it;
        }
    }
    QList<Repaint> repaints;
    for (auto it = g_repaints.begin(); it != g_repaints.end();) {
        if (!it->widget) {
            it = g_repaints.erase(it);
        } else if (it->atMs <= now + 1) {
            repaints.append(it.value());
            it = g_repaints.erase(it);
        } else {
            ++it;
        }
    }
    std::stable_sort(due.begin(), due.end(), [](const Booking &a, const Booking &b) { return a.atMs < b.atMs; });

    for (const Booking &booking : std::as_const(due)) {
        if (booking.owner && booking.due)
            booking.due();
    }
    for (const Repaint &repaint : std::as_const(repaints)) {
        if (!repaint.widget)
            continue;
        if (repaint.whole)
            repaint.widget->update();
        else
            repaint.widget->update(repaint.area);
    }

    FrameClock::holdEnd();
    g_ticking = false;
    // What changed goes out now, not at the pacer's next beat.
    if (!due.isEmpty() || !repaints.isEmpty())
        SingularityApplication::framesDue();
    arm();
}

} // namespace

qint64 FrameClock::now()
{
    return g_holds > 0 ? g_heldAt : live();
}

void FrameClock::wakeAt(QObject *owner, qint64 atMs, std::function<void()> due)
{
    if (!owner)
        return;
    g_bookings.insert(owner, Booking{QPointer<QObject>(owner), atMs, std::move(due)});
    arm();
}

void FrameClock::cancel(const QObject *owner)
{
    g_bookings.remove(owner);
}

bool FrameClock::booked(const QObject *owner)
{
    const auto it = g_bookings.constFind(owner);
    return it != g_bookings.constEnd() && it->owner;
}

void FrameClock::scheduleRepaint(QWidget *widget, int inMs, const QRect &area)
{
    if (!widget)
        return;
    const qint64 at = inMs > 0 ? now() + inMs : tickAfter(now());
    Repaint &repaint = g_repaints[widget];
    if (!repaint.widget) {
        repaint = Repaint{QPointer<QWidget>(widget), at, false, QRegion()};
    } else {
        repaint.atMs = qMin(repaint.atMs, at);
    }
    if (area.isEmpty())
        repaint.whole = true;
    else
        repaint.area |= area;
    arm();
}

void FrameClock::holdBegin()
{
    if (g_holds++ == 0)
        g_heldAt = live();
}

void FrameClock::holdEnd()
{
    if (g_holds > 0)
        --g_holds;
}

void FrameClock::setManualTime(qint64 ms)
{
    const bool wasManual = g_manual >= 0;
    g_manual = ms;
    if (ms < 0 && wasManual)
        arm();
    else if (ms >= 0 && g_timer)
        g_timer->stop();
}

void FrameClock::advanceTo(qint64 ms)
{
    if (g_manual < 0)
        return;
    // Every tick on the way, in order; a wake that keeps booking the past
    // cannot hold it here for ever.
    for (int guard = 0; guard < 100000; ++guard) {
        const qint64 tick = nextTick();
        if (tick < 0 || tick > ms)
            break;
        g_manual = qMax(g_manual, tick);
        runTick();
    }
    g_manual = ms;
}

// --- FramePulse ---------------------------------------------------------------

FramePulse::FramePulse(int everyMs, std::function<void()> beat, QObject *parent)
    : QObject(parent)
    , m_everyMs(qMax(1, everyMs))
    , m_beat(std::move(beat))
{
}

void FramePulse::start()
{
    if (m_active)
        return;
    m_active = true;
    m_nextAt = FrameClock::now() + m_everyMs;
    book();
}

void FramePulse::stop()
{
    m_active = false;
    FrameClock::cancel(this);
}

void FramePulse::book()
{
    FrameClock::wakeAt(this, m_nextAt, [this]() {
        if (!m_active)
            return;
        // Counted from the start, so the beat does not drift; after a stall
        // it carries on from now rather than catching up.
        const qint64 now = FrameClock::now();
        m_nextAt += m_everyMs;
        if (m_nextAt <= now)
            m_nextAt = now + m_everyMs;
        book();
        if (m_beat)
            m_beat();
    });
}
