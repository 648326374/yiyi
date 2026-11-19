#include "fileimportcontroller.h"
#include "documentcontroller.h"
#include "pdfcontroller.h"
#include "officecontroller.h"
#include <QFileInfo>
#include <QStringList>

FileImportController::FileImportController(DocumentController *documents, PdfController *pdf, OfficeController *office, QObject *parent)
    : QObject(parent), m_documents(documents), m_pdf(pdf), m_office(office)
{
    connect(documents, &DocumentController::changed, this, &FileImportController::availabilityChanged);
    connect(pdf, &PdfController::changed, this, &FileImportController::availabilityChanged);
    if (office) {
        connect(office, &OfficeController::changed, this, &FileImportController::availabilityChanged);
        connect(office, &OfficeController::exported, this, [this] {
            if (confirmationPending() && replacementKind() != "txt") confirmReplacement();
        });
    }
}

void FileImportController::setTextEncoding(const QString &encoding) {
    if (m_textEncoding == encoding || !QStringList{"auto", "utf8", "gb18030", "utf16le", "utf16be"}.contains(encoding)) return;
    m_textEncoding = encoding; emit encodingChanged();
}
QString FileImportController::textBlockReason() const {
    return m_documents->busy() ? QStringLiteral("请先停止 TXT 翻译，再更换文件。") : QString();
}
QString FileImportController::pdfBlockReason() const {
    if (m_pdf->busy()) return QStringLiteral("请先暂停 PDF 识别或翻译，再更换文件。");
    return m_pdf->processing() ? QStringLiteral("请等待 PDF 打开、导出或清除完成后，再更换文件。") : QString();
}
QString FileImportController::blockReason(const QString &kind) const {
    return kind == QLatin1String("txt") ? textBlockReason() : kind == "pdf" ? pdfBlockReason() : officeBlockReason();
}
QString FileImportController::officeBlockReason() const {
    return !m_office ? QStringLiteral("Office 文档模块不可用。") : m_office->busy() || m_office->processing() ? QStringLiteral("请先停止 Word / PPT 翻译，或等待文档处理完成。") : QString();
}
QString FileImportController::replacementKind() const {
    return m_pendingUrl.path().endsWith(".txt", Qt::CaseInsensitive) ? QStringLiteral("txt") : m_office ? m_office->kind() : QStringLiteral("txt");
}
QUrl FileImportController::replacementOutput() const {
    return replacementKind() == "txt" ? m_documents->suggestedOutput() : m_office ? m_office->suggestedOutput() : QUrl();
}
QString FileImportController::pendingFileName() const { return QFileInfo(m_pendingUrl.toLocalFile()).fileName(); }

QVariantMap FileImportController::inspect(const QList<QUrl> &urls) const {
    if (urls.size() != 1) return {{"error", QStringLiteral("请一次拖入一个 TXT、DOCX、PPTX 或 PDF 文件。")}};
    const auto &url = urls.first();
    if (!url.isLocalFile()) return {{"error", QStringLiteral("请选择本机文档，不支持网络链接。")}};
    const QFileInfo file(url.toLocalFile());
    if (file.isDir()) return {{"error", QStringLiteral("请拖入文件，暂不支持文件夹。")}};
    const auto kind = file.suffix().toLower();
    if (kind != "txt" && kind != "pdf" && kind != "docx" && kind != "pptx")
        return {{"error", kind == "doc" || kind == "ppt" ? QStringLiteral("旧版 DOC / PPT 请先另存为 DOCX / PPTX。") : QStringLiteral("仅支持 TXT、DOCX、PPTX 和 PDF 文件。")}};
    if (!file.isFile() || !file.isReadable()) return {{"error", QStringLiteral("无法读取文件，请检查文件是否存在及读取权限。")}};
    return {{"kind", kind}, {"fileName", file.fileName()}};
}

bool FileImportController::requestDrop(const QList<QUrl> &urls) {
    const auto info = inspect(urls);
    if (info.contains("error")) { emit toast(info["error"].toString()); return false; }
    return requestOpen(urls.first());
}
bool FileImportController::requestOpen(const QUrl &url) {
    if (confirmationPending()) { emit toast(QStringLiteral("请先处理当前文档的更换提示。")); return false; }
    const auto info = inspect({url});
    if (info.contains("error")) { emit toast(info["error"].toString()); return false; }
    const auto kind = info["kind"].toString(), reason = blockReason(kind);
    if (!reason.isEmpty()) { emit toast(reason); return false; }
    if ((kind == "txt" && m_documents->hasUnexportedResult()) || ((kind == "docx" || kind == "pptx") && m_office && m_office->hasUnexportedResult())) {
        m_pendingUrl = url; m_pendingEncoding = m_textEncoding; emit pendingChanged(); return true;
    }
    return open(url, m_textEncoding);
}
bool FileImportController::open(const QUrl &url, const QString &encoding) {
    // Revalidate after a confirmation/export dialog: files and tasks may change.
    const auto info = inspect({url});
    if (info.contains("error")) { emit toast(info["error"].toString()); return false; }
    const auto kind = info["kind"].toString(), reason = blockReason(kind);
    if (!reason.isEmpty()) { emit toast(reason); return false; }
    const bool loaded = kind == "txt" ? m_documents->loadFile(url, encoding) : kind == "pdf" ? m_pdf->loadFile(url) : m_office && m_office->loadFile(url);
    if (!loaded) { emit toast(kind == "txt" ? m_documents->error() : kind == "pdf" ? m_pdf->error() : m_office ? m_office->error() : officeBlockReason()); return false; }
    emit opened(kind); return true;
}
void FileImportController::cancelReplacement() {
    if (!confirmationPending()) return;
    m_pendingUrl.clear(); m_pendingEncoding.clear(); emit pendingChanged();
}
bool FileImportController::confirmReplacement() {
    if (!confirmationPending()) return false;
    const auto url = m_pendingUrl; const auto encoding = m_pendingEncoding;
    cancelReplacement(); return open(url, encoding);
}
bool FileImportController::exportAndReplace(const QUrl &destination) {
    if (!confirmationPending()) return false;
    // Never overwrite the incoming source with the previous document's result.
    const QFileInfo output(destination.toLocalFile()), incoming(m_pendingUrl.toLocalFile());
    const auto outputPath = output.exists() ? output.canonicalFilePath() : output.absoluteFilePath();
    const auto incomingPath = incoming.exists() ? incoming.canonicalFilePath() : incoming.absoluteFilePath();
    if (destination.isLocalFile() && outputPath.compare(incomingPath, Qt::CaseInsensitive) == 0) {
        emit toast(QStringLiteral("请另存当前译文，不能覆盖即将导入的文件。")); return false;
    }
    if (replacementKind() != "txt") {
        if (!m_office || !m_office->saveFile(destination)) { emit toast(m_office ? m_office->error() : officeBlockReason()); return false; }
        return true; // The exported signal continues replacement only after a successful write.
    }
    if (!m_documents->saveFile(destination)) { emit toast(m_documents->error()); return false; }
    return confirmReplacement();
}
