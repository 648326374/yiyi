#include "pdfengine.h"
#include "pdfscanlayout.h"
#include "pdflayout.h"
#include "pdftaskstore.h"
#include "pdfmerge.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QBuffer>
#include <QPainter>
#include <QPdfWriter>
#include <QPageSize>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibrary>
#include <QRawFont>
#include <QRegularExpression>
#include <QSet>
#include <QMap>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTextLayout>
#include <QTransform>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdio>
#endif
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdf_save.h>
#include <fpdf_ppo.h>
#include <fpdf_transformpage.h>

namespace {
constexpr int manifestVersion = 1;
constexpr int extractionVersion = 14;
#define PDF_FUNCTIONS(X) \
    X(FPDF_InitLibrary) X(FPDF_DestroyLibrary) X(FPDF_LoadMemDocument64) X(FPDF_CloseDocument) \
    X(FPDF_GetPageCount) X(FPDF_LoadPage) X(FPDF_ClosePage) X(FPDF_GetPageWidthF) X(FPDF_GetPageHeightF) \
    X(FPDFPage_GetRotation) X(FPDFPage_GetMediaBox) X(FPDFPage_GetCropBox) X(FPDFPage_CountObjects) X(FPDFPage_GetObject) X(FPDFPageObj_GetType) \
    X(FPDFPageObj_GetBounds) X(FPDFText_LoadPage) X(FPDFText_ClosePage) X(FPDFText_CountChars) \
    X(FPDFPageObj_GetFillColor) X(FPDFPageObj_SetFillColor) X(FPDFPageObj_GetStrokeColor) X(FPDFPageObj_SetStrokeColor) \
    X(FPDFPageObj_GetIsActive) X(FPDFPageObj_SetIsActive) \
    X(FPDFPageObj_GetMatrix) X(FPDFPageObj_Transform) X(FPDFPageObj_TransformClipPath) X(FPDFPageObj_GetStrokeWidth) X(FPDFPath_GetDrawMode) X(FPDFPath_CountSegments) \
    X(FPDFPath_GetPathSegment) X(FPDFPathSegment_GetType) X(FPDFPathSegment_GetPoint) X(FPDFPathSegment_GetClose) \
    X(FPDFText_GetUnicode) X(FPDFText_GetCharBox) X(FPDFText_GetCharOrigin) X(FPDFText_GetFontSize) \
    X(FPDFText_GetCharAngle) X(FPDFText_GetTextObject) X(FPDFText_GetFillColor) X(FPDFText_GetFontWeight) X(FPDFText_GetFontInfo) \
    X(FPDFBitmap_CreateEx) X(FPDFBitmap_Destroy) X(FPDF_RenderPageBitmap) \
    X(FPDF_NewXObjectFromPage) X(FPDF_CloseXObject) X(FPDF_NewFormObjectFromXObject) \
    X(FPDFPage_InsertObject) X(FPDFPage_RemoveObject) X(FPDFPageObj_Destroy) \
    X(FPDFPage_GenerateContent) X(FPDF_SaveAsCopy) X(FPDF_ImportPages) X(FPDF_CreateNewDocument) \
    X(FPDFFormObj_CountObjects) X(FPDFFormObj_GetObject) X(FPDFFormObj_RemoveObject)

struct Api {
    QLibrary library;
#define DECLARE(name) decltype(&name) name = nullptr;
    PDF_FUNCTIONS(DECLARE)
#undef DECLARE
    Api() : library(QDir(QCoreApplication::applicationDirPath()).filePath("pdf/pdfium.dll")) {
#define RESOLVE(name) name = reinterpret_cast<decltype(name)>(library.resolve(#name)); if (!name) throw QStringLiteral("PDF 组件不完整，请运行 scripts/setup-pdf.ps1 或使用完整发行包。");
        PDF_FUNCTIONS(RESOLVE)
#undef RESOLVE
        FPDF_InitLibrary();
    }
    ~Api() { if (FPDF_DestroyLibrary) FPDF_DestroyLibrary(); }
};
struct Document {
    Api &api;
    QFile file;
    uchar *mapped = nullptr;
    FPDF_DOCUMENT handle = nullptr;
    std::unique_ptr<QTemporaryDir> editingDirectory;
    QString hash;
    QJsonObject stamp;
    Document(Api &a, const QString &path, const PdfEngine::Progress &progress = {}, const QJsonObject &verified = {}) : api(a), file(path) {
        if (!file.open(QIODevice::ReadOnly)) throw QStringLiteral("无法读取 PDF：%1").arg(file.errorString());
        const auto length = file.size();
        if (length <= 0 || quint64(length) > quint64(std::numeric_limits<qsizetype>::max())) throw QStringLiteral("PDF 为空或超出当前系统的地址空间。");
        const QFileInfo info(file);
        stamp = {{"size", length}, {"modified", info.lastModified().toUTC().toString(Qt::ISODateWithMs)}, {"path", info.canonicalFilePath()}};
        if (!verified.isEmpty() && (verified["stamp"].toObject() != stamp || verified["hash"].toString().isEmpty()))
            throw QStringLiteral("原 PDF 已变化，请重新导入后翻译。");
        mapped = file.map(0, length);
        if (!mapped) throw QStringLiteral("无法映射 PDF 文件，请检查可用内存和文件是否可访问。");
        if (!verified.isEmpty()) hash = verified["hash"].toString();
        else {
            QCryptographicHash digest(QCryptographicHash::Sha256);
            constexpr qint64 chunk = 4 * 1024 * 1024;
            for (qint64 offset = 0; offset < length; offset += chunk) {
                const auto size = qMin(chunk, length - offset);
                digest.addData(QByteArrayView(reinterpret_cast<const char *>(mapped + offset), qsizetype(size)));
                if (progress) progress({{"stage", "fingerprint"}, {"completed", offset + size}, {"total", length}});
            }
            hash = QString::fromLatin1(digest.result().toHex());
        }
        handle = api.FPDF_LoadMemDocument64(mapped, size_t(length), nullptr);
        if (!handle) throw QStringLiteral("无法打开此 PDF。请使用未加密、未损坏的 PDF 文件。");
    }
    void close() {
        if (handle) api.FPDF_CloseDocument(handle); handle = nullptr;
        if (mapped) file.unmap(mapped); mapped = nullptr; file.close();
    }
    ~Document() { close(); }
    void selectPages(int first, int last) {
        if (!editingDirectory) editingDirectory = std::make_unique<QTemporaryDir>();
        if (!editingDirectory->isValid()) throw QStringLiteral("无法创建 PDF 页面工作目录。");
        const auto source = file.fileName(), copy = editingDirectory->filePath("pages.pdf");
        close();
        // PDFium can misassign graphics-state changes when a text operand or
        // unfinished path spans multiple /Contents streams. Coalesce those
        // streams without changing page objects before any editing occurs.
        // Keep encoded images file-backed and work on at most one page batch.
        PdfMerge::copyPages(source, copy, first, last);
        reopenSource(copy);
        if (api.FPDF_GetPageCount(handle) != last - first + 1) throw QStringLiteral("无法提取 PDF 页面工作副本。");
    }
    void reopenSource(const QString &path) {
        // Closing the document releases PDFium's accumulated image streams,
        // imported font objects and decoded page caches between batches.
        close(); file.setFileName(path);
        if (!file.open(QIODevice::ReadOnly)) throw QStringLiteral("无法重新读取原 PDF。");
        mapped = file.map(0, file.size());
        if (!mapped) throw QStringLiteral("无法重新映射原 PDF。");
        handle = api.FPDF_LoadMemDocument64(mapped, size_t(file.size()), nullptr);
        if (!handle) throw QStringLiteral("无法重新打开原 PDF。");
    }
};
struct Page {
    Api &api; FPDF_PAGE handle;
    Page(Document &doc, int index) : api(doc.api), handle(api.FPDF_LoadPage(doc.handle, index)) {
        if (!handle) throw QStringLiteral("无法读取 PDF 页面。");
    }
    ~Page() { api.FPDF_ClosePage(handle); }
};
struct PageObject { FPDF_PAGEOBJECT handle, parent; QTransform transform; };
QList<PageObject> pageObjects(Api &api, FPDF_PAGE page) {
    QList<PageObject> result;
    std::function<void(FPDF_PAGEOBJECT, FPDF_PAGEOBJECT, QTransform, int)> visit;
    visit = [&](FPDF_PAGEOBJECT object, FPDF_PAGEOBJECT parent, QTransform transform, int depth) {
        if (!object || depth > 24 || result.size() >= 20000) throw QStringLiteral("页面对象过多或嵌套过深，请拆分或简化 PDF。");
        result.append({object, parent, transform});
        if (api.FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_FORM) return;
        FS_MATRIX matrix; if (!api.FPDFPageObj_GetMatrix(object, &matrix)) throw QStringLiteral("无法读取嵌套页面对象的坐标。");
        const QTransform local(matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f);
        const int count = api.FPDFFormObj_CountObjects(object);
        for (int i = 0; i < count; ++i) visit(api.FPDFFormObj_GetObject(object, i), object, local * transform, depth + 1);
    };
    const int count = api.FPDFPage_CountObjects(page);
    for (int i = 0; i < count; ++i) visit(api.FPDFPage_GetObject(page, i), nullptr, {}, 0);
    return result;
}
QJsonArray rectJson(const QRectF &r) { return {r.x(), r.y(), r.width(), r.height()}; }
QRectF rectValue(const QJsonValue &v) {
    const auto a = v.toArray();
    return a.size() == 4 ? QRectF(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()) : QRectF();
}
struct Glyph {
    QString text; QRectF box; double baseline = 0, size = 12;
    int object = -1, textIndex = -1, script = 0; bool bold = false, italic = false, serif = false, mono = false, spaceBefore = false; QJsonArray color{0, 0, 0};
};
struct StyleRun { QString text; bool bold = false, italic = false; int script = 0; };
struct Line {
    QString text; QRectF box; double baseline = 0, size = 12;
    QSet<int> objects; bool bold = false, marker = false; QJsonArray color{0, 0, 0};
    QList<StyleRun> runs; QList<QRectF> masks;
    int glyphCount = 0, boldCount = 0, serifCount = 0, monoCount = 0, proseFontLetters = 0;
    QList<double> sizes;
    int lastTextIndex = -2;
    QJsonObject layout;
    QString preserveReason;
};
int lineCommentStart(const QString &text, bool *inBlock = nullptr) {
    bool block = inBlock && *inBlock, escaped = false; QChar quote;
    for (int i = 0; i < text.size(); ++i) {
        const auto c = text[i], next = i + 1 < text.size() ? text[i + 1] : QChar();
        if (block) { if (c == '*' && next == '/') { block = false; ++i; } continue; }
        if (!quote.isNull()) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == quote) quote = {}; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '/' && next == '*') { block = true; ++i; continue; }
        if (c == '/' && next == '/') { if (inBlock) *inBlock = block; return i; }
    }
    if (inBlock) *inBlock = block;
    return -1;
}
bool wrappedLineComment(const QString &previous, const QString &next, bool hanging = false) {
    // A new // introduces a separate code/comment line, never a continuation
    // of the preceding comment (including enum entries with inline comments).
    if (lineCommentStart(next) >= 0) return false;
    // Types/declarations and function calls must never be swallowed by a
    // typographically wrapped comment. Accept plain prose after a long line.
    static const QRegularExpression syntax(QStringLiteral("^(?:struct|class|enum|typedef|using|in|out|inout|uniform|layout|const|attribute|varying|smooth|flat|centroid|patch|buffer|shared|precision|void|(?:bool|int|uint|float|double)(?:[1-4](?:x[1-4])?)?|[biud]?vec[234]|d?mat[234](?:x[234])?|sampler\\w*|[iu]?image(?:1D|2D|3D|Cube|Buffer|Rect)\\w*|return|if|else|for|while|do|switch|case|break|continue|discard)(?=\\s|[({;:]|$)|^[A-Za-z_]\\w*\\s*\\("));
    static const QRegularExpression prose(QStringLiteral("^[A-Za-z0-9(][A-Za-z0-9_ ,.'’“”\"()/*+%|–—…\\-]*[.!?:]?$"));
    // A typeset continuation can restart at the left margin, e.g. "lights;"
    // or "NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;". Require both
    // the hanging position and prose evidence before allowing code-like marks.
    static const QRegularExpression hangingProse(QStringLiteral("^[A-Za-z0-9(‘“\"'][A-Za-z0-9_ ,.'‘’“”\"()/+%|–—…\\[\\]\\-]*[.!?:;]?$"));
    static const QRegularExpression proseEvidence(QStringLiteral("\\b(?:are|is|the|of|to|for|with|from|and|or)\\b|^[A-Za-z]{2,20}[).,;]*$"));
    static const QRegularExpression proseKeyword(QStringLiteral("^(?:buffer\\s+(?:with|for|of|to|in|and|or|that|which)|(?:for|in|if|return|using)\\s+(?:this|the|a|an|each|every|all|it|we)|in\\s+(?:radians|degrees|world|local|view))\\b|\\((?:including|and|or|such as|so that|because|where|which)\\b"));
    const bool syntaxLine = syntax.match(next).hasMatch() && !proseKeyword.match(next).hasMatch();
    if (lineCommentStart(previous) >= 0 && previous.size() > 20 && !QStringLiteral(".;:{}").contains(previous.back())
        && QRegularExpression("^(?:that|where|when|with|and)\\s+[A-Za-z_]\\w*(?:\\[[^\\]\\n]+\\])?\\s*=\\s*[^;{}]+[.,]$").match(next).hasMatch()) return true;
    if (lineCommentStart(previous) >= 0 && previous.endsWith(" and")
        && QRegularExpression("^[A-Za-z_]\\w*(?:\\[[^\\]\\n]+\\])?\\s*=\\s*[^;{}]+\\.$").match(next).hasMatch()) return true;
    if (hanging && lineCommentStart(previous) >= 0 && previous.size() > 20
        && !QStringLiteral(".;:{}").contains(previous.back()) && !syntaxLine
        && hangingProse.match(next).hasMatch() && (proseEvidence.match(next).hasMatch()
            || (previous.endsWith(',') && !next.isEmpty() && QStringLiteral("‘“\"'").contains(next.front())))) return true;
    const bool headingContinuation = previous.size() > 20 && QRegularExpression("^[A-Z][a-z]+(?: [A-Z][a-z]+)+$").match(next).hasMatch();
    return (previous.size() > 40 || headingContinuation) && lineCommentStart(previous) >= 0
        && !QStringLiteral(".;:{}").contains(previous.back())
        && prose.match(next).hasMatch() && !syntaxLine
        && (next.contains(' ') || previous.size() > 55 || QRegularExpression("^(?:[A-Za-z]{3,}|\\d[\\d+*./-]*)[.!?]$").match(next).hasMatch());
}
void appendRun(Line &line, const QString &text, bool bold, bool italic, int script = 0) {
    if (!line.runs.isEmpty() && line.runs.last().bold == bold && line.runs.last().italic == italic && line.runs.last().script == script) line.runs.last().text += text;
    else line.runs.append({text, bold, italic, script});
}
struct Background { QRectF box; QRgb color; int object; };
QRectF pathBounds(Api &a, FPDF_PAGEOBJECT object, const QTransform &parent, double height) {
    FS_MATRIX m; if (!a.FPDFPageObj_GetMatrix(object, &m)) return {};
    const auto transform = QTransform(m.a, m.b, m.c, m.d, m.e, m.f) * parent;
    auto point = [&](int index) { float x = 0, y = 0; a.FPDFPathSegment_GetPoint(a.FPDFPath_GetPathSegment(object, index), &x, &y); const auto p = transform.map(QPointF(x, y)); return QPointF(p.x(), height - p.y()); };
    QPainterPath path; const int count = a.FPDFPath_CountSegments(object); if (count > 10000) return {};
    for (int i = 0; i < count; ++i) {
        auto segment = a.FPDFPath_GetPathSegment(object, i); const auto type = a.FPDFPathSegment_GetType(segment);
        if (type == FPDF_SEGMENT_MOVETO) path.moveTo(point(i));
        else if (type == FPDF_SEGMENT_LINETO) path.lineTo(point(i));
        else if (type == FPDF_SEGMENT_BEZIERTO && i + 2 < count) { path.cubicTo(point(i), point(i + 1), point(i + 2)); i += 2; segment = a.FPDFPath_GetPathSegment(object, i); }
        if (a.FPDFPathSegment_GetClose(segment)) path.closeSubpath();
    }
    int fill = 0; FPDF_BOOL stroke = false; a.FPDFPath_GetDrawMode(object, &fill, &stroke);
    if (fill == FPDF_FILLMODE_NONE && !stroke) return {};
    float width = 0; a.FPDFPageObj_GetStrokeWidth(object, &width);
    const double padding = stroke ? qMax(.1, double(width) * .5 * qMax(std::hypot(transform.m11(), transform.m12()), std::hypot(transform.m21(), transform.m22()))) : 0;
    return path.boundingRect().adjusted(-padding, -padding, padding, padding);
}
bool lightRectangle(Api &a, FPDF_PAGEOBJECT object, const QRectF &bounds, double pageHeight, QRgb *color, const QTransform &parent = {}) {
    if (a.FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_PATH || bounds.width() < 5 || bounds.height() < 5) return false;
    int fill = 0; FPDF_BOOL stroke = false;
    unsigned int red = 0, green = 0, blue = 0, alpha = 0;
    if (!a.FPDFPath_GetDrawMode(object, &fill, &stroke) || (fill == FPDF_FILLMODE_NONE && !stroke)
        || !a.FPDFPageObj_GetFillColor(object, &red, &green, &blue, &alpha)
        || alpha != 255) return false;
    const int count = a.FPDFPath_CountSegments(object);
    FS_MATRIX matrix;
    if ((count != 4 && count != 5) || !a.FPDFPageObj_GetMatrix(object, &matrix)) return false;
    QList<QPointF> points; int corners = 0;
    for (int i = 0; i < count; ++i) {
        const auto segment = a.FPDFPath_GetPathSegment(object, i); float x = 0, y = 0;
        if (a.FPDFPathSegment_GetType(segment) != (i == 0 ? FPDF_SEGMENT_MOVETO : FPDF_SEGMENT_LINETO)
            || !a.FPDFPathSegment_GetPoint(segment, &x, &y)) return false;
        const auto pdfPoint = parent.map(QPointF(x * matrix.a + y * matrix.c + matrix.e, x * matrix.b + y * matrix.d + matrix.f));
        const QPointF point(pdfPoint.x(), pageHeight - pdfPoint.y());
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return false;
        if (i == 4) {
            if ((point - points.first()).manhattanLength() > .1) return false;
        } else {
            const double tolerance = stroke ? 1.5 : .1;
            const int horizontal = std::abs(point.x() - bounds.left()) < tolerance ? 0 : std::abs(point.x() - bounds.right()) < tolerance ? 1 : -1;
            const int vertical = std::abs(point.y() - bounds.top()) < tolerance ? 0 : std::abs(point.y() - bounds.bottom()) < tolerance ? 1 : -1;
            if (horizontal < 0 || vertical < 0) return false;
            corners |= 1 << (horizontal + vertical * 2); points.append(point);
        }
        if (i == count - 1 && count == 4 && !a.FPDFPathSegment_GetClose(segment)) return false;
    }
    if (corners != 15) return false;
    for (int i = 0; i < 4; ++i) {
        const auto edge = points[(i + 1) % 4] - points[i];
        if (std::abs(edge.x()) > .1 && std::abs(edge.y()) > .1) return false;
    }
    *color = qRgb(red, green, blue); return true;
}
QList<QLineF> pathRules(Api &a, FPDF_PAGEOBJECT object, const QRectF &bounds, double height, const QTransform &parent = {}) {
    QList<QLineF> rules;
    if (a.FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_PATH) return rules;
    int fill = 0; FPDF_BOOL stroke = false;
    if (!a.FPDFPath_GetDrawMode(object, &fill, &stroke)) return rules;
    if (!stroke) {
        if (fill != FPDF_FILLMODE_NONE && bounds.height() <= 3 && bounds.width() >= 12) rules.append(QLineF(bounds.left(), bounds.center().y(), bounds.right(), bounds.center().y()));
        else if (fill != FPDF_FILLMODE_NONE && bounds.width() <= 3 && bounds.height() >= 12) rules.append(QLineF(bounds.center().x(), bounds.top(), bounds.center().x(), bounds.bottom()));
        return rules;
    }
    FS_MATRIX m; if (!a.FPDFPageObj_GetMatrix(object, &m)) return rules;
    QPointF previous, start; bool hasPoint = false;
    const auto appendRule = [&](const QPointF &first, const QPointF &last) {
        const QLineF rule(first, last);
        // Closing an already closed stroke can produce a zero-length edge.
        // Its padded point is not an independent graphic obstacle.
        if (rule.length() > .05) rules.append(rule);
    };
    const int count = a.FPDFPath_CountSegments(object); if (count > 10000) return rules;
    for (int i = 0; i < count; ++i) {
        const auto segment = a.FPDFPath_GetPathSegment(object, i); float x, y;
        if (!a.FPDFPathSegment_GetPoint(segment, &x, &y)) continue;
        const auto pdfPoint = parent.map(QPointF(x * m.a + y * m.c + m.e, x * m.b + y * m.d + m.f));
        const QPointF p(pdfPoint.x(), height - pdfPoint.y());
        const int type = a.FPDFPathSegment_GetType(segment);
        if (type == FPDF_SEGMENT_MOVETO) { start = p; hasPoint = true; }
        else if (hasPoint && type == FPDF_SEGMENT_LINETO) appendRule(previous, p);
        else hasPoint = false;
        previous = p;
        if (hasPoint && a.FPDFPathSegment_GetClose(segment)) appendRule(p, start);
    }
    return rules;
}
QJsonObject inspectPage(Document &doc, int index) {
    auto &a = doc.api; Page page(doc, index);
    const double width = a.FPDF_GetPageWidthF(page.handle), height = a.FPDF_GetPageHeightF(page.handle);
    const QRectF pageBounds(0, 0, width, height);
    QJsonObject result{{"number", index}, {"analyzed", true}, {"extractionVersion", extractionVersion}, {"width", width}, {"height", height}, {"blocks", QJsonArray{}}, {"kind", "text"}};
    if (!std::isfinite(width) || !std::isfinite(height) || width < 20 || height < 20 || width > 4000 || height > 4000)
        throw QStringLiteral("页面尺寸超出支持范围。");
    QString warning;
    if (a.FPDFPage_GetRotation(page.handle) != 0) warning = QStringLiteral("旋转页面暂保留原样。");
    float ml = 0, mb = 0, mr = width, mt = height, cl = 0, cb = 0, cr = width, ct = height;
    a.FPDFPage_GetMediaBox(page.handle, &ml, &mb, &mr, &mt);
    if (!a.FPDFPage_GetCropBox(page.handle, &cl, &cb, &cr, &ct)) { cl = ml; cb = mb; }
    if (std::abs(qMax(ml, cl)) > .1 || std::abs(qMax(mb, cb)) > .1)
        warning = QStringLiteral("含偏移裁切区域，此页暂保留原样。");
    QMap<FPDF_PAGEOBJECT, int> objects;
    QList<QRectF> obstacles;
    QList<QRectF> thinRules;
    QList<Background> backgroundCandidates;
    QList<QLineF> gridRules;
    QList<QRectF> ruledObjects;
    QList<Background> imageBackgrounds;
    double largestImage = 0; bool visibleArtwork = false;
    const auto allObjects = pageObjects(a, page.handle);
    for (int i = 0; i < allObjects.size(); ++i) {
        const auto object = allObjects[i].handle;
        if (a.FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_SHADING) result["rasterBackground"] = true;
        if (a.FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_PATH) {
            int fill = 0; FPDF_BOOL stroke = false; unsigned int red, green, blue, alpha;
            if (a.FPDFPath_GetDrawMode(object, &fill, &stroke) && fill != FPDF_FILLMODE_NONE
                && !a.FPDFPageObj_GetFillColor(object, &red, &green, &blue, &alpha)) result["rasterBackground"] = true;
        }
        if (a.FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_FORM) continue;
        if (a.FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_TEXT) objects.insert(object, i);
        else {
            float l, b, r, t;
            if (a.FPDFPageObj_GetBounds(object, &l, &b, &r, &t)) {
                const auto world = allObjects[i].transform.mapRect(QRectF(l, b, r - l, t - b));
                QRectF bounds(world.left(), height - world.bottom(), world.width(), world.height());
                if (a.FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_PATH) {
                    const auto geometry = pathBounds(a, object, allObjects[i].transform, height);
                    if (geometry.isEmpty()) continue;
                    bounds = geometry;
                }
                if (a.FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_IMAGE) {
                    largestImage = qMax(largestImage, bounds.intersected(QRectF(0, 0, width, height)).width() * bounds.intersected(QRectF(0, 0, width, height)).height());
                    visibleArtwork = true;
                    imageBackgrounds.append({bounds, 0, i});
                } else if (bounds.width() < width * .95 || bounds.height() < height * .95) visibleArtwork = true;
                // A full-page background is not a flow obstacle.
                if (bounds.width() < width * .95 || bounds.height() < height * .95) obstacles.append(bounds);
                if (a.FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_PATH && bounds.height() <= 2 && bounds.width() >= 5)
                    thinRules.append(bounds);
                QRgb color;
                if (lightRectangle(a, object, bounds, height, &color, allObjects[i].transform)) backgroundCandidates.append({bounds, color, i});
                const auto rules = pathRules(a, object, bounds, height, allObjects[i].transform);
                if (!rules.isEmpty()) {
                    gridRules.append(rules); ruledObjects.append(bounds);
                    int fill = 0; FPDF_BOOL stroke = false; a.FPDFPath_GetDrawMode(object, &fill, &stroke); bool straight = true;
                    for (int s = 0; s < a.FPDFPath_CountSegments(object); ++s) if (a.FPDFPathSegment_GetType(a.FPDFPath_GetPathSegment(object, s)) == FPDF_SEGMENT_BEZIERTO) straight = false;
                    if (fill == FPDF_FILLMODE_NONE && stroke && straight) {
                        // A polyline's enclosing box is mostly empty. Treat
                        // each painted segment as an obstacle, not its interior.
                        obstacles.removeOne(bounds); float penWidth = 0; a.FPDFPageObj_GetStrokeWidth(object, &penWidth);
                        FS_MATRIX matrix; double scale = 1;
                        if (a.FPDFPageObj_GetMatrix(object, &matrix)) {
                            const auto transform = QTransform(matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f) * allObjects[i].transform;
                            scale = qMax(std::hypot(transform.m11(), transform.m12()), std::hypot(transform.m21(), transform.m22()));
                        }
                        const double padding = qMax(.15, double(penWidth) * scale / 2);
                        for (const auto &rule : rules) { const auto lineBox = QRectF(rule.p1(), rule.p2()).normalized().adjusted(-padding, -padding, padding, padding); obstacles.append(lineBox); ruledObjects.append(lineBox); }
                    }
                }
            }
        }
    }
    const auto tableRegions = PdfLayout::ruledRegions(gridRules, QSizeF(width, height), QStringLiteral("p%1").arg(index));
    for (const auto &value : tableRegions) if (value.toObject()["kind"] == "table") {
        const auto table = PdfLayout::rect(value.toObject()["rect"]).adjusted(-3, -3, 3, 3);
        for (const auto &box : ruledObjects) if (table.contains(box)) obstacles.removeOne(box);
    }
    const auto textPage = a.FPDFText_LoadPage(page.handle);
    if (!textPage) throw QStringLiteral("无法提取页面文字。");
    QList<Glyph> glyphs;
    const int count = a.FPDFText_CountChars(textPage);
    if (count > 50000) { a.FPDFText_ClosePage(textPage); throw QStringLiteral("单页文字过多，请简化此页或使用识图处理。"); }
    QString plainText;
    bool spaceBefore = false;
    for (int i = 0; i < count; ++i) {
        const char32_t code = a.FPDFText_GetUnicode(textPage, i);
        if (code > 0 && code <= 0x10ffff) plainText += QString::fromUcs4(&code, 1);
        if (code == 0 || code > 0x10ffff) { warning = QStringLiteral("文字编码不完整，页面暂保留原样。"); continue; }
        if (QChar::isSpace(code) || code < 32) { spaceBefore = true; continue; }
        const auto object = a.FPDFText_GetTextObject(textPage, i);
        if (!objects.contains(object)) { warning = QStringLiteral("含嵌套文字对象，页面暂保留原样。"); continue; }
        if (std::abs(a.FPDFText_GetCharAngle(textPage, i)) > .02) warning = QStringLiteral("含倾斜或竖排文字，页面暂保留原样。");
        double l, r, b, t, x, y;
        if (!a.FPDFText_GetCharBox(textPage, i, &l, &r, &b, &t) || !a.FPDFText_GetCharOrigin(textPage, i, &x, &y)) continue;
        Glyph g; g.text = QString::fromUcs4(&code, 1); g.box = QRectF(l, height - t, r - l, t - b);
        g.textIndex = i;
        g.spaceBefore = spaceBefore; spaceBefore = false;
        g.baseline = height - y; g.size = a.FPDFText_GetFontSize(textPage, i); g.object = objects.value(object);
        FS_MATRIX textMatrix;
        if (a.FPDFPageObj_GetMatrix(object, &textMatrix)) {
            const auto combined = QTransform(textMatrix.a, textMatrix.b, textMatrix.c, textMatrix.d, textMatrix.e, textMatrix.f) * allObjects[g.object].transform;
            g.size *= std::hypot(combined.m21(), combined.m22());
        }
        if (!std::isfinite(g.size) || g.size <= 0 || g.size > 200) { warning = QStringLiteral("文字尺寸异常，页面暂保留原样。"); continue; }
        g.bold = a.FPDFText_GetFontWeight(textPage, i) >= 600;
        char fontName[256] = {}; int flags = 0;
        a.FPDFText_GetFontInfo(textPage, i, fontName, sizeof(fontName) - 1, &flags);
        const auto name = QString::fromLatin1(fontName);
        g.bold = g.bold || (flags & (1 << 18)) || name.contains("Bold", Qt::CaseInsensitive) || name.contains("Demi", Qt::CaseInsensitive);
        g.italic = (flags & 64) || name.contains("Italic", Qt::CaseInsensitive) || name.contains("Oblique", Qt::CaseInsensitive);
        g.serif = (flags & 2) || name.contains("Times", Qt::CaseInsensitive) || name.contains("Serif", Qt::CaseInsensitive);
        g.mono = (flags & 1) || name.contains("Courier", Qt::CaseInsensitive) || name.contains("Mono", Qt::CaseInsensitive) || name.contains("Consolas", Qt::CaseInsensitive);
        unsigned int red = 0, green = 0, blue = 0, alpha = 255;
        if (a.FPDFText_GetFillColor(textPage, i, &red, &green, &blue, &alpha)) g.color = {int(red), int(green), int(blue)};
        glyphs.append(g);
    }
    a.FPDFText_ClosePage(textPage);
    result["text"] = plainText;
    if (glyphs.isEmpty() && !visibleArtwork && warning.isEmpty()) { result["kind"] = "blank"; return result; }
    // A scanned page may still have a page number, watermark or poor OCR layer.
    // Keep the manual AI override available for the cases this heuristic misses.
    auto meaningfulText = plainText; meaningfulText.remove(QRegularExpression(QStringLiteral("(?:https?://|www\\.)\\S+"), QRegularExpression::CaseInsensitiveOption));
    meaningfulText.remove(QRegularExpression(QStringLiteral("[^\\p{L}]")));
    if (glyphs.isEmpty() || (largestImage > width * height * .65 && meaningfulText.size() < 12)) {
        result["kind"] = "scan";
        result["warning"] = QStringLiteral("扫描或图像为主的页面，请先用 AI 识别原文。"); return result;
    }
    if (glyphs.isEmpty() && warning.isEmpty()) warning = QStringLiteral("此页没有可提取文字，图片或扫描页保留原样。");
    if (!warning.isEmpty()) { result["warning"] = warning; return result; }
    std::stable_sort(glyphs.begin(), glyphs.end(), [](const auto &l, const auto &r) { return l.baseline < r.baseline; });
    QList<QList<Glyph>> rows;
    for (const auto &g : glyphs) {
        if (rows.isEmpty() || std::abs(rows.last().first().baseline - g.baseline) > qMax(1.5, g.size * .18)) rows.append(QList<Glyph>{});
        rows.last().append(g);
    }
    // PDF stores small raised/lowered characters on different baselines. Keep
    // representable mathematical scripts with their base, so translating the
    // sentence cannot leave a detached exponent behind in the original PDF.
    QList<double> rowSizes;
    for (auto &row : rows) {
        std::stable_sort(row.begin(), row.end(), [](const auto &l, const auto &r) { return l.box.left() < r.box.left(); });
        QList<double> sizes; for (const auto &g : row) sizes.append(g.size);
        std::sort(sizes.begin(), sizes.end()); rowSizes.append(sizes[sizes.size() / 2]);
    }
    QList<QList<Glyph>> attached(rows.size());
    for (int r = 0; r < rows.size(); ++r) {
        const auto &row = rows[r];
        for (int first = 0; first < row.size();) {
            int last = first + 1;
            while (last < row.size() && row[last].box.left() - row[last - 1].box.right() < rowSizes[r] * .7) ++last;
            int parent = -1, script = 0; double best = 1e9; QString mapped;
            for (int p = 0; p < rows.size(); ++p) {
                if (p == r || rowSizes[p] < rowSizes[r] * 1.2 || rowSizes[p] > rowSizes[r] * 1.8) continue;
                const double offset = row.first().baseline - rows[p].first().baseline;
                if (std::abs(offset) < rowSizes[p] * .12 || std::abs(offset) > rowSizes[p] * .65) continue;
                const auto ordinary = offset < 0 ? QStringLiteral("0123456789+-−=()nTi,;") : QStringLiteral("0123456789+-−=()aeoxhklmnpstij,;");
                const auto scripts = offset < 0 ? QStringLiteral("⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻⁻⁼⁽⁾ⁿᵀⁱ,;") : QStringLiteral("₀₁₂₃₄₅₆₇₈₉₊₋₋₌₍₎ₐₑₒₓₕₖₗₘₙₚₛₜᵢⱼ,;");
                QString candidate; bool needsFormatting = false;
                for (int i = first; i < last; ++i) {
                    const int at = row[i].text.size() == 1 ? ordinary.indexOf(row[i].text.front()) : -1;
                    const bool mathematicalLetter = row[i].text.size() == 1 && row[i].text.front().isLetterOrNumber();
                    if ((at < 0 && !mathematicalLetter) || row[i].mono) { candidate.clear(); break; }
                    needsFormatting |= at < 0;
                    candidate += at >= 0 ? QString(scripts[at]) : row[i].text;
                }
                if (candidate.isEmpty() || candidate.count(',') + candidate.count(';') == candidate.size()) continue;
                for (const auto &base : rows[p]) {
                    const double gap = row[first].box.left() - base.box.right();
                    if (base.mono || gap < -rowSizes[p] * .2 || gap > rowSizes[p] * .6) continue;
                    const double distance = std::abs(offset) + std::abs(gap);
                    if (distance < best) { best = distance; parent = p; mapped = candidate; script = needsFormatting ? (offset < 0 ? 1 : -1) : 0; }
                }
            }
            for (int i = first; i < last; ++i) {
                auto g = row[i];
                if (parent >= 0) {
                    if (!script) g.text = mapped.mid(i - first, 1);
                    g.script = g.text == "," || g.text == ";" ? 0 : script;
                    g.baseline = rows[parent].first().baseline; g.spaceBefore = false;
                }
                attached[parent >= 0 ? parent : r].append(g);
            }
            first = last;
        }
    }
    rows = attached; rows.removeIf([](const auto &row) { return row.isEmpty(); });
    QList<Line> lines;
    for (auto &row : rows) {
        std::stable_sort(row.begin(), row.end(), [](const auto &l, const auto &r) { return l.box.left() < r.box.left(); });
        QString rowText; int wideGaps = 0;
        for (int i = 0; i < row.size(); ++i) {
            if (i) {
                const double gap = row[i].box.left() - row[i - 1].box.right();
                if (gap > qMax(24.0, row[i].size * 2.5)) ++wideGaps;
                if (row[i].spaceBefore || gap > row[i].size * .16) rowText += QLatin1Char(' ');
            }
            rowText += row[i].text;
        }
        // Short fully justified prose can put a large gap between every word.
        // Do not mistake that single sentence for four or more text columns.
        static const QRegularExpression connective("\\b(?:the|we|have|a|an|of|to|is|are|in|with|using|from|as)\\b");
        auto words = connective.globalMatch(rowText); int connectives = 0; while (words.hasNext()) { words.next(); ++connectives; }
        const bool justifiedProse = wideGaps >= 3 && connectives >= 2 && rowText.count(' ') >= 3
            && !rowText.contains(QRegularExpression("[;={}#]"));
        Line line; QString previousCell; int previousPanel = -1;
        for (const auto &g : row) {
            const auto gap = g.box.left() - line.box.right();
            const auto cell = PdfLayout::cellAt(tableRegions, g.box);
            int panel = -1; double panelArea = width * height;
            for (const auto &bg : backgroundCandidates) {
                const double area = bg.box.width() * bg.box.height();
                if (bg.object < g.object && area < panelArea && bg.box.adjusted(-.5, -.5, .5, .5).contains(g.box)) { panel = bg.object; panelArea = area; }
            }
            bool inlineArtwork = false;
            if (!line.text.isEmpty() && gap > 1) for (const auto &obstacle : obstacles) {
                if (obstacle.height() > 2 && obstacle.width() > 2
                    && obstacle.left() >= line.box.right() - .5 && obstacle.right() <= g.box.left() + .5
                    && obstacle.bottom() > qMin(line.box.top(), g.box.top()) && obstacle.top() < qMax(line.box.bottom(), g.box.bottom())) { inlineArtwork = true; break; }
            }
            if (!line.text.isEmpty() && (inlineArtwork || (!justifiedProse && gap > qMax(24.0, g.size * 2.5) && !(g.mono && line.monoCount > line.glyphCount * .7)) || cell != previousCell || panel != previousPanel)) {
                line.bold = line.boldCount > line.glyphCount * .6; line.masks = {line.box}; lines.append(line); line = {};
            }
            if (line.text.isEmpty()) { line.box = g.box; line.size = g.size; line.baseline = g.baseline; line.color = g.color; line.bold = g.bold; }
            else {
                // Ink boxes are narrower than glyph advances (notably 1, .).
                // A gap inside one text object must not split 12 into "1 2".
                if (g.spaceBefore || (g.textIndex != line.lastTextIndex + 1 && gap > g.size * .16)) { line.text += QLatin1Char(' '); appendRun(line, " ", g.bold, g.italic); }
                line.box = line.box.united(g.box);
            }
            line.text += g.script ? (g.script > 0 ? "^{" : "_{") + g.text + "}" : g.text;
            line.objects.insert(g.object); appendRun(line, g.text, g.bold, g.italic, g.script);
            ++line.glyphCount; if (g.bold) ++line.boldCount; if (g.serif) ++line.serifCount; if (g.mono) ++line.monoCount; line.sizes.append(g.size);
            if (!g.mono && !g.text.isEmpty() && g.text.front().isLetter()) ++line.proseFontLetters;
            previousCell = cell;
            previousPanel = panel;
            line.lastTextIndex = g.textIndex;
        }
        if (!line.text.isEmpty()) { line.bold = line.boldCount > line.glyphCount * .6; line.masks = {line.box}; lines.append(line); }
    }
    bool inBlockComment = false, precedingMonoProse = false; QString precedingCodeLine; double precedingLeft = 0;
    for (auto &line : lines) {
        std::sort(line.sizes.begin(), line.sizes.end()); if (!line.sizes.isEmpty()) line.size = line.sizes[line.sizes.size() / 2];
        const bool mono = line.monoCount > line.glyphCount * .8;
        const bool continuation = mono && !precedingCodeLine.isEmpty()
            && wrappedLineComment(precedingCodeLine, line.text, line.box.left() < precedingLeft - line.size);
        // Some books print explanatory sentences in the code font. They are
        // prose between examples, not immutable program tokens.
        bool prose = false;
        if (mono && !inBlockComment && !continuation && lineCommentStart(line.text) < 0
            && !line.text.contains(QRegularExpression("[;={}#]|/\\*|\\*/|^[A-Za-z_]\\w*\\s*\\("))) {
            static const QRegularExpression start("^(?:For|And|This|The|To|In|As|Here|When|These|Note|An|A|Constructs|Returns|Computes|Calculates|Creates|Retrieves|Sets|Gets)\\s+");
            static const QRegularExpression words("\\b(?:the|be|by|from|in|to|of|and|with|if|is|are|has)\\b");
            auto matches = words.globalMatch(line.text); int count = 0; while (matches.hasNext()) { matches.next(); ++count; }
            prose = (start.match(line.text).hasMatch() && line.text.count(' ') >= 3)
                || QRegularExpression("^(?:just|only)\\s+[A-Za-z_]\\w*[.!?:]$").match(line.text).hasMatch()
                || (precedingMonoProse && QRegularExpression("^[a-z][a-z]+(?:[ :,.]|$)|^[A-Z]\\w* [a-z]+[.!?:]$").match(line.text).hasMatch())
                || (line.proseFontLetters >= 3 && QRegularExpression("\\b(?:method|function|class|object|type)[.:]$").match(line.text).hasMatch())
                || (count >= 2 && line.text.count(' ') >= 5);
        }
        if (prose) line.monoCount = 0;
        precedingMonoProse = prose && !QRegularExpression("[.!?:]$").match(line.text).hasMatch();
        if (line.monoCount > line.glyphCount * .8) {
            if (continuation) precedingCodeLine += QLatin1Char(' ') + line.text;
            else { lineCommentStart(line.text, &inBlockComment); precedingCodeLine = line.text; }
        } else { inBlockComment = false; precedingCodeLine.clear(); }
        precedingLeft = line.box.left();
    }
    // Link underlines can cover just one digit within a multi-line paragraph.
    // Compare against the actual line baseline, not the paragraph's first line
    // or total width. Their painted paths remain in the source PDF.
    obstacles.removeIf([&](const QRectF &rule) {
        if (rule.height() > 1.5 || rule.width() < rule.height() * 3) return false;
        for (const auto &line : lines) {
            const double margin = line.size * .7;
            if (rule.left() >= line.box.left() - margin && rule.right() <= line.box.right() + margin
                && rule.top() >= line.baseline - line.size * .10 && rule.bottom() <= line.baseline + line.size * .22) return true;
        }
        return false;
    });
    // Opaque rectangles wholly behind text are background panels.
    // Repeated rectangles of the same fill inside a panel are often emitted by
    // word processors for each row. Keep them all, using the outer panel for flow.
    QList<Background> backgrounds;
    const auto behindText = [&lines](const Background &bg) {
        for (const auto &line : lines) if (bg.box.intersects(line.box))
            for (int object : line.objects) if (object < bg.object) return false;
        return true;
    };
    for (const auto &bg : backgroundCandidates) {
        bool touches = false, contains = true;
        for (const auto &line : lines) if (bg.box.intersects(line.box)) {
            touches = true;
            if (!bg.box.adjusted(-.5, -.5, .5, .5).contains(line.box)) contains = false;
        }
        if (!touches || !contains || !behindText(bg)) continue;
        backgrounds.append(bg); obstacles.removeOne(bg.box);
        for (const auto &inner : backgroundCandidates)
            if (inner.color == bg.color && bg.box.contains(inner.box) && behindText(inner)) obstacles.removeOne(inner.box);
    }
    for (const auto &bg : imageBackgrounds) {
        bool touches = false, contains = true;
        for (const auto &line : lines) if (bg.box.intersects(line.box)) {
            touches = true; if (!bg.box.adjusted(-.5, -.5, .5, .5).contains(line.box)) contains = false;
        }
        if (touches && contains && behindText(bg)) { backgrounds.append(bg); obstacles.removeOne(bg.box); }
    }
    QList<QRectF> textBackgrounds;
    for (const auto &bg : backgrounds) {
        bool nested = false;
        for (const auto &outer : backgrounds)
            if (outer.box != bg.box && outer.color == bg.color && outer.box.contains(bg.box)) { nested = true; break; }
        if (!nested && !textBackgrounds.contains(bg.box)) textBackgrounds.append(bg.box);
    }
    // An underline can cross a glyph's descender box without being a picture
    // or a separator. Keep the original path, but exclude it from text flow.
    for (const auto &rule : thinRules) for (const auto &line : lines) {
        if (rule.height() <= qMin(2.0, line.size * .16)
            && rule.left() >= line.box.left() - 1 && rule.right() <= line.box.right() + 1
            && rule.top() >= line.baseline - line.size * .05
            && rule.bottom() <= line.baseline + line.size * .3) {
            obstacles.removeOne(rule); break;
        }
    }
    // Collect a vertical word before reading-order analysis can interleave it
    // with neighboring diagram labels. Only narrow enclosing panels qualify.
    for (const auto &bg : textBackgrounds) {
        if (bg.height() < bg.width() * 3) continue;
        QList<int> members;
        for (int i = 0; i < lines.size(); ++i) if (bg.contains(lines[i].box)) members.append(i);
        if (members.size() < 4) continue;
        bool stacked = true;
        for (int i = 0; i < members.size(); ++i) {
            const auto &line = lines[members[i]];
            if (line.text.size() != 1 || !line.text.front().isLetter() || bg.width() > line.size * 2) stacked = false;
            if (i && (line.baseline - lines[members[i - 1]].baseline > line.size * 1.6
                || std::abs(line.box.center().x() - lines[members[0]].box.center().x()) > line.size * .3)) stacked = false;
        }
        if (!stacked) continue;
        auto &first = lines[members[0]];
        for (int n = 1; n < members.size(); ++n) {
            const auto &next = lines[members[n]]; first.text += next.text; first.box = first.box.united(next.box);
            first.objects.unite(next.objects); first.runs.append(next.runs); first.masks.append(next.masks);
            first.glyphCount += next.glyphCount; first.boldCount += next.boldCount; first.serifCount += next.serifCount; first.monoCount += next.monoCount;
        }
        for (int n = members.size() - 1; n > 0; --n) lines.removeAt(members[n]);
    }
    // A short numbering/bullet fragment followed by text on the same baseline
    // is a list marker, not a second column. Leave that marker in place.
    static const QRegularExpression markerPattern(QStringLiteral("^(?:[•‣◦▪▫●○·*–—-]|\\(?\\d{1,3}[.)]|\\(?[A-Za-z][.)]|\\(?[ivxlcdmIVXLCDM]{1,6}[.)])$"));
    for (int i = 0; i + 1 < lines.size(); ++i) {
        auto &line = lines[i]; const auto &next = lines[i + 1];
        auto compact = line.text; compact.remove(QRegularExpression(QStringLiteral("\\s")));
        const auto gap = next.box.left() - line.box.right();
        line.marker = line.box.width() <= line.size * 3 && markerPattern.match(compact).hasMatch()
            && std::abs(line.baseline - next.baseline) < qMax(line.size, next.size) * .3
            && gap > 0 && gap <= qMax(72.0, line.size * 8);
        if (line.marker) line.text = compact;
    }
    QJsonArray lineItems;
    for (int i = 0; i < lines.size(); ++i) {
        const auto &line = lines[i]; auto orderingBox = line.box;
        // Short bullets sit lower than the text's ascenders. Align only their
        // ordering box so the marker precedes its text, never its continuation.
        if (line.marker && i + 1 < lines.size()) orderingBox.setTop(qMin(orderingBox.top(), lines[i + 1].box.top()));
        lineItems.append(QJsonObject{{"rect", rectJson(orderingBox)}, {"source", line.text}, {"bold", line.bold}, {"size", line.size}});
    }
    const auto analysis = PdfLayout::analyze(lineItems, tableRegions, QSizeF(width, height), QStringLiteral("p%1").arg(index));
    QList<Line> orderedLines;
    for (const auto &v : analysis.items) { auto line = lines[v.toObject()["inputIndex"].toInt()]; line.layout = v.toObject(); orderedLines.append(line); }
    lines = orderedLines;
    QList<Line> blocks;
    double lastBaseline = -1000;
    for (const auto &line : lines) {
        bool join = false;
        if (!blocks.isEmpty()) {
            const auto &last = blocks.last();
            join = !last.marker && !line.marker && line.baseline - lastBaseline > line.size * .6 && line.baseline - lastBaseline < line.size * 1.55
                && std::abs(last.size - line.size) < line.size * .08 && last.color == line.color && last.bold == line.bold
                && (std::abs(last.box.left() - line.box.left()) < line.size * 1.8
                    || (last.masks.size() == 1 && last.box.left() > line.box.left()
                        && last.box.left() - line.box.left() < line.size * 2.75));
            join = join && last.layout["container"] == line.layout["container"];
            if ((last.monoCount > last.glyphCount * .8) != (line.monoCount > line.glyphCount * .8)) join = false;
            if (last.monoCount > last.glyphCount * .8 && line.monoCount > line.glyphCount * .8 && line.baseline - lastBaseline > line.size * .6
                && line.baseline - lastBaseline < line.size * 1.6 && last.layout["container"] == line.layout["container"]) join = true;
            if (last.layout["role"] == "cell" && last.layout["container"] == line.layout["container"]) join = true;
            for (const auto &o : obstacles) if (o.top() >= last.box.bottom() && o.bottom() <= line.box.top() && o.width() > 20) join = false;
            // Reflowing a paragraph through an inline formula/image would cover
            // it with translated text. Keep the surrounding text fragments in
            // separate safe regions, preserving the original artwork position.
            if (join) for (const auto &o : obstacles)
                if (o.width() > 2 && o.height() > 2 && o.intersects(last.box.united(line.box))) { join = false; break; }
            for (const auto &bg : textBackgrounds)
                if (bg.adjusted(-.5, -.5, .5, .5).contains(last.box) != bg.adjusted(-.5, -.5, .5, .5).contains(line.box)) join = false;
            if (line.layout["role"] == "index" || last.layout["role"] == "index") join = false;
        }
        if (join) {
            auto &last = blocks.last();
            if (last.monoCount > last.glyphCount * .8 && line.monoCount > line.glyphCount * .8) {
                const auto indent = QString(qMax(0, qRound((line.box.left() - last.box.left()) / (line.size * .6))), QLatin1Char(' '));
                // Book typography can wrap a // comment without repeating //.
                // Join only a plain natural-language continuation, not syntax.
                const double previousLeft = last.masks.isEmpty() ? last.box.left() : last.masks.last().left();
                const bool wrappedComment = wrappedLineComment(last.text.section('\n', -1), line.text, line.box.left() < previousLeft - line.size);
                const auto separator = wrappedComment ? QStringLiteral(" ") : QLatin1Char('\n') + indent;
                last.text += separator; appendRun(last, separator, false, false);
            }
            else if (last.text.endsWith('-') && !line.text.isEmpty() && line.text.front().isLower()) { last.text.chop(1); if (!last.runs.isEmpty()) last.runs.last().text.chop(1); }
            else { last.text += QLatin1Char(' '); appendRun(last, " ", false, false); }
            last.text += line.text; last.box = last.box.united(line.box); last.objects.unite(line.objects);
            last.runs.append(line.runs); last.masks.append(line.masks); last.glyphCount += line.glyphCount;
            last.boldCount += line.boldCount; last.serifCount += line.serifCount; last.monoCount += line.monoCount;
        } else blocks.append(line);
        lastBaseline = line.baseline;
    }
    // Unsupported regions stay in the source page; neighboring regions remain
    // usable. Never remove an object on behalf of only part of its text.
    double contentLeft = width, contentRight = 0;
    for (auto &block : blocks) {
        for (const auto &obstacle : obstacles) if (obstacle.intersects(block.box)) {
            bool belowBaseline = false;
            if (obstacle.height() <= 1.5 && obstacle.width() >= block.size * 3) for (const auto &line : lines) {
                if (block.box.contains(line.box) && obstacle.right() >= line.box.left() && obstacle.left() <= line.box.right()
                    && obstacle.top() >= line.baseline - line.size * .10 && obstacle.bottom() <= line.baseline + line.size * .22) { belowBaseline = true; break; }
            }
            if (belowBaseline) continue;
            const auto tracePath = qEnvironmentVariable("YIYI_PDF_TRACE_GEOMETRY");
            if (!tracePath.isEmpty()) {
                QFile trace(tracePath);
                if (trace.open(QIODevice::WriteOnly | QIODevice::Append)) trace.write(QJsonDocument(QJsonObject{{"page", index + 1}, {"source", block.text.left(80)},
                    {"baseline", block.baseline}, {"size", block.size}, {"box", rectJson(block.box)}, {"obstacle", rectJson(obstacle)}}).toJson(QJsonDocument::Compact) + '\n');
            }
            block.preserveReason = QStringLiteral("文字穿过图形边界，此区域保留原文；可用识图重新处理。");
        }
        const auto visible = block.box.intersected(pageBounds);
        if (!visible.isEmpty()) { contentLeft = qMin(contentLeft, visible.left()); contentRight = qMax(contentRight, visible.right()); }
    }
    QJsonArray output;
    for (int i = 0; i < blocks.size(); ++i) {
        const auto &b = blocks[i];
        auto container = PdfLayout::rect(b.layout["containerRect"]).intersected(pageBounds);
        const bool cell = b.layout["role"] == "cell";
        bool centered = b.masks.size() == 1 && std::abs(b.box.center().x() - container.center().x()) < qMax(8.0, b.size)
            && b.box.width() < container.width() * .8;
        const double halfWidth = qMax(b.box.width() / 2, qMin(b.box.center().x() - contentLeft, width - contentLeft - b.box.center().x()));
        QRectF region(centered ? b.box.center().x() - halfWidth : b.box.left(), b.box.top(),
                      centered ? halfWidth * 2 : contentRight - b.box.left(), 0);
        // A picture beside the paragraph must not be covered by longer text.
        for (const auto &o : obstacles)
            if (o.top() < b.box.bottom() && o.bottom() > b.box.top() && o.left() > b.box.right())
                region.setRight(qMin(region.right(), o.left() - 3));
        for (const auto &other : blocks) if (other.box.top() < b.box.bottom() && other.box.bottom() > b.box.top()) {
            if (other.box.left() > b.box.right()) region.setRight(qMin(region.right(), other.box.left() - 3));
            if (other.box.right() < b.box.left()) region.setLeft(qMax(region.left(), other.box.right() + 3));
        }
        double bottom = height - qMax(12.0, contentLeft * .4);
        for (int j = i + 1; j < blocks.size(); ++j) {
            const auto &next = blocks[j].box;
            if (next.top() >= b.box.bottom() - .5 && next.left() < region.right() && next.right() > region.left()) bottom = qMin(bottom, next.top() - 3);
        }
        for (const auto &o : obstacles)
            if (o.top() >= b.box.bottom() - 1 && o.left() < region.right() && o.right() > region.left())
                bottom = qMin(bottom, o.top() - (o.height() <= 2 && o.width() > b.size * 3 ? 1 : 3));
        for (const auto &bg : textBackgrounds) {
            if (bg.adjusted(-.5, -.5, .5, .5).contains(b.box)) {
                region.setLeft(qMax(region.left(), bg.left())); region.setRight(qMin(region.right(), bg.right()));
                bottom = qMin(bottom, bg.bottom());
            } else {
                if (bg.top() < b.box.bottom() && bg.bottom() > b.box.top() && bg.left() > b.box.right()) region.setRight(qMin(region.right(), bg.left() - 3));
                if (bg.top() >= b.box.bottom() - 1 && bg.left() < region.right() && bg.right() > region.left()) bottom = qMin(bottom, bg.top() - 3);
            }
        }
        region.setHeight(qMax(b.box.height(), bottom - b.box.top()));
        if (!container.isEmpty()) {
            const double verticalPadding = qMin(2.0, container.height() * .08);
            const auto limit = cell ? container.adjusted(2, verticalPadding, -2, -verticalPadding) : container;
            region = region.intersected(limit);
            if (cell) { region.setTop(qMax(limit.top(), b.box.top() - 1)); region.setBottom(limit.bottom()); }
        }
        if (!cell && b.masks.size() == 1 && region.height() < b.size * 1.05) {
            // Flow containers start at the first ink box, not at a physical
            // border. A title at their top can use the small clear margin above
            // it; actual neighboring text, artwork and panels still constrain it.
            double top = qMax(0.0, b.box.top() - b.size * .12);
            for (const auto &other : blocks) if (other.box.bottom() <= b.box.top() && other.box.left() < region.right() && other.box.right() > region.left())
                top = qMax(top, other.box.bottom() + 1);
            for (const auto &o : obstacles) if (o.bottom() <= b.box.top() && o.left() < region.right() && o.right() > region.left()) top = qMax(top, o.bottom() + 1);
            for (const auto &bg : textBackgrounds) if (bg.contains(b.box)) top = qMax(top, bg.top() + 1);
            if (top < region.top()) region.setTop(top);
        }
        // A diagram label belongs to its enclosing painted panel. A flow band
        // inferred from neighboring labels must not crop its available height.
        QRectF ownPanel;
        if (!cell) for (const auto &bg : backgrounds) {
            if (!bg.box.contains(b.box) || bg.box.width() > width * .7 || bg.box.height() > height * .5) continue;
            int occupants = 0; for (const auto &other : blocks) if (bg.box.intersects(other.box)) ++occupants;
            if (occupants == 1 && (ownPanel.isEmpty() || bg.box.width() * bg.box.height() < ownPanel.width() * ownPanel.height())) ownPanel = bg.box;
        }
        if (!ownPanel.isEmpty()) {
            container = ownPanel; region = ownPanel.adjusted(1, 1, -1, -1);
            region.setTop(qMax(region.top(), qMin(b.box.top(), region.bottom() - b.size * 1.1)));
            centered = true;
        }
        // PDF text objects may extend past the physical page (long code lines
        // in particular). They must never enlarge the translated text area.
        container = container.intersected(pageBounds);
        region = region.intersected(pageBounds);
        QJsonArray ids; auto sorted = b.objects.values(); std::sort(sorted.begin(), sorted.end()); for (int id : sorted) ids.append(id);
        const bool code = b.monoCount > b.glyphCount * .8;
        const bool fixed = b.marker || !b.text.contains(QRegularExpression(QStringLiteral("\\p{L}")))
            || (b.text.size() == 1 && b.text.front() >= 'A' && b.text.front() <= 'Z')
            || (b.text.contains('=') && !b.text.contains(QRegularExpression(QStringLiteral("[A-Za-z]{3,}|[;{}#\"]")))
                && QRegularExpression(QStringLiteral("^[\\sA-Za-z\\x{0370}-\\x{03ff}\\x{1d00}-\\x{1d7f}\\x{2070}-\\x{209f}\\p{N}\\p{Sm}.,()\\[\\]⁺⁻₊₋]+$")).match(b.text).hasMatch())
            || (code && !b.text.contains("//") && !b.text.contains("/*") && !b.text.contains("*/"))
            || QRegularExpression(QStringLiteral("^\\[\\s*[ivxlcdm]+\\s*\\]$"), QRegularExpression::CaseInsensitiveOption).match(b.text).hasMatch()
            || QRegularExpression(QStringLiteral("^(https?://|www\\.)\\S+$"), QRegularExpression::CaseInsensitiveOption).match(b.text).hasMatch();
        QString styled; bool hasEmphasis = false;
        for (const auto &run : b.runs) {
            const bool bold = run.bold && !b.bold;
            if (bold || run.italic || run.script) hasEmphasis = true;
            styled += (bold ? "<b>" : "") + QString(run.italic ? "<i>" : "")
                + (run.script > 0 ? "<sup>" : run.script < 0 ? "<sub>" : "") + run.text
                + (run.script > 0 ? "</sup>" : run.script < 0 ? "</sub>" : "")
                + (run.italic ? "</i>" : "") + (bold ? "</b>" : "");
        }
        QJsonArray masks; for (const auto &r : b.masks) masks.append(rectJson(r));
        QJsonObject item{{"id", QStringLiteral("p%1-b%2").arg(index).arg(i)}, {"source", b.text}, {"styledSource", hasEmphasis ? styled : QString()},
            {"rect", rectJson(b.box)}, {"region", rectJson(region)}, {"size", b.size}, {"bold", b.bold},
            {"color", b.color}, {"centered", centered}, {"objects", ids}, {"fixed", fixed}, {"masks", masks}, {"serif", b.serifCount > b.glyphCount * .5}, {"code", code}};
        for (const auto &key : {"container", "containerRect", "role", "table", "row", "column", "rowSpan", "columnSpan", "order"})
            if (b.layout.contains(key)) item[key] = b.layout[key];
        item["containerRect"] = rectJson(container);
        if (!b.preserveReason.isEmpty()) { item["keep"] = true; item["preserveReason"] = b.preserveReason; item["protected"] = true; }
        output.append(item);
    }
    QJsonArray boundedRegions;
    for (const auto &v : analysis.regions) {
        auto r = v.toObject(); r["rect"] = rectJson(rectValue(r["rect"]).intersected(pageBounds)); boundedRegions.append(r);
    }
    result["blocks"] = output; result["layout"] = PdfLayout::structure(output, boundedRegions);
    if (result["rasterBackground"].toBool()) result["notice"] = QStringLiteral("本页含渐变图形，将以图像保留背景；译文仍可搜索，原生矢量不再保留。");
    result["baseLayoutHash"] = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(output).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    return result;
}
struct Fonts {
    int id = -1, serifId = -1; QString family, serif;
    Fonts() {
        const auto path = QDir(QCoreApplication::applicationDirPath()).filePath("pdf/YiyiPdfSans-Regular.otf");
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) throw QStringLiteral("缺少 PDF 中文字体，请重新准备 PDF 组件。");
        file.close();
        id = QFontDatabase::addApplicationFont(path);
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (families.isEmpty()) throw QStringLiteral("排版字体加载失败。");
        family = families.first();
        serifId = QFontDatabase::addApplicationFont(QDir(QCoreApplication::applicationDirPath()).filePath("pdf/YiyiPdfSerif-Regular.otf"));
        const auto serifFamilies = QFontDatabase::applicationFontFamilies(serifId);
        serif = serifFamilies.isEmpty() ? family : serifFamilies.first();
    }
    ~Fonts() { if (id >= 0) QFontDatabase::removeApplicationFont(id); if (serifId >= 0) QFontDatabase::removeApplicationFont(serifId); }
    QStringList forBlock(const QJsonObject &block) const {
        QStringList result = block["code"].toBool() ? QStringList{QStringLiteral("Courier New"), family}
            : QStringList{block["serif"].toBool() ? serif : family};
        // The bundled CJK fonts do not cover every mathematical/phonetic
        // symbol. Use Windows' installed fonts without replacing the source
        // characters or silently accepting missing glyphs.
        result << QStringLiteral("Cambria Math") << QStringLiteral("Segoe UI Symbol");
        return result;
    }
};
QFont layoutFont(const QStringList &families, int pixels, bool bold) {
    QFont font; font.setFamilies(families); font.setPixelSize(pixels); font.setBold(bold);
    font.setHintingPreference(QFont::PreferNoHinting);
    // Qt's PDF writer maps glyph IDs back to Unicode. Avoid unencoded ligatures
    // and locale substitutions that change punctuation when copied/searched.
    for (const QFont::Tag tag : {QFont::Tag("liga"), QFont::Tag("clig"), QFont::Tag("dlig"), QFont::Tag("rlig"), QFont::Tag("ccmp"), QFont::Tag("locl")})
        font.setFeature(tag, 0);
    return font;
}
struct PlacedLine { QString text; double x, baseline, size; QList<QTextLayout::FormatRange> formats; };
struct ContentsEntry { QString label, reference; };
QList<ContentsEntry> contentsEntries(const QString &source, const QString &translation) {
    static const QRegularExpression leader(QStringLiteral("(?:\\.[ \\t]*){3,}([ivxlcdm]+|[0-9]+)(?=\\s|$)"), QRegularExpression::CaseInsensitiveOption);
    QStringList references; auto matches = leader.globalMatch(source);
    while (matches.hasNext()) references.append(matches.next().captured(1));
    if (references.isEmpty()) return {};
    QList<ContentsEntry> entries; int start = 0; matches = leader.globalMatch(translation);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const auto label = translation.mid(start, match.capturedStart() - start).trimmed();
        if (label.isEmpty() || label.contains('\n')) { entries.clear(); break; }
        entries.append({label, match.captured(1)}); start = match.capturedEnd();
    }
    if (entries.isEmpty() || !translation.mid(start).trimmed().isEmpty()) {
        entries.clear();
        static const QRegularExpression row(QStringLiteral("^(.+?)[ .\\t…·]+([ivxlcdm]+|[0-9]+)$"), QRegularExpression::CaseInsensitiveOption);
        for (const auto &line : translation.split('\n', Qt::SkipEmptyParts)) {
            const auto match = row.match(line.trimmed());
            if (!match.hasMatch()) return {};
            entries.append({match.captured(1).trimmed(), match.captured(2)});
        }
    }
    if (entries.size() != references.size()) return {};
    for (int i = 0; i < entries.size(); ++i) if (entries[i].reference != references[i]) return {};
    return entries;
}
QString styledText(const QString &input, QList<QTextLayout::FormatRange> *formats) {
    static const QRegularExpression tags("</?(?:b|i|sup|sub)>");
    auto matches = tags.globalMatch(input); QString text; int position = 0, bold = 0, italic = 0, superscript = 0, subscript = 0;
    auto append = [&](const QString &part) {
        if (!part.isEmpty() && (bold || italic || superscript || subscript)) {
            QTextLayout::FormatRange range; range.start = text.size(); range.length = part.size();
            if (bold) range.format.setFontWeight(QFont::Bold); if (italic) range.format.setFontItalic(true);
            if (superscript || subscript) range.format.setVerticalAlignment(superscript ? QTextCharFormat::AlignSuperScript : QTextCharFormat::AlignSubScript);
            formats->append(range);
        }
        text += part;
    };
    while (matches.hasNext()) {
        const auto match = matches.next(); append(input.mid(position, match.capturedStart() - position));
        const auto tag = match.captured();
        int &depth = tag.contains("sup") ? superscript : tag.contains("sub") ? subscript : tag.contains('b') ? bold : italic;
        depth = tag.startsWith("</") ? qMax(0, depth - 1) : depth + 1; position = match.capturedEnd();
    }
    append(input.mid(position)); return text;
}
QList<PlacedLine> fitText(const QJsonObject &block, const QString &text, const QStringList &families, const QRectF &pageBounds, QString *error) {
    // Bound cached and manually supplied regions as well as newly extracted
    // ones. Fitting inside an off-page rectangle still produces clipped text.
    const auto region = rectValue(block["region"]).intersected(pageBounds);
    const double original = block["size"].toDouble(12);
    if (text.size() > 16000 || region.width() < 3 || region.height() < 3) { *error = QStringLiteral("译文或区域超出限制"); return {}; }
    QList<QTextLayout::FormatRange> formats;
    auto plain = block["styledSource"].toString().isEmpty() || block["code"].toBool() ? text : styledText(text, &formats);
    if (block["code"].toBool()) {
        auto lines = plain.split('\n'); const auto originalLines = block["source"].toString().split('\n');
        if (lines.size() == originalLines.size()) for (int i = 0; i < lines.size(); ++i) {
            const auto indentation = QRegularExpression(QStringLiteral("^[ \\t]*")).match(originalLines[i]).captured();
            lines[i] = indentation + lines[i].trimmed();
        }
        plain = lines.join('\n');
    }
    QList<QRawFont> availableFonts;
    for (const auto &family : families)
        availableFonts.append(QRawFont::fromFont(layoutFont({family}, qMax(1, qRound(original * 4)), block["bold"].toBool())));
    for (const auto ch : plain.toUcs4()) {
        if (QChar::isSpace(ch)) continue;
        bool supported = false;
        for (const auto &font : availableFonts) if (font.supportsCharacter(ch)) { supported = true; break; }
        if (!supported) {
            *error = QStringLiteral("译文含当前字体不支持的字符（U+%1）").arg(uint(ch), 4, 16, QLatin1Char('0')); return {};
        }
    }
    // Dense indexes and diagram labels often start at 8 pt. They need some
    // room to shrink too; an 8 pt floor prevented any fitting at all.
    const double minimum = qMin(original, qMax(original < 8 ? original * .8 : 6.0, original * .5));
    const auto contents = block["code"].toBool() || !formats.isEmpty() ? QList<ContentsEntry>{} : contentsEntries(block["source"].toString(), plain);
    for (double size = original; size >= minimum - .01; size -= .25) {
        const auto font = layoutFont(families, qMax(1, qRound(size * 4)), block["bold"].toBool());
        if (!contents.isEmpty()) {
            const QFontMetricsF metrics(font); QList<PlacedLine> lines; bool fits = true;
            const double step = region.height() / contents.size();
            for (int i = 0; i < contents.size(); ++i) {
                const auto &entry = contents[i];
                const auto ink = metrics.tightBoundingRect(entry.label + entry.reference);
                const double labelWidth = metrics.horizontalAdvance(entry.label) / 4;
                const double referenceWidth = metrics.horizontalAdvance(entry.reference) / 4;
                const double gap = region.width() - labelWidth - referenceWidth - size;
                if (gap < 0 || ink.height() / 4 > step + .1) { fits = false; break; }
                const double baseline = region.top() + i * step - ink.top() / 4;
                lines.append({entry.label, region.left(), baseline, font.pixelSize() / 4.0, {}});
                const double dotWidth = metrics.horizontalAdvance(QStringLiteral(". ")) / 4;
                const int dots = dotWidth > 0 ? int(gap / dotWidth) : 0;
                if (dots) lines.append({QStringLiteral(". ").repeated(dots), region.left() + labelWidth + size / 2, baseline, font.pixelSize() / 4.0, {}});
                lines.append({entry.reference, region.right() - referenceWidth, baseline, font.pixelSize() / 4.0, {}});
            }
            if (fits) return lines;
            continue;
        }
        for (bool compact : {false, true}) {
        auto content = plain; content.replace('\n', QChar(0x2028));
        QTextLayout layout(content, font);
        layout.setFormats(formats);
        const bool fixedCopy = block["fixed"].toBool() && text == block["source"].toString();
        QTextOption option; option.setWrapMode(fixedCopy ? QTextOption::NoWrap : QTextOption::WrapAtWordBoundaryOrAnywhere); layout.setTextOption(option);
        QList<PlacedLine> lines; double y = 0, topInset = 0, inkBottom = 0; bool widthFits = true;
        const QFontMetricsF metrics(font);
        layout.beginLayout();
        while (true) {
            auto line = layout.createLine(); if (!line.isValid()) break;
            line.setLineWidth(region.width() * 4 * .97);
            if (fixedCopy && line.naturalTextWidth() > region.width() * 4 + .1) widthFits = false;
            const double x = block["centered"].toBool() ? region.center().x() - line.naturalTextWidth() / 8 : region.left();
            auto part = content.mid(line.textStart(), line.textLength()); part.remove(QChar(0x2028));
            const auto ink = metrics.tightBoundingRect(part);
            // Anchor the visible first glyphs, rather than the font's em box.
            // CJK ascenders otherwise shift the translated title downwards.
            if (lines.isEmpty()) topInset = line.ascent() + ink.top();
            const double baseline = compact && !lines.isEmpty()
                ? qMax(y + line.ascent() - topInset, inkBottom - ink.top() + font.pixelSize() * .08)
                : y + line.ascent() - topInset;
            QList<QTextLayout::FormatRange> lineFormats;
            for (const auto &f : formats) {
                const int start = qMax(f.start, line.textStart()), end = qMin(f.start + f.length, line.textStart() + line.textLength());
                if (end <= start) continue;
                auto range = f; range.start = start - line.textStart(); range.length = end - start; lineFormats.append(range);
            }
            lines.append({part, x, region.top() + baseline / 4, font.pixelSize() / 4.0, lineFormats});
            inkBottom = qMax(inkBottom, baseline + ink.bottom());
            y = baseline - line.ascent() + topInset + (block["code"].toBool() ? font.pixelSize() * 1.2
                : compact ? font.pixelSize() * 1.12 : line.height() * 1.04);
        }
        layout.endLayout();
        if (!lines.isEmpty() && widthFits && inkBottom / 4 <= region.height() + .1) return lines;
        }
    }
    *error = QStringLiteral("译文超出区域，请调整区域、字号或译文后排版"); return {};
}
QImage render(Page &page, double width, double height, int pixels);
QJsonArray applyPage(Document &doc, Page &page, const QJsonObject &manifest, Fonts &fonts, bool *applied, bool layoutOnly = false) {
    auto &a = doc.api; QJsonArray statuses;
    const QRectF pageBounds(0, 0, a.FPDF_GetPageWidthF(page.handle), a.FPDF_GetPageHeightF(page.handle));
    const bool scan = manifest.contains("scan") || manifest["kind"] == "scan" || manifest["rasterBackground"].toBool();
    const auto blocks = manifest.contains("scan") ? manifest["scan"].toObject()["blocks"].toArray() : manifest["blocks"].toArray();
    QMap<QString, QList<PlacedLine>> placements; QJsonArray erase;
    for (const auto &value : blocks) {
        const auto block = value.toObject(); const auto text = block["translation"].toString().trimmed();
        if (text.isEmpty() || block["fixed"].toBool() || block["keep"].toBool()) continue;
        if (!scan && manifest["preservePatternArtwork"].toBool()) {
            statuses.append(QJsonObject{{"id", block["id"]}, {"fits", false},
                {"error", QStringLiteral("本页含纹理填充图形，暂保留整页原文和原图，避免导出改变图示含义。")}});
            continue;
        }
        if (scan && (block["backgroundMode"] == "original" || text == block["source"].toString().trimmed())) {
            statuses.append(QJsonObject{{"id", block["id"]}, {"error", ""}, {"fits", true}, {"preserved", true}}); continue;
        }
        QString error;
        const auto lines = fitText(block, text, fonts.forBlock(block), pageBounds, &error);
        if (block["protected"].toBool()) error = block["preserveReason"].toString(QStringLiteral("此区域暂不能独立替换，请保留原文或使用识图。"));
        if (error.isEmpty() && (block["role"] == "cell" || block["role"] == "index") && !block["numbersConfirmed"].toBool()) error = PdfLayout::changedNumbers(block["source"].toString(), text, block["role"] == "index");
        if (error.isEmpty() && block["code"].toBool()) error = PdfLayout::changedCode(block["source"].toString(), text);
        if (block["stale"].toBool()) error = QStringLiteral("原文已修改，请更新或确认本段译文");
        if (error.isEmpty()) { placements.insert(block["id"].toString(), lines); erase.append(block); }
        statuses.append(QJsonObject{{"id", block["id"]}, {"error", error}, {"fits", error.isEmpty()}, {"backgroundReview", scan && block["backgroundMode"] == "repair" && !block["backgroundConfirmed"].toBool()}});
    }
    if (!scan) {
        QHash<int, QStringList> owners;
        QHash<QString, QJsonObject> byId;
        for (const auto &v : blocks) byId.insert(v.toObject()["id"].toString(), v.toObject());
        for (const auto &v : blocks) { const auto b = v.toObject(); for (const auto &o : b["objects"].toArray()) owners[o.toInt()].append(b["id"].toString()); }
        for (const auto &ids : owners) if (ids.size() > 1) {
            bool active = false; for (const auto &id : ids) if (placements.contains(id)) active = true;
            if (!active) continue;
            for (const auto &id : ids) {
                auto b = byId.value(id); if (placements.contains(id) || !b["fixed"].toBool() || b["keep"].toBool()) continue;
                QString error; const auto lines = fitText(b, b["source"].toString(), fonts.forBlock(b), pageBounds, &error);
                if (error.isEmpty()) placements.insert(id, lines);
                statuses.append(QJsonObject{{"id", id}, {"error", error}, {"fits", error.isEmpty()}});
            }
        }
        QSet<QString> pending;
        for (const auto &ids : owners) if (ids.size() > 1) {
            bool ready = true; for (const auto &id : ids) if (!placements.contains(id)) ready = false;
            if (!ready) for (const auto &id : ids) pending.insert(id);
        }
        // Propagate through chains of shared objects, including a split of a
        // previously merged paragraph. A group is replaced atomically.
        bool changed = true;
        while (changed) { changed = false; for (const auto &ids : owners) {
            bool linked = false; for (const auto &id : ids) if (pending.contains(id)) linked = true;
            if (linked) for (const auto &id : ids) if (!pending.contains(id)) { pending.insert(id); changed = true; }
        } }
        for (int i = 0; i < statuses.size(); ++i) {
            auto s = statuses[i].toObject(); const auto id = s["id"].toString();
            if (pending.contains(id) && placements.contains(id)) { placements.remove(id); s["fits"] = false; s["error"] = QStringLiteral("拆分区域共享原文字对象，请完成关联区域的译文后一起替换。"); statuses[i] = s; }
        }
        erase = {}; for (const auto &v : blocks) if (placements.contains(v.toObject()["id"].toString())) erase.append(v);
    }
    if (layoutOnly) return statuses;
    QImage backgroundImage;
    if (scan && !placements.isEmpty()) {
        const QSizeF size(manifest["width"].toDouble(), manifest["height"].toDouble());
        const int pixels = qRound(2200 * size.width() / qMax(size.width(), size.height()));
        // Mixed pages can retain native blocks while adding recognized image
        // labels. Hide exact native text before rasterization instead of
        // guessing which connected pixels are letters, rules or illustrations.
        QHash<QString, QJsonObject> native;
        QHash<int, QStringList> owners;
        for (const auto &v : manifest["blocks"].toArray()) {
            const auto b = v.toObject(); native.insert(b["id"].toString(), b);
            for (const auto &id : b["objects"].toArray()) owners[id.toInt()].append(b["id"].toString());
        }
        QSet<QString> exact;
        for (const auto &v : erase) {
            const auto b = v.toObject(); const auto original = native.value(b["id"].toString());
            if (!original.isEmpty() && b["source"] == original["source"] && !original["objects"].toArray().isEmpty()
                && b["backgroundMode"].toString("auto") == "auto") exact.insert(b["id"].toString());
        }
        bool changed = true;
        while (changed) { changed = false; for (const auto &ids : owners) {
            bool ready = true; for (const auto &id : ids) if (!exact.contains(id)) ready = false;
            if (!ready) for (const auto &id : ids) if (exact.remove(id)) changed = true;
        } }
        struct HiddenText {
            Api &api; QList<QPair<FPDF_PAGEOBJECT, FPDF_BOOL>> objects;
            ~HiddenText() { for (const auto &entry : objects) api.FPDFPageObj_SetIsActive(entry.first, entry.second); }
        };
        QImage original;
        {
            HiddenText hidden{a, {}};
            const auto source = pageObjects(a, page.handle); QSet<int> disabled;
            for (const auto &id : exact) for (const auto &value : native.value(id)["objects"].toArray()) {
                const int index = value.toInt(-1);
                if (index < 0 || index >= source.size() || a.FPDFPageObj_GetType(source[index].handle) != FPDF_PAGEOBJ_TEXT)
                    throw QStringLiteral("PDF 段落索引无效，请重新导入。");
                if (disabled.contains(index)) continue;
                FPDF_BOOL active = 1;
                if (!a.FPDFPageObj_GetIsActive(source[index].handle, &active) || !a.FPDFPageObj_SetIsActive(source[index].handle, false))
                    throw QStringLiteral("无法准备混合页面的文字背景。");
                hidden.objects.append({source[index].handle, active}); disabled.insert(index);
            }
            original = render(page, size.width(), size.height(), pixels);
        }
        QJsonArray bitmapErase, failures;
        for (const auto &v : erase) if (!exact.contains(v.toObject()["id"].toString())) bitmapErase.append(v);
        backgroundImage = PdfScanLayout::eraseText(original, size, bitmapErase, &failures);
        for (const auto &v : failures) {
            const auto failure = v.toObject(); placements.remove(failure["id"].toString());
            for (int i = 0; i < statuses.size(); ++i) { auto s = statuses[i].toObject(); if (s["id"] != failure["id"]) continue; s["fits"] = false; s["error"] = failure["error"]; statuses[i] = s; }
        }
    }
    if (placements.isEmpty()) return statuses;
    QByteArray overlayBytes; QBuffer buffer(&overlayBytes); buffer.open(QIODevice::WriteOnly);
    {
    // Qt embeds a subset font with ToUnicode mappings. The prepared font's
    // cmap omits radical aliases so copied text uses ordinary CJK codepoints.
    QPdfWriter writer(&buffer); writer.setResolution(72);
    writer.setPageSize(QPageSize(QSizeF(manifest["width"].toDouble(), manifest["height"].toDouble()), QPageSize::Point));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    QPainter painter(&writer);
    if (!painter.isActive()) throw QStringLiteral("无法创建译文文字层。");
    if (scan) {
        const QSizeF size(manifest["width"].toDouble(), manifest["height"].toDouble());
        painter.drawImage(QRectF(QPointF(0, 0), size), backgroundImage);
    }
    painter.scale(.25, .25);
    for (const auto &value : blocks) {
        const auto block = value.toObject(); const auto lines = placements.value(block["id"].toString());
        const auto color = block["color"].toArray();
        painter.setPen(QColor(color[0].toInt(), color[1].toInt(), color[2].toInt()));
        for (const auto &line : lines) {
            const auto font = layoutFont(fonts.forBlock(block), qRound(line.size * 4), block["bold"].toBool()); painter.setFont(font);
            QTextLayout layout(line.text, font); layout.setFormats(line.formats);
            layout.beginLayout(); auto placed = layout.createLine(); placed.setLineWidth(20000); layout.endLayout();
            if (placed.isValid()) placed.draw(&painter, QPointF(line.x * 4, line.baseline * 4 - placed.ascent()));
        }
    }
    painter.end();
    }
    const auto overlay = a.FPDF_LoadMemDocument64(overlayBytes.constData(), size_t(overlayBytes.size()), nullptr);
    if (!overlay) throw QStringLiteral("无法读取译文文字层。");
    const auto xobject = a.FPDF_NewXObjectFromPage(doc.handle, overlay, 0);
    a.FPDF_CloseDocument(overlay);
    if (!xobject) throw QStringLiteral("无法合并译文文字层。");
    const auto form = a.FPDF_NewFormObjectFromXObject(xobject); a.FPDF_CloseXObject(xobject);
    if (!form) throw QStringLiteral("无法创建译文页面对象。");
    a.FPDFPage_InsertObject(page.handle, form);
    *applied = true;
    return statuses;
}
QImage render(Page &page, double width, double height, int pixels) {
    pixels = qBound(300, pixels, 2400);
    const int imageHeight = qMax(1, qRound(pixels * height / width));
    if (qint64(pixels) * imageHeight > 16000000) throw QStringLiteral("预览尺寸过大。");
    QImage image(pixels, imageHeight, QImage::Format_ARGB32); image.fill(Qt::white);
    const auto bitmap = page.api.FPDFBitmap_CreateEx(pixels, imageHeight, FPDFBitmap_BGRA, image.bits(), image.bytesPerLine());
    if (!bitmap) throw QStringLiteral("PDF 预览内存不足。");
    page.api.FPDF_RenderPageBitmap(bitmap, page.handle, 0, 0, pixels, imageHeight, 0, FPDF_ANNOT);
    page.api.FPDFBitmap_Destroy(bitmap); return image;
}
QJsonArray translatePage(Document &doc, Page &page, const QJsonObject &manifest, Fonts &fonts) {
    const bool scan = manifest.contains("scan") || manifest["kind"] == "scan" || manifest["rasterBackground"].toBool();
    const auto blocks = manifest.contains("scan") ? manifest["scan"].toObject()["blocks"].toArray() : manifest["blocks"].toArray();
    if (std::none_of(blocks.begin(), blocks.end(), [](const auto &v) {
        const auto block = v.toObject();
        return !block["fixed"].toBool() && !block["keep"].toBool() && !block["translation"].toString().trimmed().isEmpty();
    })) return {};
    // Keep source handles stable while the new text objects are appended.
    const auto source = pageObjects(doc.api, page.handle);
    bool applied = false; const auto statuses = applyPage(doc, page, manifest, fonts, &applied);
    if (!applied) return statuses;
    if (scan) {
        for (const auto &object : source) {
            if (object.parent) continue;
            if (!doc.api.FPDFPage_RemoveObject(page.handle, object.handle)) throw QStringLiteral("无法重建扫描页。");
            doc.api.FPDFPageObj_Destroy(object.handle);
        }
        if (!doc.api.FPDFPage_GenerateContent(page.handle)) throw QStringLiteral("无法生成扫描页译文。");
        return statuses;
    }
    QSet<QString> fitted;
    for (const auto &v : statuses) if (v.toObject()["fits"].toBool()) fitted.insert(v.toObject()["id"].toString());
    // PDFium can edit a form's in-memory children but GenerateContent() does
    // not serialize those child edits. Promote affected forms' leaves to page
    // objects, preserving their full transforms and original painting order.
    // This also keeps shared source streams untouched on untranslated pages.
    QHash<FPDF_PAGEOBJECT, PageObject> references;
    for (const auto &object : source) references.insert(object.handle, object);
    QSet<FPDF_PAGEOBJECT> expanded;
    for (const auto &v : manifest["blocks"].toArray()) {
        const auto b = v.toObject(); if (!fitted.contains(b["id"].toString())) continue;
        for (const auto &id : b["objects"].toArray()) {
            const int index = id.toInt(-1); if (index < 0 || index >= source.size()) throw QStringLiteral("PDF 段落索引无效，请重新导入。");
            auto object = source[index];
            if (!object.parent) continue;
            while (object.parent) { object = references.value(object.parent); expanded.insert(object.handle); }
        }
    }
    if (!expanded.isEmpty()) {
        QList<FPDF_PAGEOBJECT> top;
        for (int i = 0; i < doc.api.FPDFPage_CountObjects(page.handle); ++i) top.append(doc.api.FPDFPage_GetObject(page.handle, i));
        std::function<void(FPDF_PAGEOBJECT)> promote = [&](FPDF_PAGEOBJECT form) {
            while (doc.api.FPDFFormObj_CountObjects(form) > 0) {
                const auto child = doc.api.FPDFFormObj_GetObject(form, 0);
                if (!doc.api.FPDFFormObj_RemoveObject(form, child)) throw QStringLiteral("无法展开嵌套页面对象。");
                if (doc.api.FPDFPageObj_GetType(child) == FPDF_PAGEOBJ_FORM && expanded.contains(child)) promote(child);
                else {
                    const auto m = references.value(child).transform;
                    doc.api.FPDFPageObj_Transform(child, m.m11(), m.m12(), m.m21(), m.m22(), m.dx(), m.dy());
                    doc.api.FPDFPageObj_TransformClipPath(child, m.m11(), m.m12(), m.m21(), m.m22(), m.dx(), m.dy());
                    doc.api.FPDFPage_InsertObject(page.handle, child);
                }
            }
            doc.api.FPDFPageObj_Destroy(form);
        };
        for (const auto object : top) if (!doc.api.FPDFPage_RemoveObject(page.handle, object)) throw QStringLiteral("无法重排嵌套页面对象。");
        for (const auto object : top) { if (expanded.contains(object)) promote(object); else doc.api.FPDFPage_InsertObject(page.handle, object); }
    }
    QSet<int> removed;
    for (const auto &v : manifest["blocks"].toArray()) {
        const auto b = v.toObject(); if (!fitted.contains(b["id"].toString())) continue;
        for (const auto &id : b["objects"].toArray()) {
            const int index = id.toInt(-1);
            if (index < 0 || index >= source.size()) throw QStringLiteral("PDF 段落索引无效，请重新导入。");
            if (removed.contains(index)) continue;
            const auto &object = source[index];
            if (doc.api.FPDFPageObj_GetType(object.handle) != FPDF_PAGEOBJ_TEXT) throw QStringLiteral("原文字对象类型已变化，请重新导入。");
            const bool ok = doc.api.FPDFPage_RemoveObject(page.handle, object.handle);
            if (!ok) throw QStringLiteral("无法替换原文对象。");
            doc.api.FPDFPageObj_Destroy(object.handle); removed.insert(index);
        }
    }
    // PDFium's content generator omits some calibrated/ICC color operands.
    // Materialize their displayed RGB values on the rewritten page so white
    // panels and colored artwork do not inherit the preceding black color.
    for (int i = 0; i < doc.api.FPDFPage_CountObjects(page.handle); ++i) {
        const auto object = doc.api.FPDFPage_GetObject(page.handle, i); const int type = doc.api.FPDFPageObj_GetType(object);
        if (type != FPDF_PAGEOBJ_PATH && type != FPDF_PAGEOBJ_TEXT) continue;
        unsigned int r, g, b, alpha;
        if (doc.api.FPDFPageObj_GetFillColor(object, &r, &g, &b, &alpha)) doc.api.FPDFPageObj_SetFillColor(object, r, g, b, alpha);
        if (doc.api.FPDFPageObj_GetStrokeColor(object, &r, &g, &b, &alpha)) doc.api.FPDFPageObj_SetStrokeColor(object, r, g, b, alpha);
    }
    if (!doc.api.FPDFPage_GenerateContent(page.handle)) throw QStringLiteral("无法生成译文页面。");
    return statuses;
}
struct Writer : FPDF_FILEWRITE {
    QSaveFile file;
    PdfEngine::Progress progress;
    explicit Writer(const QString &path, const PdfEngine::Progress &callback = {}) : file(path), progress(callback) {
        version = 1; WriteBlock = [](FPDF_FILEWRITE *self, const void *data, unsigned long size) {
            auto *writer = static_cast<Writer *>(self);
            if (writer->file.write(static_cast<const char *>(data), size) != size) return 0;
            if (writer->progress) writer->progress({{"stage", "saving"}, {"completed", writer->file.pos()}});
            return 1;
        };
        if (!file.open(QIODevice::WriteOnly)) throw QStringLiteral("无法创建导出文件。");
    }
};

void publishPdf(const QString &staged, const QString &destination) {
    // Staging lives beside the destination, so replacement is a single rename.
    // Cancellation/failure before this point cannot truncate an existing PDF.
#ifdef Q_OS_WIN
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(staged.utf16()), reinterpret_cast<LPCWSTR>(destination.utf16()), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
#else
    if (std::rename(QFile::encodeName(staged).constData(), QFile::encodeName(destination).constData()) != 0)
#endif
        throw QStringLiteral("无法替换导出目标，请关闭占用该 PDF 的程序并检查写入权限。");
}

QJsonObject recognitionImage(Document &doc, int index, const QJsonObject &info, const QJsonObject &request) {
    Page page(doc, index);
    const auto region = rectValue(request["region"]);
    if (!std::isfinite(region.x()) || !std::isfinite(region.y()) || !std::isfinite(region.width()) || !std::isfinite(region.height())
        || region.width() < .005 || region.height() < .005 || !QRectF(0, 0, 1, 1).contains(region)) throw QStringLiteral("识别区域无效。");
    const double width = info["width"].toDouble() * region.width(), height = info["height"].toDouble() * region.height();
    const double scale = qBound(1000, request["pixels"].toInt(1800), 2400) / qMax(width, height);
    const int w = qMax(1, qRound(width * scale)), h = qMax(1, qRound(height * scale));
    QImage image(w, h, QImage::Format_ARGB32); image.fill(Qt::white);
    const auto bitmap = doc.api.FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA, image.bits(), image.bytesPerLine());
    if (!bitmap) throw QStringLiteral("识别图片内存不足。");
    const int fullWidth = qRound(w / region.width()), fullHeight = qRound(h / region.height());
    doc.api.FPDF_RenderPageBitmap(bitmap, page.handle, -qRound(region.x() * fullWidth), -qRound(region.y() * fullHeight), fullWidth, fullHeight, 0, FPDF_ANNOT);
    doc.api.FPDFBitmap_Destroy(bitmap);
    if (!image.save(request["image"].toString())) throw QStringLiteral("无法保存识别图片。");
    QJsonObject result{{"saved", true}};
    if (request["detectLayout"].toBool()) {
        QJsonObject layout;
        auto blocks = PdfScanLayout::detect(image, QSizeF(info["width"].toDouble(), info["height"].toDouble()), index, &layout);
        if (blocks.isEmpty()) throw QStringLiteral("未找到水平文字区域，请使用框选识别检查原图。");
        result["blocks"] = blocks;
        result["layout"] = layout;
    }
    return result;
}
}

QJsonObject PdfEngine::execute(const QJsonObject &request, const Progress &progress) {
    try {
        const auto action = request["action"].toString();
        auto manifest = request["document"].toObject();
        PdfTaskStore store(0);
        if (!request["taskDatabase"].toString().isEmpty()) {
            if (!store.open(request["taskDatabase"].toString(), true)) throw store.error();
            manifest = store.metadata(); if (manifest.isEmpty()) throw QStringLiteral("PDF 任务信息不存在或损坏。");
        }
        // Interactive page operations may reuse the fingerprint after checking
        // the source stamp. Opening and exporting always hash the entire file.
        const bool quick = request["quickSourceCheck"].toBool() && !manifest["stamp"].toObject().isEmpty()
            && (action == "render" || action == "recognitionImage" || action == "inspectPages");
        Api api; Document doc(api, request["source"].toString(), progress, quick ? manifest : QJsonObject{});
        const int count = api.FPDF_GetPageCount(doc.handle);
        if (count < 1) throw QStringLiteral("PDF 没有可读取的页面。");
        const QJsonObject info{{"version", manifestVersion}, {"extractionVersion", extractionVersion}, {"hash", doc.hash}, {"stamp", doc.stamp}, {"pageCount", count}};
        if (action == "info") return info;
        if (action == "inspectPages") {
            if (manifest["hash"].toString() != doc.hash || manifest["pageCount"].toInt() != count) throw QStringLiteral("原 PDF 已变化，请重新导入后翻译。");
            const int first = request["first"].toInt(-1);
            if (first < 0 || first >= count) throw QStringLiteral("页码无效。");
            const int length = qMin(qBound(1, request["count"].toInt(8), 8), count - first);
            QJsonArray pages, pageFiles;
            for (int i = first; i < first + length; ++i) {
                QJsonObject inspected;
                try { inspected = inspectPage(doc, i); }
                catch (const QString &error) {
                    // A problematic page must not discard the rest of a book.
                    Page page(doc, i);
                    inspected = QJsonObject{{"number", i}, {"analyzed", true}, {"extractionVersion", extractionVersion}, {"analysisError", error}, {"warning", error},
                        {"width", api.FPDF_GetPageWidthF(page.handle)}, {"height", api.FPDF_GetPageHeightF(page.handle)}, {"kind", "text"}, {"blocks", QJsonArray{}}};
                }
                if (request["pageDirectory"].toString().isEmpty()) pages.append(inspected);
                else {
                    const auto name = QStringLiteral("page-%1.json").arg(i);
                    QSaveFile file(QDir(request["pageDirectory"].toString()).filePath(name));
                    const auto bytes = QJsonDocument(inspected).toJson(QJsonDocument::Compact);
                    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                        throw QStringLiteral("无法保存第 %1 页的解析结果，请检查临时磁盘空间。").arg(i + 1);
                    pageFiles.append(QJsonObject{{"number", i}, {"file", name}});
                }
                if (progress) progress({{"stage", "inspect"}, {"completed", i + 1}, {"total", count}});
            }
            return {{"hash", doc.hash}, {"pages", pages}, {"pageFiles", pageFiles}, {"nextPage", first + length}};
        }
        if (action == QLatin1String("inspect")) {
            QJsonArray pages;
            for (int i = 0; i < count; ++i) {
                pages.append(inspectPage(doc, i));
                if (progress) progress({{"stage", "inspect"}, {"completed", i + 1}, {"total", count}});
            }
            auto result = info; result["pages"] = PdfLayout::linkPageHeaders(pages); return result;
        }
        if (manifest["version"].toInt() != manifestVersion || manifest["hash"].toString() != doc.hash
            || manifest["pageCount"].toInt(manifest["pages"].toArray().size()) != count)
            throw QStringLiteral("原 PDF 已变化，请重新导入后翻译。");
        const auto pages = manifest["pages"].toArray();
        const int artworkFirst = action == "render" ? request["page"].toInt(-1) : 0;
        const int artworkLast = action == "render" ? artworkFirst : count - 1;
        const auto patterned = PdfMerge::patternPages(request["source"].toString(), artworkFirst, artworkLast);
        const auto pageInfo = [&](int index) {
            auto page = store.isOpen() ? store.page(index) : request.contains("pageData") ? request["pageData"].toObject() : index < pages.size() ? pages[index].toObject() : QJsonObject{};
            if (page.isEmpty() || (page.contains("number") && page["number"].toInt(-1) != index)) throw QStringLiteral("第 %1 页尚未解析或任务数据无效。").arg(index + 1);
            if (patterned.contains(index)) page["preservePatternArtwork"] = true;
            return page;
        };
        if (action == QLatin1String("recognitionImage")) {
            const int index = request["page"].toInt(-1); if (index < 0 || index >= count) throw QStringLiteral("页码无效。");
            return recognitionImage(doc, index, pageInfo(index), request);
        }
        Fonts fonts;
        if (action == QLatin1String("checkLayout")) {
            const auto requested = request["pages"].toArray();
            if (requested.isEmpty() || requested.size() > count) throw QStringLiteral("排版检查页码无效。");
            QJsonArray results; QSet<int> seen;
            for (const auto &value : requested) {
                const int index = value.toInt(-1);
                if (index < 0 || index >= count || seen.contains(index)) throw QStringLiteral("排版检查页码无效。");
                seen.insert(index); Page page(doc, index); bool applied = false;
                results.append(QJsonObject{{"page", index}, {"statuses", applyPage(doc, page, pageInfo(index), fonts, &applied, true)}});
                if (progress) progress({{"stage", "layout"}, {"completed", results.size()}, {"total", requested.size()}});
            }
            // This checks text placement and shared text objects. Image
            // background repair is still checked by actual preview/export.
            return {{"pages", results}, {"textLayoutOnly", true}};
        }
        if (action == QLatin1String("render")) {
            const int index = request["page"].toInt(-1); if (index < 0 || index >= count) throw QStringLiteral("页码无效。");
            // Editing a page in a large tagged document makes PDFium inspect
            // the entire document's reference graph for each content rewrite.
            // Work on a page copy; the original file and task remain intact.
            doc.selectPages(index, index);
            Page page(doc, 0); const auto info = pageInfo(index);
            const int pixels = request["pixels"].toInt(1200);
            if (!render(page, info["width"].toDouble(), info["height"].toDouble(), pixels).save(request["original"].toString())) throw QStringLiteral("无法保存原文预览。");
            const auto statuses = translatePage(doc, page, info, fonts);
            if (!render(page, info["width"].toDouble(), info["height"].toDouble(), pixels).save(request["translated"].toString())) throw QStringLiteral("无法保存译文预览。");
            return {{"statuses", statuses}};
        }
        if (action == QLatin1String("export")) {
            const QFileInfo source(request["source"].toString()), destination(request["destination"].toString());
            const auto target = destination.exists() ? destination.canonicalFilePath() : destination.absoluteFilePath();
            if (source.canonicalFilePath().compare(target, Qt::CaseInsensitive) == 0) throw QStringLiteral("请另存译文，不能覆盖原 PDF。");
            std::unique_ptr<QTemporaryDir> ownedDirectory;
            QString workDirectory = request["workDirectory"].toString();
            if (workDirectory.isEmpty()) {
                ownedDirectory = std::make_unique<QTemporaryDir>(destination.absolutePath() + "/.Yiyi-export-XXXXXX");
                if (!ownedDirectory->isValid()) throw QStringLiteral("无法在导出目录创建暂存文件。");
                workDirectory = ownedDirectory->path();
            }
            const QDir staging(workDirectory);
            struct CloseExportDocument { Document &document; ~CloseExportDocument() { document.close(); } } closeDocument{doc};
            if (QFileInfo(staging.absolutePath()).dir().canonicalPath() != QDir(destination.absolutePath()).canonicalPath())
                throw QStringLiteral("导出暂存目录必须与目标 PDF 位于同一目录。");
            const bool preserveFailed = request["preserveFailed"].toBool();
            QJsonArray pageStatuses; int failedPages = 0, failedBlocks = 0, firstFailure = -1;
            for (int i = 0; i < count; ++i) {
                bool applied = false;
                const auto info = pageInfo(i), scan = info["scan"].toObject();
                QJsonArray statuses;
                { Page page(doc, i); statuses = applyPage(doc, page, info, fonts, &applied, true); }
                for (int n = 0; n < statuses.size(); ++n) {
                    auto status = statuses[n].toObject();
                    if (status["backgroundReview"].toBool() || scan["stale"].toBool()) {
                        status["fits"] = false; status["error"] = scan["stale"].toBool() ? QStringLiteral("原文已修改，旧译文需要更新。") : QStringLiteral("背景修复尚未确认，请检查预览并确认。");
                        statuses[n] = status;
                    }
                }
                if (scan["blocks"].toArray().isEmpty() && !scan["translation"].toString().trimmed().isEmpty())
                    statuses.append(QJsonObject{{"id", ""}, {"fits", false}, {"error", QStringLiteral("旧版文字草稿需要重新识别文字区域。")}});
                pageStatuses.append(QJsonObject{{"page", i}, {"statuses", statuses}});
                int failures = 0; for (const auto &v : statuses) if (!v.toObject()["fits"].toBool()) ++failures;
                if (failures) { ++failedPages; failedBlocks += failures; if (firstFailure < 0) firstFailure = i; }
                if (progress) progress({{"stage", "preflight"}, {"completed", i + 1}, {"total", count}});
                if ((i + 1) % 32 == 0 && i + 1 < count) doc.reopenSource(source.absoluteFilePath());
            }
            if (failedPages && !preserveFailed) {
                QString reason;
                for (const auto &v : pageStatuses[firstFailure].toObject()["statuses"].toArray()) if (!v.toObject()["fits"].toBool()) { reason = v.toObject()["error"].toString(); break; }
                return {{"error", QStringLiteral("导出检查发现 %1 页、%2 个区域需要校对，最早在第 %3 页：%4 请打开「待校对」查看全部问题，或选择保留问题原文导出。")
                    .arg(failedPages).arg(failedBlocks).arg(firstFailure + 1).arg(reason)}, {"page", firstFailure}, {"pageStatuses", pageStatuses}};
            }
            // Do not carry the full-book preflight's PDFium caches into the
            // editing pass. Keep both passes bounded to one page batch.
            doc.reopenSource(source.absoluteFilePath());
            QStringList parts;
            int batchFirst = 0;
            for (int i = 0; i < count; ++i) {
                if (i == batchFirst) doc.selectPages(batchFirst, qMin(batchFirst + 31, count - 1));
                auto info = pageInfo(i);
                if (preserveFailed) {
                    QSet<QString> rejected;
                    for (const auto &v : pageStatuses[i].toObject()["statuses"].toArray()) if (!v.toObject()["fits"].toBool()) rejected.insert(v.toObject()["id"].toString());
                    const bool scanPage = info.contains("scan"); auto content = scanPage ? info["scan"].toObject() : info;
                    auto blocks = content["blocks"].toArray();
                    for (int n = 0; n < blocks.size(); ++n) { auto b = blocks[n].toObject(); if (rejected.contains(b["id"].toString())) b["keep"] = true; blocks[n] = b; }
                    content["blocks"] = blocks;
                    if (scanPage) { content["stale"] = false; if (rejected.contains(QString())) content["translation"] = ""; info["scan"] = content; }
                    else info = content;
                }
                const auto scan = info.value("scan").toObject();
                for (const auto &v : scan["blocks"].toArray()) {
                    const auto b = v.toObject();
                    if (b["backgroundMode"] == "repair" && !b["backgroundConfirmed"].toBool() && !b["translation"].toString().trimmed().isEmpty() && !b["keep"].toBool() && !b["fixed"].toBool())
                        return {{"error", QStringLiteral("第 %1 页的背景修复尚未确认，请检查右页预览并在区域校对中确认。").arg(i + 1)}, {"page", i},
                            {"statuses", QJsonArray{QJsonObject{{"id", b["id"]}, {"fits", false}, {"backgroundReview", true}, {"error", QStringLiteral("请检查背景修复预览并确认。")}}}}};
                }
                if (scan["blocks"].toArray().isEmpty() && !scan["translation"].toString().trimmed().isEmpty())
                    return {{"error", QStringLiteral("第 %1 页为旧版文字草稿，请重新识别文字区域后导出原版式译文。旧草稿已保留。").arg(i + 1)}, {"page", i}};
                if (scan["stale"].toBool())
                    return {{"error", QStringLiteral("第 %1 页原文已修改，请更新或清空旧译文后导出。").arg(i + 1)}, {"page", i}};
                QJsonArray statuses;
                { Page page(doc, i - batchFirst); statuses = translatePage(doc, page, info, fonts); }
                auto checked = pageStatuses[i].toObject()["statuses"].toArray();
                for (const auto &status : statuses) if (!status.toObject()["fits"].toBool()) {
                    if (!preserveFailed) return {{"error", QStringLiteral("第 %1 页有区域需要处理，请检查标记后导出。").arg(i + 1)}, {"page", i}, {"statuses", statuses}, {"pageStatuses", pageStatuses}};
                    bool found = false;
                    for (int n = 0; n < checked.size(); ++n) if (checked[n].toObject()["id"] == status.toObject()["id"]) { checked[n] = status; found = true; break; }
                    if (!found) checked.append(status);
                }
                pageStatuses[i] = QJsonObject{{"page", i}, {"statuses", checked}};
                if (progress) progress({{"stage", "export"}, {"completed", i + 1}, {"total", count}});
                // Save only this batch. Reopening the original between batches
                // prevents PDFium from retaining prior translated image streams.
                // Qt already embeds subset fonts with ToUnicode mappings.
                if ((i + 1) % 32 == 0 || i + 1 == count) {
                    struct Part {
                        Api &api; FPDF_DOCUMENT handle;
                        ~Part() { if (handle) api.FPDF_CloseDocument(handle); }
                    } part{api, api.FPDF_CreateNewDocument()};
                    if (!part.handle) throw QStringLiteral("无法创建 PDF 导出分片。");
                    const auto range = QByteArray("1-") + QByteArray::number(i + 1 - batchFirst);
                    if (!api.FPDF_ImportPages(part.handle, doc.handle, range.constData(), 0)
                        || api.FPDF_GetPageCount(part.handle) != i + 1 - batchFirst) throw QStringLiteral("无法提取 PDF 导出分片。");
                    const auto path = staging.filePath(QStringLiteral("part-%1.pdf").arg(parts.size()));
                    const auto checkpointProgress = [&](QJsonObject value) { value["stage"] = "checkpoint"; if (progress) progress(value); };
                    { Writer writer(path, checkpointProgress);
                        if (!api.FPDF_SaveAsCopy(part.handle, &writer, FPDF_NO_INCREMENTAL) || !writer.file.commit())
                            throw QStringLiteral("PDF 导出暂存失败，请检查磁盘空间。"); }
                    parts.append(path); batchFirst = i + 1;
                    if (i + 1 < count) doc.reopenSource(source.absoluteFilePath());
                }
            }
            doc.close();
            const auto output = staging.filePath("complete.pdf");
            PdfMerge::write(source.absoluteFilePath(), parts, output, count, progress);
            const QFileInfo current(source.absoluteFilePath());
            if (current.size() != doc.stamp["size"].toInteger() || current.lastModified().toUTC().toString(Qt::ISODateWithMs) != doc.stamp["modified"].toString())
                throw QStringLiteral("导出期间原 PDF 已变化，请重新导入后重试。");
            // Release mapped snapshots before QTemporaryDir removes them on Windows.
            doc.close();
            publishPdf(output, destination.absoluteFilePath());
            QJsonArray preserved; QSet<int> preservedPages;
            QString report = QStringLiteral("PDF 导出检查清单\n文件：%1\n未通过检查的区域在导出文件中保留原文，任务中的译文未删除。\n\n").arg(destination.fileName());
            for (const auto &v : pageStatuses) {
                const auto p = v.toObject(); const int number = p["page"].toInt();
                for (const auto &status : p["statuses"].toArray()) {
                    const auto s = status.toObject(); if (s["fits"].toBool()) continue;
                    preservedPages.insert(number); preserved.append(QJsonObject{{"page", number + 1}, {"id", s["id"]}, {"reason", s["error"]}});
                    report += QStringLiteral("第 %1 页 · %2\n原因：%3\n\n").arg(number + 1).arg(s["id"].toString(), s["error"].toString());
                }
            }
            report += QStringLiteral("合计：%1 页、%2 个区域保留原文。\n").arg(preservedPages.size()).arg(preserved.size());
            const auto reportPath = destination.absoluteFilePath() + QStringLiteral(".issues.txt");
            QSaveFile reportFile(reportPath); const auto reportBytes = report.toUtf8();
            const bool reportSaved = reportFile.open(QIODevice::WriteOnly) && reportFile.write(reportBytes) == reportBytes.size() && reportFile.commit();
            return {{"saved", true}, {"pageStatuses", pageStatuses}, {"preserved", preserved}, {"preservedPages", preservedPages.size()},
                {"report", reportSaved ? reportPath : QString()}, {"reportError", reportSaved ? QString() : QStringLiteral("PDF 已导出，但无法写入检查清单；请在待校对中查看。")}};
        }
        throw QStringLiteral("不支持的 PDF 操作。");
    } catch (const QString &error) { return {{"error", error}}; }
    catch (const std::bad_alloc &) { return {{"error", QStringLiteral("PDF 处理内存不足，已保存的翻译仍可继续使用。")}}; }
}
