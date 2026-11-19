#include "pdflayout.h"
#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

namespace PdfLayout {
QRectF rect(const QJsonValue &value) {
    const auto a = value.toArray();
    return a.size() == 4 ? QRectF(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()) : QRectF();
}
QJsonArray jsonRect(const QRectF &r) { return {r.x(), r.y(), r.width(), r.height()}; }
namespace {
struct Segment { bool horizontal; double at, start, end; };
struct Sets {
    QList<int> ids;
    explicit Sets(int count) { for (int i = 0; i < count; ++i) ids.append(i); }
    int root(int i) { while (i != ids[i]) { ids[i] = ids[ids[i]]; i = ids[i]; } return i; }
    void unite(int a, int b) { ids[root(a)] = root(b); }
};
QList<double> coordinates(QList<double> values) {
    std::sort(values.begin(), values.end()); QList<double> result; QList<int> counts;
    for (double v : values) {
        if (result.isEmpty() || v - result.last() > 2) { result.append(v); counts.append(1); }
        else { result.last() = (result.last() * counts.last() + v) / (counts.last() + 1); ++counts.last(); }
    }
    return result;
}
bool wall(const QList<Segment> &segments, bool horizontal, double at, double from, double to) {
    QList<QPair<double, double>> spans;
    for (const auto &s : segments) if (s.horizontal == horizontal && std::abs(s.at - at) <= 2)
        if (s.end > from && s.start < to) spans.append({qMax(from, s.start), qMin(to, s.end)});
    std::sort(spans.begin(), spans.end()); double end = from, covered = 0;
    for (const auto &s : spans) if (s.second > end) { covered += s.second - qMax(end, s.first); end = s.second; }
    return covered >= (to - from) * .88;
}
double medianHeight(const QJsonArray &items) {
    QList<double> heights;
    for (const auto &v : items) if (rect(v.toObject()["rect"]).height() > 0) heights.append(rect(v.toObject()["rect"]).height());
    if (heights.isEmpty()) return 12;
    std::sort(heights.begin(), heights.end()); return qBound(5.0, heights[heights.size() / 2], 32.0);
}
}

QList<QLineF> imageRules(const QImage &source, const QSizeF &page) {
    const auto image = source.convertToFormat(QImage::Format_RGB32); QList<QLineF> result;
    if (image.isNull()) return result;
    // Long, thin runs survive this detector; photographs and filled panels do
    // not become grid edges merely because they contain dark pixels.
    for (bool horizontal : {true, false}) {
        const int along = horizontal ? image.width() : image.height(), across = horizontal ? image.height() : image.width();
        const double alongScale = (horizontal ? page.width() : page.height()) / along;
        const double acrossScale = (horizontal ? page.height() : page.width()) / across;
        struct Run { int from, to, first, last; };
        QList<Run> active, finished;
        const int minimum = qMax(20, qRound(18 / alongScale));
        for (int y = 0; y < across; ++y) {
            QList<QPair<int, int>> runs;
            int start = -1, last = -1;
            for (int x = 0; x <= along; ++x) {
                const bool dark = x < along && qGray(horizontal ? image.pixel(x, y) : image.pixel(y, x)) < 195;
                if (dark) { if (start < 0) start = x; last = x; }
                if (start >= 0 && (x == along || x - last > 2)) {
                    if (last - start + 1 >= minimum) runs.append({start, last});
                    start = -1;
                }
            }
            QList<Run> next; QSet<int> used;
            for (const auto &r : runs) {
                int match = -1;
                for (int i = 0; i < active.size(); ++i) if (!used.contains(i)
                    && std::abs(active[i].from - r.first) < 5 && std::abs(active[i].to - r.second) < 5) { match = i; break; }
                if (match >= 0) { auto row = active[match]; row.last = y; next.append(row); used.insert(match); }
                else next.append({r.first, r.second, y, y});
            }
            for (int i = 0; i < active.size(); ++i) if (!used.contains(i)) finished.append(active[i]);
            active = next;
            if (finished.size() > 20000) return {}; // Dense artwork, not a useful grid.
        }
        finished.append(active);
        for (const auto &r : finished) if ((r.last - r.first + 1) * acrossScale <= 3.5) {
            const double at = (r.first + r.last) * .5 * acrossScale;
            result.append(horizontal ? QLineF(r.from * alongScale, at, r.to * alongScale, at)
                                     : QLineF(at, r.from * alongScale, at, r.to * alongScale));
        }
    }
    return result;
}

QJsonArray ruledRegions(const QList<QLineF> &rules, const QSizeF &page, const QString &prefix) {
    QList<Segment> segments;
    for (const auto &line : rules) {
        if (!std::isfinite(line.x1()) || !std::isfinite(line.y1()) || !std::isfinite(line.x2()) || !std::isfinite(line.y2())) continue;
        if (std::abs(line.dy()) < 1 && std::abs(line.dx()) > 8)
            segments.append({true, (line.y1() + line.y2()) / 2, qMin(line.x1(), line.x2()), qMax(line.x1(), line.x2())});
        else if (std::abs(line.dx()) < 1 && std::abs(line.dy()) > 8)
            segments.append({false, (line.x1() + line.x2()) / 2, qMin(line.y1(), line.y2()), qMax(line.y1(), line.y2())});
    }
    if (segments.size() > 3000) return {};
    Sets connected(segments.size());
    for (int i = 0; i < segments.size(); ++i) for (int j = i + 1; j < segments.size(); ++j) {
        const auto a = segments[i], b = segments[j];
        if (a.horizontal == b.horizontal) {
            if (std::abs(a.at - b.at) <= 2 && a.start <= b.end + 2 && a.end >= b.start - 2) connected.unite(i, j);
        } else {
            if (a.at >= b.start - 2 && a.at <= b.end + 2 && b.at >= a.start - 2 && b.at <= a.end + 2) connected.unite(i, j);
        }
    }
    QMap<int, QList<Segment>> groups;
    for (int i = 0; i < segments.size(); ++i) groups[connected.root(i)].append(segments[i]);
    QJsonArray regions; int tableIndex = 0;
    for (const auto &group : groups) {
        QList<double> xs, ys;
        for (const auto &s : group) (s.horizontal ? ys : xs).append(s.at);
        xs = coordinates(xs); ys = coordinates(ys);
        const int cols = xs.size() - 1, rows = ys.size() - 1;
        if (cols < 2 || rows < 2 || cols > 60 || rows > 150 || rows * cols > 3000) continue;
        const QRectF bounds(xs.first(), ys.first(), xs.last() - xs.first(), ys.last() - ys.first());
        if (!QRectF(QPointF(0, 0), page).adjusted(-1, -1, 1, 1).contains(bounds)) continue;
        // Closed perimeter prevents charts/underlines from being mistaken for tables.
        if (!wall(group, true, ys.first(), xs.first(), xs.last()) || !wall(group, true, ys.last(), xs.first(), xs.last())
            || !wall(group, false, xs.first(), ys.first(), ys.last()) || !wall(group, false, xs.last(), ys.first(), ys.last())) continue;
        Sets cells(cols * rows);
        for (int r = 0; r < rows; ++r) for (int c = 0; c < cols; ++c) {
            if (c + 1 < cols && !wall(group, false, xs[c + 1], ys[r], ys[r + 1])) cells.unite(r * cols + c, r * cols + c + 1);
            if (r + 1 < rows && !wall(group, true, ys[r + 1], xs[c], xs[c + 1])) cells.unite(r * cols + c, (r + 1) * cols + c);
        }
        QMap<int, QList<int>> merged;
        for (int i = 0; i < cols * rows; ++i) merged[cells.root(i)].append(i);
        if (merged.size() < 3) continue;
        const auto tableId = prefix + QStringLiteral("-table%1").arg(tableIndex++); QJsonArray tableCells; bool valid = true;
        for (const auto &members : merged) {
            int r0 = rows, r1 = 0, c0 = cols, c1 = 0;
            for (int i : members) { r0 = qMin(r0, i / cols); r1 = qMax(r1, i / cols); c0 = qMin(c0, i % cols); c1 = qMax(c1, i % cols); }
            if (members.size() != (r1 - r0 + 1) * (c1 - c0 + 1)) { valid = false; break; }
            const QRectF box(xs[c0], ys[r0], xs[c1 + 1] - xs[c0], ys[r1 + 1] - ys[r0]);
            if (box.width() < 8 || box.height() < 8) { valid = false; break; }
            tableCells.append(QJsonObject{{"id", tableId + QStringLiteral("-r%1c%2").arg(r0).arg(c0)}, {"parent", tableId}, {"kind", "cell"},
                {"rect", jsonRect(box)}, {"row", r0}, {"column", c0}, {"rowSpan", r1 - r0 + 1}, {"columnSpan", c1 - c0 + 1}});
        }
        if (!valid) continue;
        QJsonArray xValues, yValues; for (double x : xs) xValues.append(x); for (double y : ys) yValues.append(y);
        regions.append(QJsonObject{{"id", tableId}, {"kind", "table"}, {"rect", jsonRect(bounds)}, {"rows", rows}, {"columns", cols},
            {"xEdges", xValues}, {"yEdges", yValues}, {"headerRows", 1}, {"inferred", false}});
        for (const auto &cell : tableCells) regions.append(cell);
    }
    return regions;
}

QString cellAt(const QJsonArray &regions, const QRectF &text) {
    for (const auto &v : regions) {
        const auto r = v.toObject(); const auto box = rect(r["rect"]);
        if (r["kind"] == "cell" && box.contains(text.center()) && box.adjusted(-1, -1, 1, 1).contains(text)) return r["id"].toString();
    }
    return {};
}

namespace {
bool numericCell(const QString &text) {
    static const QRegularExpression pattern(QStringLiteral("^[+−-]?[\\d.,]+(?:\\s*[%％°µμ\\p{L}/²³]{0,8})?$"));
    return pattern.match(text.trimmed()).hasMatch();
}
QJsonArray inferredTables(const QJsonArray &items, const QJsonArray &known, double h, const QString &prefix) {
    QList<QJsonObject> sorted;
    for (const auto &v : items) if (cellAt(known, rect(v.toObject()["rect"])).isEmpty()) sorted.append(v.toObject());
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return rect(a["rect"]).center().y() < rect(b["rect"]).center().y(); });
    QList<QList<QJsonObject>> rows;
    for (const auto &item : sorted) {
        if (rows.isEmpty() || std::abs(rect(rows.last().first()["rect"]).center().y() - rect(item["rect"]).center().y()) > h * .7) rows.append(QList<QJsonObject>{});
        rows.last().append(item);
    }
    for (auto &row : rows) std::sort(row.begin(), row.end(), [](const auto &a, const auto &b) { return rect(a["rect"]).left() < rect(b["rect"]).left(); });
    QJsonArray output; int tableIndex = 0;
    for (int first = 0; first < rows.size(); ++first) {
        const int cols = rows[first].size(); if (cols < 2 || cols > 30) continue;
        int last = first;
        for (int r = first + 1; r < rows.size(); ++r) {
            if (rows[r].size() != cols || rect(rows[r].first()["rect"]).top() - rect(rows[r - 1].first()["rect"]).bottom() > h * 5) break;
            bool aligned = true;
            for (int c = 0; c < cols; ++c) {
                const auto a = rect(rows[first][c]["rect"]), b = rect(rows[r][c]["rect"]);
                if (std::abs(a.left() - b.left()) > h && std::abs(a.right() - b.right()) > h) aligned = false;
            }
            if (!aligned) break;
            last = r;
        }
        if (last - first + 1 < 3) continue;
        int compact = 0, numeric = 0, total = 0, headerBold = 0; bool hasText = false;
        QList<double> left(cols, 1e9), right(cols, -1e9), top, bottom;
        for (int r = first; r <= last; ++r) {
            double t = 1e9, b = -1e9;
            for (int c = 0; c < cols; ++c) {
                const auto &item = rows[r][c]; const auto box = rect(item["rect"]); const auto text = item["source"].toString().trimmed();
                left[c] = qMin(left[c], box.left()); right[c] = qMax(right[c], box.right()); t = qMin(t, box.top()); b = qMax(b, box.bottom());
                if (!text.isEmpty()) hasText = true;
                const int words = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size();
                if (text.isEmpty() ? box.width() <= h * 10 : (words <= 3 && text.size() <= 30)) ++compact;
                if (numericCell(text)) ++numeric;
                if (r == first && item["bold"].toBool()) ++headerBold;
                ++total;
            }
            top.append(t); bottom.append(b);
        }
        bool spaced = true;
        for (int c = 1; c < cols; ++c) if (left[c] - right[c - 1] < h * 1.2) spaced = false;
        // Repeated aligned prose is a column, not a borderless table. Require
        // compact cells and additional heading/data evidence when text exists.
        if (!spaced || compact < total * .65 || (hasText && numeric < 2 && headerBold < cols * .5 && !(cols >= 3 && compact >= total * .9))
            || (!hasText && (cols < 3 && last - first + 1 < 4))) continue;
        QList<double> xs{qMax(0.0, left.first() - 3)}, ys{qMax(0.0, top.first() - h * .6)};
        for (int c = 1; c < cols; ++c) xs.append((right[c - 1] + left[c]) / 2);
        xs.append(right.last() + h);
        for (int r = 1; r < top.size(); ++r) ys.append((bottom[r - 1] + top[r]) / 2);
        ys.append(bottom.last() + h * .6);
        const auto id = prefix + QStringLiteral("-inferred%1").arg(tableIndex++); QJsonArray xe, ye;
        for (double x : xs) xe.append(x); for (double y : ys) ye.append(y);
        output.append(QJsonObject{{"id", id}, {"kind", "table"}, {"rect", jsonRect(QRectF(xs.first(), ys.first(), xs.last() - xs.first(), ys.last() - ys.first()))},
            {"rows", last - first + 1}, {"columns", cols}, {"headerRows", 1}, {"inferred", true}, {"xEdges", xe}, {"yEdges", ye}});
        for (int r = 0; r <= last - first; ++r) for (int c = 0; c < cols; ++c) output.append(QJsonObject{
            {"id", id + QStringLiteral("-r%1c%2").arg(r).arg(c)}, {"parent", id}, {"kind", "cell"}, {"row", r}, {"column", c}, {"rowSpan", 1}, {"columnSpan", 1},
            {"rect", jsonRect(QRectF(xs[c], ys[r], xs[c + 1] - xs[c], ys[r + 1] - ys[r]))}});
        first = last;
    }
    return output;
}
}

Analysis analyze(const QJsonArray &input, const QJsonArray &tables, const QSizeF &page, const QString &prefix) {
    Analysis result; result.regions = tables;
    if (input.isEmpty()) return result;
    // Alphabetical indexes are independent lists, not borderless tables.
    // Repeated page references must remain attached to their physical entry.
    int references = 0, alphabeticHeadings = 0; QList<double> starts;
    static const QRegularExpression reference(QStringLiteral("[A-Za-z].*\\b\\d+(?:[ ,–-]+\\d+)*\\s*$"));
    static const QRegularExpression heading(QStringLiteral("^[A-Z]$"));
    static const QRegularExpression footer(QStringLiteral("^(?:\\[\\s*\\d+\\s*\\]|(?:https?://|www\\.)\\S+)$"));
    for (const auto &v : input) { const auto b = v.toObject(); const auto text = b["source"].toString().trimmed();
        if (reference.match(text).hasMatch()) ++references;
        if (heading.match(text).hasMatch()) ++alphabeticHeadings;
        if (!footer.match(text).hasMatch()) starts.append(rect(b["rect"]).left());
    }
    if (tables.isEmpty() && references >= 16 && references > input.size() * .5 && alphabeticHeadings >= 2) {
        std::sort(starts.begin(), starts.end()); double gap = 0, split = 0;
        for (int i = 3; i + 3 <= starts.size(); ++i) if (starts[i] - starts[i - 1] > gap) { gap = starts[i] - starts[i - 1]; split = (starts[i] + starts[i - 1]) / 2; }
        if (gap > page.width() * .12) {
            QList<int> groups[3]; QRectF bounds[3];
            for (int i = 0; i < input.size(); ++i) { const auto b = input[i].toObject(); const auto box = rect(b["rect"]);
                const int group = footer.match(b["source"].toString().trimmed()).hasMatch() ? 2 : box.left() < split ? 0 : 1;
                groups[group].append(i); bounds[group] = bounds[group].isEmpty() ? box : bounds[group].united(box);
            }
            int order = 0;
            for (int group = 0; group < 3; ++group) {
                if (groups[group].isEmpty()) continue;
                auto area = bounds[group]; if (group == 0) area.setRight(qMax(area.right(), bounds[1].left() - 4));
                area.setBottom(qMin(page.height(), area.bottom() + 2));
                const auto id = prefix + QStringLiteral("-index%1").arg(group);
                result.regions.append(QJsonObject{{"id", id}, {"kind", group < 2 ? "column" : "flow"}, {"rect", jsonRect(area)}});
                std::stable_sort(groups[group].begin(), groups[group].end(), [&](int a, int b) { return rect(input[a].toObject()["rect"]).top() < rect(input[b].toObject()["rect"]).top(); });
                for (int i : groups[group]) { auto item = input[i].toObject(); item["inputIndex"] = i; item["order"] = order++;
                    item["container"] = id; item["containerRect"] = jsonRect(area); item["role"] = group < 2 ? "index" : "paragraph"; result.items.append(item);
                }
            }
            return result;
        }
    }
    const double lineHeight = medianHeight(input);
    for (const auto &v : inferredTables(input, tables, lineHeight, prefix)) {
        if (QRectF(QPointF(0, 0), page).contains(rect(v.toObject()["rect"]))) result.regions.append(v);
    }
    QList<QJsonObject> items; QHash<QString, QJsonObject> regionMap;
    for (const auto &v : result.regions) regionMap.insert(v.toObject()["id"].toString(), v.toObject());
    struct Atom { QRectF box; QList<int> members; bool table = false; };
    QList<Atom> atoms; QMap<QString, QList<int>> tableItems;
    for (int i = 0; i < input.size(); ++i) {
        auto item = input[i].toObject(); item["inputIndex"] = i; const auto box = rect(item["rect"]);
        const auto cell = cellAt(result.regions, box);
        if (!cell.isEmpty()) {
            const auto c = regionMap.value(cell); item["container"] = cell; item["table"] = c["parent"]; item["role"] = "cell";
            for (const auto &key : {"row", "column", "rowSpan", "columnSpan"}) item[key] = c[key];
            item["containerRect"] = c["rect"]; tableItems[c["parent"].toString()].append(i);
        } else atoms.append({box, {i}, false});
        items.append(item);
    }
    for (auto it = tableItems.cbegin(); it != tableItems.cend(); ++it) atoms.append({rect(regionMap[it.key()]["rect"]), it.value(), true});
    QList<int> ordered; QHash<int, QString> owners; int flowIndex = 0;
    std::function<void(QList<int>, QRectF, bool, int)> partition;
    partition = [&](QList<int> indices, QRectF area, bool column, int depth) {
        if (indices.isEmpty()) return;
        struct Gap { double start = 0, end = 0; };
        auto largestGap = [&](bool vertical) {
            QList<QPair<double, double>> spans;
            for (int i : indices) { const auto b = atoms[i].box; spans.append(vertical ? qMakePair(b.left(), b.right()) : qMakePair(b.top(), b.bottom())); }
            std::sort(spans.begin(), spans.end()); double end = spans.first().second; Gap gap;
            for (const auto &span : spans) { if (span.first - end > gap.end - gap.start) gap = {end, span.first}; end = qMax(end, span.second); }
            return gap;
        };
        if (depth < 24) {
            const auto gap = largestGap(true);
            if (gap.end - gap.start >= qMax(12.0, lineHeight * 1.6)) {
                QList<int> left, right; double leftWidth = 0, rightWidth = 0;
                for (int i : indices) {
                    if (atoms[i].box.center().x() < gap.start + 1) { left.append(i); leftWidth = qMax(leftWidth, atoms[i].box.width()); }
                    else { right.append(i); rightWidth = qMax(rightWidth, atoms[i].box.width()); }
                }
                if (!left.isEmpty() && !right.isEmpty() && leftWidth > lineHeight * 3 && rightWidth > lineHeight * 3) {
                    QRectF l = area, r = area; l.setRight(gap.start + qMin(6.0, (gap.end - gap.start) / 4)); r.setLeft(gap.end);
                    partition(left, l, true, depth + 1); partition(right, r, true, depth + 1); return;
                }
            }
            // A full-width title or table separates independent column bands.
            bool parallel = false; QList<double> gutterCenters;
            for (int a : indices) for (int b : indices) if (a < b) {
                const auto x = atoms[a].box, y = atoms[b].box;
                if (x.top() < y.bottom() && y.top() < x.bottom() && x.width() > lineHeight * 3 && y.width() > lineHeight * 3
                    && qMax(x.left() - y.right(), y.left() - x.right()) >= qMax(12.0, lineHeight * 1.6)) {
                    parallel = true; const double left = qMin(x.right(), y.right()), right = qMax(x.left(), y.left()); bool intervening = false;
                    for (int k : indices) if (k != a && k != b) { const auto middle = atoms[k].box;
                        if (middle.left() > left && middle.right() < right && middle.top() < qMin(x.bottom(), y.bottom()) && middle.bottom() > qMax(x.top(), y.top())) intervening = true;
                    }
                    if (!intervening) gutterCenters.append((left + right) / 2);
                }
            }
            if (parallel) {
                auto gap = largestGap(false);
                const double minimumBandGap = qMax(3.0, lineHeight * .4);
                // Expose the gutters by separating spanning headings first.
                // A large paragraph gap inside both columns is not a section
                // boundary: cutting there would interleave their reading order.
                double earliest = 1e9; QRectF spanning;
                for (int i : indices) for (double gutter : gutterCenters) {
                    const auto box = atoms[i].box;
                    if (box.left() < gutter - lineHeight && box.right() > gutter + lineHeight && box.top() < earliest) { earliest = box.top(); spanning = box; }
                }
                if (!spanning.isEmpty()) {
                    QList<QPair<double, double>> spans; for (int i : indices) spans.append({atoms[i].box.top(), atoms[i].box.bottom()}); std::sort(spans.begin(), spans.end());
                    double end = spans.first().second, distance = 1e9;
                    for (const auto &s : spans) {
                        if (s.first - end >= minimumBandGap) {
                            const double d = qMin(std::abs(end - spanning.bottom()), std::abs(s.first - spanning.top()));
                            if (d < distance) { distance = d; gap = {end, s.first}; }
                        }
                        end = qMax(end, s.second);
                    }
                }
                if (gap.end - gap.start >= minimumBandGap) {
                    QList<int> top, bottom; for (int i : indices) (atoms[i].box.center().y() <= gap.start ? top : bottom).append(i);
                    if (!top.isEmpty() && !bottom.isEmpty()) {
                        QRectF t = area, b = area; t.setBottom((gap.start + gap.end) / 2); b.setTop(gap.end);
                        partition(top, t, column, depth + 1); partition(bottom, b, column, depth + 1); return;
                    }
                }
            }
        }
        const auto flowId = prefix + QStringLiteral("-flow%1").arg(flowIndex++);
        result.regions.append(QJsonObject{{"id", flowId}, {"kind", column ? "column" : "flow"}, {"rect", jsonRect(area)}});
        std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) {
            if (atoms[a].box.top() != atoms[b].box.top()) return atoms[a].box.top() < atoms[b].box.top(); return atoms[a].box.left() < atoms[b].box.left();
        });
        for (int a : indices) {
            auto members = atoms[a].members;
            std::stable_sort(members.begin(), members.end(), [&](int x, int y) {
                if (items[x]["row"] != items[y]["row"]) return items[x]["row"].toInt() < items[y]["row"].toInt();
                if (items[x]["column"] != items[y]["column"]) return items[x]["column"].toInt() < items[y]["column"].toInt();
                const auto l = rect(items[x]["rect"]), r = rect(items[y]["rect"]);
                return l.top() == r.top() ? l.left() < r.left() : l.top() < r.top();
            });
            for (int i : members) { ordered.append(i); if (!atoms[a].table) owners.insert(i, flowId); }
        }
    };
    QRectF content; QList<int> indices;
    for (int i = 0; i < atoms.size(); ++i) { indices.append(i); content = content.isEmpty() ? atoms[i].box : content.united(atoms[i].box); }
    if (!content.isEmpty()) { content.setBottom(qMax(content.bottom(), page.height() - qMax(12.0, content.left() * .4))); partition(indices, content, false, 0); }
    for (const auto &v : result.regions) regionMap.insert(v.toObject()["id"].toString(), v.toObject());
    int order = 0;
    for (int i : ordered) {
        auto item = items[i];
        if (owners.contains(i)) { item["container"] = owners[i]; item["role"] = "paragraph"; item["containerRect"] = regionMap[owners[i]]["rect"]; }
        item["order"] = order++; result.items.append(item);
    }
    return result;
}

QJsonObject structure(const QJsonArray &blocks, const QJsonArray &regions) {
    QHash<QString, QJsonArray> children; QJsonArray order;
    for (const auto &v : regions) { const auto r = v.toObject(); if (!r["parent"].toString().isEmpty()) children[r["parent"].toString()].append(r["id"]); }
    for (const auto &v : blocks) { const auto b = v.toObject(); children[b["container"].toString()].append(b["id"]); order.append(b["id"]); }
    QJsonArray output;
    for (const auto &v : regions) {
        auto r = v.toObject(); r["children"] = children[r["id"].toString()];
        if (r["kind"] == "table") {
            int headerRows = r["headerRows"].toInt(1);
            if (!r["headersEdited"].toBool()) {
                headerRows = 0;
                for (int row = 0; row < qMin(4, r["rows"].toInt()); ++row) {
                    int count = 0, strong = 0, numbers = 0;
                    for (const auto &b : blocks) { const auto item = b.toObject(); if (item["table"] == r["id"] && item["row"].toInt() == row) {
                        ++count; if (item["bold"].toBool() || item["columnSpan"].toInt(1) > 1) ++strong;
                        if (numericCell(item["source"].toString())) ++numbers;
                    } }
                    if (count == 0 || numbers > count * .3 || (row > 0 && strong < count * .5)) break;
                    ++headerRows;
                }
                r["headerRows"] = headerRows;
            }
            QJsonArray headers;
            for (const auto &b : blocks) { const auto item = b.toObject();
                if (item["table"] == r["id"] && item["row"].toInt() < headerRows && !item["source"].toString().trimmed().isEmpty())
                    headers.append(QJsonObject{{"column", item["column"]}, {"columnSpan", item["columnSpan"]}, {"text", item["source"]}});
            }
            r["headers"] = headers;
        }
        output.append(r);
    }
    return {{"version", 1}, {"regions", output}, {"order", order}};
}

QJsonArray linkPageHeaders(const QJsonArray &input, int firstPage) {
    auto pages = input;
    for (int p = 1; p < pages.size(); ++p) {
        auto page = pages[p].toObject(); const bool scanned = page.contains("scan"); auto scan = page.value("scan").toObject();
        auto layout = scanned ? scan["layout"].toObject() : page["layout"].toObject(); auto regions = layout["regions"].toArray();
        const auto previous = pages[p - 1].toObject(); const auto previousLayout = previous.contains("scan") ? previous["scan"].toObject()["layout"].toObject() : previous["layout"].toObject();
        for (int i = 0; i < regions.size(); ++i) {
            auto table = regions[i].toObject(); if (table["kind"] != "table" || table["continuationEdited"].toBool()) continue;
            table.remove("continuedFrom"); table.remove("inheritedHeaders");
            const auto b = rect(table["rect"]);
            if (b.top() > page["height"].toDouble() * .28) { regions[i] = table; continue; }
            for (const auto &v : previousLayout["regions"].toArray()) {
                const auto candidate = v.toObject(); const auto a = rect(candidate["rect"]);
                if (candidate["kind"] != "table" || candidate["columns"] != table["columns"] || a.bottom() < previous["height"].toDouble() * .72) continue;
                const auto xs = candidate["xEdges"].toArray(), nextXs = table["xEdges"].toArray(); bool aligned = xs.size() == nextXs.size();
                for (int c = 0; aligned && c < xs.size(); ++c)
                    if (std::abs(xs[c].toDouble() / previous["width"].toDouble() - nextXs[c].toDouble() / page["width"].toDouble()) > .025) aligned = false;
                auto headers = candidate["headers"].toArray(); if (headers.isEmpty()) headers = candidate["inheritedHeaders"].toArray();
                if (!aligned || headers.isEmpty()) continue;
                auto normalize = [](const QJsonArray &entries) { QStringList values; for (const auto &h : entries) values.append(h.toObject()["text"].toString().simplified().toCaseFolded()); return values.join('|'); };
                if (!table["headers"].toArray().isEmpty() && normalize(table["headers"].toArray()) != normalize(headers)) continue;
                table["continuedFrom"] = QJsonObject{{"page", firstPage + p - 1}, {"table", candidate["id"]}}; table["inheritedHeaders"] = headers; break;
            }
            regions[i] = table;
        }
        layout["regions"] = regions;
        if (scanned) { scan["layout"] = layout; page["scan"] = scan; } else page["layout"] = layout;
        pages[p] = page;
    }
    return pages;
}

QString cellContext(const QJsonObject &block, const QJsonArray &blocks, const QJsonArray &regions) {
    if (block["table"].toString().isEmpty()) return {};
    int headers = 1;
    QStringList context;
    for (const auto &v : regions) if (v.toObject()["id"] == block["table"]) {
        const auto table = v.toObject(); headers = table["headerRows"].toInt(1);
        for (const auto &value : table["inheritedHeaders"].toArray()) {
            const auto h = value.toObject(); const int col = block["column"].toInt();
            if (h["column"].toInt() <= col && h["column"].toInt() + h["columnSpan"].toInt(1) > col) context.append(h["text"].toString().left(300));
        }
    }
    for (const auto &v : blocks) {
        const auto b = v.toObject(); if (b["table"] != block["table"] || b["id"] == block["id"]) continue;
        const int col = block["column"].toInt(), row = block["row"].toInt();
        const bool header = b["row"].toInt() < headers && b["column"].toInt() <= col && b["column"].toInt() + b["columnSpan"].toInt(1) > col;
        const bool label = b["column"].toInt() == 0 && b["row"].toInt() <= row && b["row"].toInt() + b["rowSpan"].toInt(1) > row;
        if (header || label) context.append(b["source"].toString().left(300));
    }
    context.removeDuplicates(); return context.join(" | ").left(1200);
}

QString changedNumbers(const QString &source, const QString &translation, bool pageReferences) {
    // Chinese normally has no space before a number (e.g. 第10页). Exclude
    // Latin identifiers, not all letters, when locating numeric values.
    static const QRegularExpression number(QStringLiteral("(?<![A-Za-z_\\d])[-+−]?\\d+(?:[.,]\\d+)*(?:[%％])?"));
    auto tokens = [&](const QString &value) {
        auto normalized = value.normalized(QString::NormalizationForm_KC);
        // In an index, commas separate page references, even when Chinese
        // typography omits the following space. They are not decimal marks.
        if (pageReferences) normalized.replace(',', ' ');
        QStringList out; auto matches = number.globalMatch(normalized);
        while (matches.hasNext()) out.append(matches.next().captured().replace(QChar(0x2212), '-'));
        std::sort(out.begin(), out.end()); return out;
    };
    return tokens(source) == tokens(translation) ? QString() : QStringLiteral("译文数字与原文不一致，请核对后确认。");
}
QString changedCode(const QString &source, const QString &translation) {
    auto tokens = [](const QString &value) {
        QString code; QChar quote; bool lineComment = false, blockComment = false, escaped = false;
        for (int i = 0; i < value.size(); ++i) {
            const auto c = value[i], next = i + 1 < value.size() ? value[i + 1] : QChar();
            if (lineComment) { if (c == '\n') { lineComment = false; code += c; } continue; }
            if (blockComment) { if (c == '\n') code += c; if (c == '*' && next == '/') { blockComment = false; ++i; } continue; }
            if (!quote.isNull()) { code += c; if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == quote) quote = {}; continue; }
            if (c == '/' && next == '/') { lineComment = true; code += ' '; ++i; continue; }
            if (c == '/' && next == '*') { blockComment = true; code += ' '; ++i; continue; }
            if (c == '"' || c == '\'') quote = c;
            code += c;
        }
        // Whitespace may move between code tokens, but must not merge names,
        // operators or numeric literals. Preprocessor newlines are semantic.
        static const QRegularExpression token(QStringLiteral("[A-Za-z_]\\w*|(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eEpP][+-]?\\d+)?[a-zA-Z0-9_]*|>>=|<<=|->\\*|\\.\\.\\.|\\+\\+|--|->|\\.\\*|::|<<|>>|<=|>=|==|!=|&&|\\|\\||[+*/%&|^\\-]=|##|\\S"));
        QStringList result; bool continuedDirective = false;
        for (const auto &line : code.split('\n')) {
            const auto trimmed = line.trimmed();
            if (continuedDirective || trimmed.startsWith('#')) {
                result.append("directive:" + trimmed); continuedDirective = trimmed.endsWith('\\'); continue;
            }
            for (int i = 0; i < line.size();) {
                if (line[i].isSpace()) { ++i; continue; }
                if (line[i] == '"' || line[i] == '\'') {
                    const auto delimiter = line[i]; const int start = i++; bool escape = false;
                    while (i < line.size()) { const auto c = line[i++]; if (escape) escape = false; else if (c == '\\') escape = true; else if (c == delimiter) break; }
                    result.append(line.mid(start, i - start)); continue;
                }
                const auto match = token.match(line, i, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
                if (match.hasMatch()) { result.append(match.captured()); i = match.capturedEnd(); }
                else result.append(line.mid(i++, 1));
            }
        }
        return result;
    };
    return tokens(source) == tokens(translation) ? QString() : QStringLiteral("代码中的标识符、常量或语法发生变化，请核对；仅允许翻译注释。");
}
}
