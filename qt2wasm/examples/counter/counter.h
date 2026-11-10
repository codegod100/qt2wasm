#pragma once
#include <QObject>
#include <QString>
#include <QList>
#include <limits>
#include <optional>
#include <qt2wasm/runtime.h>
class Counter : public QObject {
    Q_OBJECT
    QT2WASM_EXPORT("demo:counter/counter@0.1.0")
    Q_PROPERTY(int value READ value WRITE setValue NOTIFY valueChanged)
public:
    enum class Mode { Linear, Doubling };
    Q_ENUM(Mode)
    Q_INVOKABLE explicit Counter(QObject *parent = nullptr);
    int value() const;
    void setValue(int v);
    Q_INVOKABLE QString describe(const QString &prefix) const;
    Q_INVOKABLE QList<double> history(int n) const;
    Q_INVOKABLE void setMode(Counter::Mode m);
public slots:
    void step();
signals:
    void valueChanged(int value);
    void overflowed(const QString &why, qint64 at);

private:
    int m_value = 0;
    Mode m_mode = Mode::Linear;
    QList<double> m_history;
public:
    int m_queuedSteps = 0;
};
