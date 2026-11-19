#include "pdfcontroller.h"
#include "appcontroller.h"
#include "visionprompt.h"
#include "pdflayout.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QPainter>
#include <QRegularExpression>
#include <cmath>

bool PdfController::isScanPage(int page) const {
    return isPageReady(page) && m_pageSummaries[page]["scan"].toBool();
}
QJsonArray PdfController::rawBlocks(int page) const {
    if (!isPageReady(page)) return {};
    const auto p = pageObject(page);
    if (!isScanPage(page)) return p["blocks"].toArray();
    return p["scan"].toObject()["blocks"].toArray();
}
QJsonArray PdfController::effectiveBlocks(int page) const {
    auto blocks = rawBlocks(page);
    for (int i = 0; i < blocks.size(); ++i) {
        auto b = blocks[i].toObject();
        if (b["stale"].toBool()) b["translation"] = QString();
        if (b["keep"].toBool()) b["fixed"] = true;
        blocks[i] = b;
    }
    return blocks;
}
QString PdfController::scanSource() const { return pageObject(m_page)["scan"].toObject()["source"].toString(); }
QString PdfController::scanTranslation() const { return pageObject(m_page)["scan"].toObject()["translation"].toString(); }
QString PdfController::recognitionDraft() const {
    const auto scan = pageObject(m_page)["scan"].toObject(); const auto source = scan["draftSource"].toString(), translation = scan["draftTranslation"].toString();
    if (source.isEmpty() && translation.isEmpty()) return {};
    return QStringLiteral("上次原文\n%1\n\n上次译文\n%2").arg(source, translation);
}
bool PdfController::translationStale() const {
    if (scanMode()) return pageObject(m_page)["scan"].toObject()["stale"].toBool();
    for (const auto &v : rawBlocks(m_page)) { const auto b = v.toObject(); if (b["stale"].toBool() && !b["keep"].toBool()) return true; }
    return false;
}
void PdfController::updateScan(int page, const QJsonObject &value) {
    auto scan = value;
    if (scan.contains("blocks")) {
        if (scan.contains("layout")) scan["layout"] = PdfLayout::structure(scan["blocks"].toArray(), scan["layout"].toObject()["regions"].toArray());
        QStringList source, translation; bool stale = false;
        for (const auto &v : scan["blocks"].toArray()) {
            const auto b = v.toObject(); const bool fixed = b["fixed"].toBool() || b["keep"].toBool();
            source.append(b["source"].toString());
            translation.append(fixed ? b["source"].toString() : b["translation"].toString());
            if (!fixed && b["stale"].toBool() && !b["translation"].toString().isEmpty()) stale = true;
        }
        scan["source"] = source.join("\n\n"); scan["translation"] = translation.join("\n\n"); scan["stale"] = stale;
    }
    auto p = pageObject(page); p["scan"] = scan; setPageObject(page, p, true);
}
void PdfController::setScanSource(const QString &text) {
    if (!loaded() || !scanMode() || busy() || processing() || text == scanSource()) return;
    if (text.size() > 32000) { m_error = QStringLiteral("识别原文每页最多 32,000 字符。"); emit changed(); return; }
    if (layoutReady()) { m_error = QStringLiteral("请点击页面上的段落修改原文，以保留其位置。"); emit changed(); return; }
    auto scan = pageObject(m_page)["scan"].toObject(); scan["source"] = text;
    if (!scan["translation"].toString().isEmpty()) scan["stale"] = true;
    updateScan(m_page, scan); m_cacheTimer.start(); emit changed();
}
void PdfController::setScanTranslation(const QString &text) {
    if (!loaded() || !scanMode() || busy() || processing() || (text == scanTranslation() && !translationStale())) return;
    if (text.size() > 64000) { m_error = QStringLiteral("每页译文最多 64,000 字符。"); emit changed(); return; }
    if (layoutReady()) { m_error = QStringLiteral("请点击页面上的段落修改译文，以保留其位置。"); emit changed(); return; }
    auto scan = pageObject(m_page)["scan"].toObject(); scan["translation"] = text; scan["stale"] = false;
    updateScan(m_page, scan); m_cacheTimer.start(); emit changed();
}
void PdfController::setVisionProviderId(const QString &id) {
    if (busy() || m_visionProviderId == id) return;
    m_visionProviderId = id; m_settings.setValue("pdf/visionProvider", id); emit changed();
}
int PdfController::scanPages() const { return m_counts.scans; }
int PdfController::recognizedPages() const { return m_counts.recognized; }
int PdfController::translatedPages() const { return m_counts.translated; }
QString PdfController::recognitionStatus() const {
    if (m_recognizing) return QStringLiteral("正在识别第 %1 页 · 区域 %2 / %3 · 完成后可校对、复制原文")
        .arg(m_recognitionQueue.value(m_recognitionIndex) + 1).arg(qMin(m_recognitionBlock + 1, int(m_recognitionBlocks.size()))).arg(m_recognitionBlocks.size());
    if (!scanMode()) return QStringLiteral("文字提取不完整时，可用 AI 重新识别当前页。");
    if (!layoutReady()) return QStringLiteral("扫描页 · 先识别文字区域与原文，再校对、翻译。");
    const auto scan = pageObject(m_page)["scan"].toObject();
    return QStringLiteral("原文可编辑、复制 · %1 · %2").arg(scan["provider"].toObject()["name"].toString(),
        scanSource().contains("[unclear]") ? QStringLiteral("含不清晰文字，请校对") : QStringLiteral("请对照原图校对"));
}
bool PdfController::prepareRecognition(bool workflow) {
    if (!loaded() || (!workflow && busy()) || processing()) return false;
    m_error.clear();
    if (m_app->busy() || m_app->documentBusy() || m_app->visionBusy() || m_app->testBusy())
        m_error = QStringLiteral("请等待当前翻译、识图或连接测试结束。");
    const auto savedId = workflow ? m_workflow["vision"].toObject()["id"].toString() : QString();
    QString error; m_visionProvider = m_app->providerById(savedId.isEmpty() ? m_visionProviderId : savedId, &error);
    if (error.isEmpty()) error = AiClient::validate(m_visionProvider);
    if (m_error.isEmpty()) m_error = error;
    if (!m_error.isEmpty()) { emit changed(); return false; }
    return true;
}
void PdfController::recognizePages(int first, int last, int pixels, bool force) {
    if (first < 1 || last < first || last > pageCount() || !prepareRecognition()) return;
    m_recognitionQueue.clear();
    for (int p = first - 1; p < last; ++p)
        if (force || !isPageReady(p) || (isScanPage(p) && rawBlocks(p).isEmpty())) m_recognitionQueue.append(p);
    if (m_recognitionQueue.isEmpty()) { emit toast(QStringLiteral("所选范围没有待识别的扫描页；重试已有内容请用「识别当前页」。")); return; }
    m_recognitionForce = force; m_waitingRecognition = false;
    m_recognitionRegion = {}; m_recognitionPixels = qBound(1000, pixels, 2400);
    m_recognitionIndex = 0; m_recognizing = true; ++m_taskGeneration; m_app->setPdfBusy(true);
    emit changed(); sendNextRecognition();
}
void PdfController::recognizeRegion(const QRectF &region, int pixels) {
    if (!std::isfinite(region.x()) || !std::isfinite(region.y()) || !std::isfinite(region.width()) || !std::isfinite(region.height())
        || region.width() < .005 || region.height() < .005 || !QRectF(0, 0, 1, 1).contains(region) || !prepareRecognition()) return;
    m_recognitionForce = true; m_waitingRecognition = false;
    m_regionText.clear(); m_recognitionRegion = region; m_recognitionPixels = qBound(1000, pixels, 2400);
    m_recognitionQueue = {m_page}; m_recognitionIndex = 0; m_recognizing = true; ++m_taskGeneration;
    m_app->setPdfBusy(true); emit changed(); sendNextRecognition();
}
void PdfController::sendNextRecognition() {
    if (!m_recognizing) return;
    m_waitingRecognition = true;
    if (!m_action.isEmpty()) return;
    while (m_recognitionIndex < m_recognitionQueue.size()) {
        const int p = m_recognitionQueue[m_recognitionIndex];
        if (!isPageReady(p)) { m_analyzing = true; scheduleWork(); emit changed(); return; }
        if (m_recognitionForce || (isScanPage(p) && rawBlocks(p).isEmpty())) break;
        ++m_recognitionIndex;
    }
    if (m_recognitionIndex >= m_recognitionQueue.size()) { stopRecognition(); return; }
    m_waitingRecognition = false;
    m_recognitionBlocks = {}; m_recognitionBlock = 0; m_recognitionImage = {};
    const auto r = m_recognitionRegion.isEmpty() ? QRectF(0, 0, 1, 1) : m_recognitionRegion;
    if (!runWorker("recognitionImage", {{"page", m_recognitionQueue[m_recognitionIndex]}, {"pixels", m_recognitionPixels},
            {"region", QJsonArray{r.x(), r.y(), r.width(), r.height()}}, {"detectLayout", m_recognitionRegion.isEmpty()}})) stopRecognition(m_error);
}
void PdfController::sendRecognitionBlock() {
    if (!m_recognizing || m_recognitionBlock >= m_recognitionBlocks.size()) return;
    const auto block = m_recognitionBlocks[m_recognitionBlock].toObject(); auto bounds = PdfLayout::rect(block["rect"]);
    for (const auto &mask : block["masks"].toArray()) bounds = bounds.united(PdfLayout::rect(mask));
    const auto page = pageObject(m_recognitionQueue[m_recognitionIndex]);
    const double sx = m_recognitionImage.width() / page["width"].toDouble(), sy = m_recognitionImage.height() / page["height"].toDouble();
    const auto area = QRectF(bounds.x() * sx, bounds.y() * sy, bounds.width() * sx, bounds.height() * sy)
        .adjusted(-6, -6, 6, 6).toAlignedRect().intersected(m_recognitionImage.rect());
    if (area.isEmpty()) { stopRecognition(QStringLiteral("文字区域坐标无效，请重试。")); return; }
    const auto crop = m_recognitionImage.copy(area);
    // Vision encoders work in square patches. Thin line crops can lose their
    // first/last characters during model-side resizing; pad with whitespace.
    QImage image(crop.width() + 48, qMax(crop.height() + 48, qMax(160, (crop.width() + 48) / 4)), QImage::Format_RGB32);
    image.fill(Qt::white); QPainter painter(&image); painter.drawImage((image.width() - crop.width()) / 2, (image.height() - crop.height()) / 2, crop); painter.end();
    const auto prompt = visionTranscriptionPrompt() + QStringLiteral("\nThis is one paragraph cropped from a PDF. Join wrapped lines in the same paragraph. "
        "Keep bullet markers. If it is only a logo or artwork, return exactly [keep]. Do not describe the image.");
    m_client.startImage(m_visionProvider, prompt, QStringLiteral("Read the original text in this region."), image);
    emit changed();
}
void PdfController::recognitionCompleted(const QString &text) {
    if (text.trimmed().isEmpty() || text.size() > 32000) { stopRecognition(QStringLiteral("识别原文为空或超过每页 32,000 字符，请缩小区域后重试。")); return; }
    if (!m_recognitionRegion.isEmpty()) {
        m_regionText = text; stopRecognition(); emit regionRecognized(); return;
    }
    const int page = m_recognitionQueue[m_recognitionIndex];
    auto block = m_recognitionBlocks[m_recognitionBlock].toObject();
    const auto content = text.trimmed(); block["source"] = content == "[keep]" ? QStringLiteral("[图形 / Logo]") : content;
    block["keep"] = content == "[keep]" || block["artwork"].toBool();
    block["fixed"] = !content.contains(QRegularExpression(QStringLiteral("\\p{L}")))
        || QRegularExpression(QStringLiteral("^(?:(?:https?://|www\\.)\\S+|[\\w.-]+\\.[a-z]{2,}(?:/\\S*)?)$"), QRegularExpression::CaseInsensitiveOption).match(content).hasMatch();
    const auto previous = rawBlocks(page);
    for (const auto &v : previous) {
        const auto old = v.toObject(); if (old["id"] != block["id"]) continue;
        block["translation"] = old["translation"];
        block["stale"] = !old["translation"].toString().isEmpty() && (old["source"] != block["source"] || old["stale"].toBool());
    }
    m_recognitionBlocks[m_recognitionBlock] = block;
    if (++m_recognitionBlock < m_recognitionBlocks.size()) {
        const auto generation = m_taskGeneration;
        QTimer::singleShot(0, this, [this, generation] { if (m_recognizing && generation == m_taskGeneration) sendRecognitionBlock(); });
        emit changed(); return;
    }
    QStringList allText;
    for (const auto &v : m_recognitionBlocks) allText.append(v.toObject()["source"].toString());
    const auto fullText = allText.join("\n\n");
    if (fullText.size() > 32000) { stopRecognition(QStringLiteral("本页原文超过 32,000 字符，请拆分页面。")); return; }
    auto scan = pageObject(page)["scan"].toObject();
    if (!scan["source"].toString().isEmpty()) scan["draftSource"] = scan.value("source");
    if (!scan["translation"].toString().isEmpty()) scan["draftTranslation"] = scan.value("translation");
    if (!pageObject(page).contains("scan")) {
        QStringList previousTranslations;
        for (const auto &value : pageObject(page)["blocks"].toArray()) {
            const auto block = value.toObject(); const auto translation = block["translation"].toString();
            if (!block["fixed"].toBool() && !translation.isEmpty()) previousTranslations.append(translation);
        }
        const auto previous = previousTranslations.join("\n\n");
        if (previous.size() > 64000) { stopRecognition(QStringLiteral("本页已有译文超过扫描页长度限制，请先导出或精简译文。")); return; }
        if (!previous.isEmpty()) scan["draftTranslation"] = previous;
    }
    scan["blocks"] = m_recognitionBlocks;
    scan["layout"] = m_recognitionLayout;
    scan["provider"] = QJsonObject{{"id", m_visionProvider.id}, {"name", m_visionProvider.name}, {"protocol", m_visionProvider.protocol},
        {"baseUrl", m_visionProvider.baseUrl}, {"model", m_visionProvider.model}, {"reasoningEffort", m_visionProvider.reasoningEffort}};
    scan["pixels"] = m_recognitionPixels;
    updateScan(page, scan);
    if (!saveCache()) { stopRecognition(m_error); return; }
    ++m_recognitionIndex;
    const auto generation = m_taskGeneration; emit changed();
    QTimer::singleShot(0, this, [this, generation] { if (generation == m_taskGeneration && m_recognizing) sendNextRecognition(); });
}
void PdfController::stopRecognition(const QString &error) {
    m_waitingRecognition = false;
    ++m_taskGeneration; m_recognizing = false;
    if (!error.isEmpty()) m_error = error + QStringLiteral(" 已保留完整识别结果、原文修改和译文。");
    m_recognitionImage = {}; m_recognitionBlocks = {}; m_recognitionLayout = {};
    if (m_workflowActive) {
        if (!error.isEmpty()) { stopTranslation(m_error); requestRender(); return; }
        m_busy = true; m_waitingTranslation = false;
        const auto generation = m_taskGeneration;
        saveCache(); requestRender(); emit changed();
        QTimer::singleShot(0, this, [this, generation] { if (m_workflowActive && m_busy && generation == m_taskGeneration) sendNext(); });
        return;
    }
    m_app->setPdfBusy(false);
    saveCache(); requestRender(); emit changed();
}
void PdfController::copyText(const QString &text) {
    if (!text.isEmpty()) { QGuiApplication::clipboard()->setText(text); emit toast(QStringLiteral("已复制")); }
}
