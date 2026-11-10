#include "canvas.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

Canvas::Canvas(int width, int height, QObject *parent)
    : QObject(parent), m_image(qMax(1, width), qMax(1, height), QImage::Format_RGBA8888) {
    m_image.fill(Qt::white);
}

void Canvas::clear(const QColor &background) {
    m_image.fill(background);
    emit painted(m_image.rect());
}

void Canvas::shape(Canvas::Shape kind, const QRect &rect, const QColor &fill, const Stroke &stroke) {
    QPainter p(&m_image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(fill);
    p.setPen(QPen(stroke.color, stroke.width));
    switch (kind) {
    case Shape::Box:
        p.drawRoundedRect(rect, 6, 6);
        break;
    case Shape::Ellipse:
        p.drawEllipse(rect);
        break;
    case Shape::Star: {
        QPainterPath path;
        const QPointF c = QRectF(rect).center();
        const qreal r = qMin(rect.width(), rect.height()) / 2.0;
        for (int i = 0; i < 10; ++i) {
            const qreal a = -M_PI / 2 + i * M_PI / 5, rr = (i % 2) ? r * 0.45 : r;
            const QPointF pt(c.x() + rr * qCos(a), c.y() + rr * qSin(a));
            i ? path.lineTo(pt) : path.moveTo(pt);
        }
        path.closeSubpath();
        p.drawPath(path);
        break;
    }
    }
    emit painted(rect.adjusted(-int(stroke.width), -int(stroke.width), int(stroke.width), int(stroke.width)));
}

QRect Canvas::text(const QPoint &at, const QString &text, int pixelSize, const QColor &color, Canvas::TextOptions options) {
    QFont f;
    const QStringList fams = QFontDatabase::families();
    if (!fams.isEmpty())
        f.setFamily(fams.first());
    f.setPixelSize(qMax(1, pixelSize));
    f.setBold(options.testFlag(Bold));
    f.setItalic(options.testFlag(Italic));
    f.setUnderline(options.testFlag(Underline));
    QPainter p(&m_image);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(f);
    p.setPen(color);
    const QRect box = QFontMetrics(f).boundingRect(text).translated(at);
    p.drawText(at, text);
    emit painted(box);
    return box;
}

QStringList Canvas::fontFamilies() const { return QFontDatabase::families(); }

std::optional<QColor> Canvas::pixel(const QPoint &at) const {
    if (!m_image.rect().contains(at))
        return std::nullopt;
    return m_image.pixelColor(at);
}
