#include "pdfanalysis.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#undef GetObject
#include <winrt/Windows.Data.Text.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Data::Text;

namespace yiyi::pdf {
namespace {

struct Rect {
    double left{}, top{}, right{}, bottom{};

    static Rect fromSize(double x, double y, double width, double height)
    {
        return {x, y, x + width, y + height};
    }

    double width() const { return right - left; }
    double height() const { return bottom - top; }
    double cx() const { return (left + right) / 2; }
    double cy() const { return (top + bottom) / 2; }
    bool empty() const { return right <= left || bottom <= top; }
    double area() const { return empty() ? 0 : width() * height(); }
    bool contains(double x, double y) const { return x >= left && x <= right && y >= top && y <= bottom; }
    bool contains(Rect const& box) const { return contains(box.left, box.top) && contains(box.right, box.bottom); }
    bool intersects(Rect const& box) const
    {
        return left < box.right && right > box.left && top < box.bottom && bottom > box.top;
    }
    Rect intersected(Rect const& box) const
    {
        Rect result{std::max(left, box.left), std::max(top, box.top), std::min(right, box.right), std::min(bottom, box.bottom)};
        return result.empty() ? Rect{} : result;
    }
    Rect united(Rect const& box) const
    {
        if (empty()) return box;
        if (box.empty()) return *this;
        return {std::min(left, box.left), std::min(top, box.top), std::max(right, box.right), std::max(bottom, box.bottom)};
    }
    Rect padded(double horizontal, double vertical) const
    {
        return {left - horizontal, top - vertical, right + horizontal, bottom + vertical};
    }
};

using Color = std::array<double, 3>;

struct Glyph {
    std::wstring text;
    Rect box;
    Color color{};
    double baseline{};
    double size{};
    int object{-1};
    int index{-1};
    int script{};
    bool bold{};
    bool italic{};
    bool serif{};
    bool mono{};
    bool spaceBefore{};
};

struct StyleRun {
    std::wstring text;
    bool bold{};
    bool italic{};
    int script{};
};

struct Line {
    std::wstring text;
    Rect box;
    Color color{};
    double baseline{};
    double size{};
    std::vector<StyleRun> runs;
    std::vector<Rect> masks;
    std::set<int> objects;
    size_t glyphCount{};
    size_t boldCount{};
    size_t serifCount{};
    size_t monoCount{};
    size_t proseFontLetters{};
    bool bold{};
    bool marker{};
    int container{-1};
    int inputIndex{-1};
    int order{};
    bool indexEntry{};

    bool code() const { return monoCount * 5 > glyphCount * 4; }
};

enum class RegionKind { Flow, Column, Table, Cell };

struct Region {
    std::wstring id;
    RegionKind kind{RegionKind::Flow};
    Rect box;
    int parent{-1};
    int row{};
    int column{};
    int rowSpan{1};
    int columnSpan{1};
    int rows{};
    int columns{};
    bool inferred{};
    std::vector<double> xEdges;
    std::vector<double> yEdges;
};

struct Rule {
    bool horizontal{};
    double coordinate{};
    double start{};
    double end{};

    Rect bounds() const
    {
        return horizontal ? Rect{start, coordinate - .25, end, coordinate + .25}
                          : Rect{coordinate - .25, start, coordinate + .25, end};
    }
};

struct Graphic {
    Rect box;
    int object{-1};
    int type{};
    std::optional<Color> fill;
    std::vector<Rule> rules;
};

struct Artwork {
    std::vector<Graphic> graphics;
    std::vector<Rule> rules;
    std::vector<Graphic> backgroundCandidates;
    double largestImage{};
    bool visibleArtwork{};
    bool rasterBackground{};
};

[[noreturn]] void fail(hstring const& message)
{
    throw hresult_error(E_FAIL, message);
}

JsonArray numbers(std::initializer_list<double> input)
{
    JsonArray result;
    for (const auto value : input) result.Append(JsonValue::CreateNumberValue(value));
    return result;
}

JsonArray numbers(std::vector<double> const& input)
{
    JsonArray result;
    for (const auto value : input) result.Append(JsonValue::CreateNumberValue(value));
    return result;
}

JsonArray jsonRect(Rect const& box)
{
    return numbers({box.left, box.top, box.width(), box.height()});
}

std::optional<Rect> rect(JsonArray const& value)
{
    if (!value || value.Size() != 4) return std::nullopt;
    const auto x = value.GetNumberAt(0), y = value.GetNumberAt(1);
    const auto w = value.GetNumberAt(2), h = value.GetNumberAt(3);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h)
        || w < 0 || h < 0 || std::abs(x) > 1e7 || std::abs(y) > 1e7 || w > 1e7 || h > 1e7)
        return std::nullopt;
    return Rect::fromSize(x, y, w, h);
}

Rect rect(JsonObject const& object)
{
    return rect(object.GetNamedArray(L"rect", nullptr)).value_or(Rect{});
}

void put(JsonObject const& object, hstring const& key, double value) { object.Insert(key, JsonValue::CreateNumberValue(value)); }
void put(JsonObject const& object, hstring const& key, int value) { put(object, key, static_cast<double>(value)); }
void put(JsonObject const& object, hstring const& key, bool value) { object.Insert(key, JsonValue::CreateBooleanValue(value)); }
void put(JsonObject const& object, hstring const& key, hstring const& value) { object.Insert(key, JsonValue::CreateStringValue(value)); }
void put(JsonObject const& object, hstring const& key, wchar_t const* value) { put(object, key, hstring(value)); }
void put(JsonObject const& object, hstring const& key, std::wstring const& value) { put(object, key, hstring(value)); }
void put(JsonObject const& object, hstring const& key, Rect const& value) { object.Insert(key, jsonRect(value)); }

std::optional<Color> color(JsonArray const& value)
{
    if (!value || (value.Size() != 3 && value.Size() != 4)) return std::nullopt;
    if (value.Size() == 4 && value.GetNumberAt(3) != 255) return std::nullopt;
    return Color{std::clamp(value.GetNumberAt(0), 0.0, 255.0), std::clamp(value.GetNumberAt(1), 0.0, 255.0),
                 std::clamp(value.GetNumberAt(2), 0.0, 255.0)};
}

std::wstring lower(std::wstring_view text)
{
    std::wstring result(text);
    if (!text.empty() && !LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), static_cast<int>(text.size()),
                                        result.data(), static_cast<int>(result.size()), nullptr, nullptr, 0)) {
        std::transform(result.begin(), result.end(), result.begin(), [](wchar_t letter) { return static_cast<wchar_t>(std::towlower(letter)); });
    }
    return result;
}

bool space(wchar_t ch)
{
    constexpr std::wstring_view spaces = L" \t\r\n\f\v\u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005"
                                         L"\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000";
    return spaces.find(ch) != spaces.npos;
}

std::wstring_view trim(std::wstring_view value)
{
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    while (!value.empty() && space(value.back())) value.remove_suffix(1);
    return value;
}

uint32_t codeAt(std::wstring_view text, size_t& i)
{
    uint32_t code = text[i];
    if (code >= 0xd800 && code <= 0xdbff && i + 1 < text.size()) {
        const uint32_t low = text[i + 1];
        if (low >= 0xdc00 && low <= 0xdfff) {
            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
            ++i;
        }
    }
    return code;
}

bool letter(uint32_t code)
{
    if (code < 128) return (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z');
    const auto category = UnicodeCharacters::GetGeneralCategory(code);
    return category >= UnicodeGeneralCategory::UppercaseLetter && category <= UnicodeGeneralCategory::OtherLetter;
}

bool hasLetter(std::wstring_view text)
{
    for (size_t i = 0; i < text.size(); ++i)
        if (letter(codeAt(text, i))) return true;
    return false;
}

bool digit(wchar_t ch) { return (ch >= L'0' && ch <= L'9') || (ch >= L'０' && ch <= L'９'); }

bool link(std::wstring_view text)
{
    return text.starts_with(L"http://") || text.starts_with(L"https://") || text.starts_with(L"www.");
}

size_t meaningfulLetters(std::wstring_view text)
{
    const auto folded = lower(text);
    size_t result = 0;
    for (size_t i = 0; i < text.size();) {
        const auto start = i;
        while (i < text.size() && !space(text[i])) ++i;
        if (!link(std::wstring_view(folded).substr(start, i - start))) {
            for (size_t j = start; j < i; ++j)
                if (letter(codeAt(text, j)) && ++result >= 12) return result;
        }
        while (i < text.size() && space(text[i])) ++i;
    }
    return result;
}

double median(std::vector<double> values, double fallback = 12)
{
    if (values.empty()) return fallback;
    const auto middle = values.begin() + static_cast<ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

hstring hashBlocks(JsonArray const& blocks)
{
    const auto bytes = to_string(blocks.Stringify());
    BCRYPT_ALG_HANDLE algorithm{};
    check_nt(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    std::array<UCHAR, 32> digest{};
    const auto status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                   static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    check_nt(status);
    constexpr std::wstring_view hex = L"0123456789abcdef";
    std::wstring result;
    result.reserve(64);
    for (const auto value : digest) {
        result += hex[value >> 4];
        result += hex[value & 15];
    }
    return hstring(result);
}

int integer(JsonObject const& object, hstring const& key, int fallback = -1)
{
    const auto value = object.GetNamedNumber(key, fallback);
    if (!std::isfinite(value) || value < (std::numeric_limits<int>::min)() || value > (std::numeric_limits<int>::max)()
        || std::floor(value) != value)
        return fallback;
    return static_cast<int>(value);
}

std::optional<Rule> readRule(JsonArray const& line)
{
    if (!line || line.Size() < 4) return std::nullopt;
    const auto x = line.GetNumberAt(0), y = line.GetNumberAt(1), u = line.GetNumberAt(2), v = line.GetNumberAt(3);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(u) || !std::isfinite(v)) return std::nullopt;
    if (std::abs(v - y) < 1 && std::abs(u - x) > 8) return Rule{true, (y + v) / 2, std::min(x, u), std::max(x, u)};
    if (std::abs(u - x) < 1 && std::abs(v - y) > 8) return Rule{false, (x + u) / 2, std::min(y, v), std::max(y, v)};
    return std::nullopt;
}

std::vector<Rule> readRules(JsonArray const& source)
{
    std::vector<Rule> output;
    if (!source) return output;
    for (const auto& value : source) {
        if (value.ValueType() != JsonValueType::Array) continue;
        if (const auto rule = readRule(value.GetArray())) output.push_back(*rule);
    }
    return output;
}

Artwork artwork(JsonObject const& raw, Rect const& page)
{
    Artwork info;
    info.rules = readRules(raw.GetNamedArray(L"rules", nullptr));
    info.rasterBackground = raw.GetNamedBoolean(L"rasterBackground", false);
    const auto objects = raw.GetNamedArray(L"objects", nullptr);
    if (!objects) return info;
    if (objects.Size() > 20000) fail(L"页面对象过多，请拆分或简化 PDF。");
    for (const auto& value : objects) {
        const auto item = value.GetObject();
        const auto type = integer(item, L"type");
        if (type == 1 || type == 5) continue; // Text and form containers are represented by their members.
        if (type == 4 || item.GetNamedBoolean(L"rasterBackground", false)) info.rasterBackground = true;
        const auto box = rect(item);
        if (box.empty()) continue;
        const bool full = box.width() >= page.width() * .95 && box.height() >= page.height() * .95;
        if (type == 3) info.largestImage = std::max(info.largestImage, box.intersected(page).area());
        if (type == 3 || !full) info.visibleArtwork = true;
        Graphic graphic{box, integer(item, L"id"), type, color(item.GetNamedArray(L"fillColor", nullptr)),
                        readRules(item.GetNamedArray(L"rules", nullptr))};
        if (type == 2 && graphic.rules.empty()) {
            if (box.height() <= 3 && box.width() >= 10) graphic.rules.push_back({true, box.cy(), box.left, box.right});
            else if (box.width() <= 3 && box.height() >= 10) graphic.rules.push_back({false, box.cx(), box.top, box.bottom});
        }
        info.rules.insert(info.rules.end(), graphic.rules.begin(), graphic.rules.end());
        if (graphic.fill || type == 3) info.backgroundCandidates.push_back(graphic);
        if (!full && !graphic.fill && !graphic.rules.empty()
            && item.GetNamedBoolean(L"strokeOnly", true)) {
            // Row and column strokes constrain text only where ink was painted.
            for (const auto& rule : graphic.rules)
                info.graphics.push_back({rule.bounds(), graphic.object, type, {}, {}});
        } else if (!full) {
            info.graphics.push_back(std::move(graphic));
        }
    }
    return info;
}

std::vector<Glyph> glyphs(JsonObject const& raw, bool& incomplete, bool& slanted)
{
    std::vector<Glyph> result;
    const auto input = raw.GetNamedArray(L"glyphs", nullptr);
    if (!input) return result;
    if (input.Size() > 50000) fail(L"单页文字过多，请简化此页或使用识图处理。");
    result.reserve(input.Size());
    for (const auto& value : input) {
        const auto item = value.GetObject();
        const auto text = item.GetNamedString(L"text", L"");
        const auto area = rect(item.GetNamedArray(L"rect", nullptr));
        const auto baseline = item.GetNamedNumber(L"baseline", -1e20);
        const auto size = item.GetNamedNumber(L"size", 0);
        const auto object = integer(item, L"object");
        const auto index = integer(item, L"textIndex");
        if (text.empty() || !area || !std::isfinite(baseline) || std::abs(baseline) > 1e7 || size <= 0 || size > 200
            || !std::isfinite(size) || object < 0 || index < 0) {
            incomplete = true;
            continue;
        }
        if (std::abs(item.GetNamedNumber(L"angle", 0)) > .02) slanted = true;
        auto box = *area;
        if (box.width() <= 0) box.right = box.left + .01;
        if (box.height() <= 0) box.bottom = box.top + .01;
        const auto name = lower(item.GetNamedString(L"fontName", L""));
        const auto flags = integer(item, L"fontFlags", 0);
        const auto has = [&](std::wstring_view fragment) { return name.find(fragment) != name.npos; };
        result.push_back({
            std::wstring(text), box, color(item.GetNamedArray(L"color", nullptr)).value_or(Color{}), baseline, size, object, index, 0,
            item.GetNamedNumber(L"fontWeight", 0) >= 600 || (flags & (1 << 18)) != 0 || has(L"bold") || has(L"demi"),
            (flags & 64) != 0 || has(L"italic") || has(L"oblique"),
            (flags & 2) != 0 || has(L"times") || has(L"serif"),
            (flags & 1) != 0 || has(L"courier") || has(L"mono") || has(L"consolas"),
            item.GetNamedBoolean(L"spaceBefore", false),
        });
    }
    return result;
}

class Connected {
public:
    explicit Connected(size_t count) : m_parent(count)
    {
        std::iota(m_parent.begin(), m_parent.end(), size_t{});
    }
    size_t root(size_t id)
    {
        while (id != m_parent[id]) {
            m_parent[id] = m_parent[m_parent[id]];
            id = m_parent[id];
        }
        return id;
    }
    void join(size_t first, size_t second) { m_parent[root(second)] = root(first); }

private:
    std::vector<size_t> m_parent;
};

std::vector<double> coordinates(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    std::vector<double> result;
    int count = 0;
    for (const auto value : values) {
        if (result.empty() || value - result.back() > 2) {
            count = 1;
            result.push_back(value);
        } else {
            result.back() = (result.back() * count + value) / (count + 1);
            ++count;
        }
    }
    return result;
}

bool wall(std::vector<Rule> const& rules, bool horizontal, double coordinate, double first, double last)
{
    std::vector<std::pair<double, double>> spans;
    for (const auto& rule : rules) {
        if (rule.horizontal == horizontal && std::abs(rule.coordinate - coordinate) <= 2
            && rule.start < last && rule.end > first) {
            spans.emplace_back(std::max(first, rule.start), std::min(last, rule.end));
        }
    }
    std::sort(spans.begin(), spans.end());
    double covered = 0;
    double position = first;
    for (const auto& [start, end] : spans) {
        if (end <= position) continue;
        covered += end - std::max(position, start);
        position = end;
    }
    return covered >= (last - first) * .88;
}

struct CellSpan {
    int row{};
    int column{};
    int rowSpan{1};
    int columnSpan{1};
};

void addTable(std::vector<Region>& regions, std::wstring id, std::vector<double> xEdges,
              std::vector<double> yEdges, std::vector<CellSpan> const& cells, bool inferred)
{
    const auto parent = static_cast<int>(regions.size());
    Region table;
    table.id = std::move(id);
    table.kind = RegionKind::Table;
    table.box = {xEdges.front(), yEdges.front(), xEdges.back(), yEdges.back()};
    table.rows = static_cast<int>(yEdges.size() - 1);
    table.columns = static_cast<int>(xEdges.size() - 1);
    table.inferred = inferred;
    table.xEdges = xEdges;
    table.yEdges = yEdges;
    regions.push_back(std::move(table));
    for (const auto& members : cells) {
        Region cell;
        cell.id = regions[parent].id + L"-r" + std::to_wstring(members.row) + L"c" + std::to_wstring(members.column);
        cell.kind = RegionKind::Cell;
        cell.box = {xEdges[members.column], yEdges[members.row],
                    xEdges[members.column + members.columnSpan], yEdges[members.row + members.rowSpan]};
        cell.parent = parent;
        cell.row = members.row;
        cell.column = members.column;
        cell.rowSpan = members.rowSpan;
        cell.columnSpan = members.columnSpan;
        regions.push_back(std::move(cell));
    }
}

std::vector<Region> ruledTables(std::vector<Rule> const& rules, Rect const& page, std::wstring const& prefix)
{
    if (rules.size() > 3000) return {};
    Connected connected(rules.size());
    for (size_t i = 0; i < rules.size(); ++i) {
        for (size_t j = i + 1; j < rules.size(); ++j) {
            const auto& a = rules[i];
            const auto& b = rules[j];
            const bool intersects = a.horizontal == b.horizontal
                ? std::abs(a.coordinate - b.coordinate) <= 2 && a.start <= b.end + 2 && a.end >= b.start - 2
                : a.coordinate >= b.start - 2 && a.coordinate <= b.end + 2 && b.coordinate >= a.start - 2 && b.coordinate <= a.end + 2;
            if (intersects) connected.join(i, j);
        }
    }
    std::map<size_t, std::vector<Rule>> groups;
    for (size_t i = 0; i < rules.size(); ++i) groups[connected.root(i)].push_back(rules[i]);
    std::vector<Region> regions;
    size_t tableIndex = 0;
    for (auto const& [_, group] : groups) {
        std::vector<double> x, y;
        for (const auto& rule : group) (rule.horizontal ? y : x).push_back(rule.coordinate);
        x = coordinates(std::move(x));
        y = coordinates(std::move(y));
        if (x.size() < 3 || y.size() < 3 || x.size() > 61 || y.size() > 151) continue;
        const auto cols = static_cast<int>(x.size() - 1);
        const auto rows = static_cast<int>(y.size() - 1);
        if (rows * cols > 3000 || !page.padded(1, 1).contains(Rect{x.front(), y.front(), x.back(), y.back()})) continue;
        if (!wall(group, true, y.front(), x.front(), x.back()) || !wall(group, true, y.back(), x.front(), x.back())
            || !wall(group, false, x.front(), y.front(), y.back()) || !wall(group, false, x.back(), y.front(), y.back()))
            continue; // Unclosed grid motifs are usually diagrams, not tables.
        Connected cells(static_cast<size_t>(rows) * cols);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const auto index = static_cast<size_t>(r) * cols + c;
                if (c + 1 < cols && !wall(group, false, x[c + 1], y[r], y[r + 1])) cells.join(index, index + 1);
                if (r + 1 < rows && !wall(group, true, y[r + 1], x[c], x[c + 1])) cells.join(index, index + cols);
            }
        }
        std::map<size_t, std::vector<int>> merged;
        for (int i = 0; i < cols * rows; ++i) merged[cells.root(i)].push_back(i);
        if (merged.size() < 3) continue;
        std::vector<CellSpan> result;
        for (const auto& [root, members] : merged) {
            int top = rows, left = cols, bottom = 0, right = 0;
            for (const auto index : members) {
                const int row = index / cols, column = index % cols;
                top = std::min(top, row);
                left = std::min(left, column);
                bottom = std::max(bottom, row);
                right = std::max(right, column);
            }
            const auto rowSpan = bottom - top + 1, columnSpan = right - left + 1;
            if (members.size() != static_cast<size_t>(rowSpan * columnSpan) || x[right + 1] - x[left] < 8 || y[bottom + 1] - y[top] < 8)
                break; // Do not invent a rectangular cell around an L-shaped merge.
            result.push_back({top, left, rowSpan, columnSpan});
        }
        if (result.size() == merged.size()) {
            std::sort(result.begin(), result.end(), [](CellSpan const& a, CellSpan const& b) {
                return std::tie(a.row, a.column) < std::tie(b.row, b.column);
            });
            addTable(regions, prefix + L"-table" + std::to_wstring(tableIndex++), std::move(x), std::move(y), result, false);
        }
    }
    return regions;
}

int cellAt(std::vector<Region> const& regions, Rect const& box)
{
    for (size_t i = 0; i < regions.size(); ++i) {
        if (regions[i].kind == RegionKind::Cell && regions[i].box.contains(box.cx(), box.cy())
            && regions[i].box.padded(1, 1).contains(box))
            return static_cast<int>(i);
    }
    return -1;
}

size_t wordCount(std::wstring_view text)
{
    bool inside = false;
    size_t count = 0;
    for (const auto ch : text) {
        if (space(ch)) inside = false;
        else if (!inside) {
            ++count;
            inside = true;
        }
    }
    return count;
}

bool numericCell(std::wstring_view text)
{
    text = trim(text);
    if (!text.empty() && (text.front() == L'+' || text.front() == L'-' || text.front() == L'−')) text.remove_prefix(1);
    size_t end = 0;
    bool hasDigit = false;
    while (end < text.size() && (digit(text[end]) || text[end] == L'.' || text[end] == L',')) {
        hasDigit = hasDigit || digit(text[end]);
        ++end;
    }
    const auto unit = trim(text.substr(end));
    if (!hasDigit || unit.size() > 8) return false;
    return std::all_of(unit.begin(), unit.end(), [](wchar_t ch) {
        return letter(ch) || std::wstring_view(L"%％°µμ/²³").find(ch) != std::wstring_view::npos;
    });
}

void inferredTables(std::vector<Line> const& lines, std::vector<Region>& regions,
                    Rect const& page, std::wstring const& prefix, double h)
{
    std::vector<Line const*> ordered;
    for (const auto& line : lines) if (cellAt(regions, line.box) < 0) ordered.push_back(&line);
    std::stable_sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->box.cy() < b->box.cy(); });
    std::vector<std::vector<Line const*>> rows;
    for (const auto* item : ordered) {
        if (rows.empty() || std::abs(rows.back().front()->box.cy() - item->box.cy()) > h * .7) rows.emplace_back();
        rows.back().push_back(item);
    }
    for (auto& row : rows)
        std::sort(row.begin(), row.end(), [](auto a, auto b) { return a->box.left < b->box.left; });
    size_t index = 0;
    for (size_t first = 0; first < rows.size(); ++first) {
        const auto cols = rows[first].size();
        if (cols < 2 || cols > 30) continue;
        auto last = first;
        for (auto r = first + 1; r < rows.size(); ++r) {
            if (rows[r].size() != cols || rows[r].front()->box.top - rows[r - 1].front()->box.bottom > h * 5) break;
            bool aligned = true;
            for (size_t column = 0; column < cols; ++column) {
                auto a = rows[first][column]->box;
                auto b = rows[r][column]->box;
                if (std::abs(a.left - b.left) > h && std::abs(a.right - b.right) > h) aligned = false;
            }
            if (!aligned) break;
            last = r;
        }
        if (last < first + 2 || (last - first + 1) * cols > 3000) continue;
        size_t compact = 0, numeric = 0, headerBold = 0, total = 0;
        bool hasText = false;
        std::vector<double> left(cols, 1e9), right(cols, -1e9), top, bottom;
        for (auto r = first; r <= last; ++r) {
            double low = 1e9, high = -1e9;
            for (size_t c = 0; c < cols; ++c) {
                const auto* item = rows[r][c];
                const auto box = item->box;
                left[c] = std::min(left[c], box.left);
                right[c] = std::max(right[c], box.right);
                low = std::min(low, box.top);
                high = std::max(high, box.bottom);
                const auto text = trim(item->text);
                hasText = hasText || !text.empty();
                if (text.empty() ? box.width() <= h * 10 : (wordCount(text) <= 3 && text.size() <= 30)) ++compact;
                if (numericCell(text)) ++numeric;
                if (r == first && item->bold) ++headerBold;
                ++total;
            }
            top.push_back(low);
            bottom.push_back(high);
        }
        bool spaced = true;
        for (size_t c = 1; c < cols; ++c) spaced = spaced && left[c] - right[c - 1] >= h * 1.2;
        // Repeated aligned prose belongs to columns. Compact values plus
        // headings or numbers distinguish a borderless table.
        if (!spaced || compact * 100 < total * 65
            || (hasText && numeric < 2 && headerBold * 2 < cols && (cols < 3 || compact * 10 < total * 9))
            || (!hasText && cols < 3 && last - first + 1 < 4))
            continue;
        std::vector<double> xs{std::max(0.0, left.front() - 3)}, ys{std::max(0.0, top.front() - h * .6)};
        for (size_t c = 1; c < cols; ++c) xs.push_back((right[c - 1] + left[c]) / 2);
        xs.push_back(right.back() + h);
        for (size_t r = 1; r < top.size(); ++r) ys.push_back((bottom[r - 1] + top[r]) / 2);
        ys.push_back(bottom.back() + h * .6);
        if (!page.contains(Rect{xs.front(), ys.front(), xs.back(), ys.back()})) continue;
        std::vector<CellSpan> cells;
        for (size_t r = 0; r <= last - first; ++r)
            for (size_t c = 0; c < cols; ++c)
                cells.push_back({static_cast<int>(r), static_cast<int>(c), 1, 1});
        addTable(regions, prefix + L"-inferred" + std::to_wstring(index++), std::move(xs), std::move(ys), cells, true);
        first = last;
    }
}

std::vector<std::vector<Glyph>> glyphRows(std::vector<Glyph> values)
{
    std::stable_sort(values.begin(), values.end(), [](Glyph const& a, Glyph const& b) { return a.baseline < b.baseline; });
    std::vector<std::vector<Glyph>> rows;
    for (auto& item : values) {
        if (rows.empty() || std::abs(rows.back().front().baseline - item.baseline) > std::max(1.5, item.size * .18))
            rows.emplace_back();
        rows.back().push_back(std::move(item));
    }
    std::vector<double> rowSizes;
    for (auto& row : rows) {
        std::stable_sort(row.begin(), row.end(), [](Glyph const& a, Glyph const& b) { return a.box.left < b.box.left; });
        std::vector<double> sizes;
        for (const auto& glyph : row) sizes.push_back(glyph.size);
        rowSizes.push_back(median(std::move(sizes)));
    }
    std::vector<std::vector<Glyph>> merged(rows.size());
    for (size_t r = 0; r < rows.size(); ++r) {
        auto& row = rows[r];
        for (size_t first = 0; first < row.size();) {
            auto last = first + 1;
            while (last < row.size() && row[last].box.left - row[last - 1].box.right < rowSizes[r] * .7) ++last;
            size_t parent = r;
            int script = 0;
            double distance = 1e9;
            std::wstring mapped;
            if (last - first <= 20 && !row[first].mono) {
                const auto base = row[first].baseline;
                auto p = std::lower_bound(rows.begin(), rows.end(), base - rowSizes[r] * 1.2,
                    [](auto const& list, double y) { return list.front().baseline < y; });
                for (; p != rows.end() && p->front().baseline <= base + rowSizes[r] * 1.2; ++p) {
                    auto index = static_cast<size_t>(p - rows.begin());
                    const auto size = rowSizes[index];
                    const auto offset = base - p->front().baseline;
                    if (index == r || size < rowSizes[r] * 1.2 || size > rowSizes[r] * 1.8
                        || std::abs(offset) < size * .12 || std::abs(offset) > size * .65) continue;
                    const std::wstring_view ordinary = offset < 0 ? L"0123456789+-−=()nTi,;" : L"0123456789+-−=()aeoxhklmnpstij,;";
                    const std::wstring_view superscript = offset < 0 ? L"⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻⁻⁼⁽⁾ⁿᵀⁱ,;" : L"₀₁₂₃₄₅₆₇₈₉₊₋₋₌₍₎ₐₑₒₓₕₖₗₘₙₚₛₜᵢⱼ,;";
                    std::wstring candidate;
                    bool styling = false;
                    for (auto g = first; g < last; ++g) {
                        if (row[g].text.size() != 1 || row[g].mono) { candidate.clear(); break; }
                        const auto ch = row[g].text.front();
                        const auto at = ordinary.find(ch);
                        if (at == ordinary.npos && !letter(ch) && !digit(ch)) { candidate.clear(); break; }
                        styling = styling || at == ordinary.npos;
                        candidate += at == ordinary.npos ? ch : superscript[at];
                    }
                    if (candidate.empty() || candidate.find_first_not_of(L",;") == candidate.npos) continue;
                    for (const auto& original : *p) {
                        const auto gap = row[first].box.left - original.box.right;
                        if (original.mono || gap < -size * .2 || gap > size * .6 || std::abs(offset) + std::abs(gap) >= distance) continue;
                        distance = std::abs(offset) + std::abs(gap);
                        parent = index;
                        mapped = candidate;
                        script = styling ? (offset < 0 ? 1 : -1) : 0;
                    }
                }
            }
            for (auto g = first; g < last; ++g) {
                auto glyph = std::move(row[g]);
                if (parent != r) {
                    if (script == 0) glyph.text = mapped.substr(g - first, 1);
                    if (glyph.text != L"," && glyph.text != L";") glyph.script = script;
                    glyph.baseline = rows[parent].front().baseline;
                    glyph.spaceBefore = false;
                }
                merged[parent].push_back(std::move(glyph));
            }
            first = last;
        }
    }
    std::erase_if(merged, [](auto const& row) { return row.empty(); });
    return merged;
}

void styleRun(Line& line, std::wstring_view text, bool bold = false, bool italic = false, int script = 0)
{
    if (!line.runs.empty() && line.runs.back().bold == bold && line.runs.back().italic == italic && line.runs.back().script == script)
        line.runs.back().text += text;
    else
        line.runs.push_back({std::wstring(text), bold, italic, script});
}

size_t countWords(std::wstring_view text, std::span<std::wstring_view const> choices)
{
    size_t total = 0;
    for (size_t start = 0; start < text.size();) {
        auto end = start;
        while (end < text.size() && text[end] >= L'a' && text[end] <= L'z') ++end;
        if (end > start && std::find(choices.begin(), choices.end(), text.substr(start, end - start)) != choices.end()) ++total;
        start = end + 1;
    }
    return total;
}

bool justified(std::vector<Glyph> const& row)
{
    std::wstring text;
    size_t gaps = 0;
    size_t spaces = 0;
    for (size_t i = 0; i < row.size(); ++i) {
        const auto& glyph = row[i];
        if (i) {
            const auto gap = glyph.box.left - row[i - 1].box.right;
            if (gap > std::max(24.0, glyph.size * 2.5)) ++gaps;
            if (glyph.spaceBefore || gap > glyph.size * .16) {
                text += L' ';
                ++spaces;
            }
        }
        text += glyph.text;
    }
    if (gaps < 3 || spaces < 3 || text.find_first_of(L";={}#") != text.npos) return false;
    constexpr std::wstring_view words[] = {
        L"the", L"we", L"have", L"a", L"an", L"of", L"to", L"is", L"are", L"in", L"with", L"using", L"from", L"as"
    };
    return countWords(lower(text), words) >= 2;
}

std::vector<Line> buildLines(std::vector<Glyph> values, Artwork const& art, std::vector<Region> const& regions, Rect const& page)
{
    std::vector<Line> result;
    for (auto& row : glyphRows(std::move(values))) {
        std::stable_sort(row.begin(), row.end(), [](Glyph const& a, Glyph const& b) { return a.box.left < b.box.left; });
        const bool prose = justified(row);
        Rect rowBox;
        for (const auto& glyph : row) rowBox = rowBox.united(glyph.box);
        std::vector<Graphic const*> obstacles, backgrounds;
        for (const auto& item : art.graphics)
            if (item.box.intersects(rowBox) && item.box.width() > 2 && item.box.height() > 2) obstacles.push_back(&item);
        for (const auto& item : art.backgroundCandidates)
            if (item.box.intersects(rowBox)) backgrounds.push_back(&item);

        Line line;
        int lastIndex = -2, previousCell = -1, previousPanel = -1;
        std::vector<double> sizes;
        const auto flush = [&] {
            if (line.text.empty()) return;
            line.bold = line.boldCount * 5 > line.glyphCount * 3;
            line.masks = {line.box};
            line.size = median(std::exchange(sizes, {}), line.size);
            result.push_back(std::exchange(line, {}));
        };
        for (const auto& glyph : row) {
            const auto gap = glyph.box.left - line.box.right;
            const auto cell = cellAt(regions, glyph.box);
            int panel = -1;
            double panelArea = page.area();
            for (const auto* candidate : backgrounds) {
                const auto area = candidate->box.area();
                if (candidate->object < glyph.object && area < panelArea && candidate->box.padded(.5, .5).contains(glyph.box)) {
                    panel = candidate->object;
                    panelArea = area;
                }
            }
            bool inlineArtwork = false;
            if (!line.text.empty() && gap > 1) {
                inlineArtwork = std::any_of(obstacles.begin(), obstacles.end(), [&](Graphic const* item) {
                    return item->box.left >= line.box.right - .5 && item->box.right <= glyph.box.left + .5
                        && item->box.top < std::max(glyph.box.bottom, line.box.bottom)
                        && item->box.bottom > std::min(glyph.box.top, line.box.top);
                });
            }
            const bool gutter = !prose && gap > std::max(24.0, glyph.size * 2.5)
                && !(glyph.mono && line.monoCount * 10 > line.glyphCount * 7);
            if (!line.text.empty() && (inlineArtwork || gutter || cell != previousCell || panel != previousPanel)) flush();
            if (line.text.empty()) {
                line.box = glyph.box;
                line.size = glyph.size;
                line.baseline = glyph.baseline;
                line.color = glyph.color;
            } else {
                // Keep consecutive digits in the same PDF text object together
                // even when their ink bounds exclude much of the advance width.
                if (glyph.spaceBefore || (glyph.index != lastIndex + 1 && gap > glyph.size * .16)) {
                    line.text += L' ';
                    styleRun(line, L" ", glyph.bold, glyph.italic);
                }
                line.box = line.box.united(glyph.box);
            }
            if (glyph.script != 0) line.text += glyph.script > 0 ? L"^{" : L"_{";
            line.text += glyph.text;
            if (glyph.script != 0) line.text += L'}';
            line.objects.insert(glyph.object);
            styleRun(line, glyph.text, glyph.bold, glyph.italic, glyph.script);
            ++line.glyphCount;
            if (glyph.bold) ++line.boldCount;
            if (glyph.serif) ++line.serifCount;
            if (glyph.mono) ++line.monoCount;
            size_t index = 0;
            if (!glyph.mono && letter(codeAt(glyph.text, index))) ++line.proseFontLetters;
            sizes.push_back(glyph.size);
            lastIndex = glyph.index;
            previousCell = cell;
            previousPanel = panel;
        }
        flush();
    }
    return result;
}

int commentStart(std::wstring_view text, bool* inBlock = nullptr)
{
    bool block = inBlock && *inBlock;
    bool escaped = false;
    wchar_t quote = 0;
    int first = -1;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto ch = text[i];
        const auto next = i + 1 < text.size() ? text[i + 1] : 0;
        if (block) {
            if (ch == L'*' && next == L'/') { block = false; ++i; }
        } else if (quote) {
            if (escaped) escaped = false;
            else if (ch == L'\\') escaped = true;
            else if (ch == quote) quote = 0;
        } else if (ch == L'"' || ch == L'\'') {
            quote = ch;
        } else if (ch == L'/' && (next == L'/' || next == L'*')) {
            if (first < 0) first = static_cast<int>(i);
            if (next == L'/') break;
            block = true;
            ++i;
        }
    }
    if (inBlock) *inBlock = block;
    return first;
}

bool wrappedComment(std::wstring_view previous, std::wstring_view next, bool hanging)
{
    if (previous.size() < 20 || next.empty() || commentStart(previous) < 0 || commentStart(next) >= 0
        || std::wstring_view(L".;:{}").find(previous.back()) != std::wstring_view::npos
        || next.find_first_of(L"{}#") != next.npos)
        return false;
    const auto word = lower(trim(next.substr(0, next.find_first_of(L" \t({;:"))));
    constexpr std::wstring_view language[] = {
        L"struct", L"class", L"enum", L"typedef", L"using", L"uniform", L"layout", L"const", L"varying", L"void",
        L"int", L"uint", L"float", L"double", L"return", L"if", L"else", L"for", L"while", L"switch", L"case", L"break", L"continue",
    };
    if (std::find(std::begin(language), std::end(language), word) != std::end(language)) return false;
    if (next.find_first_of(L";=") != next.npos && !hanging) return false;
    return wordCount(next) >= (hanging ? 1 : 2) && (hanging || previous.size() > 40);
}

void detectMonospacedProse(std::vector<Line>& lines)
{
    constexpr std::wstring_view lead[] = { L"for", L"and", L"this", L"the", L"to", L"in", L"as", L"here", L"when",
        L"these", L"note", L"an", L"a", L"constructs", L"returns", L"computes", L"calculates", L"creates", L"retrieves", L"sets", L"gets" };
    constexpr std::wstring_view phrases[] = { L"the", L"be", L"by", L"from", L"in", L"to", L"of", L"and", L"with", L"if", L"is", L"are", L"has" };
    bool block = false;
    bool previousProse = false;
    std::wstring previous;
    double previousLeft = 0;
    for (auto& line : lines) {
        bool prose = false;
        const auto text = lower(trim(line.text));
        const auto leading = std::wstring_view(text).substr(0, text.find_first_of(L" \t"));
        const bool continuation = line.code() && wrappedComment(previous, line.text, line.box.left < previousLeft - line.size);
        if (line.code() && !block && !continuation && commentStart(line.text) < 0 && text.find_first_of(L";={}#") == text.npos) {
            const auto words = wordCount(text);
            prose = (words >= 4 && std::find(std::begin(lead), std::end(lead), leading) != std::end(lead))
                || (words >= 6 && countWords(text, phrases) >= 2)
                || (previousProse && words > 1 && std::iswlower(line.text.front()));
        }
        previousProse = prose && std::wstring_view(L".!?:").find(line.text.back()) == std::wstring_view::npos;
        if (prose) line.monoCount = 0;
        if (line.code()) {
            commentStart(line.text, &block);
            if (continuation) previous += L" " + line.text;
            else previous = line.text;
        } else {
            previous.clear();
            block = false;
        }
        previousLeft = line.box.left;
    }
}

std::vector<Graphic> backgrounds(Artwork& art, std::vector<Line> const& lines)
{
    std::vector<Graphic> result;
    for (const auto& item : art.backgroundCandidates) {
        bool touches = false;
        bool behind = true;
        for (const auto& line : lines) {
            if (!item.box.intersects(line.box)) continue;
            touches = true;
            if (!item.box.padded(.5, .5).contains(line.box) || (!line.objects.empty() && *line.objects.begin() < item.object)) {
                behind = false;
                break;
            }
        }
        if (touches && behind) result.push_back(item);
    }
    std::set<int> panelIds;
    for (const auto& panel : result) panelIds.insert(panel.object);
    std::erase_if(art.graphics, [&](Graphic const& item) {
        if (panelIds.contains(item.object)) return true;
        if (item.type != 2 || item.box.height() > 1.5 || item.box.width() < item.box.height() * 3) return false;
        for (const auto& line : lines) {
            const auto margin = line.size * .7;
            if (item.box.left >= line.box.left - margin && item.box.right <= line.box.right + margin
                && item.box.top >= line.baseline - line.size * .1 && item.box.bottom <= line.baseline + line.size * .22)
                return true; // Hyperlink underline, not a body-text obstruction.
        }
        return false;
    });
    return result;
}

bool listMarker(std::wstring_view text)
{
    if (text.size() == 1 && std::wstring_view(L"•‣◦▪▫●○·*–—-").find(text.front()) != std::wstring_view::npos) return true;
    if (text.size() < 2 || (text.back() != L'.' && text.back() != L')')) return false;
    text.remove_suffix(1);
    if (!text.empty() && text.front() == L'(') text.remove_prefix(1);
    if (text.size() == 1 && letter(text.front())) return true;
    if (text.size() <= 3 && !text.empty() && std::all_of(text.begin(), text.end(), digit)) return true;
    return !text.empty() && text.size() <= 6 && text.find_first_not_of(L"ivxlcdmIVXLCDM") == text.npos;
}

void markers(std::vector<Line>& lines)
{
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        auto& line = lines[i];
        const auto& next = lines[i + 1];
        auto compact = line.text;
        std::erase_if(compact, space);
        const auto gap = next.box.left - line.box.right;
        line.marker = line.box.width() <= line.size * 3 && listMarker(compact)
            && std::abs(line.baseline - next.baseline) < std::max(line.size, next.size) * .3
            && gap > 0 && gap <= std::max(72.0, line.size * 8);
        if (line.marker) line.text = std::move(compact);
    }
}

void stackPanelWords(std::vector<Line>& lines, std::vector<Graphic> const& panels)
{
    std::set<size_t> consumed;
    for (const auto& panel : panels) {
        if (panel.box.height() < panel.box.width() * 3) continue;
        std::vector<size_t> members;
        for (size_t i = 0; i < lines.size(); ++i)
            if (!consumed.contains(i) && panel.box.contains(lines[i].box)) members.push_back(i);
        if (members.size() < 4) continue;
        bool stacked = true;
        for (size_t i = 0; i < members.size(); ++i) {
            const auto& line = lines[members[i]];
            if (line.text.size() != 1 || !letter(line.text.front()) || panel.box.width() > line.size * 2) stacked = false;
            if (i && (line.baseline - lines[members[i - 1]].baseline > line.size * 1.6
                || std::abs(line.box.cx() - lines[members.front()].box.cx()) > line.size * .3)) stacked = false;
        }
        if (!stacked) continue;
        auto& first = lines[members.front()];
        for (size_t i = 1; i < members.size(); ++i) {
            auto& item = lines[members[i]];
            first.text += item.text;
            first.box = first.box.united(item.box);
            first.objects.merge(item.objects);
            first.runs.insert(first.runs.end(), item.runs.begin(), item.runs.end());
            first.masks.insert(first.masks.end(), item.masks.begin(), item.masks.end());
            first.glyphCount += item.glyphCount;
            first.boldCount += item.boldCount;
            first.serifCount += item.serifCount;
            first.monoCount += item.monoCount;
            consumed.insert(members[i]);
        }
        first.bold = first.boldCount * 5 > first.glyphCount * 3;
    }
    for (auto it = consumed.rbegin(); it != consumed.rend(); ++it)
        lines.erase(lines.begin() + static_cast<ptrdiff_t>(*it));
}

int addFlow(std::vector<Region>& regions, std::wstring id, Rect const& rect, bool column)
{
    Region flow;
    flow.id = std::move(id);
    flow.kind = column ? RegionKind::Column : RegionKind::Flow;
    flow.box = rect;
    regions.push_back(std::move(flow));
    return static_cast<int>(regions.size() - 1);
}

bool referenceLine(std::wstring_view value)
{
    value = trim(value);
    if (value.empty() || !digit(value.back())) return false;
    auto end = value.size();
    bool separator = false;
    while (end && (digit(value[end - 1]) || space(value[end - 1]) || std::wstring_view(L",–-").find(value[end - 1]) != value.npos)) {
        separator = separator || !digit(value[end - 1]);
        --end;
    }
    const auto alphabetic = [](wchar_t ch) { return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z'); };
    return separator && std::any_of(value.begin(), value.begin() + static_cast<ptrdiff_t>(end), alphabetic);
}

bool indexFooter(std::wstring_view value)
{
    value = trim(value);
    if (link(lower(value)) && wordCount(value) == 1) return true;
    if (value.size() < 3 || value.front() != L'[' || value.back() != L']') return false;
    const auto number = trim(value.substr(1, value.size() - 2));
    return !number.empty() && std::all_of(number.begin(), number.end(), digit);
}

std::vector<Line> indexOrder(std::vector<Line>& lines, std::vector<Region>& regions, Rect const& page, std::wstring const& prefix)
{
    if (!regions.empty()) return {};
    size_t references = 0;
    size_t headings = 0;
    std::vector<double> starts;
    for (const auto& line : lines) {
        auto text = trim(line.text);
        if (referenceLine(text)) ++references;
        if (text.size() == 1 && text.front() >= L'A' && text.front() <= L'Z') ++headings;
        if (!indexFooter(text)) starts.push_back(line.box.left);
    }
    if (references < 16 || references * 2 <= lines.size() || headings < 2) return {};
    std::sort(starts.begin(), starts.end());
    double gap = 0, split = 0;
    for (size_t i = 3; i + 3 <= starts.size(); ++i) {
        if (starts[i] - starts[i - 1] > gap) {
            gap = starts[i] - starts[i - 1];
            split = (starts[i] + starts[i - 1]) / 2;
        }
    }
    if (gap <= page.width() * .12) return {};
    std::array<std::vector<size_t>, 3> groups;
    std::array<Rect, 3> boxes{};
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto group = indexFooter(lines[i].text) ? 2 : lines[i].box.left < split ? 0 : 1;
        groups[group].push_back(i);
        boxes[group] = boxes[group].united(lines[i].box);
    }
    std::vector<Line> result;
    for (size_t group = 0; group < groups.size(); ++group) {
        if (groups[group].empty()) continue;
        auto area = boxes[group];
        if (group == 0) area.right = std::max(area.right, boxes[1].left - 4);
        area.bottom = std::min(page.bottom, area.bottom + 2);
        const auto region = addFlow(regions, prefix + L"-index" + std::to_wstring(group), area, group < 2);
        auto& members = groups[group];
        std::stable_sort(members.begin(), members.end(), [&](auto a, auto b) { return lines[a].box.top < lines[b].box.top; });
        for (const auto index : members) {
            auto& line = lines[index];
            line.container = region;
            line.order = static_cast<int>(result.size());
            line.indexEntry = group < 2;
            result.push_back(std::move(line));
        }
    }
    return result;
}

struct Atom {
    Rect box;
    std::vector<size_t> members;
    bool table{};
};

using Span = std::pair<double, double>;

Span largestGap(std::vector<Span> const& positions)
{
    Span result{};
    if (positions.empty()) return result;
    double end = positions.front().second;
    for (const auto& [first, last] : positions) {
        if (first - end > result.second - result.first) result = {end, first};
        end = std::max(end, last);
    }
    return result;
}

std::vector<Span> axes(std::vector<Atom> const& atoms, std::vector<size_t> const& indices, bool x)
{
    std::vector<Span> output;
    for (const auto index : indices) {
        const auto box = atoms[index].box;
        output.emplace_back(x ? box.left : box.top, x ? box.right : box.bottom);
    }
    std::sort(output.begin(), output.end());
    return output;
}

std::vector<double> sideBySide(std::vector<Atom> const& atoms, std::vector<size_t> indices, double height)
{
    std::stable_sort(indices.begin(), indices.end(), [&](auto a, auto b) { return atoms[a].box.top < atoms[b].box.top; });
    std::vector<double> gutters;
    for (size_t i = 0; i < indices.size(); ++i) {
        const auto a = atoms[indices[i]].box;
        if (a.width() <= height * 3) continue;
        for (size_t j = i + 1; j < indices.size(); ++j) {
            const auto b = atoms[indices[j]].box;
            if (b.top >= a.bottom) break;
            if (b.width() <= height * 3) continue;
            const auto gap = std::max(a.left - b.right, b.left - a.right);
            if (gap < std::max(12.0, height * 1.6)) continue;
            const auto gutter = (std::min(a.right, b.right) + std::max(a.left, b.left)) / 2;
            if (std::none_of(gutters.begin(), gutters.end(), [&](auto known) { return std::abs(known - gutter) < height; }))
                gutters.push_back(gutter);
            if (gutters.size() >= 32) return gutters;
        }
    }
    return gutters;
}

Span sectionGap(std::vector<Atom> const& atoms, std::vector<size_t> const& indices, double height)
{
    const auto gutters = sideBySide(atoms, indices, height);
    if (gutters.empty()) return {};
    const auto bands = axes(atoms, indices, false);
    auto gap = largestGap(bands);
    Rect span;
    for (const auto index : indices) {
        const auto box = atoms[index].box;
        for (const auto x : gutters) {
            if (box.left < x - height && box.right > x + height && (span.empty() || box.top < span.top)) span = box;
        }
    }
    if (span.empty()) return gap;
    double end = bands.front().second, distance = 1e9;
    for (const auto& [first, last] : bands) {
        if (first - end >= std::max(3.0, height * .4)) {
            const auto offset = std::min(std::abs(end - span.bottom), std::abs(first - span.top));
            if (offset < distance) {
                distance = offset;
                gap = {end, first};
            }
        }
        end = std::max(end, last);
    }
    return gap;
}

void partition(std::vector<Atom> const& atoms, std::vector<size_t> indices, Rect area, bool column, int depth,
               double height, std::wstring const& prefix, size_t& flow, std::vector<Region>& regions,
               std::vector<Line>& lines, std::vector<Line>& output, std::vector<Rect> const& orderingBoxes)
{
    if (indices.empty()) return;
    if (depth < 24 && indices.size() > 1) {
        const auto gutter = largestGap(axes(atoms, indices, true));
        if (gutter.second - gutter.first >= std::max(12.0, height * 1.6)) {
            std::vector<size_t> left, right;
            double widestLeft = 0, widestRight = 0;
            for (const auto index : indices) {
                const auto box = atoms[index].box;
                if (box.cx() < gutter.first + 1) {
                    left.push_back(index);
                    widestLeft = std::max(widestLeft, box.width());
                } else {
                    right.push_back(index);
                    widestRight = std::max(widestRight, box.width());
                }
            }
            if (!left.empty() && !right.empty() && widestLeft > height * 3 && widestRight > height * 3) {
                auto leftArea = area, rightArea = area;
                leftArea.right = gutter.first + std::min(6.0, (gutter.second - gutter.first) / 4);
                rightArea.left = gutter.second;
                partition(atoms, std::move(left), leftArea, true, depth + 1, height, prefix, flow, regions, lines, output, orderingBoxes);
                partition(atoms, std::move(right), rightArea, true, depth + 1, height, prefix, flow, regions, lines, output, orderingBoxes);
                return;
            }
        }
        const auto band = sectionGap(atoms, indices, height);
        if (band.second - band.first >= std::max(3.0, height * .4)) {
            std::vector<size_t> top, bottom;
            for (const auto index : indices)
                (atoms[index].box.cy() <= band.first ? top : bottom).push_back(index);
            if (!top.empty() && !bottom.empty()) {
                auto topArea = area, bottomArea = area;
                topArea.bottom = (band.first + band.second) / 2;
                bottomArea.top = band.second;
                partition(atoms, std::move(top), topArea, column, depth + 1, height, prefix, flow, regions, lines, output, orderingBoxes);
                partition(atoms, std::move(bottom), bottomArea, column, depth + 1, height, prefix, flow, regions, lines, output, orderingBoxes);
                return;
            }
        }
    }
    const auto container = addFlow(regions, prefix + L"-flow" + std::to_wstring(flow++), area, column);
    std::stable_sort(indices.begin(), indices.end(), [&](auto a, auto b) {
        const auto x = atoms[a].box, y = atoms[b].box;
        return std::tie(x.top, x.left) < std::tie(y.top, y.left);
    });
    for (const auto index : indices) {
        auto members = atoms[index].members;
        if (atoms[index].table) {
            std::stable_sort(members.begin(), members.end(), [&](auto a, auto b) {
                const auto& x = regions[lines[a].container];
                const auto& y = regions[lines[b].container];
                const auto& first = orderingBoxes[a];
                const auto& last = orderingBoxes[b];
                return std::tie(x.row, x.column, first.top, first.left) < std::tie(y.row, y.column, last.top, last.left);
            });
        }
        for (const auto member : members) {
            auto& line = lines[member];
            if (!atoms[index].table) line.container = container;
            line.order = static_cast<int>(output.size());
            output.push_back(std::move(line));
        }
    }
}

std::vector<Line> readingOrder(std::vector<Line> lines, std::vector<Region>& regions, Rect const& page, std::wstring const& prefix)
{
    if (lines.empty()) return {};
    if (auto index = indexOrder(lines, regions, page, prefix); !index.empty()) return index;
    std::vector<double> heights;
    for (const auto& line : lines) heights.push_back(line.box.height());
    const auto height = std::clamp(median(std::move(heights)), 5.0, 32.0);
    inferredTables(lines, regions, page, prefix, height);
    std::vector<Atom> atoms;
    std::map<int, std::vector<size_t>> tableMembers;
    std::vector<Rect> orderingBoxes;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto& line = lines[i];
        auto area = line.box;
        if (line.marker && i + 1 < lines.size())
            area.top = std::min(area.top, lines[i + 1].box.top);
        orderingBoxes.push_back(area);
        line.container = cellAt(regions, area);
        if (line.container < 0) atoms.push_back({area, {i}, false});
        else tableMembers[regions[line.container].parent].push_back(i);
    }
    for (auto& [table, members] : tableMembers)
        atoms.push_back({regions[table].box, std::move(members), true});
    Rect area;
    std::vector<size_t> indices(atoms.size());
    std::iota(indices.begin(), indices.end(), size_t{});
    for (const auto& atom : atoms) area = area.united(atom.box);
    area.bottom = std::max(area.bottom, page.bottom - std::max(12.0, area.left * .4));
    size_t flow = 0;
    std::vector<Line> output;
    partition(atoms, std::move(indices), area, false, 0, height, prefix, flow, regions, lines, output, orderingBoxes);
    return output;
}

std::vector<Line> paragraphs(std::vector<Line> const& lines, std::vector<Region> const& regions,
                             Artwork const& art, std::vector<Graphic> const& panels)
{
    std::vector<Line> blocks;
    double previousBaseline = -1e9;
    for (const auto& line : lines) {
        bool join = false;
        if (!blocks.empty()) {
            const auto& previous = blocks.back();
            const auto baselineGap = line.baseline - previousBaseline;
            const bool sameContainer = previous.container == line.container;
            const auto cell = regions[line.container].kind == RegionKind::Cell;
            join = !previous.marker && !line.marker && baselineGap > line.size * .6 && baselineGap < line.size * 1.55
                && std::abs(previous.size - line.size) < line.size * .08 && line.color == previous.color && line.bold == previous.bold
                && (std::abs(previous.box.left - line.box.left) < line.size * 1.8
                    || (previous.masks.size() == 1 && previous.box.left > line.box.left && previous.box.left - line.box.left < line.size * 2.75));
            if (line.code() != previous.code()) join = false;
            if (line.code() && previous.code() && baselineGap > line.size * .6 && baselineGap < line.size * 1.6) join = true;
            if (cell) join = true;
            join = join && sameContainer && !line.indexEntry && !previous.indexEntry;
            if (join) {
                const auto combined = previous.box.united(line.box);
                for (const auto& artwork : art.graphics) {
                    const auto box = artwork.box;
                    if (box.left >= combined.right || box.right <= combined.left) continue;
                    if ((box.top >= previous.box.bottom && box.bottom <= line.box.top && box.width() > 20)
                        || (box.width() > 2 && box.height() > 2 && box.intersects(combined))) {
                        join = false;
                        break;
                    }
                }
                for (const auto& panel : panels) {
                    const auto box = panel.box.padded(.5, .5);
                    if (box.contains(previous.box) != box.contains(line.box)) { join = false; break; }
                }
            }
        }
        if (join) {
            auto& previous = blocks.back();
            if (previous.code() && line.code()) {
                const auto previousLeft = previous.masks.empty() ? previous.box.left : previous.masks.back().left;
                const auto lastLine = std::wstring_view(previous.text).substr(previous.text.find_last_of(L'\n') + 1);
                const auto indent = std::max(0l, std::lround((line.box.left - previous.box.left) / (line.size * .6)));
                const auto separator = wrappedComment(lastLine, line.text, line.box.left < previousLeft - line.size)
                    ? L" " : L"\n" + std::wstring(static_cast<size_t>(indent), L' ');
                previous.text += separator;
                styleRun(previous, separator);
            } else if (!previous.text.empty() && previous.text.back() == L'-' && !line.text.empty()
                       && (std::iswlower(line.text.front()) || lower(line.text.substr(0, 1)) == line.text.substr(0, 1)
                           && letter(line.text.front()))) {
                previous.text.pop_back();
                if (!previous.runs.empty() && !previous.runs.back().text.empty()) previous.runs.back().text.pop_back();
            } else {
                previous.text += L' ';
                styleRun(previous, L" ");
            }
            previous.text += line.text;
            previous.box = previous.box.united(line.box);
            previous.objects.insert(line.objects.begin(), line.objects.end());
            previous.runs.insert(previous.runs.end(), line.runs.begin(), line.runs.end());
            previous.masks.insert(previous.masks.end(), line.masks.begin(), line.masks.end());
            previous.glyphCount += line.glyphCount;
            previous.boldCount += line.boldCount;
            previous.serifCount += line.serifCount;
            previous.monoCount += line.monoCount;
        } else {
            blocks.push_back(line);
        }
        previousBaseline = line.baseline;
    }
    return blocks;
}

bool formula(std::wstring_view text)
{
    if (text.find(L'=') == text.npos || text.find_first_of(L";{}#\"") != text.npos) return false;
    size_t asciiLetters = 0;
    for (const auto ch : text) {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z')) {
            if (++asciiLetters >= 3) return false;
        } else {
            asciiLetters = 0;
            const auto category = UnicodeCharacters::GetGeneralCategory(ch);
            if (!space(ch) && !digit(ch) && !(ch >= 0x370 && ch <= 0x3ff)
                && !(ch >= 0x1d00 && ch <= 0x1d7f) && !(ch >= 0x2070 && ch <= 0x209f)
                && category != UnicodeGeneralCategory::MathSymbol
                && std::wstring_view(L".,()[]⁺⁻₊₋").find(ch) == std::wstring_view::npos)
                return false;
        }
    }
    return true;
}

bool fixed(Line const& block)
{
    const auto text = trim(block.text);
    if (block.marker || !hasLetter(text) || (text.size() == 1 && text.front() >= L'A' && text.front() <= L'Z') || formula(text))
        return true;
    if (block.code() && commentStart(text) < 0 && text.find(L"*/") == text.npos) return true;
    if (link(lower(text)) && wordCount(text) == 1) return true;
    if (text.size() > 2 && text.front() == L'[' && text.back() == L']') {
        const auto inner = trim(text.substr(1, text.size() - 2));
        return !inner.empty() && inner.find_first_not_of(L"ivxlcdmIVXLCDM") == inner.npos;
    }
    return false;
}

std::wstring styledSource(Line const& block)
{
    std::wstring text;
    bool emphasis = false;
    for (const auto& run : block.runs) {
        if (run.text.empty()) continue;
        const bool bold = run.bold && !block.bold;
        emphasis = emphasis || bold || run.italic || run.script != 0;
        if (bold) text += L"<b>";
        if (run.italic) text += L"<i>";
        if (run.script != 0) text += run.script > 0 ? L"<sup>" : L"<sub>";
        text += run.text;
        if (run.script != 0) text += run.script > 0 ? L"</sup>" : L"</sub>";
        if (run.italic) text += L"</i>";
        if (bold) text += L"</b>";
    }
    return emphasis ? text : std::wstring();
}

bool protectedByArtwork(Line const& block, std::vector<Line> const& lines, Artwork const& artwork)
{
    for (const auto& item : artwork.graphics) {
        const auto obstacle = item.box;
        if (!obstacle.intersects(block.box)) continue;
        if (obstacle.height() <= 1.5 && obstacle.width() >= block.size * 3) {
            const auto underline = std::any_of(lines.begin(), lines.end(), [&](Line const& line) {
                return block.box.contains(line.box) && obstacle.right >= line.box.left && obstacle.left <= line.box.right
                    && obstacle.top >= line.baseline - line.size * .10 && obstacle.bottom <= line.baseline + line.size * .22;
            });
            if (underline) continue;
        }
        return true;
    }
    return false;
}

struct Placement {
    Rect region;
    Rect container;
    bool centered{};
};

Placement placement(Line const& block, std::vector<Line> const& blocks, std::vector<Region> const& regions,
                    Artwork const& artwork, std::vector<Graphic> const& panels, Rect const& page, Rect const& content)
{
    auto container = regions[block.container].box.intersected(page);
    const bool cell = regions[block.container].kind == RegionKind::Cell;
    bool centered = block.masks.size() == 1 && !container.empty()
        && std::abs(block.box.cx() - container.cx()) < std::max(8.0, block.size)
        && block.box.width() < container.width() * .8;
    const auto half = std::max(block.box.width() / 2, std::min(block.box.cx() - content.left, page.right - content.left - block.box.cx()));
    Rect region{centered ? block.box.cx() - half : block.box.left, block.box.top,
                centered ? block.box.cx() + half : content.right, block.box.top};
    for (const auto& item : artwork.graphics) {
        const auto o = item.box;
        if (o.top < block.box.bottom && o.bottom > block.box.top && o.left > block.box.right)
            region.right = std::min(region.right, o.left - 3);
    }
    for (const auto& other : blocks) {
        if (other.box.top >= block.box.bottom || other.box.bottom <= block.box.top) continue;
        if (other.box.left > block.box.right) region.right = std::min(region.right, other.box.left - 3);
        if (other.box.right < block.box.left) region.left = std::max(region.left, other.box.right + 3);
    }
    double bottom = page.bottom - std::max(12.0, content.left * .4);
    for (const auto& next : blocks) {
        if (next.box.top >= block.box.bottom - .5 && next.box.left < region.right && next.box.right > region.left)
            bottom = std::min(bottom, next.box.top - 3);
    }
    for (const auto& item : artwork.graphics) {
        const auto o = item.box;
        if (o.top >= block.box.bottom - 1 && o.left < region.right && o.right > region.left)
            bottom = std::min(bottom, o.top - (o.height() <= 2 && o.width() > block.size * 3 ? 1 : 3));
    }
    for (const auto& panel : panels) {
        const auto box = panel.box;
        if (box.padded(.5, .5).contains(block.box)) {
            region.left = std::max(region.left, box.left);
            region.right = std::min(region.right, box.right);
            bottom = std::min(bottom, box.bottom);
        } else {
            if (box.top < block.box.bottom && box.bottom > block.box.top && box.left > block.box.right)
                region.right = std::min(region.right, box.left - 3);
            if (box.top >= block.box.bottom - 1 && box.left < region.right && box.right > region.left)
                bottom = std::min(bottom, box.top - 3);
        }
    }
    region.bottom = std::max(block.box.bottom, bottom);
    if (!container.empty()) {
        const auto limit = cell ? container.padded(-2, -std::min(2.0, container.height() * .08)) : container;
        region = region.intersected(limit);
        if (cell && !region.empty()) {
            region.top = std::max(limit.top, block.box.top - 1);
            region.bottom = limit.bottom;
        }
    }
    if (!cell && !region.empty() && block.masks.size() == 1 && region.height() < block.size * 1.05) {
        double top = std::max(0.0, block.box.top - block.size * .12);
        for (const auto& other : blocks) {
            if (other.box.bottom <= block.box.top && other.box.left < region.right && other.box.right > region.left)
                top = std::max(top, other.box.bottom + 1);
        }
        for (const auto& item : artwork.graphics) {
            const auto o = item.box;
            if (o.bottom <= block.box.top && o.left < region.right && o.right > region.left) top = std::max(top, o.bottom + 1);
        }
        for (const auto& panel : panels)
            if (panel.box.contains(block.box)) top = std::max(top, panel.box.top + 1);
        region.top = std::min(region.top, top);
    }
    Rect ownPanel;
    if (!cell) for (const auto& panel : panels) {
        const auto box = panel.box;
        if (!box.contains(block.box) || box.width() > page.width() * .7 || box.height() > page.height() * .5) continue;
        const auto occupants = std::count_if(blocks.begin(), blocks.end(), [&](Line const& item) { return box.intersects(item.box); });
        if (occupants == 1 && (ownPanel.empty() || box.area() < ownPanel.area())) ownPanel = box;
    }
    if (!ownPanel.empty()) {
        container = ownPanel;
        region = ownPanel.padded(-1, -1);
        region.top = std::max(region.top, std::min(block.box.top, region.bottom - block.size * 1.1));
        centered = true;
    }
    return {region.intersected(page), container.intersected(page), centered};
}

JsonArray outputBlocks(std::vector<Line> const& blocks, std::vector<Line> const& lines, std::vector<Region> const& regions,
                       Artwork const& art, std::vector<Graphic> const& panels, Rect const& page, std::wstring const& prefix)
{
    Rect content;
    for (const auto& block : blocks) content = content.united(block.box.intersected(page));
    JsonArray output;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& block = blocks[i];
        const auto area = placement(block, blocks, regions, art, panels, page, content);
        const auto& container = regions[block.container];
        JsonObject item;
        put(item, L"id", prefix + L"-b" + std::to_wstring(i));
        put(item, L"source", block.text);
        put(item, L"styledSource", styledSource(block));
        put(item, L"rect", block.box);
        put(item, L"region", area.region);
        put(item, L"containerRect", area.container);
        put(item, L"container", container.id);
        put(item, L"order", static_cast<int>(i));
        put(item, L"role", container.kind == RegionKind::Cell ? L"cell" : block.indexEntry ? L"index" : L"paragraph");
        put(item, L"size", block.size);
        put(item, L"bold", block.bold);
        put(item, L"serif", block.serifCount * 2 > block.glyphCount);
        put(item, L"code", block.code());
        put(item, L"fixed", fixed(block));
        put(item, L"centered", area.centered);
        item.Insert(L"color", numbers({block.color[0], block.color[1], block.color[2]}));
        JsonArray objects;
        for (const auto object : block.objects) objects.Append(JsonValue::CreateNumberValue(object));
        item.Insert(L"objects", objects);
        JsonArray masks;
        for (const auto& mask : block.masks) masks.Append(jsonRect(mask.intersected(page)));
        item.Insert(L"masks", masks);
        if (container.kind == RegionKind::Cell) {
            put(item, L"table", regions[container.parent].id);
            put(item, L"row", container.row);
            put(item, L"column", container.column);
            put(item, L"rowSpan", container.rowSpan);
            put(item, L"columnSpan", container.columnSpan);
        }
        const bool graphics = protectedByArtwork(block, lines, art);
        if (graphics || area.region.empty()) {
            put(item, L"keep", true);
            put(item, L"protected", true);
            put(item, L"preserveReason", graphics ? L"文字穿过图形边界，此区域保留原文；可用识图重新处理。"
                                                 : L"文字区域超出页面或没有可用的排版空间，保留原文。");
        }
        output.Append(item);
    }
    return output;
}

JsonObject layoutStructure(JsonArray const& blocks, std::vector<Region> const& regions, Rect const& page)
{
    std::map<hstring, JsonArray> children;
    JsonArray order;
    for (const auto& child : regions) {
        if (child.parent >= 0)
            children[hstring(regions[child.parent].id)].Append(JsonValue::CreateStringValue(hstring(child.id)));
    }
    std::map<hstring, std::vector<JsonObject>> tableBlocks;
    for (const auto& value : blocks) {
        const auto block = value.GetObject();
        const auto id = block.GetNamedValue(L"id");
        order.Append(id);
        children[block.GetNamedString(L"container")].Append(id);
        if (const auto table = block.GetNamedString(L"table", L""); !table.empty())
            tableBlocks[table].push_back(block);
    }
    JsonArray structure;
    for (const auto& region : regions) {
        JsonObject object;
        const auto id = hstring(region.id);
        put(object, L"id", id);
        put(object, L"rect", region.box.intersected(page));
        object.Insert(L"children", children[id]);
        if (region.kind == RegionKind::Table) {
            put(object, L"kind", L"table");
            put(object, L"rows", region.rows);
            put(object, L"columns", region.columns);
            put(object, L"inferred", region.inferred);
            object.Insert(L"xEdges", numbers(region.xEdges));
            object.Insert(L"yEdges", numbers(region.yEdges));
            const auto& contents = tableBlocks[id];
            int headerRows = 0;
            for (int row = 0; row < std::min(4, region.rows); ++row) {
                int total = 0, bold = 0, numeric = 0;
                for (const auto& block : contents) {
                    if (integer(block, L"row") != row) continue;
                    ++total;
                    if (block.GetNamedBoolean(L"bold", false) || integer(block, L"columnSpan", 1) > 1) ++bold;
                    if (numericCell(block.GetNamedString(L"source"))) ++numeric;
                }
                if (!total || numeric * 10 > total * 3 || (row > 0 && bold * 2 < total)) break;
                ++headerRows;
            }
            put(object, L"headerRows", headerRows);
            JsonArray headers;
            for (const auto& block : contents) {
                if (integer(block, L"row") >= headerRows) continue;
                const auto text = block.GetNamedString(L"source");
                if (trim(text).empty()) continue;
                JsonObject header;
                put(header, L"column", integer(block, L"column"));
                put(header, L"columnSpan", integer(block, L"columnSpan", 1));
                put(header, L"text", text);
                headers.Append(header);
            }
            object.Insert(L"headers", headers);
        } else if (region.kind == RegionKind::Cell) {
            put(object, L"kind", L"cell");
            put(object, L"parent", regions[region.parent].id);
            put(object, L"row", region.row);
            put(object, L"column", region.column);
            put(object, L"rowSpan", region.rowSpan);
            put(object, L"columnSpan", region.columnSpan);
        } else {
            put(object, L"kind", region.kind == RegionKind::Column ? L"column" : L"flow");
        }
        structure.Append(object);
    }
    JsonObject result;
    put(result, L"version", 1);
    result.Insert(L"regions", structure);
    result.Insert(L"order", order);
    return result;
}

std::wstring normalizeHeaders(JsonArray const& entries)
{
    std::wstring result;
    for (const auto& value : entries) {
        if (!result.empty()) result += L'|';
        const auto text = lower(value.GetObject().GetNamedString(L"text", L""));
        bool gap = false;
        for (const auto ch : trim(text)) {
            if (space(ch)) gap = true;
            else {
                if (gap) result += L' ';
                gap = false;
                result += ch;
            }
        }
    }
    return result;
}

}

JsonObject analyzePageInput(JsonObject const& raw, int pageNumber)
{
    if (pageNumber < 0) fail(L"页码无效。");
    const auto width = raw.GetNamedNumber(L"width", 0);
    const auto height = raw.GetNamedNumber(L"height", 0);
    if (!std::isfinite(width) || !std::isfinite(height) || width < 20 || height < 20 || width > 4000 || height > 4000)
        fail(L"页面尺寸超出支持范围。");
    const Rect page = Rect::fromSize(0, 0, width, height);
    auto graphics = artwork(raw, page);
    bool incomplete = raw.GetNamedBoolean(L"incompleteText", false), slanted = false;
    auto characters = glyphs(raw, incomplete, slanted);
    const auto text = raw.GetNamedString(L"text", L"");
    JsonObject result;
    put(result, L"number", pageNumber);
    put(result, L"analyzed", true);
    put(result, L"extractionVersion", nativePdfExtractionVersion);
    put(result, L"width", width);
    put(result, L"height", height);
    put(result, L"text", text);
    put(result, L"kind", L"text");
    result.Insert(L"blocks", JsonArray{});
    result.Insert(L"layout", layoutStructure(JsonArray{}, {}, page));
    if (graphics.rasterBackground) put(result, L"rasterBackground", true);
    if (characters.empty() && !graphics.visibleArtwork && !incomplete && text.empty()) {
        put(result, L"kind", L"blank");
        return result;
    }
    if (characters.empty() || (graphics.largestImage > page.area() * .65 && meaningfulLetters(text) < 12)) {
        put(result, L"kind", L"scan");
        put(result, L"warning", L"扫描或图像为主的页面，请先用 AI 识别原文。");
        return result;
    }
    hstring warning;
    if (integer(raw, L"rotation", 0) != 0) warning = L"旋转页面暂保留原样。";
    const auto media = raw.GetNamedArray(L"mediaBox", nullptr);
    auto crop = raw.GetNamedArray(L"cropBox", nullptr);
    if (!crop) crop = media;
    if (crop && media && crop.Size() == 4 && media.Size() == 4
        && (std::abs(std::max(media.GetNumberAt(0), crop.GetNumberAt(0))) > .1
            || std::abs(std::max(media.GetNumberAt(1), crop.GetNumberAt(1))) > .1))
        warning = L"含偏移裁切区域，此页暂保留原样。";
    if (incomplete) warning = L"文字编码或位置不完整，页面暂保留原样。";
    if (slanted) warning = L"含倾斜或竖排文字，页面暂保留原样。";
    if (!warning.empty()) {
        put(result, L"warning", warning);
        return result;
    }
    const auto prefix = L"p" + std::to_wstring(pageNumber);
    auto regions = ruledTables(graphics.rules, page, prefix);
    auto lines = buildLines(std::move(characters), graphics, regions, page);
    detectMonospacedProse(lines);
    const auto panels = backgrounds(graphics, lines);
    stackPanelWords(lines, panels);
    markers(lines);
    lines = readingOrder(std::move(lines), regions, page, prefix);
    const auto blocks = outputBlocks(paragraphs(lines, regions, graphics, panels), lines, regions, graphics, panels, page, prefix);
    result.Insert(L"blocks", blocks);
    result.Insert(L"layout", layoutStructure(blocks, regions, page));
    put(result, L"baseLayoutHash", hashBlocks(blocks));
    if (graphics.rasterBackground)
        put(result, L"notice", L"本页含渐变图形，将以图像保留背景；译文仍可搜索，原生矢量不再保留。");
    return result;
}

void linkPageTableHeaders(JsonObject const& previous, JsonObject const& current)
{
    const auto first = integer(previous, L"number");
    if (first < 0 || integer(current, L"number") != first + 1) return;
    const auto previousWidth = previous.GetNamedNumber(L"width", 0);
    const auto currentWidth = current.GetNamedNumber(L"width", 0);
    const auto previousHeight = previous.GetNamedNumber(L"height", 0);
    const auto currentHeight = current.GetNamedNumber(L"height", 0);
    if (previousWidth <= 0 || currentWidth <= 0 || previousHeight <= 0 || currentHeight <= 0) return;
    const auto getRegions = [](JsonObject const& page) {
        const auto scan = page.GetNamedObject(L"scan", nullptr);
        const auto layout = scan ? scan.GetNamedObject(L"layout", nullptr) : page.GetNamedObject(L"layout", nullptr);
        return layout ? layout.GetNamedArray(L"regions", nullptr) : JsonArray{nullptr};
    };
    const auto before = getRegions(previous);
    const auto after = getRegions(current);
    if (!before || !after) return;
    for (const auto& value : after) {
        const auto table = value.GetObject();
        if (table.GetNamedString(L"kind", L"") != L"table" || table.GetNamedBoolean(L"continuationEdited", false)) continue;
        if (table.HasKey(L"continuedFrom")) table.Remove(L"continuedFrom");
        if (table.HasKey(L"inheritedHeaders")) table.Remove(L"inheritedHeaders");
        if (rect(table).top > currentHeight * .28) continue;
        const auto xs = table.GetNamedArray(L"xEdges", nullptr);
        if (!xs || xs.Size() < 3) continue;
        for (const auto& item : before) {
            const auto candidate = item.GetObject();
            if (candidate.GetNamedString(L"kind", L"") != L"table" || integer(candidate, L"columns") != integer(table, L"columns")
                || rect(candidate).bottom < previousHeight * .72)
                continue;
            const auto firstXs = candidate.GetNamedArray(L"xEdges", nullptr);
            if (!firstXs || firstXs.Size() != xs.Size()) continue;
            bool aligned = true;
            for (uint32_t i = 0; i < xs.Size(); ++i)
                aligned = aligned && std::abs(firstXs.GetNumberAt(i) / previousWidth - xs.GetNumberAt(i) / currentWidth) <= .025;
            auto headers = candidate.GetNamedArray(L"headers", nullptr);
            if (!headers || headers.Size() == 0) headers = candidate.GetNamedArray(L"inheritedHeaders", nullptr);
            if (!aligned || !headers || headers.Size() == 0) continue;
            const auto next = table.GetNamedArray(L"headers", nullptr);
            if (next && next.Size() > 0 && normalizeHeaders(next) != normalizeHeaders(headers)) continue;
            JsonObject origin;
            put(origin, L"page", first);
            put(origin, L"table", candidate.GetNamedString(L"id"));
            table.Insert(L"continuedFrom", origin);
            table.Insert(L"inheritedHeaders", headers);
            break;
        }
    }
}

JsonObject analyzeScanRows(JsonArray const& rows, JsonArray const& rules, double pageWidth, double pageHeight, int pageNumber)
{
    const Rect page = Rect::fromSize(0, 0, pageWidth, pageHeight);
    const auto prefix = L"scan-p" + std::to_wstring(pageNumber);
    auto regions = ruledTables(readRules(rules), page, prefix);
    std::vector<Line> lines;
    for (uint32_t index = 0; index < rows.Size(); ++index) {
        const auto item = rows.GetObjectAt(index);
        const auto box = rect(item);
        if (box.empty()) continue;
        Line line;
        line.box = box;
        line.inputIndex = static_cast<int>(index);
        line.size = item.GetNamedNumber(L"size", box.height());
        lines.push_back(std::move(line));
    }
    lines = readingOrder(std::move(lines), regions, page, prefix);
    JsonArray items;
    for (const auto& line : lines) {
        JsonObject item;
        const auto& region = regions[line.container];
        put(item, L"id", prefix + L"-line" + std::to_wstring(line.inputIndex));
        put(item, L"source", L"");
        put(item, L"inputIndex", line.inputIndex);
        put(item, L"order", line.order);
        put(item, L"container", region.id);
        put(item, L"containerRect", region.box.intersected(page));
        put(item, L"role", region.kind == RegionKind::Cell ? L"cell" : L"paragraph");
        if (region.kind == RegionKind::Cell) {
            put(item, L"table", regions[region.parent].id);
            put(item, L"row", region.row);
            put(item, L"column", region.column);
            put(item, L"rowSpan", region.rowSpan);
            put(item, L"columnSpan", region.columnSpan);
        }
        items.Append(item);
    }
    JsonObject result;
    result.Insert(L"items", items);
    result.Insert(L"layout", layoutStructure(items, regions, page));
    return result;
}

}
