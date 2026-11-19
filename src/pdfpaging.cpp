#include "pdfcontroller.h"
#include "appcontroller.h"
#include "pdfcache.h"
#include "pdflayout.h"
#include <QDir>
#include <QJsonDocument>

void PdfController::setPageSummary(int index, const QJsonObject &page) {
    applyPageSummary(index, PdfTaskStore::summary(index, page));
}
void PdfController::applyPageSummary(int index, const QJsonObject &summary) {
    if (index < 0 || index >= m_pageSummaries.size()) return;
    const auto adjust = [&](const QJsonObject &s, int sign) {
        if (!s["analyzed"].toBool() || s["extractionVersion"] != m_document["extractionVersion"]) return;
        m_counts.analyzed += sign;
        m_counts.total += sign * s["total"].toInt(); m_counts.completed += sign * s["completed"].toInt();
        m_counts.preserved += sign * s["preserved"].toInt();
        m_counts.scans += sign * int(s["scan"].toBool()); m_counts.recognized += sign * int(s["recognized"].toBool());
        m_counts.translated += sign * int(s["total"].toInt() > 0 && s["total"] == s["completed"]);
        m_counts.unsupported += sign * int(s["scan"].toBool() ? !s["hasBlocks"].toBool() : !s["warning"].toString().isEmpty());
    };
    adjust(m_pageSummaries[index], -1);
    m_pageSummaries[index] = summary;
    adjust(m_pageSummaries[index], 1);
}
bool PdfController::isPageReady(int page) const {
    return page >= 0 && page < m_pageSummaries.size() && m_pageSummaries[page]["analyzed"].toBool()
        && m_pageSummaries[page]["extractionVersion"] == m_document["extractionVersion"];
}

void PdfController::setPageObject(int index, const QJsonObject &value, bool linkHeaders) {
    if (index < 0 || index >= pageCount()) return;
    auto page = value; page["number"] = index;
    if (linkHeaders && index > 0) {
        const auto previous = pageObject(index - 1);
        if (isPageReady(index - 1)) page = PdfLayout::linkPageHeaders({previous, page}, index - 1)[1].toObject();
    }
    if (page != pageObject(index)) {
        m_dirtyPages[index] = page; setPageSummary(index, page); m_pageRevisions[index] = ++m_revision;
    }
    if (!linkHeaders) return;
    // Propagate changed inherited headers only while the following pages
    // actually change. At most a small batch is retained outside the database.
    for (int next = index + 1; next < pageCount(); ++next) {
        if (!isPageReady(next)) break;
        const auto before = pageObject(next);
        const auto after = PdfLayout::linkPageHeaders({page, before}, next - 1)[1].toObject();
        if (before == after) break;
        m_dirtyPages[next] = after; setPageSummary(next, after); m_pageRevisions[next] = ++m_revision; page = after;
        if (m_dirtyPages.size() >= 8 && !saveCache()) return;
    }
}

QString PdfController::cachePath() const {
    return QDir(m_app->dataDirectory()).filePath("pdf-tasks/" + m_document["hash"].toString() + ".sqlite");
}
bool PdfController::saveCache() {
    m_cacheTimer.stop();
    if (m_document.isEmpty()) return true;
    if (!m_store.isOpen()) { m_error = QStringLiteral("PDF 任务数据库未打开。"); return false; }
    auto metadata = m_store.metadata();
    if (!m_store.error().isEmpty()) { m_error = m_store.error(); return false; }
    for (auto it = m_document.constBegin(); it != m_document.constEnd(); ++it) metadata[it.key()] = it.value();
    metadata["snapshot"] = m_snapshot; metadata["storageVersion"] = 6;
    metadata["workflow"] = m_workflow;
    if (resetPending()) metadata["resetCursor"] = m_resetPage; else metadata.remove("resetCursor");
    if (m_dirtyPages.isEmpty() && metadata == m_store.metadata()) return true;
    if (!m_store.begin()) { m_error = m_store.error(); return false; }
    for (auto it = m_dirtyPages.cbegin(); it != m_dirtyPages.cend(); ++it) {
        if (!m_store.putPage(it.key(), it.value())) { m_error = m_store.error(); m_store.rollback(); return false; }
    }
    if (!m_store.setMetadata(metadata) || !m_store.commit()) { m_error = m_store.error(); m_store.rollback(); return false; }
    m_dirtyPages.clear(); return true;
}

bool PdfController::restoreCache() {
    m_dirtyPages.clear(); m_pageSummaries = QVector<QJsonObject>(pageCount()); m_counts = {};
    m_pageRevisions = QVector<quint64>(pageCount(), ++m_revision); m_analysisCursor = 0;
    if (!m_store.open(cachePath())) { m_error = m_store.error(); return false; }
    auto metadata = m_store.metadata();
    if (!m_store.error().isEmpty()) { m_error = m_store.error(); return false; }
    if ((!metadata.isEmpty() && metadata["hash"] != m_document["hash"]) || metadata["storageVersion"].toInt() > 6) {
        m_error = QStringLiteral("PDF 任务数据与源文件或当前版本不兼容，已保留任务文件。"); return false;
    }
    for (auto it = m_document.constBegin(); it != m_document.constEnd(); ++it) metadata[it.key()] = it.value();
    if (!m_store.setMetadata(metadata)) { m_error = m_store.error(); return false; }
    const auto legacyPath = QDir(m_app->dataDirectory()).filePath("pdf-tasks/" + m_document["hash"].toString() + ".json");
    if (!PdfCache::importLegacy(legacyPath, m_store, &m_error)) return false;
    const auto restored = m_store.metadata(); m_snapshot = restored["snapshot"].toObject();
    m_workflow = restored["workflow"].toObject(); m_workflowActive = false;
    if (!taskPending()) m_workflow = {};
    if (m_workflow.value("state") == "running") m_workflow["state"] = "paused";
    if (m_workflow.value("state") == "failed") m_error = m_workflow.value("error").toString();
    m_issueIndexCursor = 0;
    m_resetPage = restored["resetCursor"].toInt(-1); m_resetting = resetPending();
    if (m_resetPage > pageCount()) { m_error = QStringLiteral("PDF 清除进度无效，任务数据已保留。"); return false; }
    bool revisitEarlierPages = false;
    for (const auto &value : m_store.summaries()) {
        const auto summary = value.toObject(); applyPageSummary(summary["number"].toInt(-1), summary);
        const int number = summary["number"].toInt(-1);
        if (m_workflow["kind"] == "translate" && number >= m_workflow["first"].toInt() && number < m_workflow["cursor"].toInt()
            && summary.contains("extractionVersion") && summary["extractionVersion"] != m_document["extractionVersion"]) revisitEarlierPages = true;
    }
    if (revisitEarlierPages) {
        // Re-extraction can recover paragraphs before the saved cursor. Revisit
        // them once; matching completed translations are reused by the queue.
        m_workflow["cursor"] = m_workflow["first"]; m_workflow["state"] = "paused";
        m_workflow.remove("error"); m_error.clear();
    }
    if (!m_store.error().isEmpty()) { m_error = m_store.error(); return false; }
    m_ready = isPageReady(0); m_opening = !m_ready;
    m_analyzing = m_counts.analyzed < pageCount(); m_renderPending = true;
    return saveCache();
}

bool PdfController::acceptPages(const QJsonArray &pages) {
    for (const auto &value : pages) {
        const auto fresh = value.toObject(); const int number = fresh["number"].toInt(-1);
        if (number < 0 || number >= pageCount() || fresh["extractionVersion"] != m_document["extractionVersion"] || !fresh["analyzed"].toBool()) {
            m_error = QStringLiteral("PDF 分页解析结果无效，请重试。"); return false;
        }
        if (isPageReady(number)) continue;
        const auto saved = pageObject(number);
        if (!m_store.error().isEmpty()) { m_error = m_store.error(); return false; }
        auto page = PdfCache::reconcile(fresh, saved);
        setPageObject(number, page, true);
        if (number == m_page) m_renderPending = true;
    }
    if (!saveCache()) return false;
    while (m_analysisCursor < pageCount() && isPageReady(m_analysisCursor)) ++m_analysisCursor;
    m_ready = isPageReady(0); if (m_ready) m_opening = false;
    if (m_counts.analyzed == pageCount()) m_analyzing = false;
    return true;
}

void PdfController::scheduleWork() { if (!m_workTimer.isActive()) m_workTimer.start(0); }

void PdfController::pump() {
    if (!m_action.isEmpty() || m_document.isEmpty() || !m_store.isOpen()) return;
    if (resetPending()) { if (m_resetting) resetBatch(); return; }
    if (m_busy && m_waitingTranslation) sendNext();
    if (!m_action.isEmpty()) return;
    if (m_recognizing && m_waitingRecognition) sendNextRecognition();
    if (!m_action.isEmpty()) return;
    if (m_exportRequested && m_counts.analyzed == pageCount()) {
        if (!runWorker("export", {{"destination", m_exportPath}, {"preserveFailed", m_preserveExportFailures}})) { m_exportRequested = false; emit changed(); }
        return;
    }
    if (m_renderPending && isPageReady(m_page)) {
        m_renderPending = false; m_renderPage = m_page; m_renderRevision = m_pageRevisions[m_page];
        runWorker("render", {{"page", m_page}, {"pixels", 1500}}); return;
    }
    while (m_analysisCursor < pageCount() && isPageReady(m_analysisCursor)) ++m_analysisCursor;
    int first = -1;
    if (m_renderPending && !isPageReady(m_page)) first = m_page;
    else if (m_busy && m_waitingTranslation && m_translationPage <= m_translationLast) first = m_translationPage;
    else if (m_recognizing && m_waitingRecognition && m_recognitionIndex < m_recognitionQueue.size()) first = m_recognitionQueue[m_recognitionIndex];
    else if (m_analyzing && m_analysisCursor < pageCount()) first = m_analysisCursor;
    if (first >= 0) {
        // Include the adjacent predecessor when jumping to a later page, so
        // ordinary continued tables immediately receive their header context.
        if (first > 0 && !isPageReady(first - 1) && first != m_analysisCursor) --first;
        if (isPageReady(first)) first = m_analysisCursor;
        if (first >= pageCount()) return;
        int count = 1;
        while (count < (m_analyzing ? 8 : 2) && first + count < pageCount() && !isPageReady(first + count)) ++count;
        m_inspectFirst = first; m_inspectCount = count;
        if (!runWorker("inspectPages", {{"first", first}, {"count", count}, {"quickSourceCheck", true}})) {
            m_analyzing = false; m_opening = false; m_renderPending = false; emit changed();
        }
        return;
    }
    indexIssuesBatch();
}

void PdfController::pauseAnalysis() {
    if (m_workflowActive) pauseTask();
    m_analyzing = false; m_opening = false;
    if (m_action == "inspectPages" || m_action == "recognitionImage") stopWorker();
    if (!isPageReady(m_page)) m_renderPending = false;
    if (m_busy) { m_client.cancel(); stopTranslation(QStringLiteral("解析已暂停，已完成的译文保留。")); }
    if (m_recognizing) { m_client.cancel(); stopRecognition(QStringLiteral("解析已暂停。")); }
    saveCache(); emit changed(); scheduleWork();
}
void PdfController::resumeAnalysis() {
    if (resetPending()) { m_error.clear(); m_resetting = true; emit changed(); scheduleWork(); return; }
    if (m_document.isEmpty() || processing() || m_counts.analyzed == pageCount()) return;
    if (!m_store.isOpen()) { loadFile(QUrl::fromLocalFile(m_path)); return; }
    m_error.clear(); m_analyzing = true; m_opening = !m_ready;
    if (!isPageReady(m_page)) m_renderPending = true;
    emit changed(); scheduleWork();
}

void PdfController::workerProgress() {
    m_progressBuffer += m_worker.readAllStandardOutput();
    if (m_progressBuffer.size() > 1024 * 1024) { m_progressBuffer.clear(); return; }
    bool updated = false;
    for (qsizetype end; (end = m_progressBuffer.indexOf('\n')) >= 0;) {
        const auto line = m_progressBuffer.first(end); m_progressBuffer.remove(0, end + 1);
        const auto value = QJsonDocument::fromJson(line).object()["progress"].toObject();
        if (m_action.isEmpty() || value.isEmpty()) continue;
        const auto stage = value["stage"].toString(); const auto done = value["completed"].toInteger();
        if (stage.isEmpty() || done < 0 || (stage == m_progressStage && done <= m_progressCompleted)) continue;
        m_progressStage = stage; m_progressCompleted = done; m_progressTotal = value["total"].toInteger();
        m_timeout.start(); updated = true;
    }
    if (updated) emit changed();
}
QString PdfController::operationStatus() const {
    if (resetPending()) return QStringLiteral("%1 · %2 / %3 页").arg(m_resetting ? QStringLiteral("正在清除译文") : QStringLiteral("清除译文已暂停")).arg(m_resetPage).arg(pageCount());
    if (m_action == "info") return QStringLiteral("正在读取 PDF · 校验 %1 / %2 MiB").arg(m_progressCompleted / 1048576).arg(m_progressTotal / 1048576);
    if (m_action == "export") {
        if (m_progressStage == "fingerprint") return QStringLiteral("正在核对原 PDF…");
        if (m_progressStage == "preflight") return QStringLiteral("正在检查全书排版 · %1 / %2 页").arg(m_progressCompleted).arg(pageCount());
        if (m_progressStage == "saving") return QStringLiteral("正在写入译文 PDF · %1 MiB").arg(m_progressCompleted / 1048576);
        if (m_progressStage == "checkpoint") return QStringLiteral("正在暂存已排版页面 · %1 MiB").arg(m_progressCompleted / 1048576);
        if (m_progressStage == "assembling") return QStringLiteral("正在写入译文 PDF · %1%").arg(m_progressCompleted);
        return QStringLiteral("正在导出 · %1 / %2 页").arg(m_progressCompleted).arg(pageCount());
    }
    if (m_exportRequested) return QStringLiteral("准备导出 · 已解析 %1 / %2 页").arg(analyzedPages()).arg(pageCount());
    if (m_analyzing || m_action == "inspectPages") return QStringLiteral("正在解析 · %1 / %2 页 · 已解析页面可以阅读和校对").arg(analyzedPages()).arg(pageCount());
    if (pageCount() && m_counts.analyzed < pageCount()) return QStringLiteral("解析已暂停 · %1 / %2 页").arg(analyzedPages()).arg(pageCount());
    return {};
}
double PdfController::operationProgress() const {
    if (resetPending()) return pageCount() ? double(m_resetPage) / pageCount() : 0;
    if (m_action == "info" || m_action == "export") return m_progressTotal > 0 ? qBound(0.0, double(m_progressCompleted) / double(m_progressTotal), 1.0) : 0;
    return pageCount() ? double(analyzedPages()) / pageCount() : 0;
}

void PdfController::resetBatch() {
    if (!m_resetting || !resetPending()) return;
    const auto clearBlocks = [](QJsonObject value) {
        value.remove("translation"); value.remove("stale");
        auto blocks = value["blocks"].toArray();
        for (int i = 0; i < blocks.size(); ++i) {
            auto block = blocks[i].toObject();
            for (const auto &key : {"translation", "stale", "layoutError", "numbersConfirmed", "backgroundConfirmed", "backgroundPreviewed", "translationError", "rejectedTranslation", "translationAttempts"}) block.remove(key);
            blocks[i] = block;
        }
        if (value.contains("blocks")) value["blocks"] = blocks;
        return value;
    };
    const int last = qMin(m_resetPage + 8, pageCount());
    while (m_resetPage < last) {
        auto page = pageObject(m_resetPage);
        if (!m_store.error().isEmpty()) { m_error = m_store.error(); m_resetting = false; emit changed(); return; }
        if (!page.isEmpty()) {
            page = clearBlocks(page);
            if (page.contains("scan")) page["scan"] = clearBlocks(page["scan"].toObject());
            if (page.contains("legacy")) {
                auto legacy = page["legacy"].toObject(); legacy.remove("translations");
                if (legacy.contains("layout")) legacy["layout"] = clearBlocks(legacy["layout"].toObject());
                if (legacy.contains("scan")) legacy["scan"] = clearBlocks(legacy["scan"].toObject());
                page["legacy"] = legacy;
            }
            setPageObject(m_resetPage, page); m_layoutUndo.remove(m_resetPage);
        }
        ++m_resetPage;
    }
    if (m_resetPage == pageCount()) { m_resetPage = -1; m_resetting = false; m_snapshot = {}; m_renderPending = true; }
    if (!saveCache()) { m_resetting = false; emit changed(); return; }
    emit changed(); scheduleWork();
}
