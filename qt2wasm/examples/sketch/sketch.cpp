#include "sketch.h"

#include <qt2wasm/gui.h>

#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyleHints>
#include <QTime>
#include <QWheelEvent>

#include <cmath>

Sketch::Sketch() {
    setTitle(QStringLiteral("Sketch (Qt on wasm)"));
    setMinimumSize(QSize(320, 240));
    resize(720, 480);
    m_clock.start(1000, this);
}

bool Sketch::event(QEvent *e) {
    if (e->type() == QEvent::ThemeChange)
        update();
    return QRasterWindow::event(e);
}

void Sketch::timerEvent(QTimerEvent *e) {
    if (e->timerId() == m_clock.timerId())
        update();
}

void Sketch::paintEvent(QPaintEvent *) {
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    const QColor bg = dark ? QColor(0x1e, 0x1f, 0x22) : QColor(0xfa, 0xfa, 0xf7);
    const QColor fg = dark ? QColor(0xe8, 0xe6, 0xe3) : QColor(0x22, 0x22, 0x22);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(QRect(QPoint(), size()), bg);

    // Strokes
    for (const Stroke &s : std::as_const(m_strokes)) {
        p.setPen(QPen(s.color, s.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (s.points.size() == 1)
            p.drawPoint(s.points.first());
        else
            p.drawPolyline(s.points);
    }

    // Clock, top right
    const qreal r = 54;
    const QPointF c(width() - r - 16, r + 16);
    // The rim in the desktop's accent color (host.accent-color via the palette).
    p.setPen(QPen(QGuiApplication::palette().color(QPalette::Accent), 3));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, r, r);
    p.setPen(QPen(fg, 2));
    for (int i = 0; i < 12; ++i) {
        const qreal a = i * M_PI / 6;
        p.drawLine(c + QPointF(std::sin(a), -std::cos(a)) * (r - 8), c + QPointF(std::sin(a), -std::cos(a)) * (r - 2));
    }
    const QTime t = QTime::currentTime();
    auto hand = [&](qreal turns, qreal len, qreal w, QColor col) {
        const qreal a = turns * 2 * M_PI;
        p.setPen(QPen(col, w, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c, c + QPointF(std::sin(a), -std::cos(a)) * len);
    };
    hand((t.hour() % 12 + t.minute() / 60.0) / 12.0, r * 0.5, 4, fg);
    hand((t.minute() + t.second() / 60.0) / 60.0, r * 0.75, 3, fg);
    hand(t.second() / 60.0, r * 0.85, 1.5, QColor::fromHsv(m_hue, 200, 230));

    // Text and help
    QFont f = p.font();
    f.setPixelSize(15);
    p.setFont(f);
    p.setPen(fg);
    const QString help = QStringLiteral(
        "Drag to draw. Wheel: pen %1 px. Space: color. Ctrl+V pastes, Ctrl+C copies, "
        "Backspace erases, Esc clears, Ctrl+Q quits.").arg(m_pen, 0, 'f', 0);
    p.drawText(QRectF(16, height() - 64, width() - 32, 48), Qt::TextWordWrap | Qt::AlignBottom, help);
    f.setPixelSize(28);
    p.setFont(f);
    p.setPen(QColor::fromHsv(m_hue, 160, dark ? 240 : 170));
    p.drawText(QRectF(16, 16, width() - 2 * r - 64, 120), Qt::TextWordWrap,
               m_text.isEmpty() ? QStringLiteral("Type something…") : m_text);

    // Pen preview
    p.setPen(Qt::NoPen);
    p.setBrush(QColor::fromHsv(m_hue, 200, 230));
    p.drawEllipse(QPointF(width() - r - 16, 2 * r + 48), m_pen / 2, m_pen / 2);
}

void Sketch::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    m_drawing = true;
    m_strokes.append({QPolygonF{e->position()}, QColor::fromHsv(m_hue, 200, 230), m_pen});
    update();
}

void Sketch::mouseMoveEvent(QMouseEvent *e) {
    if (!m_drawing)
        return;
    m_strokes.last().points.append(e->position());
    update();
}

void Sketch::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton)
        m_drawing = false;
}

void Sketch::wheelEvent(QWheelEvent *e) {
    m_pen = qBound<qreal>(1, m_pen + e->angleDelta().y() / 120.0, 40);
    update();
}

void Sketch::keyPressEvent(QKeyEvent *e) {
    if (e->matches(QKeySequence::Paste)) {
        m_text += qt2wasm::gui::clipboardText();
    } else if (e->matches(QKeySequence::Copy)) {
        qt2wasm::gui::setClipboardText(m_text);
    } else if (e->matches(QKeySequence::Quit) || (e->key() == Qt::Key_Q && e->modifiers() & Qt::ControlModifier)) {
        QGuiApplication::quit();
        return;
    } else if (e->key() == Qt::Key_Escape) {
        m_strokes.clear();
        m_text.clear();
    } else if (e->key() == Qt::Key_Backspace) {
        m_text.chop(1);
    } else if (e->key() == Qt::Key_Space && m_text.isEmpty()) {
        m_hue = (m_hue + 47) % 360;
    } else if (!e->text().isEmpty() && e->text().at(0).isPrint()) {
        m_text += e->text();
    } else {
        return;
    }
    update();
}

int qt2wasm_gui_main(int, char **) {
    auto *w = new Sketch;
    w->show();
    return 0;
}
