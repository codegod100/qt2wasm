// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// The window of one latha:gui app: the host side of the world in Qt.
#pragma once
#include "host/latha_host.h"
#include "latha_pty.h"

#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QTimer>
#include <QWidget>
#include <map>
#include <memory>

struct RunOptions {
    QString screenshot; // save the frame here and quit (tests)
    int quitAfterMs = -1;
    QString typeText;   // typed (as key events) once the app is up (tests)
    int typeAfterMs = 500;
    bool debug = false;
    bool verify = false;
};

class AppWindow : public QWidget
{
    Q_OBJECT
public:
    AppWindow(LathaApp *app, const QString &appName, const RunOptions &opts);
    ~AppWindow() override;

    // The host imports (called from inside an app call).
    void requestFrame();
    void requestTick(quint32 ms);
    void setAppTitle(const QString &t);
    void setAppCursor(quint32 shape);
    void setAppMinSize(quint32 w, quint32 h);
    void quitApp(int code);
    int64_t ptySpawn(const QList<QByteArray> &argv, const QList<QPair<QByteArray, QByteArray>> &env,
                     const QByteArray &cwd, quint16 cols, quint16 rows, QString *err);
    Pty *pty(quint32 id);
    void ptyClose(quint32 id);
    bool prefersDark() const;

    /// The app trapped or asked to quit: leave with `code`.
    void finish(int code);

protected:
    bool event(QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void inputMethodEvent(QInputMethodEvent *e) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery q) const override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    bool focusNextPrevChild(bool) override { return false; }

private:
    bool check(int r);
    void schedulePump(int ms = 0);
    void pump();
    void applySize();
    void paintApp();
    void applyMove(const LathaMove &m);
    void verifyFrame();
    void key(QKeyEvent *e, bool pressed);
    void pointer(int kind, QMouseEvent *e, int clicks);
    double scale() const;
    QRect toLogical(const QRect &device) const;

    LathaApp *m_app;
    QString m_appName;
    RunOptions m_opts;
    QImage m_frame; // device pixels, ARGB32_Premultiplied (= latha's BGRA)
    QTimer m_pumpTimer;
    QElapsedTimer m_clock;
    qint64 m_tickAt = -1;    // m_clock ms of the next tick, -1 none
    bool m_tickNow = true;   // an event or pty output since the last tick
    bool m_frameWanted = true;
    qint64 m_lastPaint = -1000;
    int m_quitCode = -1;
    bool m_finished = false;
    quint32 m_appliedW = 0, m_appliedH = 0;
    double m_appliedScale = 0;
    // clicks: our own count (press within the double-click time and distance)
    int m_clicks = 0;
    qint64 m_lastPress = -10000;
    QPointF m_lastPressPos;
    Qt::MouseButton m_lastButton = Qt::NoButton;
    std::map<quint32, std::unique_ptr<Pty>> m_ptys;
    quint32 m_nextPty = 1;
    bool m_darkOverride = false, m_dark = false;
};
