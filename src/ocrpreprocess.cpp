#include "ocrpreprocess.h"
#include <QPainter>
#include <algorithm>
#include <vector>

QImage prepareOcrImage(const QImage &source)
{
    if (source.isNull()) return {};
    QImage image(source.size(), QImage::Format_RGB32); image.fill(Qt::white);
    QPainter painter(&image); painter.drawImage(0, 0, source); painter.end();
    // Estimate background from the perimeter. Invert uniformly dark backgrounds
    // without thresholding away antialiased strokes or changing the original.
    std::vector<int> edge;
    const int step = qMax(1, qMin(image.width(), image.height()) / 80);
    for (int x = 0; x < image.width(); x += step) { edge.push_back(qGray(image.pixel(x, 0))); edge.push_back(qGray(image.pixel(x, image.height() - 1))); }
    for (int y = 0; y < image.height(); y += step) { edge.push_back(qGray(image.pixel(0, y))); edge.push_back(qGray(image.pixel(image.width() - 1, y))); }
    const auto dark = std::count_if(edge.begin(), edge.end(), [](int value) { return value < 100; });
    if (dark > edge.size() * 0.8) image.invertPixels();
    const int longest = qMax(image.width(), image.height());
    if (longest < 1600) image = image.scaled(image.size() * qMin(2.0, 2048.0 / longest), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}
