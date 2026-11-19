#pragma once
#include <QJsonObject>
#include <functional>

// Runs in the disposable PDF worker. Coordinates in the manifest are PDF
// points with a top-left origin; the source file is always opened read-only.
namespace PdfEngine {
using Progress = std::function<void(const QJsonObject &)>;
QJsonObject execute(const QJsonObject &request, const Progress &progress = {});
}
