#include "pdfscanlayout.h"
#include "pdflayout.h"
#include "pdfbackground.h"
#include <QPainter>
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
QJsonArray rectJson(const QRectF &r) { return {r.x(), r.y(), r.width(), r.height()}; }
QRectF rectValue(const QJsonValue &value) {
    const auto a = value.toArray(); return a.size() == 4 ? QRectF(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()) : QRectF();
}
struct Ink { QRect rect; int pixels; bool colored; };
// Connected ink lets us distinguish letters from long rules and large artwork.
// Erasing only complete letter components avoids wiping a paragraph rectangle.
QList<Ink> components(const QImage &source, bool light = false) {
    const auto image = source.convertToFormat(QImage::Format_RGB32);
    const int w = image.width(), h = image.height();
    QByteArray visited(w * h, 0); QList<Ink> output; QList<int> queue;
    auto dark = [&](int x, int y) { const auto c = reinterpret_cast<const QRgb *>(image.constScanLine(y))[x]; return light ? qGray(c) > 210 : qGray(c) < 210; };
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const int start = y * w + x;
        if (visited[start] || !dark(x, y)) continue;
        visited[start] = 1; queue.clear(); queue.append(start);
        int left = x, right = x, top = y, bottom = y, colored = 0;
        for (qsizetype i = 0; i < queue.size(); ++i) {
            const int px = queue[i] % w, py = queue[i] / w;
            left = qMin(left, px); right = qMax(right, px); top = qMin(top, py); bottom = qMax(bottom, py);
            const QRgb pixel = reinterpret_cast<const QRgb *>(image.constScanLine(py))[px];
            if (qMax(qRed(pixel), qMax(qGreen(pixel), qBlue(pixel))) - qMin(qRed(pixel), qMin(qGreen(pixel), qBlue(pixel))) > 35) ++colored;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const int nx = px + dx, ny = py + dy;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                const int next = ny * w + nx;
                if (!visited[next] && dark(nx, ny)) { visited[next] = 1; queue.append(next); }
            }
        }
        output.append({QRect(QPoint(left, top), QPoint(right, bottom)), int(queue.size()), colored > queue.size() / 4});
    }
    return output;
}
bool letter(const Ink &ink, const QRectF &line) {
    return line.adjusted(-2, -2, 2, 2).contains(ink.rect) && ink.rect.height() <= line.height() * 1.15
        && ink.rect.width() <= line.height() * 4 && !(ink.rect.width() > ink.rect.height() * 12 && ink.rect.width() > line.height() * 2);
}
struct Row { QRectF rect; QList<QRectF> masks; bool bullet = false; double weight = 0; QJsonObject layout; QList<double> inkHeights; double estimatedHeight = 0; };
}

QJsonArray PdfScanLayout::detect(const QImage &image, const QSizeF &page, int pageIndex, QJsonObject *layout) {
    const auto runtime = QDir(QCoreApplication::applicationDirPath()).filePath("ocr");
#ifdef Q_OS_WIN
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto cookie = AddDllDirectory(reinterpret_cast<LPCWSTR>(runtime.utf16()));
#endif
    QLibrary library(QDir(runtime).filePath("YiyiOcr.dll"));
    using Detect = int (*)(const unsigned char *, int, char *, int);
    const auto detect = reinterpret_cast<Detect>(library.resolve("YiyiDetect"));
    if (!detect) throw QStringLiteral("缺少文字区域检测组件，请使用完整的 0.7.0 发行包。");
    QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG");
    QByteArray bytes(1024 * 1024, 0); const auto cwd = QDir::currentPath();
    if (!QDir::setCurrent(runtime)) throw QStringLiteral("无法打开文字检测组件目录。");
    const int count = detect(reinterpret_cast<const unsigned char *>(png.constData()), png.size(), bytes.data(), bytes.size());
    QDir::setCurrent(cwd);
#ifdef Q_OS_WIN
    if (cookie) RemoveDllDirectory(cookie);
#endif
    if (count < 0) throw QStringLiteral("文字区域检测失败，请重试或框选识别。");
    const auto detected = QJsonDocument::fromJson(bytes.left(count)).array();
    if (detected.size() > 1500) throw QStringLiteral("本页文字区域过多，请拆分或裁切页面。");
    const double sx = page.width() / image.width(), sy = page.height() / image.height();
    auto points = [&](const QRectF &r) { return QRectF(r.x() * sx, r.y() * sy, r.width() * sx, r.height() * sy); };
    const auto tables = PdfLayout::ruledRegions(PdfLayout::imageRules(image, page), page, QStringLiteral("scan-p%1").arg(pageIndex));
    const auto ink = components(image); QList<Ink> lightInk; bool haveLightInk = false; QList<Row> lines; QList<QRectF> obstacles;
    for (const auto &item : ink) if (item.rect.width() > image.width() * .12 && item.rect.width() > item.rect.height() * 12)
        obstacles.append(item.rect);
    for (const auto &value : detected) {
        const auto p = value.toArray(); if (p.size() != 8) continue;
        QRectF box(QPointF(p[0].toDouble(), p[1].toDouble()), QSizeF(1, 1));
        for (int i = 2; i < 8; i += 2) box = box.united(QRectF(p[i].toDouble(), p[i + 1].toDouble(), 1, 1));
        box = box.intersected(image.rect());
        if (box.height() < 5 || box.width() < 3) continue;
        // Vertical/rotated text is outside the horizontal book-page scope.
        if (std::abs(p[3].toDouble() - p[1].toDouble()) > qMax(5.0, box.width() * .12)) continue;
        QList<QRectF> pieces;
        for (const auto &v : tables) if (v.toObject()["kind"] == "cell") {
            const auto cell = PdfLayout::rect(v.toObject()["rect"]); const QRectF pixels(cell.x() / sx, cell.y() / sy, cell.width() / sx, cell.height() / sy);
            const auto part = box.intersected(pixels.adjusted(2, 2, -2, -2));
            if (part.height() > box.height() * .6 && part.width() > 3) pieces.append(part);
        }
        if (pieces.isEmpty()) pieces.append(box);
        for (const auto &piece : pieces) {
            QRectF tight;
            for (const auto &item : ink) if (item.pixels >= qMax(5, int(piece.height() * .15))
                && !(item.pixels > item.rect.width() * item.rect.height() * .85 && item.rect.width() > item.rect.height() * .6)
                && letter(item, piece)) tight = tight.isEmpty() ? item.rect : tight.united(item.rect);
            const auto bg = PdfBackground::describe(image, piece.toAlignedRect());
            // Nearby table rules can skew the border-ring fit. When the actual
            // detector box is mostly white paper, keep measured glyph height;
            // a padded detector box must not turn a short cell into a heading.
            const auto sampleBox = piece.toAlignedRect().intersected(image.rect()); int paperPixels = 0, samples = 0;
            for (int y = sampleBox.top(); y <= sampleBox.bottom(); y += 2) for (int x = sampleBox.left(); x <= sampleBox.right(); x += 2) {
                const auto pixel = image.pixel(x, y); ++samples;
                if (qRed(pixel) >= 225 && qGreen(pixel) >= 225 && qBlue(pixel) >= 225) ++paperPixels;
            }
            const auto bgColor = bg["color"].toArray();
            const bool complexBackground = paperPixels < samples * .6 && (bg["kind"] != "solid"
                || qMin(bgColor[0].toInt(), qMin(bgColor[1].toInt(), bgColor[2].toInt())) < 220);
            auto mask = piece;
            if (complexBackground && bg["kind"] == "solid" && bg["textColor"].toArray()[0].toInt() == 255) {
                if (!haveLightInk) { lightInk = components(image, true); haveLightInk = true; }
                // Widely spaced white lettering is sometimes detected only in
                // the middle. Extend through aligned glyphs on a solid band.
                bool expanded = true;
                while (expanded) {
                    expanded = false;
                    for (const auto &item : lightInk) {
                        const auto r = QRectF(item.rect);
                        if (item.pixels < 5 || r.height() < piece.height() * .25 || r.height() > piece.height() * 1.15
                            || r.width() > piece.height() * 2 || r.top() < piece.top() - 2 || r.bottom() > piece.bottom() + 2) continue;
                        if (qMax(r.left() - mask.right(), mask.left() - r.right()) > piece.height() * 2) continue;
                        const auto next = mask.united(r); if (next != mask) { mask = next; expanded = true; }
                    }
                }
            }
            if (tight.isEmpty() || complexBackground) {
                // Dark/photo backgrounds join into large connected components.
                // The detector's quadrilateral still locates the text; keep it
                // for AI transcription instead of silently dropping white text.
                if (complexBackground) tight = mask.adjusted(1, 1, -1, -1);
            }
            if (!tight.isEmpty()) { Row row{tight, {mask.united(tight)}}; if (complexBackground) row.estimatedHeight = tight.height() * .85; lines.append(row); }
        }
    }
    std::sort(lines.begin(), lines.end(), [](const Row &a, const Row &b) { return a.rect.center().y() < b.rect.center().y(); });
    QList<Row> rows;
    for (const auto &line : lines) {
        bool merged = false;
        for (int i = rows.size() - 1; i >= 0; --i) {
            auto &row = rows[i];
            if (line.rect.top() - row.rect.bottom() > line.rect.height()) break;
            if (std::abs(line.rect.center().y() - row.rect.center().y()) < qMax(line.rect.height(), row.rect.height()) * .45
                && qMax(line.rect.left() - row.rect.right(), row.rect.left() - line.rect.right()) < line.rect.height() * 3
                && PdfLayout::cellAt(tables, points(line.rect)) == PdfLayout::cellAt(tables, points(row.rect))) {
                row.rect = row.rect.united(line.rect); row.masks.append(line.masks); merged = true; break;
            }
        }
        if (!merged) rows.append(line);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.rect.top() < b.rect.top(); });
    for (auto &row : rows) for (const auto &item : ink) {
        const auto &r = item.rect;
        if (r.height() >= row.rect.height() * .15 && r.height() <= row.rect.height() * .55
            && r.width() >= r.height() * .6 && r.width() <= r.height() * 1.5
            && r.right() < row.rect.left() && row.rect.left() - r.right() < row.rect.height() * 3
            && std::abs(r.center().y() - row.rect.center().y()) < row.rect.height() * .35) row.bullet = true;
    }
    QList<Row> paragraphs; QList<double> heights, weights;
    double left = image.width(), right = 0;
    for (auto &row : rows) {
        row.inkHeights = {row.estimatedHeight > 0 ? row.estimatedHeight : row.rect.height()};
        const auto box = row.rect.toAlignedRect().intersected(image.rect()); int darkPixels = 0;
        for (int y = box.top(); y <= box.bottom(); ++y) for (int x = box.left(); x <= box.right(); ++x) if (qGray(image.pixel(x, y)) < 180) ++darkPixels;
        row.weight = double(darkPixels) / qMax(1, box.width()); weights.append(row.weight);
        heights.append(row.rect.height()); left = qMin(left, row.rect.left()); right = qMax(right, row.rect.right());
    }
    std::sort(heights.begin(), heights.end()); const double bodyHeight = heights.isEmpty() ? 20 : heights[heights.size() / 2];
    std::sort(weights.begin(), weights.end()); const double bodyWeight = weights.isEmpty() ? 5 : weights[weights.size() / 2];
    QJsonArray rowItems;
    for (const auto &row : rows) rowItems.append(QJsonObject{{"rect", rectJson(points(row.rect))}});
    const auto analysis = PdfLayout::analyze(rowItems, tables, page, QStringLiteral("scan-p%1").arg(pageIndex));
    QList<Row> ordered;
    for (const auto &v : analysis.items) { auto row = rows[v.toObject()["inputIndex"].toInt()]; row.layout = v.toObject(); ordered.append(row); }
    rows = ordered;
    double previousHeight = 0, previousWeight = 0;
    for (const auto &row : rows) {
        bool join = false;
        if (!paragraphs.isEmpty()) {
            const auto &last = paragraphs.last(); const double gap = row.rect.top() - last.rect.bottom();
            join = !row.bullet && gap > -row.rect.height() * .25 && gap < qMin(previousHeight, row.rect.height()) * .9
                && std::abs(previousWeight - row.weight) < qMin(previousWeight, row.weight) * .3
                && std::abs(last.rect.left() - row.rect.left()) < bodyHeight * 1.5;
            join = join && last.layout["container"] == row.layout["container"];
            if (last.layout["role"] == "cell" && last.layout["container"] == row.layout["container"]) join = true;
            for (const auto &o : obstacles) if (o.top() >= last.rect.bottom() && o.bottom() <= row.rect.top()) join = false;
        }
        if (join) { paragraphs.last().rect = paragraphs.last().rect.united(row.rect); paragraphs.last().masks.append(row.masks); paragraphs.last().inkHeights.append(row.inkHeights); }
        else paragraphs.append(row);
        previousHeight = row.rect.height();
        previousWeight = row.weight;
    }
    QJsonArray result;
    for (int i = 0; i < paragraphs.size(); ++i) {
        const auto &para = paragraphs[i]; const auto rect = points(para.rect);
        const auto container = PdfLayout::rect(para.layout["containerRect"]);
        auto lineHeights = para.inkHeights;
        std::sort(lineHeights.begin(), lineHeights.end()); const double lineHeight = lineHeights[lineHeights.size() / 2];
        const bool title = lineHeight > bodyHeight * 1.25 || (para.layout["role"] != "cell" && para.masks.size() == 1 && para.weight > bodyWeight * 1.35);
        const bool centered = title && para.masks.size() == 1 && para.rect.width() < (right - left) * .8
            && std::abs(rect.center().x() - container.center().x()) < bodyHeight * sx * 1.5;
        bool artwork = false;
        if (para.masks.size() == 1 && lineHeight > bodyHeight * 1.5) for (const auto &item : ink) {
            const auto box = item.rect;
            if (item.colored && item.pixels > 100 && box.height() >= lineHeight * 1.1 && box.height() < lineHeight * 3.5
                && box.right() <= para.rect.left() + lineHeight && para.rect.left() - box.right() < lineHeight * 2.5
                && box.top() < para.rect.bottom() && box.bottom() > para.rect.top()) artwork = true;
        }
        QRectF region(centered ? left * sx : rect.left(), rect.top(), centered ? (right - left) * sx : right * sx - rect.left(), rect.height());
        double bottom = qMin(double(image.height() - 20), para.rect.bottom() + lineHeight * 3);
        for (int j = i + 1; j < paragraphs.size(); ++j) if (paragraphs[j].rect.top() >= para.rect.bottom()) bottom = qMin(bottom, paragraphs[j].rect.top() - bodyHeight * .3);
        for (const auto &o : obstacles) if (o.top() >= para.rect.bottom() && o.right() > para.rect.left() && o.left() < right) bottom = qMin(bottom, o.top() - bodyHeight * .3);
        region.setBottom(qMax(rect.bottom(), bottom * sy));
        for (const auto &graphic : ink) {
            const auto box = graphic.rect;
            if (box.width() < image.width() * .08 || box.height() < image.height() * .025 || graphic.pixels < box.width() * box.height() * .3
                || (!graphic.colored && graphic.pixels < box.width() * box.height() * .7) || box.intersects(para.rect.toAlignedRect())) continue;
            const auto obstacle = points(box);
            if (obstacle.top() < rect.bottom() && obstacle.bottom() > rect.top() && obstacle.left() > rect.right()) region.setRight(qMin(region.right(), obstacle.left() - 3));
            if (obstacle.top() >= rect.bottom() && obstacle.left() < region.right() && obstacle.right() > region.left()) region.setBottom(qMin(region.bottom(), obstacle.top() - 3));
        }
        if (!container.isEmpty()) {
            const bool cell = para.layout["role"] == "cell"; const auto limit = cell ? container.adjusted(2, 2, -2, -2) : container;
            region = region.intersected(limit);
            if (cell) { region.setTop(qMax(limit.top(), rect.top() - 1)); region.setBottom(limit.bottom()); }
        }
        QJsonArray masks; for (const auto &r : para.masks) masks.append(rectJson(points(r.adjusted(-1, -1, 1, 1))));
        QJsonObject item{{"id", QStringLiteral("scan-p%1-b%2").arg(pageIndex).arg(i)}, {"rect", rectJson(rect)}, {"region", rectJson(region)},
            {"masks", masks}, {"size", qBound(8.0, (title || para.estimatedHeight > 0 ? lineHeight : qMax(lineHeight, bodyHeight * .9)) * sy, 50.0)}, {"bold", title}, {"centered", centered},
            {"serif", !title}, {"color", QJsonArray{0, 0, 0}}, {"scan", true}, {"artwork", artwork}};
        for (const auto &key : {"container", "containerRect", "role", "table", "row", "column", "rowSpan", "columnSpan", "order"})
            if (para.layout.contains(key)) item[key] = para.layout[key];
        const auto background = PdfBackground::describe(image, para.rect.toAlignedRect()); item["background"] = background;
        item["color"] = background["textColor"];
        result.append(item);
    }
    if (layout) *layout = PdfLayout::structure(result, analysis.regions);
    return result;
}

QImage PdfScanLayout::eraseText(const QImage &image, const QSizeF &page, const QJsonArray &blocks, QJsonArray *failures) {
    auto cleaned = image.convertToFormat(QImage::Format_RGB32); const auto ink = components(image);
    const double sx = image.width() / page.width(), sy = image.height() / page.height();
    const auto rules = PdfLayout::imageRules(image, page);
    const auto grid = PdfLayout::ruledRegions(rules, page, "erase");
    for (const auto &value : blocks) {
        const auto block = value.toObject(); const auto mode = block["backgroundMode"].toString("auto");
        auto pixels = [&](const QJsonValue &v) { const auto r = rectValue(v); return QRectF(r.x() * sx, r.y() * sy, r.width() * sx, r.height() * sy).toAlignedRect().intersected(image.rect()); };
        QRect allowed = image.rect();
        if (block["role"] == "cell" && !rectValue(block["containerRect"]).isEmpty()) {
            // Detector masks and cleaning halos can touch a faint scanned
            // grid that imageRules does not recover completely. The known
            // cell boundaries constrain every cleaning mode, including panels.
            const auto cell = pixels(block["containerRect"]);
            const int dx = qMin(qMax(2, qCeil(sx * 1.25)), qMax(0, (cell.width() - 3) / 2));
            const int dy = qMin(qMax(2, qCeil(sy * 1.25)), qMax(0, (cell.height() - 3) / 2));
            allowed = cell.adjusted(dx, dy, -dx, -dy).intersected(image.rect());
        }
        if (mode == "original") continue;
        if (mode == "panel") {
            QColor color(block["panelColor"].toString("#ffffff")); if (!color.isValid()) color = Qt::white; color.setAlpha(255);
            QPainter painter(&cleaned); painter.setClipRect(allowed); painter.fillRect(pixels(block["rect"]).adjusted(-2, -2, 2, 2), color); painter.fillRect(pixels(block["region"]), color); continue;
        }
        const auto background = PdfBackground::describe(image, pixels(block["rect"])); const auto bgColor = background["color"].toArray();
        if (mode == "repair" || background["kind"] != "solid" || bgColor[0].toInt() < 225 || bgColor[1].toInt() < 225 || bgColor[2].toInt() < 225
            || std::abs(bgColor[0].toInt() - bgColor[2].toInt()) > 5) {
            QList<QRect> masks; for (const auto &mask : block["masks"].toArray()) masks.append(pixels(mask).adjusted(-2, -2, 2, 2));
            QString error;
            if (!allowed.isEmpty() && !PdfBackground::erase(cleaned, image, masks, mode, &error, allowed)) {
                if (failures) failures->append(QJsonObject{{"id", block["id"]}, {"error", error}}); else throw error;
            }
            continue;
        }
        const auto before = cleaned;
        bool failed = false;
        for (const auto &mask : block["masks"].toArray()) {
        const auto r = rectValue(mask); const QRectF line(r.x() * sx, r.y() * sy, r.width() * sx, r.height() * sy);
        const auto area = line.adjusted(-6, -6, 6, 6).toAlignedRect().intersected(allowed);
        if (area.isEmpty()) continue;
        // Clean a narrow line mask, including antialiasing/compression halos.
        // Preserve rules and larger artwork components even when a detector's
        // padded rectangle touches them. A whole paragraph is never blanked.
        QList<QRect> protectedInk;
        for (const auto &rule : rules) {
            bool gridRule = false;
            for (const auto &v : grid) if (v.toObject()["kind"] == "table") {
                const auto table = PdfLayout::rect(v.toObject()["rect"]);
                if (!table.adjusted(-2, -2, 2, 2).contains(rule.p1()) || !table.adjusted(-2, -2, 2, 2).contains(rule.p2())) continue;
                const auto edges = v.toObject()[std::abs(rule.dx()) < 1 ? "xEdges" : "yEdges"].toArray();
                const double at = std::abs(rule.dx()) < 1 ? rule.x1() : rule.y1();
                for (const auto &edge : edges) if (std::abs(edge.toDouble() - at) < 2) gridRule = true;
            }
            if (!gridRule) continue;
            const QRectF box(QPointF(rule.x1() * sx, rule.y1() * sy), QPointF(rule.x2() * sx, rule.y2() * sy));
            if (box.normalized().adjusted(-2, -2, 2, 2).intersects(area)) protectedInk.append(box.normalized().adjusted(-2, -2, 2, 2).toAlignedRect());
        }
        for (const auto &item : ink) if (item.rect.intersects(area)) {
            const auto box = item.rect;
            // A connected grid encloses all its cells. Protect its actual rules,
            // not its bounding rectangle, which would retain the source words.
            if (box.width() > line.width() + 8 && box.height() > line.height() * 2
                && item.pixels < box.width() * box.height() * .25) continue;
            if ((item.pixels > 8 && !line.adjusted(-2, -2, 2, 2).contains(box.center()))
                || box.height() > line.height() * 1.5 || box.width() > line.width() + 8
                || (box.width() > box.height() * 12 && box.width() > line.height() * 2)
                || (item.colored && item.pixels > box.width() * box.height() * .65 && box.width() > line.height() * 2 && box.height() > line.height() * .9))
                protectedInk.append(box.adjusted(-2, -2, 2, 2));
        }
        QList<int> samples;
        for (int x = area.left(); x <= area.right(); ++x) {
            samples.append(qGray(image.pixel(x, area.top()))); samples.append(qGray(image.pixel(x, area.bottom())));
        }
        std::sort(samples.begin(), samples.end()); const int bg = samples.isEmpty() ? 255 : samples[samples.size() * 9 / 10];
        if (bg < 225) {
            const auto error = QStringLiteral("文字区域背景复杂，请选择背景修复、底色框或保留原图对照。");
            if (failures) failures->append(QJsonObject{{"id", block["id"]}, {"error", error}}); else throw error;
            failed = true; break;
        }
        for (int y = area.top(); y <= area.bottom(); ++y) {
            auto *row = reinterpret_cast<QRgb *>(cleaned.scanLine(y));
            for (int x = area.left(); x <= area.right(); ++x) {
                bool preserve = false; for (const auto &box : protectedInk) if (box.contains(x, y)) { preserve = true; break; }
                if (!preserve) row[x] = qRgb(bg, bg, bg);
            }
        }
        }
        if (failed) cleaned = before;
    }
    return cleaned;
}
