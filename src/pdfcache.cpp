#include "pdfcache.h"
#include "pdftaskstore.h"
#include "pdflayout.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <cmath>

namespace {
QJsonObject provider(const QJsonObject &input) {
    QJsonObject result;
    for (const auto &key : {"id", "name", "protocol", "baseUrl", "model", "reasoningEffort"})
        if (input.contains(key)) result[key] = input[key];
    return result;
}
bool validBlocks(const QJsonArray &blocks, const QJsonObject &page, bool scan, const QSet<int> &objects = {}) {
    if (blocks.size() > 1500) return false;
    qsizetype source = 0, translation = 0; QSet<QString> ids;
    for (const auto &item : blocks) {
        const auto b = item.toObject();
        if (!PdfCache::validBlock(b, page, scan) || ids.contains(b["id"].toString())) return false;
        ids.insert(b["id"].toString()); source += b["source"].toString().size(); translation += b["translation"].toString().size();
        if (!scan) {
            if (b["objects"].toArray().isEmpty()) return false;
            for (const auto &object : b["objects"].toArray()) if (!objects.contains(object.toInt(-1))) return false;
        }
    }
    return source <= 32000 && translation <= 64000;
}
bool hasOnlyGeneratedLayout(const QJsonObject &entry) {
    if (entry["baseLayoutHash"].toString().isEmpty()) return false;
    QJsonArray baseline;
    for (const auto &value : entry["blocks"].toArray()) {
        auto block = value.toObject();
        for (const auto &key : {"translation", "stale", "layoutError", "numbersConfirmed", "backgroundReview", "backgroundConfirmed", "backgroundPreviewed", "translationError", "rejectedTranslation", "translationAttempts"}) block.remove(key);
        baseline.append(block);
    }
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(baseline).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())
        == entry["baseLayoutHash"].toString();
}
}

bool PdfCache::validBlock(const QJsonObject &b, const QJsonObject &page, bool scan) {
    if (b["id"].toString().isEmpty() || b["source"].toString().size() > 16000 || b["translation"].toString().size() > 16000) return false;
    const QRectF bounds(0, 0, page["width"].toDouble(), page["height"].toDouble());
    const auto validRect = [&](const QJsonValue &value) {
        const auto r = value.toArray(); if (r.size() != 4) return false;
        for (const auto &v : r) if (!v.isDouble() || !std::isfinite(v.toDouble())) return false;
        return r[2].toDouble() > 0 && r[3].toDouble() > 0 && bounds.adjusted(-1, -1, 1, 1).contains(PdfLayout::rect(value));
    };
    const double size = b["size"].toDouble();
    if (!validRect(b["rect"]) || !validRect(b["region"]) || !std::isfinite(size) || size < 1 || size > 200) return false;
    if (scan) {
        const auto masks = b["masks"].toArray(); if ((masks.isEmpty() && !b["manualSplit"].toBool()) || masks.size() > 1500) return false;
        for (const auto &r : masks) if (!validRect(r)) return false;
    }
    return true;
}

bool PdfCache::importLegacy(const QString &path, PdfTaskStore &store, QString *error) {
    auto metadata = store.metadata();
    if (metadata["legacyImported"].toBool() || !QFile::exists(path)) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = QStringLiteral("旧 PDF 任务无法读取：%1").arg(file.errorString()); return false; }
    QJsonParseError parseError; const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) { *error = QStringLiteral("旧 PDF 任务损坏，原缓存已保留。"); return false; }
    const auto cache = document.object(); const int version = cache["version"].toInt();
    if (version < 1 || version > 4 || cache["hash"] != metadata["hash"]) { *error = QStringLiteral("旧 PDF 任务版本或源文件校验不匹配，原缓存已保留。"); return false; }
    QHash<int, QJsonObject> pages;
    const auto validPage = [&](int page) { return page >= 0 && page < metadata["pageCount"].toInt(); };
    static const QRegularExpression pageId(QStringLiteral("^p(\\d+)-"));
    for (const auto &value : cache["translations"].toArray()) {
        const auto block = value.toObject(); const auto match = pageId.match(block["id"].toString());
        bool ok = false; const int page = match.captured(1).toInt(&ok); if (!ok || !validPage(page)) continue;
        auto record = pages.value(page); auto translations = record["translations"].toArray(); translations.append(block);
        record["translations"] = translations; pages[page] = record;
    }
    for (const auto &value : cache["layouts"].toArray()) {
        const auto entry = value.toObject(); const int page = entry["page"].toInt(-1); if (!validPage(page)) continue;
        auto record = pages.value(page); record["layout"] = entry; pages[page] = record;
    }
    for (const auto &value : cache["scans"].toArray()) {
        const auto entry = value.toObject(); const int page = entry["page"].toInt(-1); if (!validPage(page)) continue;
        auto record = pages.value(page); auto scan = entry["scan"].toObject(); scan["provider"] = provider(scan["provider"].toObject());
        record["scan"] = scan; pages[page] = record;
    }
    if (!store.begin()) { *error = store.error(); return false; }
    for (auto it = pages.cbegin(); it != pages.cend(); ++it) {
        // Never replace a page already saved in the new task format.
        if (store.contains(it.key())) continue;
        auto record = it.value(); record["version"] = version;
        if (!store.putPage(it.key(), {{"number", it.key()}, {"analyzed", false}, {"legacy", record}})) {
            *error = store.error(); store.rollback(); return false;
        }
    }
    auto snapshot = cache["snapshot"].toObject(); snapshot["provider"] = provider(snapshot["provider"].toObject());
    if (!metadata.contains("snapshot") && !cache["snapshot"].toObject().isEmpty()) metadata["snapshot"] = snapshot;
    metadata["legacyImported"] = true;
    if (!store.setMetadata(metadata) || !store.commit()) { *error = store.error(); store.rollback(); return false; }
    return true;
}

QJsonObject PdfCache::reconcile(const QJsonObject &fresh, const QJsonObject &saved) {
    if (saved.isEmpty()) return fresh;
    auto legacy = saved["legacy"].toObject();
    if (legacy.isEmpty()) {
        legacy = {{"version", 4}, {"layout", QJsonObject{{"blocks", saved["blocks"]}, {"layout", saved["layout"]}, {"baseLayoutHash", saved["baseLayoutHash"]}}}};
        QJsonArray translations;
        for (const auto &v : saved["blocks"].toArray()) {
            const auto b = v.toObject(); if (!b["translation"].toString().isEmpty()) translations.append(b);
        }
        legacy["translations"] = translations;
        if (saved.contains("scan")) legacy["scan"] = saved["scan"];
    }
    auto page = fresh; auto blocks = page["blocks"].toArray();
    QHash<QString, QJsonObject> translations, bySource;
    QHash<QString, int> savedCounts, freshCounts;
    for (const auto &v : legacy["translations"].toArray()) {
        const auto b = v.toObject(); translations.insert(b["id"].toString(), b);
        const auto source = b["source"].toString(); bySource.insert(source, b); ++savedCounts[source];
    }
    for (const auto &v : blocks) ++freshCounts[v.toObject()["source"].toString()];
    for (int i = 0; i < blocks.size(); ++i) {
        auto b = blocks[i].toObject(); auto s = translations.value(b["id"].toString());
        const auto source = b["source"].toString();
        // Improved extraction can insert a block and shift every later ID.
        // An exact source occurring once on each side is an unambiguous match;
        // repeated labels and edited/changed text still require their old ID.
        if (s["source"] != b["source"] && savedCounts.value(source) == 1 && freshCounts.value(source) == 1) s = bySource.value(source);
        if (s["source"] == b["source"] && s["translation"].toString().size() <= 16000) {
            b["translation"] = s["translation"];
            for (const auto &key : {"stale", "numbersConfirmed"}) if (s.contains(key)) b[key] = s[key];
        }
        blocks[i] = b;
    }
    const auto entry = legacy["layout"].toObject(); bool structureRestored = false;
    if (legacy["version"].toInt() >= 3 && !entry.isEmpty()) {
        QSet<int> allowed;
        for (const auto &v : blocks) for (const auto &object : v.toObject()["objects"].toArray()) allowed.insert(object.toInt());
        const auto edits = entry["blocks"].toArray();
        if (legacy["version"].toInt() == 4 && !page["baseLayoutHash"].toString().isEmpty() && entry["baseLayoutHash"] == page["baseLayoutHash"]
            && !edits.isEmpty() && validBlocks(edits, page, false, allowed)) {
            blocks = edits; page["layout"] = PdfLayout::structure(edits, entry["layout"].toObject()["regions"].toArray()); structureRestored = true;
        }
        if (!structureRestored) {
            // Runtime translation state does not make the original generated
            // rectangles into user edits. An exact baseline digest lets an
            // upgraded extractor supply improved bounds without losing real
            // geometry/source edits in either new or legacy tasks.
            const bool generatedLayout = hasOnlyGeneratedLayout(entry);
            QHash<QString, QJsonObject> edits;
            for (const auto &item : entry["blocks"].toArray()) if (validBlock(item.toObject(), page, false)) edits.insert(item.toObject()["id"].toString(), item.toObject());
            for (int i = 0; i < blocks.size(); ++i) {
                auto b = blocks[i].toObject(); const auto edit = edits.value(b["id"].toString());
                if (edit.value("originalSource").toString(edit["source"].toString()) != b["source"].toString()) continue;
                for (const auto &key : {"source", "originalSource", "translation", "region", "size", "centered", "bold", "serif", "keep", "stale", "numbersConfirmed", "translationError", "rejectedTranslation", "translationAttempts"}) {
                    if (QString::fromLatin1(key) == "keep" && edit["protected"].toBool()) continue;
                    if (generatedLayout && QStringList{"region", "size", "centered", "bold", "serif"}.contains(QString::fromLatin1(key))) continue;
                    if (edit.contains(key)) b[key] = edit[key];
                }
                blocks[i] = b;
            }
        }
    }
    page["blocks"] = blocks;
    if (legacy.contains("scan")) {
        const auto scan = legacy["scan"].toObject();
        if (scan["source"].toString().size() <= 32000 && scan["translation"].toString().size() <= 64000) {
            QJsonObject restored;
            for (const auto &key : {"source", "translation", "stale", "pixels"}) if (scan.contains(key)) restored[key] = scan[key];
            restored["provider"] = provider(scan["provider"].toObject());
            for (const auto &key : {"draftSource", "draftTranslation"}) if (scan[key].toString().size() <= 64000 && scan.contains(key)) restored[key] = scan[key];
            const auto blocks = scan["blocks"].toArray();
            if (legacy["version"].toInt() >= 3 && scan.contains("blocks") && validBlocks(blocks, page, true)) {
                restored["blocks"] = blocks; restored["layout"] = PdfLayout::structure(blocks, scan["layout"].toObject()["regions"].toArray());
            }
            page["scan"] = restored;
        }
    }
    return page;
}
