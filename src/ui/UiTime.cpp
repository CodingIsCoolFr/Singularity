#include "ui/UiTime.h"

#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QSlider>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

// What a piece of time is charged to: the kind of object, and what kind of
// event. A QTimer's own class says nothing, so its time goes to its owner.
struct Key
{
    const QMetaObject *type = nullptr;
    int event = 0;
    bool timer = false;
    bool operator==(const Key &other) const
    {
        return type == other.type && event == other.event && timer == other.timer;
    }
};

size_t qHash(const Key &key, size_t seed = 0)
{
    return ::qHash(reinterpret_cast<quintptr>(key.type), seed) ^ (size_t(key.event) << 1) ^ size_t(key.timer);
}

struct Frame
{
    qint64 start = 0;
    qint64 inside = 0;   // time spent in events delivered from within this one
};

QElapsedTimer &uiClock()
{
    static QElapsedTimer timer;
    if (!timer.isValid())
        timer.start();
    return timer;
}

QHash<Key, qint64> s_spent;       // nanoseconds, exclusive
QList<Frame> s_stack;
qint64 s_windowStart = 0;         // wall clock, ns
qint64 s_threadCpuStart = -1;     // the window thread's own CPU time, ns

// The frame pacer (see the header). Off until the main window chooses: the
// sign-in window stands on the black hole, where pacing does harm.
int s_paceMs = 0;
qint64 s_lastInputMs = -100000;   // when a person last did something
int s_redraws = 0;                // window redraws since the last report
int s_held = 0;                   // redraw requests folded into a later one

// Per window: when it last redrew, and whether a held redraw is booked.
struct Pace
{
    qint64 lastMs = -100000;
    bool booked = false;
};
QHash<const QObject *, Pace> s_pace;

// How long after a person's input redraws go out at once.
constexpr qint64 InputGraceMs = 250;

bool isInput(QEvent::Type type)
{
    switch (type) {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::InputMethod:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
        return true;
    default:
        return false;
    }
}

qint64 threadCpuNs()
{
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        const auto ns = [](const FILETIME &t) {
            return qint64((quint64(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 100;
        };
        return ns(kernel) + ns(user);
    }
#endif
    return -1;
}

QString eventName(int type)
{
    switch (type) {
    case QEvent::Paint:          return QStringLiteral("paint");
    case QEvent::UpdateRequest:  return QStringLiteral("repaint");
    case QEvent::Timer:          return QStringLiteral("timer");
    case QEvent::MetaCall:       return QStringLiteral("signal");
    case QEvent::Resize:         return QStringLiteral("resize");
    case QEvent::LayoutRequest:  return QStringLiteral("layout");
    case QEvent::MouseMove:      return QStringLiteral("mouse");
    case QEvent::Wheel:          return QStringLiteral("scroll");
    default:                     return QStringLiteral("event %1").arg(type);
    }
}

// A number box, a slider, or a dropdown changes its value when the wheel
// passes over it. That is how a scroll through Settings quietly edits a
// setting. The wheel only edits one that was clicked. Otherwise it is handed
// to the page, so the page still moves.
QWidget *valueControl(QWidget *widget)
{
    for (QWidget *cursor = widget; cursor; cursor = cursor->parentWidget()) {
        if (qobject_cast<QAbstractSpinBox *>(cursor) || qobject_cast<QComboBox *>(cursor)
            || qobject_cast<QSlider *>(cursor))
            return cursor;
        if (qobject_cast<QAbstractScrollArea *>(cursor))
            return nullptr;
    }
    return nullptr;
}

bool controlFocused(QWidget *control)
{
    if (control->hasFocus())
        return true;
    for (QWidget *focus = QApplication::focusWidget(); focus; focus = focus->parentWidget()) {
        if (focus == control)
            return true;
    }
    return false;
}

void passWheelToPage(QWidget *from, QWheelEvent *wheel)
{
    for (QWidget *cursor = from->parentWidget(); cursor; cursor = cursor->parentWidget()) {
        auto *area = qobject_cast<QAbstractScrollArea *>(cursor);
        if (!area || !area->viewport())
            continue;
        QWheelEvent copy(wheel->position(), wheel->globalPosition(), wheel->pixelDelta(), wheel->angleDelta(),
                         wheel->buttons(), wheel->modifiers(), wheel->phase(), wheel->inverted(), wheel->source());
        QCoreApplication::sendEvent(area->viewport(), &copy);
        return;
    }
}

} // namespace

SingularityApplication::SingularityApplication(int &argc, char **argv)
    : QApplication(argc, argv)
{
    uiClock();
    s_windowStart = uiClock().nsecsElapsed();
    s_threadCpuStart = threadCpuNs();
}

bool SingularityApplication::notify(QObject *receiver, QEvent *event)
{
    // Only the window thread. Other threads deliver their own events and
    // are counted elsewhere (the media thread has its own lines).
    if (!receiver || !event || QThread::currentThread() != thread())
        return QApplication::notify(receiver, event);

    // A deletion is not timed at all: the receiver is gone when it returns.
    if (event->type() == QEvent::DeferredDelete)
        return QApplication::notify(receiver, event);

    if (event->type() == QEvent::Wheel && receiver->isWidgetType()) {
        if (QWidget *control = valueControl(static_cast<QWidget *>(receiver))) {
            if (!controlFocused(control)) {
                passWheelToPage(control, static_cast<QWheelEvent *>(event));
                return true;
            }
        }
    }

    const qint64 nowMs = uiClock().elapsed();
    if (isInput(event->type()))
        s_lastInputMs = nowMs;

    // The frame pacer. A window's redraw request that comes too soon after
    // its last redraw is held, and one redraw is booked for the moment the
    // beat allows. Qt does not post another request while one is pending, so
    // everything that changes meanwhile is gathered into that one redraw.
    if (event->type() == QEvent::UpdateRequest && receiver->isWidgetType()
        && static_cast<QWidget *>(receiver)->isWindow()) {
        if (!s_pace.contains(receiver)) {
            QObject::connect(receiver, &QObject::destroyed, [receiver]() { s_pace.remove(receiver); });
        }
        Pace &pace = s_pace[receiver];
        const bool someoneActing = nowMs - s_lastInputMs < InputGraceMs;
        const qint64 due = pace.lastMs + s_paceMs;
        if (s_paceMs > 0 && !someoneActing && nowMs < due) {
            ++s_held;
            if (!pace.booked) {
                pace.booked = true;
                auto *window = static_cast<QWidget *>(receiver);
                QTimer::singleShot(int(due - nowMs), Qt::PreciseTimer, window, [window]() {
                    s_pace[window].booked = false;
                    // Stamped as allowed now, so this one is not held again.
                    s_pace[window].lastMs = -100000;
                    QCoreApplication::postEvent(window, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
                });
            }
            return true;
        }
        pace.lastMs = nowMs;
        ++s_redraws;
    }

    // Worked out BEFORE the event is delivered, and nothing about the
    // receiver or the event is touched afterwards. Handling an event can
    // delete the object it was sent to (or its parent), and asking a deleted
    // object for its metaObject() afterwards was an access violation in
    // Qt6Core - the crash of 26 September 2026, 13:14:59, in 0.8.17-0.8.21.
    Key key;
    key.event = int(event->type());
    const QObject *owner = receiver;
    if (qobject_cast<const QTimer *>(receiver) && receiver->parent()) {
        owner = receiver->parent();
        key.timer = true;
    }
    key.type = owner->metaObject();

    s_stack.append({uiClock().nsecsElapsed(), 0});
    const bool handled = QApplication::notify(receiver, event);
    const Frame frame = s_stack.takeLast();
    const qint64 total = uiClock().nsecsElapsed() - frame.start;
    if (!s_stack.isEmpty())
        s_stack.last().inside += total;

    s_spent[key] += total - frame.inside;
    return handled;
}

QString SingularityApplication::takeReport()
{
    const qint64 now = uiClock().nsecsElapsed();
    const qint64 wall = qMax<qint64>(1, now - s_windowStart);
    const qint64 cpu = threadCpuNs();

    QList<QPair<qint64, Key>> biggest;
    biggest.reserve(s_spent.size());
    for (auto it = s_spent.constBegin(); it != s_spent.constEnd(); ++it)
        biggest.append({it.value(), it.key()});
    std::sort(biggest.begin(), biggest.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });

    const auto share = [wall](qint64 ns) { return QString::number(100.0 * double(ns) / double(wall), 'f', 1); };

    QStringList parts;
    for (int i = 0; i < biggest.size() && i < 7; ++i) {
        const Key &key = biggest.at(i).second;
        QString name = QString::fromLatin1(key.type ? key.type->className() : "?");
        name += key.timer ? QStringLiteral(" timer") : QLatin1Char(' ') + eventName(key.event);
        parts << QStringLiteral("%1 %2%").arg(name, share(biggest.at(i).first));
    }

    QString line;
    if (cpu >= 0 && s_threadCpuStart >= 0)
        line = QStringLiteral("window thread used %1% of a core").arg(share(cpu - s_threadCpuStart));
    else
        line = QStringLiteral("window thread");

    // How often whole windows were redrawn, and how many requests the pacer
    // folded into those instead of drawing them one by one.
    const double seconds = double(wall) / 1e9;
    line += QStringLiteral(", %1 redraws a second (%2 more folded in, pace %3 ms)")
                .arg(s_redraws / seconds, 0, 'f', 1)
                .arg(s_held / seconds, 0, 'f', 1)
                .arg(s_paceMs);
    s_redraws = 0;
    s_held = 0;
    if (!parts.isEmpty())
        line += QStringLiteral("; biggest: ") + parts.join(QStringLiteral(", "));

    s_spent.clear();
    s_windowStart = now;
    s_threadCpuStart = cpu;
    return line;
}

void SingularityApplication::setFramePace(int ms)
{
    s_paceMs = qBound(0, ms, 200);
}

int SingularityApplication::framePace()
{
    return s_paceMs;
}
