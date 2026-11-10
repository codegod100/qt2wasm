#include "wasieventdispatcher.h"

#include <QCoreApplication>
#include <QTimerEvent>

namespace qt2wasm {

WasiEventDispatcher::WasiEventDispatcher(QObject *parent) : QAbstractEventDispatcher(parent) {
    m_clock.start();
}

bool WasiEventDispatcher::processEvents(QEventLoop::ProcessEventsFlags flags) {
    Q_UNUSED(flags);
    emit awake();
    QCoreApplication::sendPostedEvents();
    const int fired = fireDueTimers();
    // A timer slot may have posted more events.
    QCoreApplication::sendPostedEvents();
    return fired > 0;
}

int WasiEventDispatcher::fireDueTimers() {
    const qint64 now = m_clock.elapsed();
    // Copy the ids first: a timer event may (un)register timers.
    QList<int> due;
    for (const Timer &t : std::as_const(m_timers))
        if (t.due <= now)
            due.append(t.id);
    int fired = 0;
    for (int id : due) {
        auto it = std::find_if(m_timers.begin(), m_timers.end(), [id](const Timer &t) { return t.id == id; });
        if (it == m_timers.end())
            continue;
        it->due = now + qMax<qint64>(it->interval, 1);
        QObject *obj = it->object;
        QTimerEvent ev(id);
        QCoreApplication::sendEvent(obj, &ev);
        ++fired;
    }
    return fired;
}

void WasiEventDispatcher::registerTimer(int timerId, qint64 interval, Qt::TimerType timerType, QObject *object) {
    m_timers.append({timerId, interval, m_clock.elapsed() + interval, timerType, object});
}

bool WasiEventDispatcher::unregisterTimer(int timerId) {
    return m_timers.removeIf([timerId](const Timer &t) { return t.id == timerId; }) > 0;
}

bool WasiEventDispatcher::unregisterTimers(QObject *object) {
    return m_timers.removeIf([object](const Timer &t) { return t.object == object; }) > 0;
}

QList<QAbstractEventDispatcher::TimerInfo> WasiEventDispatcher::registeredTimers(QObject *object) const {
    QList<TimerInfo> out;
    for (const Timer &t : m_timers)
        if (t.object == object)
            out.append(TimerInfo(t.id, int(t.interval), t.type));
    return out;
}

int WasiEventDispatcher::remainingTime(int timerId) {
    for (const Timer &t : std::as_const(m_timers))
        if (t.id == timerId)
            return int(qMax<qint64>(0, t.due - m_clock.elapsed()));
    return -1;
}

qint64 WasiEventDispatcher::msUntilNextTimer() const {
    qint64 best = -1;
    const qint64 now = m_clock.elapsed();
    for (const Timer &t : m_timers) {
        const qint64 left = qMax<qint64>(0, t.due - now);
        if (best < 0 || left < best)
            best = left;
    }
    return best;
}

}  // namespace qt2wasm
