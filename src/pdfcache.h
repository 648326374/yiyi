#pragma once
#include <QJsonObject>
#include <QString>
class PdfTaskStore;
namespace PdfCache {
bool validBlock(const QJsonObject &block, const QJsonObject &page, bool scan);
// Import once into page-sized migration records. The old JSON is never changed.
bool importLegacy(const QString &path, PdfTaskStore &store, QString *error);
// Reapply edits only after the new page has been extracted and checked.
QJsonObject reconcile(const QJsonObject &fresh, const QJsonObject &saved);
}
