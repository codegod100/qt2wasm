#include "counter.h"

#include <QTimer>

/*!
    \class Counter
    \brief A counter that steps by one or doubles.

    The doc comments in this file are qdoc's. qt2wasm runs qdoc over the
    project and writes them into the generated WIT as \c{///} docs.
*/

/*!
    \enum Counter::Mode
    How step() advances the value.

    \value Linear Add one.
    \value Doubling Double the value (zero becomes one).
*/

/*!
    \property Counter::value
    \brief the current count.

    Setting it records the new value in history() and emits valueChanged().
*/

/*!
    \fn void Counter::valueChanged(int value)
    Emitted when the count changes to \a value.
*/

/*!
    \fn void Counter::overflowed(const QString &why, qint64 at)
    Emitted instead of stepping past \c INT_MAX: \a why says why and \a at
    is the value the step would have reached.
*/

/*!
    Creates a counter at zero in \l Linear mode.
*/
Counter::Counter(QObject *parent) : QObject(parent) {}

int Counter::value() const { return m_value; }

void Counter::setValue(int v) {
    if (v == m_value)
        return;
    m_value = v;
    m_history.append(v);
    emit valueChanged(v);
}

/*!
    Returns \a prefix followed by the value and the mode, for example
    \c{"n=3 (linear)"}.
*/
QString Counter::describe(const QString &prefix) const {
    return QStringLiteral("%1%2 (%3)").arg(prefix).arg(m_value).arg(m_mode == Mode::Linear ? u"linear" : u"doubling");
}

/*!
    Returns the last \a n values the counter held, oldest first.
*/
QList<double> Counter::history(int n) const { return m_history.mid(qMax(0, m_history.size() - n)); }

/*!
    Switches to mode \a m for the next step().
*/
void Counter::setMode(Counter::Mode m) { m_mode = m; }

/*!
    Advances the counter once, by the current Mode.

    \sa setMode(), overflowed()
*/
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
