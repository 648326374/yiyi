#include "pdfcontroller.h"
#include "appcontroller.h"
#include <QCryptographicHash>
#include <QJsonDocument>

namespace {
QJsonObject visionIdentity(const AiProvider &p) {
    return {{"id", p.id}, {"name", p.name}, {"protocol", p.protocol}, {"baseUrl", p.baseUrl},
            {"model", p.model}, {"reasoningEffort", p.reasoningEffort}};
}
QStringList reviewReasons(const QJsonObject &s) {
    const auto issues = s["issues"].toObject(); QStringList reasons;
    if (issues["unclear"].toInt()) reasons << QStringLiteral("文字不清晰");
    if (issues["stale"].toInt()) reasons << QStringLiteral("原文已变动");
    if (issues["layout"].toInt()) reasons << QStringLiteral("排版待调整");
    if (issues["background"].toInt()) reasons << QStringLiteral("背景待确认");
    if (issues["translation"].toInt()) reasons << QStringLiteral("译文返回异常，待重试");
    return reasons;
}
}

QString PdfController::taskProviderId() const { return m_snapshot["provider"].toObject()["id"].toString(); }
QString PdfController::taskTarget() const { return m_snapshot["target"].toString(m_app->targetLanguage()); }
void PdfController::setRequestTimeoutMinutes(int minutes) {
    if (busy() || minutes < 1 || minutes > 60 || minutes == requestTimeoutMinutes()) return;
    m_client.setInactivityTimeout(minutes * 60000);
    m_settings.setValue("pdf/requestTimeoutMinutes", minutes); emit changed();
}
QString PdfController::taskModel() const {
    if (m_recognizing) return m_visionProvider.model;
    if (!m_snapshot.isEmpty()) return m_snapshot["provider"].toObject()["model"].toString();
    return m_app->modelName();
}
QString PdfController::taskScope() const {
    if (m_workflow.isEmpty()) return {};
    const auto first = m_workflow["first"].toInt(), last = m_workflow["last"].toInt();
    if (m_workflow["kind"] == "retranslate") return QStringLiteral("重译第 %1 页").arg(first + 1);
    return first == last ? QStringLiteral("第 %1 页").arg(first + 1) : QStringLiteral("整篇文档");
}
void PdfController::translate(bool currentPageOnly, int pixels) {
    if (taskPending()) { resumeTask(); return; }
    if (!loaded() || busy() || processing()) return;
    m_workflow = {{"kind", "translate"}, {"first", currentPageOnly ? m_page : 0},
                  {"last", currentPageOnly ? m_page : pageCount() - 1}, {"cursor", currentPageOnly ? m_page : 0},
                  {"pixels", qBound(1000, pixels, 2400)}, {"state", "paused"}};
    resumeTask();
}
bool PdfController::retranslatePage() {
    if (!loaded() || !pageReady() || busy() || processing() || pageTotal() == 0) return false;
    // Nothing on the page is changed until all replacement paragraphs succeed.
    m_workflow = {{"kind", "retranslate"}, {"first", m_page}, {"last", m_page}, {"cursor", m_page},
                  {"signature", retranslationSignature(m_page)}, {"staged", QJsonObject{}}, {"state", "paused"}};
    resumeTask(); return m_workflowActive;
}
void PdfController::resumeTask() {
    if (!taskPending() || !loaded() || busy() || processing()) return;
    m_error.clear();
    if (!prepareSnapshot()) { m_workflow["state"] = "failed"; m_workflow["error"] = m_error; saveCache(); emit changed(); return; }
    const int first = m_workflow["first"].toInt(), last = m_workflow["last"].toInt();
    if (first < 0 || last < first || last >= pageCount()) { m_error = QStringLiteral("已保存的任务范围无效，请结束此任务后重新开始。"); emit changed(); return; }
    if (m_workflow["kind"] == "retranslate") {
        const auto signature = retranslationSignature(first);
        if (m_workflow["signature"].toString() != signature) {
            m_workflow["staged"] = QJsonObject{}; m_workflow["signature"] = signature;
            m_workflow["cursor"] = first;
        }
    }
    m_workflow["state"] = "running"; m_workflow.remove("error");
    if (!saveCache()) { emit changed(); return; }
    m_workflowActive = true;
    beginTranslation(qBound(first, m_workflow["cursor"].toInt(first), last), last);
}
void PdfController::discardTask() {
    if (busy() || processing()) return;
    m_workflow = {}; m_error.clear(); saveCache(); emit changed();
}
void PdfController::pauseTask() {
    // Invalidate queued callbacks before cancelling network/worker operations.
    ++m_taskGeneration; m_workflowActive = false; m_workflow["state"] = "paused"; m_workflow.remove("error");
    m_translationRetryPending = false; m_retryBlockKey.clear(); m_translationRetry = 0;
    m_client.cancel(); if (m_action == "recognitionImage") stopWorker();
    m_busy = m_recognizing = m_waitingTranslation = m_waitingRecognition = false;
    m_pendingTranslation.clear(); m_parts.clear(); m_recognitionImage = {}; m_recognitionBlocks = {}; m_recognitionLayout = {};
    m_app->setPdfBusy(false); m_error.clear(); saveCache(); requestRender(); emit changed();
}
bool PdfController::beginWorkflowRecognition(int page) {
    if (!prepareRecognition(true)) { stopTranslation(m_error); return false; }
    const auto identity = visionIdentity(m_visionProvider);
    const auto saved = m_workflow["vision"].toObject();
    if (!saved.isEmpty()) {
        for (const auto &key : {"id", "protocol", "baseUrl", "model", "reasoningEffort"}) if (identity[key] != saved[key]) {
            stopTranslation(QStringLiteral("任务的识图模型配置已改变，请恢复原设置后继续，或结束此任务后重新开始。")); return false;
        }
    } else m_workflow["vision"] = identity;
    if (!saveCache()) { stopTranslation(m_error); return false; }
    m_busy = false; m_recognizing = true; m_recognitionForce = false; m_waitingRecognition = false;
    m_recognitionRegion = {}; m_recognitionPixels = m_workflow["pixels"].toInt(1800);
    m_recognitionQueue = {page}; m_recognitionIndex = 0;
    emit changed(); sendNextRecognition(); return true;
}
QString PdfController::retranslationSignature(int page) const {
    QJsonArray content;
    for (const auto &v : rawBlocks(page)) {
        const auto b = v.toObject(); QJsonObject entry;
        for (const auto &key : {"id", "source", "translation", "stale", "fixed", "keep"}) entry[key] = b[key];
        content.append(entry);
    }
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(content).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
bool PdfController::stageTranslation(const QString &id, const QString &translation) {
    auto staged = m_workflow["staged"].toObject(); staged[id] = translation; m_workflow["staged"] = staged;
    return saveCache();
}
bool PdfController::commitRetranslation() {
    const int page = m_workflow["first"].toInt();
    if (m_workflow["signature"].toString() != retranslationSignature(page)) {
        m_error = QStringLiteral("页面内容已改变，已保留原译文，请继续以重新翻译当前内容。"); return false;
    }
    auto blocks = rawBlocks(page); const auto staged = m_workflow["staged"].toObject();
    for (int i = 0; i < blocks.size(); ++i) {
        auto b = blocks[i].toObject(); if (b["fixed"].toBool() || b["keep"].toBool()) continue;
        const auto text = staged[b["id"].toString()].toString();
        if (text.isEmpty()) { m_error = QStringLiteral("本页重译结果尚不完整，原译文已保留。"); return false; }
        b["translation"] = text; b["stale"] = false;
        for (const auto &key : {"layoutError", "backgroundConfirmed", "backgroundPreviewed", "translationError", "rejectedTranslation", "translationAttempts"}) b.remove(key);
        blocks[i] = b;
    }
    // Page contents and clearing the pending task are one SQLite transaction.
    const auto previousPage = pageObject(page), previousWorkflow = m_workflow;
    if (isScanPage(page)) { auto scan = previousPage["scan"].toObject(); scan["blocks"] = blocks; updateScan(page, scan); }
    else { auto p = previousPage; p["blocks"] = blocks; setPageObject(page, p); }
    m_workflow = {};
    if (!saveCache()) { setPageObject(page, previousPage); m_workflow = previousWorkflow; return false; }
    if (page == m_page) requestRender(); return true;
}

QString PdfController::activityStatus() const {
    if (processing()) return operationStatus();
    if (m_translationRetryPending) {
        const auto position = m_queue.value(m_queueIndex);
        return QStringLiteral("第 %1 页 · 第 %2 段返回异常，正在自动重试 %3 / 2 · 已有译文保留")
            .arg(position.first + 1).arg(position.second + 1).arg(m_translationRetry);
    }
    const auto waiting = m_client.requestStatus();
    const auto suffix = waiting.isEmpty() ? QString() : QStringLiteral(" · ") + waiting;
    if (m_recognizing) return QStringLiteral("正在识别第 %1 页 · %2 / %3 个区域").arg(m_recognitionQueue.value(m_recognitionIndex) + 1)
        .arg(qMin(m_recognitionBlock + 1, int(m_recognitionBlocks.size()))).arg(m_recognitionBlocks.size()) + suffix;
    if (m_busy) {
        const int page = m_waitingTranslation ? m_translationPage : m_queue.value(m_queueIndex, {m_translationPage, 0}).first;
        return QStringLiteral("%1第 %2 页 · %3 / %4 段").arg(m_waitingTranslation ? QStringLiteral("正在准备") : m_workflow["kind"] == "retranslate" ? QStringLiteral("正在重译") : QStringLiteral("正在翻译"))
            .arg(page + 1).arg(m_queueIndex).arg(m_queue.size()) + suffix;
    }
    if (taskPending()) return QStringLiteral("%1 · %2").arg(taskScope(), m_workflow["state"] == "failed" ? QStringLiteral("处理失败，可继续重试") : QStringLiteral("已暂停，可继续"));
    if (!loaded()) return QStringLiteral("打开 PDF，保留原版式翻译");
    if (m_analyzing) return operationStatus();
    return pageStatus();
}
void PdfController::rejectTranslation(const QString &error) {
    if (!m_busy || m_queueIndex >= m_queue.size()) return;
    const auto position = m_queue[m_queueIndex];
    const auto located = QStringLiteral("第 %1 页，第 %2 段：%3").arg(position.first + 1).arg(position.second + 1).arg(error);
    // Direct block retry remains explicit. Document workflows get bounded
    // retries and durable deferred failures.
    if (!m_workflowActive) { stopTranslation(located); return; }
    auto block = rawBlocks(position.first)[position.second].toObject();
    const bool retranslate = m_workflow["kind"] == "retranslate";
    if (retranslate) {
        // Do not mutate the page/signature: replacement remains atomic.
        m_workflow["rejected"] = QJsonObject{{"page", position.first}, {"id", block["id"]},
            {"error", error}, {"response", m_pendingTranslation.left(16000)}, {"attempts", m_translationRetry + 1}};
    } else {
        block["translationError"] = error; block["rejectedTranslation"] = m_pendingTranslation.left(16000);
        block["translationAttempts"] = m_translationRetry + 1;
        updateBlock(position.first, position.second, block);
        auto deferred = m_workflow["deferredPages"].toArray();
        if (!deferred.contains(position.first)) deferred.append(position.first);
        m_workflow["deferredPages"] = deferred;
    }
    if (!saveCache()) { stopTranslation(m_error); return; }
    const auto generation = m_taskGeneration;
    if (m_translationRetry < 2) {
        ++m_translationRetry; m_translationRetryPending = true; m_error.clear();
        const auto key = m_retryBlockKey;
        QTimer::singleShot(m_translationRetry * 1000, this, [this, generation, key] {
            if (!m_busy || !m_workflowActive || generation != m_taskGeneration || key != m_retryBlockKey || !m_translationRetryPending) return;
            m_translationRetryPending = false; sendNext();
        });
        emit changed(); return;
    }
    m_translationRetryPending = false;
    if (retranslate || ++m_consecutiveRejectedBlocks >= 3) {
        if (!retranslate) for (const auto &p : m_workflow["deferredPages"].toArray())
            m_workflow["cursor"] = qMin(m_workflow["cursor"].toInt(), p.toInt());
        stopTranslation(located + (retranslate ? QStringLiteral(" 自动重试 2 次仍失败，本页原译文已保留。")
            : QStringLiteral(" 连续 3 段在重试后仍失败，已暂停，避免继续消耗请求。")));
        return;
    }
    // Keep this block untranslated and visible in the issue list, then move
    // on once. Finish with deferred work still pending instead of claiming
    // that the entire document has been translated.
    ++m_queueIndex; m_retryBlockKey.clear(); m_translationRetry = 0; m_pendingTranslation.clear(); m_error.clear();
    QTimer::singleShot(0, this, [this, generation] { if (m_busy && generation == m_taskGeneration) sendNext(); });
    emit toast(QStringLiteral("第 %1 页的异常段落已记入待校对，继续处理后文。").arg(position.first + 1)); emit changed();
}

QString PdfController::deferredTranslationError() {
    if (!m_workflowActive || m_workflow["kind"] != "translate") return {};
    int first = pageCount(), remaining = 0; QJsonArray pendingPages;
    for (const auto &v : m_workflow["deferredPages"].toArray()) {
        const int page = v.toInt(-1);
        if (page < m_workflow["first"].toInt() || page > m_workflow["last"].toInt() || page >= pageCount()) continue;
        int count = 0;
        for (const auto &value : rawBlocks(page)) {
            const auto b = value.toObject();
            if (!b["fixed"].toBool() && !b["keep"].toBool() && b["translation"].toString().isEmpty() && !b["translationError"].toString().isEmpty()) ++count;
        }
        if (!m_store.error().isEmpty()) return m_store.error();
        if (count) { remaining += count; first = qMin(first, page); pendingPages.append(page); }
    }
    m_workflow["deferredPages"] = pendingPages;
    if (!remaining) return {};
    m_workflow["cursor"] = first;
    return QStringLiteral("本轮已处理到第 %1 页，仍有 %2 段返回异常（最早在第 %3 页）。可查看待校对，或点击继续重试；已完成段落不会重复翻译。")
        .arg(m_translationLast + 1).arg(remaining).arg(first + 1);
}

double PdfController::activityProgress() const {
    if (processing()) return operationProgress();
    if (m_workflowActive) {
        const int first = m_workflow["first"].toInt(), last = m_workflow["last"].toInt();
        double part = m_queue.isEmpty() ? 0 : double(m_queueIndex) / m_queue.size();
        if (m_recognizing) part = m_recognitionBlocks.isEmpty() ? 0 : .5 * m_recognitionBlock / m_recognitionBlocks.size();
        return qBound(0.0, (m_workflow["cursor"].toInt(first) - first + part) / qMax(1, last - first + 1), 1.0);
    }
    if (m_recognizing) return double(m_recognitionIndex) / qMax(1, int(m_recognitionQueue.size()));
    if (m_busy) return double(completed()) / qMax(1, total());
    return operationProgress();
}
int PdfController::pendingPages() const {
    int count = 0;
    for (int p = 0; p < pageCount(); ++p) {
        const auto &s = m_pageSummaries[p];
        if (!isPageReady(p) || (s["scan"].toBool() && !s["hasBlocks"].toBool()) || s["total"].toInt() > s["completed"].toInt()
            || (!s["scan"].toBool() && !s["warning"].toString().isEmpty())) ++count;
    }
    return count;
}
int PdfController::reviewPages() const {
    int count = 0; for (int p = 0; p < m_pageSummaries.size(); ++p) if (isPageReady(p) && m_pageSummaries[p]["review"].toInt()) ++count; return count;
}
QVariantList PdfController::issuePages() const {
    QVariantList rows;
    for (int p = 0; p < pageCount(); ++p) {
        const auto &s = m_pageSummaries[p]; QStringList reasons; QString severity = "pending";
        if (!isPageReady(p)) reasons << QStringLiteral("尚未解析");
        else {
            if (s["scan"].toBool() && !s["hasBlocks"].toBool()) reasons << QStringLiteral("扫描页待识别");
            else if (s["total"].toInt() > s["completed"].toInt()) reasons << QStringLiteral("%1 段待翻译").arg(s["total"].toInt() - s["completed"].toInt());
            if (!s["scan"].toBool() && !s["warning"].toString().isEmpty()) reasons << s["warning"].toString();
            const auto reviews = reviewReasons(s); if (!reviews.isEmpty()) { severity = "review"; reasons << reviews; }
        }
        if (!s["analysisError"].toString().isEmpty()) { reasons << s["analysisError"].toString(); severity = "error"; }
        if (taskPending() && m_workflow["state"] == "failed" && m_workflow["cursor"].toInt() == p) {
            reasons << m_workflow["error"].toString(); severity = "error";
        }
        if (!reasons.isEmpty()) rows << QVariantMap{{"page", p}, {"reason", reasons.join(QStringLiteral(" · "))}, {"severity", severity}};
    }
    return rows;
}
QVariantList PdfController::pageIssues() const {
    QVariantList result;
    for (const auto &v : rawBlocks(m_page)) {
        const auto b = v.toObject(); const auto issues = PdfTaskStore::blockIssues(b); QStringList reasons;
        for (const auto &issue : issues) reasons << issue.toObject()["reason"].toString();
        if (!reasons.isEmpty()) result << QVariantMap{{"blockId", b["id"].toString()}, {"reason", reasons.join(QStringLiteral(" · "))}, {"source", b["source"].toString()}};
    }
    return result;
}
void PdfController::indexIssuesBatch() {
    if (m_issueIndexCursor < 0) return;
    const int last = qMin(m_issueIndexCursor + 8, pageCount());
    for (; m_issueIndexCursor < last; ++m_issueIndexCursor) {
        const int p = m_issueIndexCursor;
        if (!isPageReady(p) || m_pageSummaries[p]["issueVersion"].toInt() >= 1) continue;
        const auto value = pageObject(p);
        if (!m_store.error().isEmpty()) { m_error = m_store.error(); m_issueIndexCursor = -1; emit changed(); return; }
        m_dirtyPages[p] = value; setPageSummary(p, value);
    }
    if (m_issueIndexCursor >= pageCount()) m_issueIndexCursor = -1;
    if (!saveCache()) m_issueIndexCursor = -1;
    emit changed(); if (m_issueIndexCursor >= 0) scheduleWork();
}
