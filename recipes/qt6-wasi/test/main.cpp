#include <QImage>
#include <QPainter>
#include <QString>
#include <cstdio>

int main() {
    QImage img(32, 32, QImage::Format_ARGB32);
    img.fill(Qt::black);
    QPainter p(&img);
    p.setBrush(Qt::white);
    p.drawEllipse(4, 4, 24, 24);
    p.end();
    const QString s = QString::fromLatin1("qt6-wasi %1 ok, centre=%2")
                          .arg(QString::fromLatin1(qVersion()))
                          .arg(img.pixelColor(16, 16).name());
    std::puts(s.toUtf8().constData());
    return img.pixelColor(16, 16) == QColor(Qt::white) ? 0 : 1;
}
