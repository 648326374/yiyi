#pragma once
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <QSizeF>

// Geometry only. No model, network, or source-document mutation. All coordinates
// use PDF points with the origin at the page's top left.
namespace PdfLayout {
struct Analysis { QJsonArray items; QJsonArray regions; };
QRectF rect(const QJsonValue &value);
QJsonArray jsonRect(const QRectF &value);
QList<QLineF> imageRules(const QImage &image, const QSizeF &page);
QJsonArray ruledRegions(const QList<QLineF> &rules, const QSizeF &page, const QString &prefix);
QString cellAt(const QJsonArray &regions, const QRectF &text);
Analysis analyze(const QJsonArray &items, const QJsonArray &tables, const QSizeF &page, const QString &prefix);
QJsonObject structure(const QJsonArray &blocks, const QJsonArray &regions);
QJsonArray linkPageHeaders(const QJsonArray &pages, int firstPage = 0);
QString cellContext(const QJsonObject &block, const QJsonArray &blocks, const QJsonArray &regions);
QString changedNumbers(const QString &source, const QString &translation, bool pageReferences = false);
QString changedCode(const QString &source, const QString &translation);
}
