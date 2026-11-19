#include "pdfbackground.h"
#include <QJsonArray>
#include <QQueue>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
struct Sample { double x, y; std::array<double, 3> color; };
struct Plane {
    std::array<std::array<double, 3>, 3> coefficients{};
    QRect rect; double noise = 0, variation = 0; bool lightText = false;
    QColor color(int x, int y) const {
        const double u = (x - rect.center().x()) / double(qMax(1, rect.width())), v = (y - rect.center().y()) / double(qMax(1, rect.height()));
        int channels[3]; for (int c = 0; c < 3; ++c) channels[c] = qBound(0, qRound(coefficients[c][0] + coefficients[c][1] * u + coefficients[c][2] * v), 255);
        return QColor(channels[0], channels[1], channels[2]);
    }
    QString kind() const { return noise < 7 && variation < 12 ? "solid" : noise < 13 ? "gradient" : "texture"; }
};
double median(QList<double> values) { if (values.isEmpty()) return 0; std::sort(values.begin(), values.end()); return values[values.size() / 2]; }
std::array<double, 3> solve(double matrix[3][3], double rhs[3]) {
    double a[3][4]; for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) a[r][c] = matrix[r][c]; a[r][3] = rhs[r]; }
    for (int c = 0; c < 3; ++c) {
        int pivot = c; for (int r = c + 1; r < 3; ++r) if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
        if (std::abs(a[pivot][c]) < 1e-8) return {matrix[0][0] > 0 ? rhs[0] / matrix[0][0] : 255, 0, 0};
        for (int j = c; j < 4; ++j) std::swap(a[c][j], a[pivot][j]); const double divisor = a[c][c];
        for (int j = c; j < 4; ++j) a[c][j] /= divisor;
        for (int r = 0; r < 3; ++r) if (r != c) { const double factor = a[r][c]; for (int j = c; j < 4; ++j) a[r][j] -= factor * a[c][j]; }
    }
    return {a[0][3], a[1][3], a[2][3]};
}
Plane fit(const QImage &image, QRect text) {
    Plane plane; text = text.intersected(image.rect()); plane.rect = text;
    if (text.isEmpty()) return plane;
    const int band = qBound(3, text.height() / 5, 14); const auto ring = text.adjusted(-band, -band, band, band).intersected(image.rect());
    QList<Sample> samples; const int stride = qMax(1, (ring.width() + ring.height()) / 600);
    for (int y = ring.top(); y <= ring.bottom(); y += stride) for (int x = ring.left(); x <= ring.right(); x += stride) {
        if (text.contains(x, y)) continue;
        const auto p = image.pixel(x, y); samples.append({(x - text.center().x()) / double(text.width()), (y - text.center().y()) / double(text.height()), {double(qRed(p)), double(qGreen(p)), double(qBlue(p))}});
    }
    if (samples.isEmpty()) return plane;
    QList<double> residuals; double cutoff = 1e9;
    for (int pass = 0; pass < 3; ++pass) {
        double matrix[3][3] = {}, rhs[3][3] = {};
        for (int i = 0; i < samples.size(); ++i) {
            if (pass && residuals[i] > cutoff) continue;
            const auto &s = samples[i]; const double basis[]{1, s.x, s.y};
            for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) matrix[r][c] += basis[r] * basis[c]; for (int channel = 0; channel < 3; ++channel) rhs[channel][r] += basis[r] * s.color[channel]; }
        }
        for (int c = 0; c < 3; ++c) plane.coefficients[c] = solve(matrix, rhs[c]);
        residuals.clear();
        for (const auto &s : samples) {
            double residual = 0;
            for (int c = 0; c < 3; ++c) residual += std::abs(s.color[c] - plane.coefficients[c][0] - plane.coefficients[c][1] * s.x - plane.coefficients[c][2] * s.y);
            residuals.append(residual / 3);
        }
        cutoff = qMax(12.0, median(residuals) * 2.5);
    }
    plane.noise = median(residuals);
    for (const auto &channel : plane.coefficients) plane.variation = qMax(plane.variation, std::abs(channel[1]) + std::abs(channel[2]));
    QList<double> luminance;
    for (int y = text.top(); y <= text.bottom(); y += 2) for (int x = text.left(); x <= text.right(); x += 2) luminance.append(qGray(image.pixel(x, y)));
    std::sort(luminance.begin(), luminance.end());
    if (!luminance.isEmpty()) { const double bg = qGray(plane.color(text.center().x(), text.center().y()).rgb());
        plane.lightText = luminance[luminance.size() * 95 / 100] - bg > (bg - luminance[luminance.size() * 5 / 100]) * 1.1;
    }
    return plane;
}

bool repair(QImage &output, const QImage &original, const QRect &text, const Plane &plane, QString *error) {
    const int padding = qBound(12, text.height(), 60); const QRect area = text.adjusted(-padding, -padding, padding, padding).intersected(original.rect());
    const int w = area.width(), h = area.height(); if (w * h > 2000000) { *error = QStringLiteral("背景修复区域过大，请拆分文字区域或使用底色框。"); return false; }
    QByteArray holes(w * h, 0); int total = 0;
    // A local mean separates both light and dark letter strokes from gradual
    // changes in a photograph. It is a repair mask, not an OCR transcript.
    QList<qint64> integral((w + 1) * (h + 1), 0);
    for (int y = 0; y < h; ++y) { qint64 row = 0; for (int x = 0; x < w; ++x) {
        row += qGray(original.pixel(area.left() + x, area.top() + y)); integral[(y + 1) * (w + 1) + x + 1] = integral[y * (w + 1) + x + 1] + row;
    } }
    const int radius = qBound(4, text.height() / 2, 25); const double threshold = qBound(18.0, plane.noise * .8, 42.0);
    for (int py = text.top(); py <= text.bottom(); ++py) for (int px = text.left(); px <= text.right(); ++px) {
        const int x = px - area.left(), y = py - area.top(); const int x0 = qMax(0, x - radius), x1 = qMin(w, x + radius + 1), y0 = qMax(0, y - radius), y1 = qMin(h, y + radius + 1);
        const double mean = double(integral[y1 * (w + 1) + x1] - integral[y0 * (w + 1) + x1] - integral[y1 * (w + 1) + x0] + integral[y0 * (w + 1) + x0]) / ((x1 - x0) * (y1 - y0));
        const int value = qGray(original.pixel(px, py)); if (plane.lightText ? value > mean + threshold : value < mean - threshold) holes[y * w + x] = 1;
    }
    const auto initial = holes;
    for (int y = 1; y + 1 < h; ++y) for (int x = 1; x + 1 < w; ++x) if (initial[y * w + x])
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) if (text.contains(area.left() + x + dx, area.top() + y + dy)) holes[(y + dy) * w + x + dx] = 1;
    for (char c : holes) if (c) ++total;
    if (total < 3 || total > text.width() * text.height() * .8) { *error = QStringLiteral("无法可靠分离文字与背景，请使用底色框或保留原图对照。"); return false; }
    const auto originalHoles = holes; QQueue<int> queue; QByteArray queued(w * h, 0);
    const QList<QPoint> neighbors{{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    auto enqueue = [&](int x, int y) {
        if (x < 1 || y < 1 || x + 1 >= w || y + 1 >= h || !holes[y * w + x] || queued[y * w + x]) return;
        for (const auto &d : neighbors) if (!holes[(y + d.y()) * w + x + d.x()]) { queue.enqueue(y * w + x); queued[y * w + x] = 1; break; }
    };
    for (int y = 1; y + 1 < h; ++y) for (int x = 1; x + 1 < w; ++x) enqueue(x, y);
    while (!queue.isEmpty()) {
        const int index = queue.dequeue(), x = index % w, y = index / w; if (!holes[index]) continue;
        QList<QPoint> known; for (const auto &d : neighbors) if (!holes[(y + d.y()) * w + x + d.x()]) known.append(d);
        if (known.isEmpty()) continue;
        double best = 1e20; QRgb color = plane.color(area.left() + x, area.top() + y).rgb();
        const int search = qBound(12, text.height(), 40), step = qMax(2, search / 6);
        for (int cy = qMax(1, y - search); cy < qMin(h - 1, y + search); cy += step) for (int cx = qMax(1, x - search); cx < qMin(w - 1, x + search); cx += step) {
            if (originalHoles[cy * w + cx]) continue;
            bool usable = true; double score = .05 * (std::abs(cx - x) + std::abs(cy - y));
            for (const auto &d : known) {
                if (originalHoles[(cy + d.y()) * w + cx + d.x()]) { usable = false; break; }
                const auto a = output.pixel(area.left() + x + d.x(), area.top() + y + d.y()), b = original.pixel(area.left() + cx + d.x(), area.top() + cy + d.y());
                score += (std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b))) / double(known.size());
            }
            if (usable && score < best) { best = score; color = original.pixel(area.left() + cx, area.top() + cy); }
        }
        output.setPixel(area.left() + x, area.top() + y, color); holes[index] = 0;
        for (const auto &d : neighbors) enqueue(x + d.x(), y + d.y());
    }
    for (char remaining : holes) if (remaining) { *error = QStringLiteral("文字过于接近图片边缘，请调整区域或使用底色框。"); return false; }
    return true;
}
}

QJsonObject PdfBackground::describe(const QImage &image, const QRect &text) {
    const auto plane = fit(image, text); const auto c = plane.color(text.center().x(), text.center().y()); const int ink = plane.lightText ? 255 : 0;
    return {{"kind", plane.kind()}, {"color", QJsonArray{c.red(), c.green(), c.blue()}}, {"textColor", QJsonArray{ink, ink, ink}}, {"noise", plane.noise}};
}
bool PdfBackground::erase(QImage &output, const QImage &original, const QList<QRect> &lines, const QString &mode, QString *error, const QRect &clip) {
    // Work on a detached image; a failed line never leaves a partly erased block.
    auto working = output;
    for (const auto &line : lines) {
        const auto sample = line.intersected(original.rect()); if (sample.isEmpty()) continue;
        // Fit around the complete detected text, then limit the pixels changed.
        // Clipping before fitting can put letters in the background sample ring.
        const auto plane = fit(original, sample);
        const auto area = clip.isNull() ? sample : sample.intersected(clip); if (area.isEmpty()) continue;
        if (mode == "repair") { if (!repair(working, original, area, plane, error)) return false; }
        else {
            if (plane.kind() == "texture") { *error = QStringLiteral("文字位于复杂背景，请选择背景修复、底色框或保留原图对照。"); return false; }
            for (int y = area.top(); y <= area.bottom(); ++y) { auto *row = reinterpret_cast<QRgb *>(working.scanLine(y)); for (int x = area.left(); x <= area.right(); ++x) row[x] = plane.color(x, y).rgb(); }
        }
    }
    output = working; return true;
}
