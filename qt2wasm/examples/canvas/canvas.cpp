#include "canvas.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

/*!
    \class Stroke
    \brief How the outline of a shape is drawn.
*/

/*!
    \property Stroke::color
    \brief the outline colour. The default is black.
*/

/*!
    \property Stroke::width
    \brief the outline width in pixels. The default is 1.
*/

/*!
    \class Canvas
    \brief An RGBA image you draw shapes and text on with QPainter.

    The example shows QtGui inside a WASI component: QPainter and QFont draw
    into a QImage, and the host gets the pixels back as a record from image().
    Every drawing call emits painted() with the area it changed.
*/

/*!
    \enum Canvas::Shape
    The shapes shape() can draw.

    \value Box A rectangle with rounded corners.
    \value Ellipse An ellipse filling the rectangle.
    \value Star A five-pointed star centred in the rectangle.
*/

/*!
    \enum Canvas::TextOption
    Styles for text(); combine them as flags.

    \value Bold Bold weight.
    \value Italic Italic.
    \value Underline Underlined.
*/

/*!
    \property Canvas::size
    \brief the canvas size in pixels, fixed when it is created.
*/

/*!
    \fn void Canvas::painted(const QRect &dirty)
    Emitted after each drawing call: \a dirty is the area that changed.
*/

/*!
    \fn QImage Canvas::image() const
    Returns the pixels drawn so far, as RGBA.
*/

/*!
    Creates a white canvas of \a width by \a height pixels (at least 1 by 1).
*/
Canvas::Canvas(int width, int height, QObject *parent)
    : QObject(parent), m_image(qMax(1, width), qMax(1, height), QImage::Format_RGBA8888) {
    m_image.fill(Qt::white);
}

/*!
    Fills the whole canvas with \a background.
*/
void Canvas::clear(const QColor &background) {
    m_image.fill(background);
    emit painted(m_image.rect());
}

/*!
    Draws a \a kind shape inside \a rect, filled with \a fill and outlined
    with \a stroke, antialiased.
*/
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

/*!
    Draws \a text with its baseline starting at \a at, \a pixelSize pixels
    high, in \a color, styled by \a options. Returns the text's bounding box.

    The font is the first family in fontFamilies(): the fonts compiled into
    the component.
*/
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

/*!
    Returns the font families the component can draw with.
*/
QStringList Canvas::fontFamilies() const { return QFontDatabase::families(); }

/*!
    Returns the colour of the pixel at \a at, or nothing when \a at is
    outside the canvas.
*/
std::optional<QColor> Canvas::pixel(const QPoint &at) const {
    if (!m_image.rect().contains(at))
        return std::nullopt;
    return m_image.pixelColor(at);
}
