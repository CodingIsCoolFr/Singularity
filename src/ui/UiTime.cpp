#include "ui/UiTime.h"

#include <QElapsedTimer>
#include <QEvent>
#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

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
    if (!parts.isEmpty())
        line += QStringLiteral("; biggest: ") + parts.join(QStringLiteral(", "));

    s_spent.clear();
    s_windowStart = now;
    s_threadCpuStart = cpu;
    return line;
}
