#pragma once
#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QRect>

namespace PdfBackground {
// Coordinates here are image pixels. Plane fitting and local exemplar repair
// run on the CPU in the disposable worker; no image generation or network.
QJsonObject describe(const QImage &image, const QRect &text);
bool erase(QImage &output, const QImage &original, const QList<QRect> &lines,
           const QString &mode, QString *error, const QRect &clip = {});
}
