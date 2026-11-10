#include "counter.h"

#include <QTimer>

Counter::Counter(QObject *parent) : QObject(parent) {}

int Counter::value() const { return m_value; }

void Counter::setValue(int v) {
    if (v == m_value)
        return;
    m_value = v;
    m_history.append(v);
    emit valueChanged(v);
}

QString Counter::describe(const QString &prefix) const {
    return QStringLiteral("%1%2 (%3)").arg(prefix).arg(m_value).arg(m_mode == Mode::Linear ? u"linear" : u"doubling");
}

QList<double> Counter::history(int n) const { return m_history.mid(qMax(0, m_history.size() - n)); }

void Counter::setMode(Counter::Mode m) { m_mode = m; }

void Counter::step() {
    const qint64 next = m_mode == Mode::Linear ? qint64(m_value) + 1 : qint64(m_value ? m_value : 1) * 2;
    if (next > std::numeric_limits<int>::max()) {
        emit overflowed(QStringLiteral("past INT_MAX"), next);
        return;
    }
    setValue(int(next));
    // Exercises the reactor event loop: delivered when this call returns.
    QTimer::singleShot(0, this, [this] { m_queuedSteps++; });
}
