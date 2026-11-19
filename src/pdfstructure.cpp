#include "pdfcontroller.h"
#include "pdflayout.h"
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace {
QString uniqueId(const QString &prefix) { return prefix + "-" + QUuid::createUuid().toString(QUuid::Id128); }
bool finiteRect(const QRectF &r) { return std::isfinite(r.x()) && std::isfinite(r.y()) && std::isfinite(r.width()) && std::isfinite(r.height()) && r.width() >= 3 && r.height() >= 3; }
void reindexTables(QJsonArray &regions, QJsonArray &blocks) {
    for (int t = 0; t < regions.size(); ++t) {
        auto table = regions[t].toObject(); if (table["kind"] != "table") continue;
        QList<double> xs, ys; QRectF bounds;
        for (const auto &v : regions) { const auto cell = v.toObject(); if (cell["parent"] != table["id"] || cell["kind"] != "cell") continue;
            const auto r = PdfLayout::rect(cell["rect"]); xs << r.left() << r.right(); ys << r.top() << r.bottom(); bounds = bounds.isEmpty() ? r : bounds.united(r);
        }
        if (bounds.isEmpty()) continue;
        auto unique = [](QList<double> &values) {
            std::sort(values.begin(), values.end()); values.erase(std::unique(values.begin(), values.end(), [](double a, double b) { return std::abs(a - b) < .5; }), values.end());
        }; unique(xs); unique(ys);
        auto at = [](const QList<double> &values, double coordinate) { int best = 0; for (int i = 1; i < values.size(); ++i) if (std::abs(values[i] - coordinate) < std::abs(values[best] - coordinate)) best = i; return best; };
        const auto oldYs = table["yEdges"].toArray(); const int headers = table["headerRows"].toInt();
        if (headers > 0 && headers < oldYs.size()) table["headerRows"] = at(ys, oldYs[headers].toDouble());
        QJsonArray xe, ye; for (double x : xs) xe.append(x); for (double y : ys) ye.append(y);
        table["xEdges"] = xe; table["yEdges"] = ye; table["rect"] = PdfLayout::jsonRect(bounds); table["rows"] = ys.size() - 1; table["columns"] = xs.size() - 1; regions[t] = table;
        for (int i = 0; i < regions.size(); ++i) {
            auto cell = regions[i].toObject(); if (cell["parent"] != table["id"] || cell["kind"] != "cell") continue;
            const auto r = PdfLayout::rect(cell["rect"]); cell["column"] = at(xs, r.left()); cell["row"] = at(ys, r.top());
            cell["columnSpan"] = at(xs, r.right()) - cell["column"].toInt(); cell["rowSpan"] = at(ys, r.bottom()) - cell["row"].toInt(); regions[i] = cell;
            for (int b = 0; b < blocks.size(); ++b) { auto block = blocks[b].toObject(); if (block["container"] != cell["id"]) continue;
                for (const auto &key : {"row", "column", "rowSpan", "columnSpan"}) block[key] = cell[key]; block["containerRect"] = cell["rect"]; blocks[b] = block;
            }
        }
    }
}
}

QJsonArray PdfController::regionsForPage(int page) const {
    if (!isPageReady(page)) return {};
    const auto p = pageObject(page); return (isScanPage(page) ? p["scan"].toObject()["layout"].toObject() : p["layout"].toObject())["regions"].toArray();
}
QVariantList PdfController::layoutRegions() const { return regionsForPage(m_page).toVariantList(); }
bool PdfController::structureError(const QString &message) { m_error = message; emit changed(); return false; }
bool PdfController::commitStructure(const QJsonArray &input, const QJsonArray &regions) {
    if (!loaded() || busy() || processing()) return false;
    if (input.size() > 1500 || regions.size() > 6000) return structureError(QStringLiteral("区域过多，请拆分文档。"));
    const QRectF pageBounds(0, 0, pageWidth(), pageObject(m_page)["height"].toDouble());
    qsizetype sourceSize = 0, translationSize = 0; QSet<QString> ids; auto blocks = input;
    for (int i = 0; i < blocks.size(); ++i) {
        auto b = blocks[i].toObject(); const auto r = PdfLayout::rect(b["region"]), original = PdfLayout::rect(b["rect"]);
        if (!finiteRect(r) || original.isEmpty() || !pageBounds.adjusted(-1, -1, 1, 1).contains(r) || !pageBounds.adjusted(-1, -1, 1, 1).contains(original)
            || b["id"].toString().isEmpty() || ids.contains(b["id"].toString())) return structureError(QStringLiteral("校正后的区域无效或超出页面。"));
        if (b["source"].toString().size() > 16000 || b["translation"].toString().size() > 16000) return structureError(QStringLiteral("单个区域最多 16,000 字符。"));
        for (const auto &v : rawBlocks(m_page)) if (v.toObject()["id"] == b["id"]) {
            const auto old = v.toObject(); if (old["region"] != b["region"] || old["rect"] != b["rect"] || old["masks"] != b["masks"]) { b.remove("backgroundConfirmed"); b.remove("backgroundPreviewed"); }
        }
        sourceSize += b["source"].toString().size(); translationSize += b["translation"].toString().size(); ids.insert(b["id"].toString()); b["order"] = i; b.remove("layoutError"); blocks[i] = b;
    }
    if (sourceSize > 32000 || translationSize > 64000) return structureError(QStringLiteral("校正后的页面文字超过限制，请拆分文档。"));
    ids.clear();
    for (const auto &v : regions) { const auto r = v.toObject();
        if (ids.contains(r["id"].toString()) || r["id"].toString().isEmpty() || !finiteRect(PdfLayout::rect(r["rect"]))
            || !pageBounds.adjusted(-1, -1, 1, 1).contains(PdfLayout::rect(r["rect"]))) return structureError(QStringLiteral("栏或单元格边界无效。"));
        ids.insert(r["id"].toString());
    }
    if ((!m_undoStore.isOpen() && !m_undoStore.open(m_previewDirectory.filePath("undo.sqlite")))
        || !m_undoStore.putPage(m_page, pageObject(m_page))) return structureError(m_undoStore.error());
    m_layoutUndo.insert(m_page);
    const auto layout = PdfLayout::structure(blocks, regions);
    if (isScanPage(m_page)) { auto scan = pageObject(m_page)["scan"].toObject(); scan["blocks"] = blocks; scan["layout"] = layout; updateScan(m_page, scan); }
    else { auto p = pageObject(m_page); p["blocks"] = blocks; p["layout"] = layout; setPageObject(m_page, p, true); }
    m_error.clear(); saveCache(); requestRender(); emit changed(); return true;
}
bool PdfController::undoLayout() {
    if (busy() || processing() || !m_layoutUndo.contains(m_page)) return false;
    const auto previous = m_undoStore.page(m_page);
    if (previous.isEmpty()) return structureError(m_undoStore.error());
    setPageObject(m_page, previous, true); m_layoutUndo.remove(m_page); m_error.clear(); saveCache(); requestRender(); emit changed(); return true;
}
bool PdfController::moveBlock(const QString &id, int position) {
    auto blocks = rawBlocks(m_page); if (position < 0 || position >= blocks.size()) return structureError(QStringLiteral("阅读顺序超出范围。"));
    for (int i = 0; i < blocks.size(); ++i) if (blocks[i].toObject()["id"] == id) { const auto b = blocks.takeAt(i); blocks.insert(position, b); return commitStructure(blocks, regionsForPage(m_page)); }
    return false;
}
bool PdfController::reviseRegion(const QString &id, const QVariantMap &changes) {
    auto regions = regionsForPage(m_page); auto blocks = rawBlocks(m_page);
    for (int i = 0; i < regions.size(); ++i) {
        auto node = regions[i].toObject(); if (node["id"] != id) continue;
        if (changes.contains("rect")) {
            const auto r = PdfLayout::rect(QJsonArray::fromVariantList(changes["rect"].toList()));
            if (!finiteRect(r)) return structureError(QStringLiteral("区域宽高至少 3 点。"));
            if (node["kind"] == "table") return structureError(QStringLiteral("请调整表格中的单元格边界。"));
            for (int j = 0; j < regions.size(); ++j) { const auto other = regions[j].toObject(); if (i == j || other["parent"] != node["parent"] || other["kind"] != node["kind"]) continue;
                const auto overlap = r.intersected(PdfLayout::rect(other["rect"])); if (overlap.width() > .5 && overlap.height() > .5) return structureError(QStringLiteral("区域与相邻栏或单元格重叠，请先缩小相邻区域。"));
            }
            node["rect"] = PdfLayout::jsonRect(r);
            for (int b = 0; b < blocks.size(); ++b) { auto block = blocks[b].toObject(); if (block["container"] != id) continue;
                if (!r.adjusted(-1, -1, 1, 1).contains(PdfLayout::rect(block["rect"]))) return structureError(QStringLiteral("边界不能切掉原文。需要拆分时，请使用拆分区域并校对两部分原文。"));
                auto area = PdfLayout::rect(block["region"]); const auto old = PdfLayout::rect(block["containerRect"]);
                if (std::abs(area.right() - old.right()) < 4) area.setRight(r.right() - (node["kind"] == "cell" ? 2 : 0));
                if (std::abs(area.bottom() - old.bottom()) < 4) area.setBottom(r.bottom() - (node["kind"] == "cell" ? 2 : 0));
                block["containerRect"] = node["rect"]; block["region"] = PdfLayout::jsonRect(area.intersected(r));
                block.remove("backgroundConfirmed"); block.remove("backgroundPreviewed"); blocks[b] = block;
            }
        }
        if (changes.contains("headerRows")) {
            const int headers = changes["headerRows"].toInt(); if (node["kind"] != "table" || headers < 0 || headers > node["rows"].toInt()) return structureError(QStringLiteral("表头行数无效。"));
            node["headerRows"] = headers; node["headersEdited"] = true;
        }
        if (changes.contains("previousPage")) {
            if (node["kind"] != "table") return false;
            const int previous = changes["previousPage"].toInt(); node["continuationEdited"] = true; node.remove("continuedFrom"); node.remove("inheritedHeaders");
            if (previous >= 0) {
                if (previous >= m_page) return structureError(QStringLiteral("表头只能关联前面的页面。"));
                QJsonObject match;
                for (const auto &v : regionsForPage(previous)) { const auto candidate = v.toObject();
                    if (candidate["kind"] == "table" && candidate["columns"] == node["columns"]
                        && (changes["previousTable"].toString().isEmpty() || candidate["id"].toString() == changes["previousTable"].toString())) match = candidate;
                }
                auto headers = match["headers"].toArray(); if (headers.isEmpty()) headers = match["inheritedHeaders"].toArray();
                if (headers.isEmpty()) return structureError(QStringLiteral("所选页没有列数相同且已识别表头的表格。"));
                node["continuedFrom"] = QJsonObject{{"page", previous}, {"table", match["id"]}}; node["inheritedHeaders"] = headers;
            }
        }
        regions[i] = node; reindexTables(regions, blocks); return commitStructure(blocks, regions);
    }
    return false;
}

bool PdfController::mergeBlocks(const QStringList &selection) {
    const QSet<QString> ids(selection.begin(), selection.end()); if (ids.size() < 2) return structureError(QStringLiteral("请选择至少两个相邻区域。"));
    auto blocks = rawBlocks(m_page), regions = regionsForPage(m_page); QList<int> indices; QSet<QString> containers;
    QRectF sourceRect, area; QStringList sources, translations; QJsonArray masks, objects; QSet<int> objectIds; double cellArea = 0;
    QJsonObject merged;
    for (int i = 0; i < blocks.size(); ++i) {
        const auto b = blocks[i].toObject(); if (!ids.contains(b["id"].toString())) continue;
        if (b["protected"].toBool()) return structureError(QStringLiteral("原生文字对象不能独立修改，请先用识图处理该页。"));
        if (merged.isEmpty()) merged = b;
        if (b["table"] != merged["table"] || (b["table"].toString().isEmpty() && b["container"] != merged["container"])) return structureError(QStringLiteral("只能合并同一栏或同一表格中的相邻区域。"));
        indices.append(i); sources.append(b["source"].toString()); if (!b["translation"].toString().isEmpty()) translations.append(b["translation"].toString());
        const auto rect = PdfLayout::rect(b["rect"]), region = PdfLayout::rect(b["region"]); sourceRect = sourceRect.isEmpty() ? rect : sourceRect.united(rect); area = area.isEmpty() ? region : area.united(region);
        containers.insert(b["container"].toString()); for (const auto &m : b["masks"].toArray()) masks.append(m); for (const auto &o : b["objects"].toArray()) objectIds.insert(o.toInt());
    }
    if (indices.size() != ids.size()) return structureError(QStringLiteral("部分区域已变化，请重新选择。"));
    for (int i = indices.first(); i <= indices.last(); ++i) if (!ids.contains(blocks[i].toObject()["id"].toString())) return structureError(QStringLiteral("合并区域之间有其他文字，请按阅读顺序选择连续区域。"));
    if (!merged["table"].toString().isEmpty() && containers.size() > 1) {
        QJsonObject table; QRectF cells;
        for (const auto &v : regions) { const auto r = v.toObject(); if (r["id"] == merged["table"]) table = r;
            if (containers.contains(r["id"].toString())) { const auto b = PdfLayout::rect(r["rect"]); cells = cells.isEmpty() ? b : cells.united(b); cellArea += b.width() * b.height(); }
        }
        if (!table["inferred"].toBool()) return structureError(QStringLiteral("这些单元格之间有原始表格线，合并会让文字跨线。可合并无边框表格或同一单元格内的文字。"));
        if (std::abs(cells.width() * cells.height() - cellArea) > cellArea * .02) return structureError(QStringLiteral("请选择能组成矩形的相邻单元格。"));
        for (const auto &v : blocks) if (containers.contains(v.toObject()["container"].toString()) && !ids.contains(v.toObject()["id"].toString())) return structureError(QStringLiteral("请选中这些单元格内的全部文字。"));
        QJsonArray updated; for (const auto &v : regions) if (!containers.contains(v.toObject()["id"].toString())) updated.append(v);
        const auto cellId = uniqueId("cell"); updated.append(QJsonObject{{"id", cellId}, {"kind", "cell"}, {"parent", merged["table"]}, {"rect", PdfLayout::jsonRect(cells)}}); regions = updated;
        merged["container"] = cellId; merged["containerRect"] = PdfLayout::jsonRect(cells); area = cells.adjusted(2, 2, -2, -2);
    }
    for (int id : objectIds) objects.append(id);
    merged["id"] = uniqueId("block"); merged["source"] = sources.join('\n'); merged["draftTranslation"] = translations.join("\n\n"); merged["translation"] = "";
    merged["rect"] = PdfLayout::jsonRect(sourceRect); merged["region"] = PdfLayout::jsonRect(area); merged["objects"] = objects; merged["masks"] = masks;
    merged["fixed"] = false; merged["keep"] = false; merged["stale"] = false; merged.remove("styledSource"); merged.remove("originalSource"); merged.remove("numbersConfirmed");
    merged.remove("backgroundConfirmed"); merged.remove("backgroundPreviewed");
    QJsonArray updated;
    for (int i = 0; i < blocks.size(); ++i) { if (i == indices.first()) updated.append(merged); if (!ids.contains(blocks[i].toObject()["id"].toString())) updated.append(blocks[i]); }
    reindexTables(regions, updated); return commitStructure(updated, regions);
}

bool PdfController::splitBlock(const QString &id, bool vertical, double position, const QString &firstSource, const QString &secondSource) {
    if (!std::isfinite(position) || firstSource.trimmed().isEmpty() || secondSource.trimmed().isEmpty()) return structureError(QStringLiteral("请设置分界位置，并填写两部分的原文。"));
    auto blocks = rawBlocks(m_page), regions = regionsForPage(m_page);
    for (int i = 0; i < blocks.size(); ++i) {
        const auto original = blocks[i].toObject(); if (original["id"] != id) continue;
        if (original["protected"].toBool()) return structureError(QStringLiteral("请先用识图处理该页，再拆分区域。"));
        const bool cell = original["role"] == "cell";
        const auto bounds = PdfLayout::rect(original[cell ? "containerRect" : "region"]); QRectF first = bounds, second = bounds;
        if (vertical) { first.setRight(position); second.setLeft(position); } else { first.setBottom(position); second.setTop(position); }
        if (first.width() < 6 || first.height() < 6 || second.width() < 6 || second.height() < 6 || !bounds.contains(first) || !bounds.contains(second)) return structureError(QStringLiteral("分界必须位于区域内，两部分宽高至少 6 点。"));
        QJsonArray parts;
        if (cell) {
            for (const auto &v : blocks) if (v.toObject()["container"] == original["container"] && v.toObject()["id"] != id) return structureError(QStringLiteral("请先合并单元格内的文字，再拆分。"));
            QJsonArray updated; for (const auto &v : regions) if (v.toObject()["id"] != original["container"]) updated.append(v); regions = updated;
        }
        int partIndex = 0;
        for (const auto &area : {first, second}) {
            auto b = original; const auto partId = uniqueId("block"); b["id"] = partId;
            b["source"] = (partIndex++ == 0 ? firstSource : secondSource).trimmed(); b["draftTranslation"] = original["translation"]; b["draftSource"] = original["source"]; b["translation"] = "";
            b["stale"] = false; b["fixed"] = false; b["keep"] = false; b["manualSplit"] = true; b.remove("originalSource"); b.remove("styledSource"); b.remove("numbersConfirmed");
            b.remove("backgroundConfirmed"); b.remove("backgroundPreviewed");
            const auto source = PdfLayout::rect(original["rect"]).intersected(area); b["rect"] = PdfLayout::jsonRect(source.isEmpty() ? area.adjusted(2, 2, -2, -2) : source);
            b["region"] = PdfLayout::jsonRect(cell ? area.adjusted(2, 2, -2, -2) : area);
            QJsonArray masks; for (const auto &m : original["masks"].toArray()) { const auto r = PdfLayout::rect(m).intersected(area); if (!r.isEmpty()) masks.append(PdfLayout::jsonRect(r)); } b["masks"] = masks;
            if (cell) { const auto cellId = uniqueId("cell"); b["container"] = cellId; b["containerRect"] = PdfLayout::jsonRect(area);
                regions.append(QJsonObject{{"id", cellId}, {"kind", "cell"}, {"parent", b["table"]}, {"rect", PdfLayout::jsonRect(area)}});
            }
            parts.append(b);
        }
        blocks.removeAt(i); blocks.insert(i, parts[1]); blocks.insert(i, parts[0]); reindexTables(regions, blocks); return commitStructure(blocks, regions);
    }
    return false;
}
