#include "officedocument.h"
#include "miniz.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTextStream>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>

namespace {
QString namespaceFor(QDomElement e, const QString &prefix) {
    const auto attribute = prefix.isEmpty() ? QStringLiteral("xmlns") : "xmlns:" + prefix;
    for (; !e.isNull(); e = e.parentNode().toElement()) if (e.hasAttribute(attribute)) return e.attribute(attribute);
    return {};
}
QString namespaceUri(const QDomElement &e) {
    if (!e.namespaceURI().isEmpty()) return e.namespaceURI();
    return namespaceFor(e, e.tagName().contains(':') ? e.tagName().section(':', 0, 0) : QString());
}
bool family(const QDomElement &e, const QString &suffix) {
    return namespaceUri(e).endsWith(suffix);
}
QString local(const QDomElement &e) { return e.localName().isEmpty() ? e.tagName().section(':', -1) : e.localName(); }
QString attr(const QDomElement &e, const QString &key) {
    const auto attributes = e.attributes();
    for (int i = 0; i < attributes.size(); ++i) {
        const auto a = attributes.item(i).toAttr();
        if (a.localName() == key || a.name().section(':', -1) == key) return a.value();
    }
    return {};
}
void descendants(const QDomElement &root, const QString &name, QList<QDomElement> &out) {
    if (local(root) == name) out.append(root);
    for (auto node = root.firstChildElement(); !node.isNull(); node = node.nextSiblingElement()) descendants(node, name, out);
}
QList<QDomElement> named(const QDomElement &root, const QString &name) {
    QList<QDomElement> out; descendants(root, name, out); return out;
}
QList<QDomElement> paragraphs(const QDomDocument &doc) {
    QList<QDomElement> out;
    for (const auto &e : named(doc.documentElement(), "p"))
        if (family(e, "/wordprocessingml/2006/main") || family(e, "/wordprocessingml/main")
            || family(e, "/drawingml/2006/main") || family(e, "/drawingml/main")) out.append(e);
    return out;
}
void paragraphTexts(const QDomElement &root, QList<QDomElement> &out) {
    for (auto e = root.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        if (local(e) == "p") continue; // Nested text boxes have their own paragraph.
        if (local(e) == "t" && (namespaceUri(e).contains("wordprocessingml") || namespaceUri(e).contains("drawingml"))) out.append(e);
        else paragraphTexts(e, out);
    }
}
QList<QList<int>> textPieces(const QDomElement &paragraph) {
    QList<QList<int>> pieces; QList<int> current; int ordinal = 0;
    auto walk = [&](auto &&self, const QDomElement &root) -> void {
        for (auto e = root.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
            if (local(e) == "p") continue;
            if (local(e) == "t" && (namespaceUri(e).contains("wordprocessingml") || namespaceUri(e).contains("drawingml"))) current.append(ordinal++);
            else if (local(e) == "br" || local(e) == "tab" || local(e) == "cr" || local(e) == "footnoteReference" || local(e) == "endnoteReference") {
                if (!current.isEmpty()) { pieces.append(current); current.clear(); }
            } else self(self, e);
        }
    };
    walk(walk, paragraph); if (!current.isEmpty()) pieces.append(current); return pieces;
}
QString nodeStyle(QDomElement e) {
    QString style;
    for (auto p = e.parentNode().toElement(); !p.isNull(); p = p.parentNode().toElement()) {
        if (local(p) == "hyperlink") style += "link:" + attr(p, "id");
        if (local(p) == "r") for (auto c = p.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) {
            if (local(c) == "rPr") { QString text; QTextStream stream(&text); c.save(stream, 0); style += text; }
        }
        if (local(p) == "p") break;
    }
    return style;
}
bool parseXml(const QByteArray &bytes, QDomDocument *doc, QString *error) {
    QXmlStreamReader check(bytes);
    while (!check.atEnd()) {
        if (check.readNext() == QXmlStreamReader::DTD) { *error = QStringLiteral("文档包含不支持的 XML DTD。"); return false; }
    }
    if (check.hasError()) { *error = QStringLiteral("文档 XML 损坏：%1").arg(check.errorString()); return false; }
    // QDom's namespace mode discards declarations referenced only by values
    // such as mc:Ignorable. Preserve qualified names/declarations verbatim and
    // resolve namespace URIs through the original ancestor declarations.
    auto options = QDomDocument::ParseOptions(QDomDocument::ParseOption::PreserveSpacingOnlyNodes);
    const auto result = doc->setContent(bytes, options);
    if (!result) { *error = QStringLiteral("无法解析文档 XML：%1").arg(result.errorMessage); return false; }
    return true;
}
struct ZipReader {
    mz_zip_archive zip{};
    bool ready = false;
    explicit ZipReader(const QByteArray &data) { ready = mz_zip_reader_init_mem(&zip, data.constData(), size_t(data.size()), 0); }
    ~ZipReader() { if (ready) mz_zip_reader_end(&zip); }
    QByteArray read(int index, QString *error) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, mz_uint(index), &stat) || stat.m_uncomp_size > 32 * 1024 * 1024 || stat.m_is_encrypted) {
            *error = QStringLiteral("文档部件过大、已加密或损坏。"); return {};
        }
        QByteArray data(qsizetype(stat.m_uncomp_size), Qt::Uninitialized);
        if (!mz_zip_reader_extract_to_mem(&zip, mz_uint(index), data.data(), size_t(data.size()), 0)) {
            *error = QStringLiteral("无法读取文档部件，文件可能损坏。"); return {};
        }
        return data;
    }
};
QString relationshipPart(const QString &part) {
    const auto slash = part.lastIndexOf('/');
    return part.left(slash + 1) + "_rels/" + part.mid(slash + 1) + ".rels";
}
QString resolve(const QString &part, const QString &target) {
    const auto base = part.left(part.lastIndexOf('/') + 1);
    return QDir::cleanPath(target.startsWith('/') ? target.mid(1) : base + target);
}
double textHeight(const QString &text, double size, double width) {
    QFont font(QStringLiteral("Arial")); font.setPointSizeF(size);
    QTextLayout layout(text, font); QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); layout.setTextOption(option);
    layout.beginLayout(); double height = 0;
    while (true) { auto line = layout.createLine(); if (!line.isValid()) break; line.setLineWidth(qMax(1.0, width * 96 / 72)); height += line.height() * 72 / 96; }
    layout.endLayout(); return height;
}
QString withWhitespace(const QString &source, const QString &target) {
    int start = 0, end = source.size();
    while (start < end && source[start].isSpace()) ++start;
    while (end > start && source[end - 1].isSpace()) --end;
    return source.left(start) + target.trimmed() + source.mid(end);
}
void replaceText(QDomElement e, const QString &text) {
    while (!e.firstChild().isNull()) e.removeChild(e.firstChild());
    e.appendChild(e.ownerDocument().createTextNode(text));
    e.setAttribute("xml:space", "preserve");
}
}

OfficeDocument OfficeDocument::load(const QString &filePath) {
    OfficeDocument result; result.path = QFileInfo(filePath).absoluteFilePath();
    const auto suffix = QFileInfo(filePath).suffix().toLower();
    if (suffix != "docx" && suffix != "pptx") { result.error = QStringLiteral("请选择 .docx 或 .pptx；旧版 .doc / .ppt 请先另存为新格式。"); return result; }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 512ll * 1024 * 1024) { result.error = QStringLiteral("无法读取文档，或文件超过 512 MiB。"); return result; }
    result.m_archive = file.readAll();
    if (file.error() != QFileDevice::NoError) { result.error = QStringLiteral("文档读取失败。"); return result; }
    result.hash = QString::fromLatin1(QCryptographicHash::hash(result.m_archive, QCryptographicHash::Sha256).toHex());
    ZipReader zip(result.m_archive);
    if (!zip.ready) { result.error = QStringLiteral("文件不是有效的 Office 开放文档。加密文档请先解除密码，旧格式请先转换。"); return result; }
    QHash<QString, int> entries;
    const auto count = mz_zip_reader_get_num_files(&zip.zip);
    if (count > 100000) { result.error = QStringLiteral("文档包含过多部件。"); return result; }
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip.zip, i, &stat)) { result.error = QStringLiteral("文档目录损坏。"); return result; }
        if (mz_zip_reader_get_filename(&zip.zip, i, nullptr, 0) > sizeof(stat.m_filename)) { result.error = QStringLiteral("文档部件名称过长。"); return result; }
        const auto name = QString::fromUtf8(stat.m_filename);
        if (entries.contains(name) || name.contains('\\') || name.startsWith('/') || name.contains("../") || stat.m_is_encrypted) { result.error = QStringLiteral("文档包含不规范的部件路径，或部件已加密。"); return result; }
        entries[name] = int(i);
        if (name.startsWith("_xmlsignatures/")) { result.error = QStringLiteral("带数字签名的文档请先保存为未签名副本，再进行翻译。"); return result; }
        if (name.endsWith("vbaProject.bin", Qt::CaseInsensitive)) { result.error = QStringLiteral("请先另存为不含宏的 DOCX / PPTX 文档。"); return result; }
        if (name.contains("/media/") && !stat.m_is_directory) {
            ++result.imageCount;
            result.images.append(QVariantMap{{"id", name}, {"name", QFileInfo(name).fileName()}});
        }
    }
    auto xml = [&](const QString &name) -> QDomDocument {
        QDomDocument doc;
        if (!entries.contains(name)) { result.error = QStringLiteral("文档缺少必要部件：%1").arg(name); return doc; }
        const auto bytes = zip.read(entries[name], &result.error);
        if (result.error.isEmpty()) parseXml(bytes, &doc, &result.error);
        return doc;
    };
    const auto rootRels = xml("_rels/.rels");
    QString main;
    for (const auto &e : named(rootRels.documentElement(), "Relationship"))
        if (e.attribute("Type").endsWith("/officeDocument") && e.attribute("TargetMode") != "External") main = resolve({}, e.attribute("Target"));
    if (!result.error.isEmpty()) return result;
    if (main.isEmpty() || !entries.contains("[Content_Types].xml")) { result.error = QStringLiteral("文档缺少 Office 类型或主文档关系。"); return result; }
    const auto mainDoc = xml(main);
    if (!result.error.isEmpty()) return result;
    const auto expected = suffix == "docx" ? "document" : "presentation";
    if (local(mainDoc.documentElement()) != expected) { result.error = QStringLiteral("文件内容与扩展名不一致。"); return result; }
    result.kind = suffix;
    QList<QPair<QString, QString>> parts;
    QHash<QString, int> pageOf;
    if (suffix == "docx") { parts.append({main, QStringLiteral("正文")}); result.pages = 1; }
    else {
        const auto rels = xml(relationshipPart(main)); QHash<QString, QString> slides;
        for (const auto &e : named(rels.documentElement(), "Relationship"))
            if (e.attribute("Type").endsWith("/slide") && e.attribute("TargetMode") != "External") slides[e.attribute("Id")] = resolve(main, e.attribute("Target"));
        for (const auto &e : named(mainDoc.documentElement(), "sldId")) {
            QString id;
            const auto attributes = e.attributes();
            for (int i = 0; i < attributes.size(); ++i) {
                const auto a = attributes.item(i).toAttr();
                if (a.name().section(':', -1) == "id" && a.name().contains(':') && namespaceFor(e, a.name().section(':', 0, 0)).contains("relationships")) id = a.value();
            }
            if (!slides.contains(id)) { result.error = QStringLiteral("幻灯片关系缺失。"); return result; }
            const auto slide = slides[id]; ++result.pages; pageOf[slide] = result.pages;
            parts.append({slide, QStringLiteral("第 %1 页").arg(result.pages)});
            const auto relName = relationshipPart(slide);
            if (!entries.contains(relName)) continue;
            const auto slideRels = xml(relName);
            for (const auto &rel : named(slideRels.documentElement(), "Relationship")) {
                if (rel.attribute("TargetMode") == "External") continue;
                const auto type = rel.attribute("Type"), target = resolve(slide, rel.attribute("Target"));
                QString label;
                if (type.endsWith("/notesSlide")) label = QStringLiteral("备注");
                else if (type.endsWith("/chart")) label = QStringLiteral("图表");
                else if (type.endsWith("/diagramData") || type.endsWith("/diagramDrawing")) label = QStringLiteral("SmartArt");
                if (!label.isEmpty()) { pageOf[target] = result.pages; parts.append({target, QStringLiteral("第 %1 页 · %2").arg(result.pages).arg(label)}); }
            }
        }
    }
    // Word stories, drawing text and diagram parts keep their independent styles.
    QStringList names = entries.keys(); names.sort();
    for (const auto &name : names) {
        QString label;
        if (suffix == "docx" && name.startsWith("word/") && name.endsWith(".xml")) {
            if (QFileInfo(name).fileName().startsWith("header")) label = QStringLiteral("页眉");
            else if (QFileInfo(name).fileName().startsWith("footer")) label = QStringLiteral("页脚");
            else if (name.endsWith("/footnotes.xml")) label = QStringLiteral("脚注");
            else if (name.endsWith("/endnotes.xml")) label = QStringLiteral("尾注");
            else if (name.contains("/charts/")) label = QStringLiteral("图表");
            else if (name.contains("/diagrams/")) label = QStringLiteral("SmartArt");
        }
        if (suffix == "pptx" && name.startsWith("ppt/diagrams/") && name.endsWith(".xml")) label = QStringLiteral("SmartArt");
        if (!label.isEmpty()) parts.append({name, label});
        if (name.contains("/embeddings/")) result.notices.append(QVariantMap{{"location", name}, {"message", QStringLiteral("嵌入文件保留原样，未翻译内部内容。")}});
    }
    QSet<QString> seen;
    for (const auto &part : parts) {
        if (seen.contains(part.first)) continue; seen.insert(part.first);
        const auto doc = part.first == main ? mainDoc : xml(part.first);
        if (!result.error.isEmpty()) return result;
        result.m_parts[part.first] = doc;
        const auto ps = paragraphs(doc);
        int ordinal = 0, fieldDepth = 0;
        for (int p = 0; p < ps.size(); ++p) {
            const auto paragraph = ps[p]; QList<QDomElement> texts; paragraphTexts(paragraph, texts);
            bool protectedContent = fieldDepth > 0;
            for (const auto &field : named(paragraph, "fldChar")) {
                protectedContent = true;
                if (attr(field, "fldCharType") == "begin") ++fieldDepth;
                if (attr(field, "fldCharType") == "end") fieldDepth = qMax(0, fieldDepth - 1);
            }
            for (auto ancestor = paragraph.parentNode().toElement(); !ancestor.isNull(); ancestor = ancestor.parentNode().toElement())
                if (QStringList{"ins", "del", "moveFrom", "moveTo"}.contains(local(ancestor))) protectedContent = true;
            if (texts.isEmpty()) continue;
            QString all; for (const auto &e : texts) all += e.text();
            if (!all.contains(QRegularExpression(QStringLiteral("\\p{L}")))) continue;
            for (const auto &name : {"fldChar", "fldSimple", "fld", "ins", "del", "moveFrom", "moveTo"})
                if (!named(paragraph, name).isEmpty()) protectedContent = true;
            if (protectedContent) {
                result.notices.append(QVariantMap{{"location", part.second}, {"message", QStringLiteral("含自动域或修订的段落保留原文：%1").arg(all.left(70))}}); continue;
            }
            OfficeSegment segment; segment.part = part.first; segment.paragraph = p; segment.source = all;
            segment.id = part.first + ':' + QString::number(p); segment.page = pageOf.value(part.first);
            segment.location = part.second + QStringLiteral(" · 段落 %1").arg(++ordinal);
            for (auto shape = paragraph.parentNode().toElement(); !shape.isNull(); shape = shape.parentNode().toElement()) {
                if (local(shape) != "sp") continue;
                const auto ids = named(shape, "cNvPr"), extents = named(shape, "ext");
                segment.group = part.first + ':' + (ids.isEmpty() ? QString::number(p) : attr(ids.first(), "id"));
                if (!ids.isEmpty() && !attr(ids.first(), "name").isEmpty()) segment.location = part.second + " · " + attr(ids.first(), "name") + QStringLiteral(" · 段落 %1").arg(ordinal);
                for (const auto &ext : extents) if (ext.hasAttribute("cx") && ext.hasAttribute("cy")) {
                    segment.width = ext.attribute("cx").toDouble() / 12700; segment.height = ext.attribute("cy").toDouble() / 12700; break;
                }
                const auto bodies = named(shape, "bodyPr");
                auto margin = [&](const QString &key, double fallback) { return bodies.isEmpty() || !bodies.first().hasAttribute(key) ? fallback : bodies.first().attribute(key).toDouble() / 12700; };
                segment.width -= margin("lIns", 7.2) + margin("rIns", 7.2); segment.height -= margin("tIns", 3.6) + margin("bIns", 3.6);
                const auto properties = named(paragraph, "rPr");
                for (const auto &prop : properties) if (prop.hasAttribute("sz")) { segment.fontSize = prop.attribute("sz").toDouble() / 100; break; }
                break;
            }
            if (part.first.contains("notesSlides/")) { segment.width = segment.height = 0; segment.location = part.second + QStringLiteral(" · 段落 %1").arg(ordinal); }
            const auto pieces = textPieces(paragraph); int piece = 0;
            for (const auto &nodes : pieces) {
                auto unit = segment; unit.source.clear(); unit.nodes = nodes;
                QSet<QString> styles;
                for (int node : nodes) { unit.source += texts[node].text(); styles.insert(nodeStyle(texts[node])); }
                if (!unit.source.contains(QRegularExpression(QStringLiteral("\\p{L}")))) continue;
                if (pieces.size() > 1) { unit.id += ':' + QString::number(piece); unit.location += QStringLiteral(" · 片段 %1").arg(piece + 1); }
                ++piece; unit.mixedStyle = styles.size() > 1;
                if (unit.mixedStyle) result.notices.append(QVariantMap{{"id", unit.id}, {"location", unit.location}, {"message", QStringLiteral("段内有多种样式或链接，译文样式位置按长度映射，请校对强调范围。")}});
                result.segments.append(unit);
            }
        }
        if (part.first.contains("/charts/") && !named(doc.documentElement(), "strRef").isEmpty())
            result.notices.append(QVariantMap{{"location", part.second}, {"message", QStringLiteral("图表标题可翻译；与嵌入工作簿关联的系列名、坐标标签保留原文。")}});
    }
    if (result.imageCount) result.notices.append(QVariantMap{{"location", QStringLiteral("图片")}, {"message", QStringLiteral("保留 %1 个媒体资源；图片内文字未改写。可通过“图片取字”识别、复制并单独翻译。").arg(result.imageCount)}});
    if (result.segments.isEmpty() && !result.imageCount) result.error = QStringLiteral("没有找到可翻译的文字或可查看的图片。");
    return result;
}

QImage OfficeDocument::image(const QString &id, QString *failure) const {
    bool found = false;
    for (const auto &entry : images) if (entry.toMap()["id"].toString() == id) { found = true; break; }
    if (!found) { *failure = QStringLiteral("图片不存在。"); return {}; }
    ZipReader zip(m_archive);
    const auto index = zip.ready ? mz_zip_reader_locate_file(&zip.zip, id.toUtf8().constData(), nullptr, 0) : -1;
    if (index < 0) { *failure = QStringLiteral("无法读取文档图片。"); return {}; }
    const auto data = zip.read(index, failure);
    auto result = QImage::fromData(data);
    if (result.isNull() && failure->isEmpty()) *failure = QStringLiteral("此媒体格式暂不支持取字，可在 Office 中截图后识别。");
    return result;
}

QHash<QString, double> OfficeDocument::fitScales(const QHash<QString, QString> &translations, bool fitText, double minimumFont) const {
    QHash<QString, QList<const OfficeSegment *>> groups;
    for (const auto &segment : segments) if (!segment.group.isEmpty() && segment.width > 0 && segment.height > 0) groups[segment.group].append(&segment);
    QHash<QString, double> scales;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const auto &items = it.value();
        auto height = [&](double scale) {
            double value = 0;
            for (const auto *s : items) value += textHeight(translations.value(s->id, s->source), qMax(qMin(minimumFont, s->fontSize), s->fontSize * scale), s->width) * 1.12;
            return value;
        };
        double scale = 1;
        if (fitText && height(scale) > items.first()->height) {
            double low = 0.05, high = 1;
            for (int i = 0; i < 16; ++i) { const double middle = (low + high) / 2; if (height(middle) <= items.first()->height) low = middle; else high = middle; }
            scale = low;
        }
        scales[it.key()] = scale;
    }
    return scales;
}
QVariantList OfficeDocument::issues(const QHash<QString, QString> &translations, bool fitText, double minimumFont) const {
    auto result = notices; const auto scales = fitScales(translations, fitText, minimumFont);
    QHash<QString, double> heights; QHash<QString, const OfficeSegment *> first;
    for (const auto &s : segments) {
        if (s.width <= 0 || s.height <= 0 || s.group.isEmpty()) continue;
        const auto size = qMax(qMin(minimumFont, s.fontSize), s.fontSize * scales.value(s.group, 1));
        heights[s.group] += textHeight(translations.value(s.id, s.source), size, s.width) * 1.12; first[s.group] = &s;
    }
    for (auto it = heights.cbegin(); it != heights.cend(); ++it) {
        const auto *s = first[it.key()];
        if (it.value() > s->height) result.append(QVariantMap{{"id", s->id}, {"location", s->location}, {"message", QStringLiteral("文字可能超出文本框；请在 Office 中检查字号与换行。此处为估算。")}});
        else if (scales.value(it.key(), 1) < 0.99) result.append(QVariantMap{{"id", s->id}, {"location", s->location}, {"message", QStringLiteral("导出时将在字号下限内缩小文字，建议检查实际版式。")}});
    }
    return result;
}

bool OfficeDocument::save(const QString &destination, const QHash<QString, QString> &translations, bool fitText, double minimumFont, QString *failure) const {
    if (!valid()) { *failure = error; return false; }
    const QFileInfo source(path), output(destination);
    const auto from = source.exists() ? source.canonicalFilePath() : source.absoluteFilePath();
    const auto to = output.exists() ? output.canonicalFilePath() : output.absoluteFilePath();
    if (from.compare(to, Qt::CaseInsensitive) == 0 || output.suffix().compare(kind, Qt::CaseInsensitive) != 0) {
        *failure = QStringLiteral("请另存为不同文件名的 .%1 文件，不能覆盖原文。").arg(kind); return false;
    }
    QHash<QString, QDomDocument> documents; QSet<QString> scaled;
    const auto scales = fitScales(translations, fitText, minimumFont);
    for (const auto &s : segments) {
        if (!translations.contains(s.id)) continue;
        if (!documents.contains(s.part)) documents[s.part] = m_parts[s.part].cloneNode(true).toDocument();
        auto doc = documents[s.part]; const auto ps = paragraphs(doc);
        if (s.paragraph >= ps.size()) { *failure = QStringLiteral("文档段落映射发生变化。"); return false; }
        auto p = ps[s.paragraph]; QList<QDomElement> allTexts, texts; paragraphTexts(p, allTexts);
        for (int node : s.nodes) {
            if (node >= allTexts.size()) { *failure = QStringLiteral("文档文字映射发生变化。"); return false; }
            texts.append(allTexts[node]);
        }
        const auto target = withWhitespace(s.source, translations[s.id]);
        int consumed = 0, sourceConsumed = 0;
        for (int i = 0; i < texts.size(); ++i) {
            sourceConsumed += texts[i].text().size();
            int end = i == texts.size() - 1 ? target.size() : qRound(double(sourceConsumed) * target.size() / qMax(1, s.source.size()));
            end = qBound(consumed, end, int(target.size()));
            // Never split a surrogate or combining character between styled runs.
            QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, target); boundary.setPosition(end);
            if (!boundary.isAtBoundary()) { const int next = boundary.toNextBoundary(); if (next >= 0) end = next; }
            replaceText(texts[i], target.mid(consumed, end - consumed)); consumed = end;
        }
        const double scale = scales.value(s.group, 1);
        const auto paragraphKey = s.part + ':' + QString::number(s.paragraph);
        if (scale < 0.99 && kind == "pptx" && !scaled.contains(paragraphKey)) {
            scaled.insert(paragraphKey);
            for (auto run = p.firstChildElement(); !run.isNull(); run = run.nextSiblingElement()) {
                if (local(run) != "r" && local(run) != "fld") continue;
                auto properties = run.firstChildElement();
                while (!properties.isNull() && local(properties) != "rPr") properties = properties.nextSiblingElement();
                if (properties.isNull()) {
                    const auto prefix = p.tagName().contains(':') ? p.tagName().section(':', 0, 0) + ':' : QString();
                    properties = doc.createElement(prefix + "rPr"); run.insertBefore(properties, run.firstChild());
                }
                const double original = properties.hasAttribute("sz") ? properties.attribute("sz").toDouble() / 100 : s.fontSize;
                properties.setAttribute("sz", qRound(qMax(qMin(minimumFont, original), original * scale) * 100));
            }
        }
    }
    ZipReader reader(m_archive); QSaveFile file(destination);
    if (!reader.ready || !file.open(QIODevice::WriteOnly)) { *failure = QStringLiteral("无法创建译文文件，请检查保存位置。"); return false; }
    mz_zip_archive writer{}; writer.m_pIO_opaque = &file;
    writer.m_pWrite = [](void *opaque, mz_uint64 offset, const void *data, size_t size) -> size_t {
        auto *out = static_cast<QSaveFile *>(opaque); if (!out->seek(qint64(offset))) return 0;
        const auto n = out->write(static_cast<const char *>(data), qint64(size)); return n < 0 ? 0 : size_t(n);
    };
    bool ok = mz_zip_writer_init(&writer, 0);
    if (!ok) { *failure = QStringLiteral("无法初始化 Office 导出。"); return false; }
    for (mz_uint i = 0; ok && i < mz_zip_reader_get_num_files(&reader.zip); ++i) {
        mz_zip_archive_file_stat stat{}; ok = mz_zip_reader_file_stat(&reader.zip, i, &stat);
        if (!ok) break;
        const auto name = QString::fromUtf8(stat.m_filename);
        if (documents.contains(name)) {
            auto doc = documents[name];
            for (auto n = doc.firstChild(); !n.isNull();) {
                const auto next = n.nextSibling(); if (n.isProcessingInstruction() && n.nodeName() == "xml") doc.removeChild(n); n = next;
            }
            doc.insertBefore(doc.createProcessingInstruction("xml", "version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\""), doc.firstChild());
            const auto bytes = doc.toByteArray(-1);
            ok = mz_zip_writer_add_mem(&writer, stat.m_filename, bytes.constData(), size_t(bytes.size()), MZ_BEST_SPEED);
        } else ok = mz_zip_writer_add_from_zip_reader(&writer, &reader.zip, i);
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&writer);
    mz_zip_writer_end(&writer);
    if (!ok || !file.commit()) { *failure = QStringLiteral("导出失败，原文件未改动，请检查磁盘空间和目录权限。"); return false; }
    return true;
}
