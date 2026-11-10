// qt6-wasi example: draw with QPainter into a QImage, save it as PNG and print
// a truecolor half-block preview to the terminal. A plain WASI command (main),
// no QGuiApplication or QPA plugin needed for raster painting.
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QtMath>
#include <cstdio>

static QImage draw(int w, int h) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);

    QLinearGradient sky(0, 0, 0, h);
    sky.setColorAt(0, QColor("#0b1d51"));
    sky.setColorAt(1, QColor("#ff7e5f"));
    p.fillRect(img.rect(), sky);

    QRadialGradient sun(w * 0.7, h * 0.45, h * 0.3);
    sun.setColorAt(0, QColor("#fff6b0"));
    sun.setColorAt(1, QColor(255, 200, 80, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(sun);
    p.drawEllipse(QPointF(w * 0.7, h * 0.45), h * 0.3, h * 0.3);

    QPainterPath hills;
    hills.moveTo(0, h);
    for (int x = 0; x <= w; x += 4)
        hills.lineTo(x, h * 0.72 + qSin(x * 0.045) * h * 0.08 + qSin(x * 0.013) * h * 0.06);
    hills.lineTo(w, h);
    p.setBrush(QColor("#1d3b2a"));
    p.drawPath(hills);

    p.end();
    return img;
}

int main(int argc, char **argv) {
    const QImage img = draw(320, 160);
    const char *out = argc > 1 ? argv[1] : "qt6-wasi-example.png";
    if (!img.save(QString::fromLocal8Bit(out), "PNG")) {
        std::fprintf(stderr, "could not write %s (run with a preopened dir, e.g. wasmtime --dir=.)\n", out);
        return 1;
    }
    // Two pixel rows per text row with the upper-half block.
    const QImage small = img.scaled(64, 32, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int y = 0; y < small.height(); y += 2) {
        for (int x = 0; x < small.width(); ++x) {
            const QColor a = small.pixelColor(x, y), b = small.pixelColor(x, y + 1);
            std::printf("\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm▀", a.red(), a.green(), a.blue(),
                        b.red(), b.green(), b.blue());
        }
        std::printf("\x1b[0m\n");
    }
    std::printf("Qt %s on wasm32-wasip1: wrote %s (%dx%d)\n", qVersion(), out, img.width(), img.height());
    return 0;
}
