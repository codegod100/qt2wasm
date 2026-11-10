#pragma once
// QtGui in a component: QPainter + QFont draw into a QImage that the host
// receives as an RGBA record. Also exercises Q_GADGET records, Q_FLAG flags,
// QColor/QRect/QSize/QPoint, QStringList and std::optional.
#include <QColor>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QStringList>
#include <optional>
#include <qt2wasm/runtime.h>

struct Stroke {
    Q_GADGET
    Q_PROPERTY(QColor color MEMBER color)
    Q_PROPERTY(double width MEMBER width)
public:
    QColor color = Qt::black;
    double width = 1.0;
};

class Canvas : public QObject {
    Q_OBJECT
    QT2WASM_EXPORT("demo:canvas/canvas@0.1.0")
    Q_PROPERTY(QSize size READ size)
public:
    enum class Shape { Box, Ellipse, Star };
    Q_ENUM(Shape)
    enum TextOption { Bold = 0x1, Italic = 0x2, Underline = 0x4 };
    Q_DECLARE_FLAGS(TextOptions, TextOption)
    Q_FLAG(TextOptions)

    Q_INVOKABLE Canvas(int width, int height, QObject *parent = nullptr);
    QSize size() const { return m_image.size(); }

    Q_INVOKABLE void clear(const QColor &background);
    Q_INVOKABLE void shape(Canvas::Shape kind, const QRect &rect, const QColor &fill, const Stroke &stroke);
    // Draws text; returns its bounding box.
    Q_INVOKABLE QRect text(const QPoint &at, const QString &text, int pixelSize, const QColor &color,
                           Canvas::TextOptions options);
    Q_INVOKABLE QStringList fontFamilies() const;
    // The colour at a pixel, or nothing outside the canvas.
    Q_INVOKABLE std::optional<QColor> pixel(const QPoint &at) const;
    Q_INVOKABLE QImage image() const { return m_image; }

signals:
    void painted(const QRect &dirty);

private:
    QImage m_image;
};
Q_DECLARE_OPERATORS_FOR_FLAGS(Canvas::TextOptions)
