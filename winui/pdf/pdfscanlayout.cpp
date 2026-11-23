#include "pdfscanlayout.h"
#include "pdfanalysis.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef GetObject

#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace winrt;
using namespace Windows::Data::Json;

namespace yiyi::pdf {
namespace {

constexpr int maxPixels = 16'000'000;
constexpr int maxDetected = 1500;

struct Pixels {
    int left{}, top{}, right{}, bottom{};

    int width() const { return right - left; }
    int height() const { return bottom - top; }
    int area() const { return empty() ? 0 : width() * height(); }
    double cx() const { return (left + right - 1) * .5; }
    double cy() const { return (top + bottom - 1) * .5; }
    bool empty() const { return right <= left || bottom <= top; }
    bool contains(int x, int y) const { return x >= left && x < right && y >= top && y < bottom; }
    bool contains(Pixels const& other) const
    {
        return left <= other.left && right >= other.right && top <= other.top && bottom >= other.bottom;
    }
    bool intersects(Pixels const& other) const
    {
        return left < other.right && right > other.left && top < other.bottom && bottom > other.top;
    }
    Pixels expanded(int horizontal, int vertical) const
    {
        return {left - horizontal, top - vertical, right + horizontal, bottom + vertical};
    }
    Pixels intersected(Pixels const& area) const
    {
        const Pixels result{std::max(left, area.left), std::max(top, area.top), std::min(right, area.right), std::min(bottom, area.bottom)};
        return result.empty() ? Pixels{} : result;
    }
    Pixels united(Pixels const& area) const
    {
        if (empty()) return area;
        if (area.empty()) return *this;
        return {std::min(left, area.left), std::min(top, area.top), std::max(right, area.right), std::max(bottom, area.bottom)};
    }
};

using Color = std::array<int, 3>;

class Raster {
public:
    Raster(std::span<const uint8_t> pixels, int width, int height, int stride, double pageWidth, double pageHeight)
        : bytes(pixels), w(width), h(height), rowSize(stride), sx(pageWidth / width), sy(pageHeight / height)
    {
        if (w < 1 || h < 1 || static_cast<int64_t>(w) * h > maxPixels || rowSize < static_cast<int64_t>(w) * 4
            || static_cast<uint64_t>(rowSize) * h > bytes.size() || !std::isfinite(sx) || !std::isfinite(sy) || sx <= 0 || sy <= 0)
            throw hresult_invalid_argument(L"扫描页图像尺寸或像素格式无效。");
    }

    Pixels bounds() const { return {0, 0, w, h}; }
    size_t offset(int x, int y) const { return static_cast<size_t>(y) * rowSize + static_cast<size_t>(x) * 4; }
    Color at(int x, int y) const
    {
        const auto index = offset(x, y);
        return {bytes[index + 2], bytes[index + 1], bytes[index]};
    }
    JsonArray points(Pixels const& box) const;
    Pixels pixels(JsonArray const& points) const;

    std::span<const uint8_t> bytes;
    int w{}, h{}, rowSize{};
    double sx{}, sy{};
};

JsonArray values(std::initializer_list<double> input)
{
    JsonArray result;
    for (const auto value : input) result.Append(JsonValue::CreateNumberValue(value));
    return result;
}

void put(JsonObject const& obj, hstring const& name, double value) { obj.Insert(name, JsonValue::CreateNumberValue(value)); }
void put(JsonObject const& obj, hstring const& name, bool value) { obj.Insert(name, JsonValue::CreateBooleanValue(value)); }
void put(JsonObject const& obj, hstring const& name, hstring const& value) { obj.Insert(name, JsonValue::CreateStringValue(value)); }
void put(JsonObject const& obj, hstring const& name, wchar_t const* value) { put(obj, name, hstring(value)); }

JsonArray Raster::points(Pixels const& box) const
{
    return values({box.left * sx, box.top * sy, box.width() * sx, box.height() * sy});
}

Pixels Raster::pixels(JsonArray const& coordinates) const
{
    if (!coordinates || coordinates.Size() != 4) return {};
    const auto l = coordinates.GetNumberAt(0) / sx, t = coordinates.GetNumberAt(1) / sy;
    const auto r = l + coordinates.GetNumberAt(2) / sx, b = t + coordinates.GetNumberAt(3) / sy;
    if (!std::isfinite(l) || !std::isfinite(t) || !std::isfinite(r) || !std::isfinite(b)) return {};
    return Pixels{static_cast<int>(std::floor(std::clamp(l, 0.0, static_cast<double>(w)))),
                  static_cast<int>(std::floor(std::clamp(t, 0.0, static_cast<double>(h)))),
                  static_cast<int>(std::ceil(std::clamp(r, 0.0, static_cast<double>(w)))),
                  static_cast<int>(std::ceil(std::clamp(b, 0.0, static_cast<double>(h))))};
}

int gray(Color const& color) { return (color[0] * 11 + color[1] * 16 + color[2] * 5) / 32; }

void pixel(uint8_t* output, Color const& color)
{
    output[0] = static_cast<uint8_t>(color[2]);
    output[1] = static_cast<uint8_t>(color[1]);
    output[2] = static_cast<uint8_t>(color[0]);
    output[3] = 255;
}

struct Ink {
    Pixels box;
    int count{};
    bool colored{};
};

std::vector<Ink> components(Raster const& raster, bool light = false)
{
    std::vector<uint8_t> visited(static_cast<size_t>(raster.w) * raster.h);
    std::vector<int> queue;
    std::vector<Ink> result;
    const auto ink = [&](int x, int y) {
        const auto value = gray(raster.at(x, y));
        return light ? value > 210 : value < 210;
    };
    for (int y = 0; y < raster.h; ++y) {
        for (int x = 0; x < raster.w; ++x) {
            const int start = y * raster.w + x;
            if (visited[start] || !ink(x, y)) continue;
            visited[start] = 1;
            queue.clear();
            queue.push_back(start);
            Pixels box{x, y, x + 1, y + 1};
            size_t colored = 0;
            for (size_t i = 0; i < queue.size(); ++i) {
                const auto px = queue[i] % raster.w, py = queue[i] / raster.w;
                box = box.united({px, py, px + 1, py + 1});
                const auto value = raster.at(px, py);
                if (*std::max_element(value.begin(), value.end()) - *std::min_element(value.begin(), value.end()) > 35) ++colored;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const auto xx = px + dx, yy = py + dy;
                        if (xx < 0 || yy < 0 || xx >= raster.w || yy >= raster.h) continue;
                        const auto next = yy * raster.w + xx;
                        if (!visited[next] && ink(xx, yy)) {
                            visited[next] = 1;
                            queue.push_back(next);
                        }
                    }
                }
            }
            if (queue.size() >= 3) result.push_back({box, static_cast<int>(queue.size()), colored > queue.size() / 4});
        }
    }
    return result;
}

JsonArray rules(Raster const& raster)
{
    struct Stripe { int first, last, start, end; };
    JsonArray output;
    for (const bool horizontal : {true, false}) {
        const int along = horizontal ? raster.w : raster.h, across = horizontal ? raster.h : raster.w;
        const double alongScale = horizontal ? raster.sx : raster.sy, acrossScale = horizontal ? raster.sy : raster.sx;
        const auto minimum = std::max(20, static_cast<int>(std::min<double>(along + 1, std::round(18 / alongScale))));
        std::vector<Stripe> active;
        const auto append = [&](Stripe const& span) {
            if ((span.end - span.start + 1) * acrossScale > 3.5) return;
            const auto at = (span.start + span.end) * .5 * acrossScale;
            const auto from = span.first * alongScale, to = (span.last + 1) * alongScale;
            output.Append(horizontal ? values({from, at, to, at}) : values({at, from, at, to}));
        };
        for (int y = 0; y < across; ++y) {
            std::vector<Stripe> next;
            std::vector<bool> used(active.size());
            int first = -1, last = -1;
            for (int x = 0; x <= along; ++x) {
                const bool dark = x < along && gray(horizontal ? raster.at(x, y) : raster.at(y, x)) < 195;
                if (dark) {
                    if (first < 0) first = x;
                    last = x;
                }
                if (first < 0 || (x != along && x - last <= 2)) continue;
                if (last - first + 1 >= minimum) {
                    auto match = active.size();
                    for (size_t i = 0; i < active.size(); ++i) {
                        if (!used[i] && std::abs(active[i].first - first) < 5 && std::abs(active[i].last - last) < 5) {
                            match = i;
                            break;
                        }
                    }
                    if (match == active.size()) next.push_back({first, last, y, y});
                    else {
                        auto continuous = active[match];
                        continuous.end = y;
                        next.push_back(continuous);
                        used[match] = true;
                    }
                }
                first = -1;
            }
            for (size_t i = 0; i < active.size(); ++i) if (!used[i]) append(active[i]);
            active = std::move(next);
            if (output.Size() > 3000) return JsonArray{};
        }
        for (const auto& span : active) append(span);
    }
    return output;
}

double median(std::vector<double> values, double fallback = 0)
{
    if (values.empty()) return fallback;
    const auto middle = values.begin() + static_cast<ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

struct Plane {
    std::array<std::array<double, 3>, 3> parameters{{{255, 0, 0}, {255, 0, 0}, {255, 0, 0}}};
    Pixels area;
    double noise{};
    double variation{};
    bool lightInk{};

    Color bg(double x, double y) const
    {
        const auto u = (x - area.cx()) / std::max(1, area.width()), v = (y - area.cy()) / std::max(1, area.height());
        Color result{};
        for (size_t c = 0; c < result.size(); ++c) {
            const auto& p = parameters[c];
            result[c] = static_cast<int>(std::lround(std::clamp(p[0] + p[1] * u + p[2] * v, 0.0, 255.0)));
        }
        return result;
    }
    hstring kind() const { return noise < 7 && variation < 12 ? L"solid" : noise < 13 ? L"gradient" : L"texture"; }
};

std::array<double, 3> solve(const double (&matrix)[3][3], const double (&rhs)[3])
{
    double a[3][4]{};
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) a[row][column] = matrix[row][column];
        a[row][3] = rhs[row];
    }
    for (size_t column = 0; column < 3; ++column) {
        auto pivot = column;
        for (auto row = column + 1; row < 3; ++row)
            if (std::abs(a[row][column]) > std::abs(a[pivot][column])) pivot = row;
        if (std::abs(a[pivot][column]) < 1e-8) return {matrix[0][0] > 0 ? rhs[0] / matrix[0][0] : 255, 0, 0};
        std::swap(a[column], a[pivot]);
        const auto divisor = a[column][column];
        for (auto value = column; value < 4; ++value) a[column][value] /= divisor;
        for (size_t row = 0; row < 3; ++row) {
            if (row == column) continue;
            const auto factor = a[row][column];
            for (auto value = column; value < 4; ++value) a[row][value] -= factor * a[column][value];
        }
    }
    return {a[0][3], a[1][3], a[2][3]};
}

int percentile(std::array<int, 256> const& counts, int count, int fraction)
{
    if (count == 0) return 255;
    const auto threshold = count * fraction / 100;
    int total = 0;
    for (size_t value = 0; value < counts.size(); ++value) {
        total += counts[value];
        if (total > threshold) return static_cast<int>(value);
    }
    return 255;
}

Plane fit(Raster const& raster, Pixels area)
{
    Plane plane;
    plane.area = area.intersected(raster.bounds());
    if (plane.area.empty()) return plane;
    area = plane.area;
    const auto band = std::clamp(area.height() / 5, 3, 14);
    const auto ring = area.expanded(band, band).intersected(raster.bounds());
    const auto stride = std::max(1, (ring.width() + ring.height()) / 600);
    struct Sample { double x, y; Color rgb; };
    std::vector<Sample> samples;
    for (int y = ring.top; y < ring.bottom; y += stride) {
        for (int x = ring.left; x < ring.right; x += stride) {
            if (!area.contains(x, y))
                samples.push_back({(x - area.cx()) / area.width(), (y - area.cy()) / area.height(), raster.at(x, y)});
        }
    }
    if (samples.empty()) {
        plane.noise = 255; // No surrounding background to sample reliably.
        return plane;
    }
    std::vector<double> residuals(samples.size());
    double cutoff = 1e9;
    for (int pass = 0; pass < 3; ++pass) {
        double matrix[3][3]{};
        double rhs[3][3]{};
        for (size_t index = 0; index < samples.size(); ++index) {
            if (pass && residuals[index] > cutoff) continue;
            const auto& sample = samples[index];
            const double basis[] = {1, sample.x, sample.y};
            for (size_t row = 0; row < 3; ++row) {
                for (size_t column = 0; column < 3; ++column) matrix[row][column] += basis[row] * basis[column];
                for (size_t c = 0; c < 3; ++c) rhs[c][row] += basis[row] * sample.rgb[c];
            }
        }
        for (size_t c = 0; c < 3; ++c) plane.parameters[c] = solve(matrix, rhs[c]);
        for (size_t index = 0; index < samples.size(); ++index) {
            const auto& sample = samples[index];
            double difference = 0;
            for (size_t c = 0; c < 3; ++c) {
                const auto& p = plane.parameters[c];
                difference += std::abs(sample.rgb[c] - p[0] - p[1] * sample.x - p[2] * sample.y);
            }
            residuals[index] = difference / 3;
        }
        cutoff = std::max(12.0, median(residuals) * 2.5);
    }
    plane.noise = median(std::move(residuals));
    for (const auto& channel : plane.parameters)
        plane.variation = std::max(plane.variation, std::abs(channel[1]) + std::abs(channel[2]));
    std::array<int, 256> luma{};
    int count = 0;
    for (int y = area.top; y < area.bottom; y += 2) for (int x = area.left; x < area.right; x += 2) {
        ++luma[gray(raster.at(x, y))];
        ++count;
    }
    const auto background = gray(plane.bg(area.cx(), area.cy()));
    plane.lightInk = percentile(luma, count, 95) - background > (background - percentile(luma, count, 5)) * 1.1;
    return plane;
}

JsonObject describe(Plane const& plane)
{
    const auto color = plane.bg(plane.area.cx(), plane.area.cy());
    const auto ink = plane.lightInk ? 255.0 : 0.0;
    JsonObject details;
    put(details, L"kind", plane.kind());
    put(details, L"noise", plane.noise);
    details.Insert(L"color", values({static_cast<double>(color[0]), static_cast<double>(color[1]), static_cast<double>(color[2])}));
    details.Insert(L"textColor", values({ink, ink, ink}));
    return details;
}

std::vector<uint8_t> bitmap(Raster const& source)
{
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    static_assert(sizeof(file) == 14 && sizeof(info) == 40);
    const auto row = static_cast<size_t>(source.w) * 4;
    const auto offset = sizeof(file) + sizeof(info);
    std::vector<uint8_t> result(offset + row * source.h);
    file.bfType = 0x4d42;
    file.bfSize = static_cast<DWORD>(result.size());
    file.bfOffBits = static_cast<DWORD>(offset);
    info.biSize = sizeof(info);
    info.biWidth = source.w;
    info.biHeight = -source.h;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::memcpy(result.data(), &file, sizeof(file));
    std::memcpy(result.data() + sizeof(file), &info, sizeof(info));
    for (int y = 0; y < source.h; ++y)
        std::memcpy(result.data() + offset + static_cast<size_t>(y) * row, source.bytes.data() + source.offset(0, y), row);
    return result;
}

JsonArray detections(Raster const& raster, std::filesystem::path const& runtime)
{
    struct WorkingDirectory {
        std::wstring before;
        ~WorkingDirectory() { if (!before.empty()) SetCurrentDirectoryW(before.c_str()); }
    };
    auto folder = std::filesystem::absolute(runtime);
    std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> bridge(
        LoadLibraryExW((folder / L"YiyiOcr.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32),
        FreeLibrary);
    using Detect = int (*)(uint8_t const*, int, char*, int);
    const auto detect = bridge ? std::bit_cast<Detect>(GetProcAddress(bridge.get(), "YiyiDetect")) : nullptr;
    if (!detect || !std::filesystem::is_regular_file(folder / L"det.onnx"))
        throw hresult_error(E_FAIL, L"缺少文字区域检测组件，请使用完整发行包。");
    std::wstring current(32768, L'\0');
    const auto length = GetCurrentDirectoryW(static_cast<DWORD>(current.size()), current.data());
    if (!length || length >= current.size() || !SetCurrentDirectoryW(folder.c_str()))
        throw hresult_error(E_FAIL, L"无法打开文字检测组件目录。");
    current.resize(length);
    const WorkingDirectory restore{std::move(current)};
    const auto bytes = bitmap(raster);
    std::string output(1024 * 1024, '\0');
    const auto received = detect(bytes.data(), static_cast<int>(bytes.size()), output.data(), static_cast<int>(output.size()));
    JsonArray result{nullptr};
    if (received < 0 || static_cast<size_t>(received) >= output.size()
        || !JsonArray::TryParse(to_hstring(std::string_view(output.data(), static_cast<size_t>(received))), result))
        throw hresult_error(E_FAIL, L"文字区域检测失败，请重试或框选识别。");
    if (result.Size() > maxDetected) throw hresult_error(E_FAIL, L"本页文字区域过多，请拆分或裁切页面。");
    return result;
}

struct Row {
    Pixels box;
    std::vector<Pixels> masks;
    JsonObject layout{nullptr};
    std::vector<double> heights;
    double weight{};
    double estimated{};
    bool bullet{};
};

std::vector<Pixels> fallbackBoxes(std::vector<Ink> const& ink, Raster const& raster)
{
    std::vector<Pixels> glyphs;
    for (const auto& item : ink) {
        const auto box = item.box;
        if (box.height() >= 5 && box.height() <= std::max(12, raster.h / 15) &&
            box.width() >= 2 && box.width() <= box.height() * 4 && item.count >= 6 &&
            !(item.count > box.area() * .94 && box.width() > box.height() * .7))
            glyphs.push_back(box);
    }
    std::stable_sort(glyphs.begin(), glyphs.end(), [](Pixels const& a, Pixels const& b) { return a.cy() < b.cy(); });
    std::vector<std::vector<Pixels>> rows;
    for (const auto& glyph : glyphs) {
        if (rows.empty() || std::abs(glyph.cy() - rows.back().front().cy()) > .45 * std::max(glyph.height(), rows.back().front().height()))
            rows.emplace_back();
        rows.back().push_back(glyph);
    }
    std::vector<Pixels> boxes;
    for (auto& row : rows) {
        std::sort(row.begin(), row.end(), [](Pixels const& a, Pixels const& b) { return a.left < b.left; });
        Pixels line;
        int count = 0;
        const auto finish = [&] {
            if (count >= 2 && !line.empty()) boxes.push_back(line.expanded(2, 2).intersected(raster.bounds()));
            line = {};
            count = 0;
        };
        for (const auto& glyph : row) {
            if (!line.empty() && glyph.left - line.right > 2.8 * std::max(line.height(), glyph.height())) finish();
            line = line.united(glyph);
            ++count;
        }
        finish();
    }
    return boxes;
}

std::vector<Pixels> detectorBoxes(JsonArray const& detected, Raster const& raster)
{
    std::vector<Pixels> boxes;
    for (const auto& value : detected) {
        if (value.ValueType() != JsonValueType::Array || value.GetArray().Size() != 8) continue;
        const auto points = value.GetArray();
        double left = raster.w, top = raster.h, right = 0, bottom = 0;
        bool valid = true;
        for (uint32_t i = 0; i < points.Size(); i += 2) {
            const auto x = points.GetNumberAt(i), y = points.GetNumberAt(i + 1);
            if (!std::isfinite(x) || !std::isfinite(y)) { valid = false; break; }
            left = std::min(left, x);
            right = std::max(right, x + 1);
            top = std::min(top, y);
            bottom = std::max(bottom, y + 1);
        }
        if (!valid || bottom - top < 5 || right - left < 3
            || std::abs(points.GetNumberAt(3) - points.GetNumberAt(1)) > std::max(5.0, (right - left) * .12))
            continue;
        const Pixels box{static_cast<int>(std::floor(std::clamp(left, 0.0, static_cast<double>(raster.w)))),
                         static_cast<int>(std::floor(std::clamp(top, 0.0, static_cast<double>(raster.h)))),
                         static_cast<int>(std::ceil(std::clamp(right, 0.0, static_cast<double>(raster.w)))),
                         static_cast<int>(std::ceil(std::clamp(bottom, 0.0, static_cast<double>(raster.h))))};
        if (!box.empty()) boxes.push_back(box);
    }
    return boxes;
}

std::vector<Pixels> cellBoxes(JsonObject const& layout, Raster const& raster)
{
    std::vector<Pixels> result;
    for (const auto& value : layout.GetNamedArray(L"regions")) {
        const auto region = value.GetObject();
        if (region.GetNamedString(L"kind", L"") == L"cell") result.push_back(raster.pixels(region.GetNamedArray(L"rect")));
    }
    return result;
}

int cellAt(std::vector<Pixels> const& cells, Pixels const& text)
{
    for (size_t i = 0; i < cells.size(); ++i)
        if (cells[i].expanded(2, 2).contains(text) && cells[i].contains(static_cast<int>(text.cx()), static_cast<int>(text.cy())))
            return static_cast<int>(i);
    return -1;
}

std::vector<Row> rowsFromBoxes(Raster const& raster, std::vector<Pixels> const& boxes, std::vector<Ink> const& ink,
                             std::vector<Pixels> const& cells)
{
    std::vector<Ink> lightInk;
    bool acquiredLightInk = false;
    std::vector<Row> lines;
    for (const auto& box : boxes) {
        std::vector<Pixels> pieces;
        for (const auto& cell : cells) {
            const auto part = box.intersected(cell.expanded(-2, -2));
            if (part.height() > box.height() * .6 && part.width() > 3) pieces.push_back(part);
        }
        if (pieces.empty()) pieces.push_back(box);
        for (const auto& piece : pieces) {
            Pixels tight;
            for (const auto& item : ink) {
                const auto region = item.box;
                if (item.count < std::max(5, static_cast<int>(piece.height() * .15))
                    || (item.count > region.area() * .85 && region.width() > region.height() * .60)
                    || !piece.expanded(2, 2).contains(region)
                    || region.height() > piece.height() * 1.15 || region.width() > piece.height() * 4
                    || (region.width() > region.height() * 12 && region.width() > piece.height() * 2))
                    continue;
                tight = tight.united(region);
            }
            const auto plane = fit(raster, piece);
            const auto bg = plane.bg(piece.cx(), piece.cy());
            int white = 0, samples = 0;
            for (int y = piece.top; y < piece.bottom; y += 2) for (int x = piece.left; x < piece.right; x += 2) {
                const auto value = raster.at(x, y);
                if (*std::min_element(value.begin(), value.end()) >= 225) ++white;
                ++samples;
            }
            const bool complex = white < samples * .6 && (plane.kind() != L"solid" || *std::min_element(bg.begin(), bg.end()) < 220);
            auto mask = piece;
            if (complex && plane.kind() == L"solid" && plane.lightInk) {
                if (!acquiredLightInk) { lightInk = components(raster, true); acquiredLightInk = true; }
                for (bool expanded = true; expanded;) {
                    expanded = false;
                    for (const auto& light : lightInk) {
                        const auto area = light.box;
                        if (light.count < 5 || area.height() < piece.height() * .25 || area.height() > piece.height() * 1.15
                            || area.width() > piece.height() * 2 || area.top < piece.top - 2 || area.bottom > piece.bottom + 2
                            || std::max(area.left - mask.right, mask.left - area.right) > piece.height() * 2)
                            continue;
                        if (!mask.contains(area)) { mask = mask.united(area); expanded = true; }
                    }
                }
            }
            if (complex) tight = mask.expanded(-1, -1);
            if (tight.empty()) continue;
            Row line;
            line.box = tight;
            line.masks = {mask.united(tight)};
            if (complex) line.estimated = tight.height() * .85;
            lines.push_back(std::move(line));
        }
    }

    std::sort(lines.begin(), lines.end(), [](Row const& a, Row const& b) { return a.box.cy() < b.box.cy(); });
    std::vector<Row> rows;
    for (auto& line : lines) {
        bool merged = false;
        for (auto row = rows.rbegin(); row != rows.rend(); ++row) {
            if (line.box.top - row->box.bottom > line.box.height()) break;
            if (std::abs(line.box.cy() - row->box.cy()) < std::max(line.box.height(), row->box.height()) * .45
                && std::max(line.box.left - row->box.right, row->box.left - line.box.right) < line.box.height() * 3
                && cellAt(cells, line.box) == cellAt(cells, row->box)) {
                row->box = row->box.united(line.box);
                row->masks.insert(row->masks.end(), line.masks.begin(), line.masks.end());
                row->estimated = std::max(row->estimated, line.estimated);
                merged = true;
                break;
            }
        }
        if (!merged) rows.push_back(std::move(line));
    }
    std::stable_sort(rows.begin(), rows.end(), [](Row const& a, Row const& b) { return a.box.top < b.box.top; });
    for (auto& row : rows) {
        row.heights.push_back(row.estimated > 0 ? row.estimated : row.box.height());
        int dark = 0;
        for (int y = row.box.top; y < row.box.bottom; ++y)
            for (int x = row.box.left; x < row.box.right; ++x)
                if (gray(raster.at(x, y)) < 180) ++dark;
        row.weight = static_cast<double>(dark) / std::max(1, row.box.width());
        row.bullet = std::any_of(ink.begin(), ink.end(), [&](Ink const& item) {
            const auto r = item.box;
            return r.height() >= row.box.height() * .15 && r.height() <= row.box.height() * .55
                && r.width() >= r.height() * .6 && r.width() <= r.height() * 1.5
                && r.right < row.box.left && row.box.left - r.right < row.box.height() * 3
                && std::abs(r.cy() - row.box.cy()) < row.box.height() * .35;
        });
    }
    return rows;
}

JsonObject finalLayout(JsonObject const& source, JsonArray const& blocks)
{
    auto regions = source.GetNamedArray(L"regions");
    std::map<hstring, JsonArray> children;
    for (const auto& item : regions) {
        const auto region = item.GetObject();
        const auto parent = region.GetNamedString(L"parent", L"");
        if (!parent.empty()) children[parent].Append(region.GetNamedValue(L"id"));
    }
    JsonArray order;
    for (const auto& value : blocks) {
        const auto block = value.GetObject();
        const auto id = block.GetNamedValue(L"id");
        const auto parent = block.GetNamedString(L"container", L"");
        if (!parent.empty()) children[parent].Append(id);
        order.Append(id);
    }
    for (const auto& item : regions) {
        const auto region = item.GetObject();
        region.Insert(L"children", children[region.GetNamedString(L"id")]);
    }
    source.Insert(L"order", order);
    return source;
}

std::vector<Pixels> obstacles(std::vector<Ink> const& ink, Raster const& raster)
{
    std::vector<Pixels> result;
    for (auto const& item : ink) {
        const auto& box = item.box;
        if (box.width() >= raster.w * .08 && box.height() >= raster.h * .025
            && item.count >= box.area() * (item.colored ? .3 : .7))
            result.push_back(box);
    }
    return result;
}

std::vector<Row> paragraphs(JsonObject const& analysis, std::vector<Row>& rows, std::vector<Pixels> const& graphics, double bodyHeight)
{
    std::vector<Row> result;
    double previousHeight = 0;
    double previousWeight = 0;
    for (auto const& item : analysis.GetNamedArray(L"items")) {
        const auto layout = item.GetObject();
        auto& row = rows.at(static_cast<size_t>(layout.GetNamedNumber(L"inputIndex")));
        row.layout = layout;
        bool join = false;
        if (!result.empty()) {
            const auto& last = result.back();
            const auto gap = row.box.top - last.box.bottom;
            if (layout.GetNamedString(L"container") == last.layout.GetNamedString(L"container")) {
                join = layout.GetNamedString(L"role") == L"cell" || (!row.bullet && gap > -row.box.height() * .25
                    && gap < std::min(previousHeight, static_cast<double>(row.box.height())) * .9
                    && std::abs(previousWeight - row.weight) <= std::min(previousWeight, row.weight) * .3
                    && std::abs(last.box.left - row.box.left) < bodyHeight * 1.5);
            }
            if (join && std::any_of(graphics.begin(), graphics.end(), [&](Pixels const& box) {
                return box.top >= last.box.bottom && box.bottom <= row.box.top
                    && box.right > std::min(last.box.left, row.box.left) && box.left < std::max(last.box.right, row.box.right);
            })) join = false;
        }
        previousHeight = row.box.height();
        previousWeight = row.weight;
        if (join) {
            auto& last = result.back();
            last.box = last.box.united(row.box);
            last.masks.insert(last.masks.end(), row.masks.begin(), row.masks.end());
            last.heights.insert(last.heights.end(), row.heights.begin(), row.heights.end());
            last.estimated = std::max(last.estimated, row.estimated);
        } else {
            result.push_back(std::move(row));
        }
    }
    return result;
}

Color panelColor(hstring const& value)
{
    if (value.size() != 7 || value[0] != L'#') return {255, 255, 255};
    Color color{};
    for (hstring::size_type index = 1; index < value.size(); ++index) {
        const auto c = value[index];
        const auto digit = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'A' && c <= L'F' ? c - L'A' + 10
            : c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
        if (digit < 0) return {255, 255, 255};
        auto& channel = color[(index - 1) / 2];
        channel = channel * 16 + digit;
    }
    return color;
}

std::vector<Pixels> gridRules(JsonArray const& detected, JsonObject const& layout, Raster const& raster)
{
    std::vector<JsonObject> tables;
    for (const auto& value : layout.GetNamedArray(L"regions")) {
        const auto region = value.GetObject();
        if (region.GetNamedString(L"kind") == L"table") tables.push_back(region);
    }
    std::vector<Pixels> result;
    for (const auto& value : detected) {
        const auto line = value.GetArray();
        const auto x0 = line.GetNumberAt(0), y0 = line.GetNumberAt(1);
        const auto x1 = line.GetNumberAt(2), y1 = line.GetNumberAt(3);
        const bool vertical = std::abs(x1 - x0) < 1;
        auto area = raster.pixels(values({x0, y0, x1 - x0 + raster.sx, y1 - y0 + raster.sy}));
        for (auto const& table : tables) {
            const auto bounds = raster.pixels(table.GetNamedArray(L"rect"));
            if (!bounds.expanded(4, 4).contains(area)) continue;
            const auto edges = table.GetNamedArray(vertical ? L"xEdges" : L"yEdges");
            bool matches = false;
            for (const auto& edge : edges)
                if (std::abs(edge.GetNumber() - (vertical ? x0 : y0)) < 2) { matches = true; break; }
            if (!matches) continue;
            result.push_back(area.expanded(2, 2).intersected(raster.bounds()));
            break;
        }
    }
    return result;
}

std::vector<Pixels> protectedInk(Raster const& raster, Plane const& plane, Pixels const& mask, Pixels const& area,
                                 std::vector<Pixels> const& grid, std::vector<Ink> const& ink)
{
    std::vector<Pixels> result;
    for (const auto& line : grid) if (line.intersects(area)) result.push_back(line);
    for (const auto& item : ink) {
        const auto& box = item.box;
        if (!box.intersects(area) || (box.width() > mask.width() + 8 && box.height() > mask.height() * 2
            && (item.count < box.area() * .25 || box.contains(mask)))) continue;
        if (box.height() > mask.height() * 2) {
            const auto overlap = box.intersected(area);
            if (overlap.area() > area.area() * .45) {
                int similar = 0, total = 0;
                const auto step = std::max(2, overlap.height() / 8);
                for (int y = overlap.top; y < overlap.bottom; y += step) for (int x = overlap.left; x < overlap.right; x += step) {
                    const auto actual = raster.at(x, y), background = plane.bg(x, y);
                    if (std::abs(actual[0] - background[0]) + std::abs(actual[1] - background[1])
                        + std::abs(actual[2] - background[2]) <= std::max(21., plane.noise * 4)) ++similar;
                    ++total;
                }
                if (similar >= total * .55) continue;
            }
        }
        if ((item.count > 8 && !mask.expanded(2, 2).contains(static_cast<int>(box.cx()), static_cast<int>(box.cy())))
            || box.height() > mask.height() * 1.5 || box.width() > mask.width() + 8
            || (box.width() > box.height() * 12 && box.width() > mask.height() * 2)
            || (item.colored && item.count > box.area() * .65 && box.width() > mask.height() * 2 && box.height() > mask.height() * .9))
            result.push_back(box.expanded(2, 2));
    }
    return result;
}

bool covered(std::vector<Pixels> const& protectedAreas, int x, int y)
{
    return std::any_of(protectedAreas.begin(), protectedAreas.end(), [&](Pixels const& box) { return box.contains(x, y); });
}

class BlockPixels {
public:
    BlockPixels(Raster const& raster, std::span<uint8_t> target, Pixels bounds)
        : m_target(target), m_stride(raster.rowSize), m_offset(raster.offset(bounds.left, bounds.top)),
          m_bounds(bounds), m_rowBytes(static_cast<size_t>(bounds.width()) * 4), m_bytes(m_rowBytes * bounds.height())
    {
        for (int y = 0; y < m_bounds.height(); ++y)
            std::memcpy(m_bytes.data() + y * m_rowBytes, m_target.data() + m_offset + y * m_stride, m_rowBytes);
    }

    void set(int x, int y, Color const& color)
    {
        pixel(m_bytes.data() + (y - m_bounds.top) * m_rowBytes + (x - m_bounds.left) * 4, color);
    }

    void commit() const
    {
        for (int y = 0; y < m_bounds.height(); ++y)
            std::memcpy(m_target.data() + m_offset + y * m_stride, m_bytes.data() + y * m_rowBytes, m_rowBytes);
    }

private:
    std::span<uint8_t> m_target;
    size_t m_stride, m_offset;
    Pixels m_bounds;
    size_t m_rowBytes;
    std::vector<uint8_t> m_bytes;
};

hstring repair(Raster const& raster, Pixels const& text, Plane const& plane,
               std::vector<Pixels> const& preserved, BlockPixels& edited)
{
    const auto padding = std::clamp(text.height(), 12, 60);
    const auto area = text.expanded(padding, padding).intersected(raster.bounds());
    if (area.area() > 2'000'000) return L"背景修复区域过大，请拆分文字区域或使用底色框。";
    const auto w = area.width(), h = area.height(), iStride = w + 1;
    std::vector<uint32_t> integral(static_cast<size_t>(iStride) * (h + 1));
    std::vector<Color> working(static_cast<size_t>(w) * h);
    const auto index = [w](int x, int y) { return y * w + x; };
    for (int y = 0; y < h; ++y) {
        uint32_t sum = 0;
        for (int x = 0; x < w; ++x) {
            auto& color = working[index(x, y)];
            color = raster.at(area.left + x, area.top + y);
            sum += gray(color);
            integral[(y + 1) * iStride + x + 1] = integral[y * iStride + x + 1] + sum;
        }
    }
    std::vector<uint8_t> holes(working.size());
    const auto radius = std::clamp(text.height() / 2, 4, 25);
    const auto threshold = std::clamp(plane.noise * .8, 18., 42.);
    for (int y = text.top; y < text.bottom; ++y) for (int x = text.left; x < text.right; ++x) {
        if (covered(preserved, x, y)) continue;
        const auto px = x - area.left, py = y - area.top;
        const auto x0 = std::max(0, px - radius), x1 = std::min(w, px + radius + 1);
        const auto y0 = std::max(0, py - radius), y1 = std::min(h, py + radius + 1);
        const auto sum = integral[y1 * iStride + x1] - integral[y0 * iStride + x1]
            - integral[y1 * iStride + x0] + integral[y0 * iStride + x0];
        const auto mean = static_cast<double>(sum) / ((x1 - x0) * (y1 - y0));
        const auto luma = gray(working[index(px, py)]);
        if (plane.lightInk ? luma <= mean + threshold : luma >= mean - threshold) continue;
        for (int yy = std::max(text.top, y - 1); yy < std::min(text.bottom, y + 2); ++yy)
            for (int xx = std::max(text.left, x - 1); xx < std::min(text.right, x + 2); ++xx)
                if (!covered(preserved, xx, yy)) holes[index(xx - area.left, yy - area.top)] = 1;
    }
    const auto total = std::count(holes.begin(), holes.end(), 1);
    if (total < 3 || total > text.area() * .8)
        return L"无法可靠分离文字与背景，请使用底色框或保留原图对照。";
    const auto originalHoles = holes;
    std::deque<int> queue;
    std::vector<uint8_t> queued(holes.size());
    constexpr std::array<std::pair<int, int>, 4> neighbors{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
    const auto enqueue = [&](int x, int y) {
        if (x < 1 || y < 1 || x + 1 >= w || y + 1 >= h || !holes[index(x, y)] || queued[index(x, y)]) return;
        for (const auto& [dx, dy] : neighbors) {
            if (holes[index(x + dx, y + dy)]) continue;
            queued[index(x, y)] = 1;
            queue.push_back(index(x, y));
            break;
        }
    };
    for (int y = 1; y + 1 < h; ++y) for (int x = 1; x + 1 < w; ++x) enqueue(x, y);
    const auto search = std::clamp(text.height(), 12, 40), step = std::max(2, search / 6);
    while (!queue.empty()) {
        const auto i = queue.front(), x = i % w, y = i / w;
        queue.pop_front();
        auto color = plane.bg(area.left + x, area.top + y);
        double best = std::numeric_limits<double>::infinity();
        for (int yy = std::max(1, y - search); yy < std::min(h - 1, y + search); yy += step) {
            for (int xx = std::max(1, x - search); xx < std::min(w - 1, x + search); xx += step) {
                if (originalHoles[index(xx, yy)] || covered(preserved, area.left + xx, area.top + yy)) continue;
                double score = .05 * (std::abs(xx - x) + std::abs(yy - y));
                bool usable = true;
                for (const auto& [dx, dy] : neighbors) {
                    if (holes[index(x + dx, y + dy)]) continue;
                    if (originalHoles[index(xx + dx, yy + dy)]) { usable = false; break; }
                    const auto& a = working[index(x + dx, y + dy)];
                    const auto b = raster.at(area.left + xx + dx, area.top + yy + dy);
                    for (size_t c = 0; c < a.size(); ++c) score += std::abs(a[c] - b[c]);
                }
                if (usable && score < best) { best = score; color = raster.at(area.left + xx, area.top + yy); }
            }
        }
        working[i] = color;
        holes[i] = 0;
        edited.set(area.left + x, area.top + y, color);
        for (const auto& [dx, dy] : neighbors) enqueue(x + dx, y + dy);
    }
    if (std::find(holes.begin(), holes.end(), 1) != holes.end())
        return L"文字过于接近图片边缘，请调整区域或使用底色框。";
    return {};
}

}

JsonObject analyzeScanPage(std::span<const uint8_t> bgra, int pixelWidth, int pixelHeight, int stride,
                           double pageWidth, double pageHeight, int pageNumber, std::filesystem::path const& ocrRuntime)
{
    const Raster raster(bgra, pixelWidth, pixelHeight, stride, pageWidth, pageHeight);
    const auto imageRules = rules(raster);
    const auto initial = analyzeScanRows(JsonArray{}, imageRules, pageWidth, pageHeight, pageNumber);
    const auto ink = components(raster);
    const auto boxes = ocrRuntime.empty() ? fallbackBoxes(ink, raster) : detectorBoxes(detections(raster, ocrRuntime), raster);
    if (boxes.size() > maxDetected) throw hresult_error(E_FAIL, L"本页文字区域过多，请拆分或裁切页面。");
    auto rows = rowsFromBoxes(raster, boxes, ink, cellBoxes(initial.GetNamedObject(L"layout"), raster));
    const auto graphics = obstacles(ink, raster);
    std::vector<double> heights, weights;
    int left = raster.w, right = 0;
    JsonArray inputs;
    for (auto const& row : rows) {
        heights.push_back(row.box.height());
        weights.push_back(row.weight);
        left = std::min(left, row.box.left);
        right = std::max(right, row.box.right);
        JsonObject data;
        data.Insert(L"rect", raster.points(row.box));
        put(data, L"size", row.heights.front() * raster.sy);
        inputs.Append(data);
    }
    const auto bodyHeight = median(std::move(heights), 20), bodyWeight = median(std::move(weights), 5);
    const auto analysis = analyzeScanRows(inputs, imageRules, pageWidth, pageHeight, pageNumber);
    const auto blocks = paragraphs(analysis, rows, graphics, bodyHeight);
    JsonArray output;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& block = blocks[i];
        const auto& box = block.box;
        const auto container = raster.pixels(block.layout.GetNamedArray(L"containerRect"));
        const bool cell = block.layout.GetNamedString(L"role") == L"cell";
        const auto lineHeight = median(block.heights);
        const bool title = lineHeight > bodyHeight * 1.25 || (!cell && block.masks.size() == 1 && block.weight > bodyWeight * 1.35);
        const bool centered = !cell && title && block.masks.size() == 1 && box.width() < (right - left) * .8
            && std::abs(box.cx() - container.cx()) < bodyHeight * 1.5;
        Pixels region{centered ? left : box.left, box.top, right,
            std::min(raster.h, box.bottom + static_cast<int>(std::ceil(lineHeight * 3)))};
        for (auto const& other : blocks) {
            if (other.box.top >= box.bottom && other.box.right > region.left && other.box.left < region.right)
                region.bottom = std::min(region.bottom, other.box.top - static_cast<int>(bodyHeight * .3));
        }
        for (auto const& graphic : graphics) {
            if (graphic.intersects(box)) continue;
            if (graphic.top < box.bottom && graphic.bottom > box.top && graphic.left >= box.right)
                region.right = std::min(region.right, graphic.left - 3);
            if (graphic.top >= box.bottom && graphic.left < region.right && graphic.right > region.left)
                region.bottom = std::min(region.bottom, graphic.top - 3);
        }
        region = region.united(box);
        if (!container.empty()) {
            const auto insetX = static_cast<int>(std::clamp(2 / raster.sx, 1.0, std::max(1.0, (container.width() - 3) * .5)));
            const auto insetY = static_cast<int>(std::clamp(2 / raster.sy, 1.0, std::max(1.0, (container.height() - 3) * .5)));
            const auto limit = cell ? container.expanded(-insetX, -insetY) : container;
            region = region.intersected(limit);
            if (cell && !region.empty()) {
                region.top = std::max(limit.top, box.top - 1);
                region.right = limit.right;
                region.bottom = limit.bottom;
            }
            if (region.empty()) region = box;
        }
        const auto artwork = block.masks.size() == 1 && lineHeight > bodyHeight * 1.5
            && std::any_of(ink.begin(), ink.end(), [&](Ink const& item) {
                const auto& graphic = item.box;
                return item.colored && item.count > 100 && graphic.height() >= lineHeight * 1.1 && graphic.height() < lineHeight * 3.5
                    && graphic.right <= box.left + lineHeight && box.left - graphic.right < lineHeight * 2.5
                    && graphic.top < box.bottom && graphic.bottom > box.top;
            });
        JsonObject item;
        put(item, L"id", hstring(L"scan-p" + std::to_wstring(pageNumber) + L"-b" + std::to_wstring(i)));
        item.Insert(L"rect", raster.points(box));
        item.Insert(L"region", raster.points(region));
        JsonArray masks;
        for (auto const& mask : block.masks) masks.Append(raster.points(mask.expanded(1, 1).intersected(raster.bounds())));
        item.Insert(L"masks", masks);
        put(item, L"size", std::clamp((title || block.estimated > 0 ? lineHeight : std::max(lineHeight, bodyHeight * .9)) * raster.sy, 8., 50.));
        put(item, L"bold", title);
        put(item, L"centered", centered);
        put(item, L"serif", !title);
        put(item, L"scan", true);
        put(item, L"artwork", artwork);
        put(item, L"order", static_cast<double>(i));
        for (auto key : {L"container", L"containerRect", L"role", L"table", L"row", L"column", L"rowSpan", L"columnSpan"})
            if (block.layout.HasKey(key)) item.Insert(key, block.layout.GetNamedValue(key));
        const auto background = describe(fit(raster, box));
        item.Insert(L"background", background);
        item.Insert(L"color", background.GetNamedArray(L"textColor"));
        output.Append(item);
    }
    JsonObject result;
    result.Insert(L"blocks", output);
    result.Insert(L"layout", finalLayout(analysis.GetNamedObject(L"layout"), output));
    return result;
}

JsonArray eraseScanText(std::span<uint8_t> bgra, int pixelWidth, int pixelHeight, int stride,
                        double pageWidth, double pageHeight, JsonArray const& blocks)
{
    JsonArray failures;
    if (blocks.Size() == 0) return failures;
    const Raster output(bgra, pixelWidth, pixelHeight, stride, pageWidth, pageHeight);
    const auto pixels = bgra.first(static_cast<size_t>(output.rowSize) * output.h);
    const std::vector<uint8_t> original(pixels.begin(), pixels.end());
    const Raster raster(original, pixelWidth, pixelHeight, stride, pageWidth, pageHeight);
    const auto imageRules = rules(raster);
    const auto structure = analyzeScanRows(JsonArray{}, imageRules, pageWidth, pageHeight, 0);
    const auto grid = gridRules(imageRules, structure.GetNamedObject(L"layout"), raster);
    const auto ink = components(raster);
    for (auto const& value : blocks) {
        const auto block = value.GetObject();
        const auto mode = block.GetNamedString(L"backgroundMode", L"auto");
        if (mode == L"original") continue;
        const auto rect = raster.pixels(block.GetNamedArray(L"rect", nullptr));
        auto clip = raster.bounds();
        if (block.GetNamedString(L"role", L"") == L"cell") {
            const auto cell = raster.pixels(block.GetNamedArray(L"containerRect", nullptr));
            if (!cell.empty()) {
                const auto dx = static_cast<int>(std::clamp(std::ceil(1.25 / raster.sx), 2., std::max(2., (cell.width() - 3) * .5)));
                const auto dy = static_cast<int>(std::clamp(std::ceil(1.25 / raster.sy), 2., std::max(2., (cell.height() - 3) * .5)));
                clip = cell.expanded(-dx, -dy);
            }
        }
        if (mode == L"panel") {
            const auto color = panelColor(block.GetNamedString(L"panelColor", L"#ffffff"));
            const auto region = raster.pixels(block.GetNamedArray(L"region", nullptr));
            for (auto const& box : {rect.empty() ? rect : rect.expanded(2, 2), region}) {
                const auto area = box.intersected(clip);
                for (int y = area.top; y < area.bottom; ++y) for (int x = area.left; x < area.right; ++x)
                    if (!covered(grid, x, y)) pixel(bgra.data() + raster.offset(x, y), color);
            }
            continue;
        }
        const auto masks = block.GetNamedArray(L"masks", nullptr);
        std::vector<Pixels> maskBoxes;
        Pixels editingBounds;
        if (masks) for (auto const& item : masks) {
            const auto mask = raster.pixels(item.GetArray());
            if (mask.empty()) continue;
            maskBoxes.push_back(mask);
            editingBounds = editingBounds.united(mask.expanded(6, 6).intersected(clip));
        }
        hstring error;
        if (!editingBounds.empty()) {
            BlockPixels edited(raster, bgra, editingBounds);
            for (auto const& mask : maskBoxes) {
                const auto sample = mask.expanded(2, 2).intersected(raster.bounds());
                const auto plane = fit(raster, sample);
                const auto background = plane.bg(sample.cx(), sample.cy());
                const auto kind = plane.kind();
                const auto bright = kind == L"solid" && *std::min_element(background.begin(), background.end()) >= 225
                    && std::abs(background[0] - background[2]) <= 5;
                const auto area = (mode == L"auto" && bright ? mask.expanded(6, 6) : sample).intersected(clip);
                if (area.empty()) continue;
                const auto preserved = protectedInk(raster, plane, mask, area, grid, ink);
                if (mode == L"repair") {
                    error = repair(raster, area, plane, preserved, edited);
                } else if (kind == L"texture") {
                    error = L"文字位于复杂背景，请选择背景修复、底色框或保留原图对照。";
                } else {
                    for (int y = area.top; y < area.bottom; ++y) for (int x = area.left; x < area.right; ++x)
                        if (!covered(preserved, x, y)) edited.set(x, y, plane.bg(x, y));
                }
                if (!error.empty()) break;
            }
            if (error.empty()) edited.commit();
        } else {
            error = L"文字区域缺少可清理的遮罩，请重新框选文字。";
        }
        if (!error.empty()) {
            JsonObject failed;
            failed.Insert(L"id", block.GetNamedValue(L"id", JsonValue::CreateStringValue(L"")));
            put(failed, L"error", error);
            failures.Append(failed);
        }
    }
    return failures;
}

}
