// A QAbstractEventDispatcher that never blocks: there are no sockets, no
// threads and no way to sleep inside a component call. processEvents()
// delivers posted events and fires timers that are due, then returns.
#pragma once

#include <QAbstractEventDispatcher>
#include <QElapsedTimer>
#include <QList>

namespace qt2wasm {

class WasiEventDispatcher : public QAbstractEventDispatcher {
    Q_OBJECT
public:
    explicit WasiEventDispatcher(QObject *parent = nullptr);

    bool processEvents(QEventLoop::ProcessEventsFlags flags) override;
    void registerSocketNotifier(QSocketNotifier *) override {}
    void unregisterSocketNotifier(QSocketNotifier *) override {}
    void registerTimer(int timerId, qint64 interval, Qt::TimerType timerType, QObject *object) override;
    bool unregisterTimer(int timerId) override;
    bool unregisterTimers(QObject *object) override;
    QList<TimerInfo> registeredTimers(QObject *object) const override;
    int remainingTime(int timerId) override;
    void wakeUp() override {}
    void interrupt() override {}

    // Milliseconds until the next timer is due, or -1 with none registered.
    // A host that wants timers to fire on time can call back after this long.
    qint64 msUntilNextTimer() const;

private:
    struct Timer {
        int id;
        qint64 interval;
        qint64 due;
        Qt::TimerType type;
        QObject *object;
    };
    QList<Timer> m_timers;
    QElapsedTimer m_clock;
    int fireDueTimers();
};

}  // namespace qt2wasm
