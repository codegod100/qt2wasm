// sketch: a small QtGui program built as a GUI component
// (`qt2wasm build --gui`), to run in a latha:gui host such as perry-gui.
#pragma once

#include <QBasicTimer>
#include <QColor>
#include <QList>
#include <QPolygonF>
#include <QRasterWindow>
#include <QString>

class Sketch : public QRasterWindow {
    Q_OBJECT
public:
    Sketch();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void timerEvent(QTimerEvent *) override;
    bool event(QEvent *) override;

private:
    struct Stroke {
        QPolygonF points;
        QColor color;
        qreal width;
    };
    QList<Stroke> m_strokes;
    QString m_text;
    qreal m_pen = 4;
    int m_hue = 200;
    bool m_drawing = false;
    QBasicTimer m_clock;
};
