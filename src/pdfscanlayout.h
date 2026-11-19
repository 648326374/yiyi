#pragma once
#include <QImage>
#include <QJsonArray>
#include <QSizeF>

namespace PdfScanLayout {
// Both run only in the disposable PDF worker. Detection never transcribes text.
QJsonArray detect(const QImage &image, const QSizeF &page, int pageIndex, QJsonObject *layout = nullptr);
QImage eraseText(const QImage &image, const QSizeF &page, const QJsonArray &blocks, QJsonArray *failures = nullptr);
}
