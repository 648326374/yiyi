#include "PdfStructure.h"

#include <windows.h>
#undef GetObject

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace yiyi {
namespace {

using namespace winrt;
using namespace Windows::Data::Json;
using Objects = std::vector<JsonObject>;

[[noreturn]] void fail(hstring const& message)
{
    throw hresult_invalid_argument(message);
}

void text(JsonObject const& object, hstring const& key, hstring const& value)
{
    object.Insert(key, JsonValue::CreateStringValue(value));
}

void number(JsonObject const& object, hstring const& key, double value)
{
    object.Insert(key, JsonValue::CreateNumberValue(value));
}

void flag(JsonObject const& object, hstring const& key, bool value)
{
    object.Insert(key, JsonValue::CreateBooleanValue(value));
}

void remove(JsonObject const& object, hstring const& key)
{
    if (object.HasKey(key)) object.Remove(key);
}

int integer(JsonObject const& object, hstring const& key, int fallback = 0)
{
    const auto value = object.GetNamedNumber(key, fallback);
    if (!std::isfinite(value) || std::trunc(value) != value || value < (std::numeric_limits<int>::min)()
        || value > (std::numeric_limits<int>::max)()) fail(L"结构中的整数无效。");
    return static_cast<int>(value);
}

std::wstring_view trim(std::wstring_view value)
{
    constexpr std::wstring_view spaces = L" \r\n\t\f\v\u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005"
        L"\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000";
    const auto first = value.find_first_not_of(spaces);
    return first == value.npos ? std::wstring_view{} : value.substr(first, value.find_last_not_of(spaces) - first + 1);
}

hstring uniqueId(std::wstring_view prefix)
{
    auto value = std::wstring(to_hstring(Windows::Foundation::GuidHelper::CreateNewGuid()));
    std::erase_if(value, [](wchar_t ch) { return ch == L'{' || ch == L'}' || ch == L'-'; });
    return hstring(std::wstring(prefix) + L"-" + value);
}

struct Rect {
    double left{}, top{}, right{}, bottom{};

    double width() const { return right - left; }
    double height() const { return bottom - top; }
    bool empty() const { return width() <= 0 || height() <= 0; }
    Rect inset(double amount) const { return {left + amount, top + amount, right - amount, bottom - amount}; }
    bool contains(Rect other) const
    {
        return left <= other.left && top <= other.top && right >= other.right && bottom >= other.bottom;
    }
    Rect united(Rect other) const
    {
        if (empty()) return other;
        return {(std::min)(left, other.left), (std::min)(top, other.top), (std::max)(right, other.right), (std::max)(bottom, other.bottom)};
    }
    Rect intersected(Rect other) const
    {
        return {(std::max)(left, other.left), (std::max)(top, other.top), (std::min)(right, other.right), (std::min)(bottom, other.bottom)};
    }
};

Rect rect(JsonArray const& array)
{
    if (!array || array.Size() != 4) return {};
    const auto x = array.GetNumberAt(0);
    const auto y = array.GetNumberAt(1);
    return {x, y, x + array.GetNumberAt(2), y + array.GetNumberAt(3)};
}

Rect rect(JsonObject const& object, hstring const& key)
{
    return rect(object.GetNamedArray(key, nullptr));
}

JsonArray numbers(std::vector<double> const& values)
{
    JsonArray array;
    for (double value : values) array.Append(JsonValue::CreateNumberValue(value));
    return array;
}

JsonArray rectValue(Rect box)
{
    return numbers({box.left, box.top, box.width(), box.height()});
}

bool finiteRect(Rect box, double minimum)
{
    return std::isfinite(box.left) && std::isfinite(box.top) && std::isfinite(box.right) && std::isfinite(box.bottom)
        && box.width() >= minimum && box.height() >= minimum;
}

Objects objects(JsonArray const& input)
{
    Objects result;
    if (input) for (const auto& value : input) result.push_back(value.GetObject());
    return result;
}

JsonArray array(Objects const& values)
{
    JsonArray result;
    for (const auto& value : values) result.Append(value);
    return result;
}

bool numericCell(std::wstring_view source)
{
    auto value = trim(source);
    if (!value.empty() && std::wstring_view(L"+−-").find(value.front()) != value.npos) value.remove_prefix(1);
    const auto digit = [](wchar_t ch) {
        WORD kind{};
        return GetStringTypeW(CT_CTYPE1, &ch, 1, &kind) && (kind & C1_DIGIT);
    };
    size_t end = 0;
    while (end < value.size() && (digit(value[end]) || value[end] == L'.' || value[end] == L',')) ++end;
    const auto unit = trim(value.substr(end));
    if (end == 0 || unit.size() > 8) return false;
    return std::all_of(unit.begin(), unit.end(), [](wchar_t ch) {
        WORD kind{};
        return std::wstring_view(L"%％°µμ/²³").find(ch) != std::wstring_view::npos
            || (GetStringTypeW(CT_CTYPE1, &ch, 1, &kind) && (kind & C1_ALPHA));
    });
}

bool childCell(JsonObject const& object, hstring const& table)
{
    return object.GetNamedString(L"kind", L"") == L"cell" && object.GetNamedString(L"parent", L"") == table;
}

void reindexTables(Objects const& regions, Objects const& blocks)
{
    auto const unique = [](std::vector<double>& edges) {
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end(), [](double left, double right) { return std::abs(left - right) < .5; }), edges.end());
    };
    auto const at = [](std::vector<double> const& edges, double coordinate) {
        return static_cast<int>(std::min_element(edges.begin(), edges.end(), [&](double left, double right) {
            return std::abs(left - coordinate) < std::abs(right - coordinate);
        }) - edges.begin());
    };
    for (const auto& table : regions) {
        if (table.GetNamedString(L"kind", L"") != L"table") continue;
        const auto tableId = table.GetNamedString(L"id");
        std::vector<double> xs, ys;
        Rect bounds;
        for (const auto& cell : regions) {
            if (!childCell(cell, tableId)) continue;
            const auto box = rect(cell, L"rect");
            xs.insert(xs.end(), {box.left, box.right});
            ys.insert(ys.end(), {box.top, box.bottom});
            bounds = bounds.united(box);
        }
        if (bounds.empty()) continue;
        unique(xs);
        unique(ys);

        const auto oldYs = table.GetNamedArray(L"yEdges", nullptr);
        const auto headers = integer(table, L"headerRows");
        if (oldYs && headers > 0 && static_cast<uint32_t>(headers) < oldYs.Size())
            number(table, L"headerRows", at(ys, oldYs.GetNumberAt(headers)));
        table.Insert(L"xEdges", numbers(xs));
        table.Insert(L"yEdges", numbers(ys));
        table.Insert(L"rect", rectValue(bounds));
        number(table, L"rows", static_cast<double>(ys.size() - 1));
        number(table, L"columns", static_cast<double>(xs.size() - 1));

        for (const auto& cell : regions) {
            if (!childCell(cell, tableId)) continue;
            const auto box = rect(cell, L"rect");
            const auto column = at(xs, box.left);
            const auto row = at(ys, box.top);
            number(cell, L"column", column);
            number(cell, L"row", row);
            number(cell, L"columnSpan", at(xs, box.right) - column);
            number(cell, L"rowSpan", at(ys, box.bottom) - row);
            for (const auto& block : blocks) {
                if (block.GetNamedString(L"container", L"") != cell.GetNamedString(L"id")) continue;
                for (auto key : {L"row", L"column", L"rowSpan", L"columnSpan"}) block.Insert(key, cell.Lookup(key));
                block.Insert(L"containerRect", cell.Lookup(L"rect"));
            }
        }
    }
}

void refreshHeaders(JsonObject const& table, std::vector<JsonObject> const& blocks)
{
    auto headerRows = integer(table, L"headerRows", 1);
    if (!table.GetNamedBoolean(L"headersEdited", false)) {
        headerRows = 0;
        for (int row = 0; row < (std::min)(4, integer(table, L"rows")); ++row) {
            int count = 0, strong = 0, numeric = 0;
            for (const auto& block : blocks) {
                if (integer(block, L"row") != row) continue;
                ++count;
                if (block.GetNamedBoolean(L"bold", false) || integer(block, L"columnSpan", 1) > 1) ++strong;
                if (numericCell(block.GetNamedString(L"source", L""))) ++numeric;
            }
            if (count == 0 || numeric * 10 > count * 3 || (row > 0 && strong * 2 < count)) break;
            ++headerRows;
        }
        number(table, L"headerRows", headerRows);
    }
    JsonArray headers;
    for (const auto& block : blocks) {
        const auto source = block.GetNamedString(L"source", L"");
        if (integer(block, L"row") >= headerRows || trim(source).empty()) continue;
        JsonObject header;
        number(header, L"column", integer(block, L"column"));
        number(header, L"columnSpan", integer(block, L"columnSpan", 1));
        text(header, L"text", source);
        headers.Append(header);
    }
    table.Insert(L"headers", headers);
}

JsonObject content(JsonObject const& page)
{
    return page.GetNamedObject(L"scan", page);
}

struct Edit {
    JsonObject page;
    JsonObject data;
    JsonObject layout;
    Objects blocks;
    Objects regions;

    explicit Edit(JsonObject const& original)
        : page(original ? JsonObject::Parse(original.Stringify()) : nullptr),
          data(page ? content(page) : nullptr), layout(data ? data.GetNamedObject(L"layout", JsonObject{}) : nullptr)
    {
        if (!data) fail(L"没有可校正的页面。");
        blocks = objects(data.GetNamedArray(L"blocks", nullptr));
        regions = objects(layout.GetNamedArray(L"regions", nullptr));
    }

    JsonObject commit()
    {
        if (blocks.size() > 1500 || regions.size() > 6000) fail(L"区域过多，请拆分文档。");
        const Rect bounds{0, 0, page.GetNamedNumber(L"width", 0), page.GetNamedNumber(L"height", 0)};
        if (!finiteRect(bounds, 3)) fail(L"页面尺寸无效。");
        const auto pageBounds = bounds.inset(-1);
        size_t sourceSize = 0, translationSize = 0;
        const bool scanned = static_cast<bool>(page.GetNamedObject(L"scan", nullptr));
        std::wstring scanSource, scanTranslation;
        bool staleScan = false;
        std::set<hstring> ids;
        std::map<hstring, JsonArray> children;
        std::map<hstring, Objects> tableBlocks;
        JsonArray order;
        for (const auto& region : regions) {
            const auto id = region.GetNamedString(L"id", L"");
            const auto box = rect(region, L"rect");
            if (id.empty() || !ids.insert(id).second || !finiteRect(box, 3) || !pageBounds.contains(box))
                fail(L"栏或单元格边界无效。");
            if (const auto parent = region.GetNamedString(L"parent", L""); !parent.empty())
                children[parent].Append(JsonValue::CreateStringValue(id));
        }
        ids.clear();
        for (size_t index = 0; index < blocks.size(); ++index) {
            const auto& block = blocks[index];
            const auto id = block.GetNamedString(L"id", L"");
            const auto area = rect(block, L"region");
            const auto original = rect(block, L"rect");
            if (id.empty() || !ids.insert(id).second || !finiteRect(area, 3) || original.empty()
                || !pageBounds.contains(area) || !pageBounds.contains(original))
                fail(L"校正后的区域无效或超出页面。");
            const auto source = block.GetNamedString(L"source", L"").size();
            const auto translation = block.GetNamedString(L"translation", L"").size();
            if (source > 16000 || translation > 16000) fail(L"单个区域最多 16,000 字符。");
            sourceSize += source;
            translationSize += translation;
            if (scanned) {
                if (index) {
                    scanSource += L"\n\n";
                    scanTranslation += L"\n\n";
                }
                const bool fixed = block.GetNamedBoolean(L"fixed", false) || block.GetNamedBoolean(L"keep", false);
                const auto text = block.GetNamedString(L"source", L"");
                scanSource.append(std::wstring_view(text));
                scanTranslation.append(std::wstring_view(fixed ? text : block.GetNamedString(L"translation", L"")));
                staleScan = staleScan || (!fixed && translation && block.GetNamedBoolean(L"stale", false));
            }
            number(block, L"order", static_cast<double>(index));
            remove(block, L"layoutError");
            order.Append(JsonValue::CreateStringValue(id));
            children[block.GetNamedString(L"container", L"")].Append(JsonValue::CreateStringValue(id));
            if (const auto table = block.GetNamedString(L"table", L""); !table.empty()) tableBlocks[table].push_back(block);
        }
        if (sourceSize > 32000 || translationSize > 64000) fail(L"校正后的页面文字超过限制，请拆分文档。");
        for (const auto& region : regions) {
            const auto id = region.GetNamedString(L"id");
            region.Insert(L"children", children[id]);
            if (region.GetNamedString(L"kind", L"") == L"table") refreshHeaders(region, tableBlocks[id]);
        }
        number(layout, L"version", 1);
        layout.Insert(L"order", order);
        layout.Insert(L"regions", array(regions));
        data.Insert(L"blocks", array(blocks));
        data.Insert(L"layout", layout);
        if (scanned) {
            text(data, L"source", hstring(scanSource));
            text(data, L"translation", hstring(scanTranslation));
            flag(data, L"stale", staleScan);
        }
        return page;
    }
};

auto byId(Objects& values, hstring const& id)
{
    return std::find_if(values.begin(), values.end(), [&](const auto& value) { return value.GetNamedString(L"id", L"") == id; });
}

void resetBackground(JsonObject const& block)
{
    remove(block, L"backgroundConfirmed");
    remove(block, L"backgroundPreviewed");
}

void resetTranslation(JsonObject const& block)
{
    text(block, L"translation", L"");
    for (auto key : {L"fixed", L"keep", L"stale"}) flag(block, key, false);
    for (auto key : {L"styledSource", L"originalSource", L"numbersConfirmed"}) remove(block, key);
    resetBackground(block);
}

JsonObject cellEntry(hstring const& id, hstring const& table, Rect area)
{
    JsonObject cell;
    text(cell, L"id", id);
    text(cell, L"kind", L"cell");
    text(cell, L"parent", table);
    cell.Insert(L"rect", rectValue(area));
    return cell;
}

hstring joined(std::vector<hstring> const& values, std::wstring_view separators)
{
    std::wstring result;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) result.append(separators);
        result.append(std::wstring_view(values[i]));
    }
    return hstring(result);
}

}

JsonObject movePdfBlock(JsonObject const& page, hstring const& id, int position)
{
    Edit edit(page);
    if (position < 0 || static_cast<size_t>(position) >= edit.blocks.size()) fail(L"阅读顺序超出范围。");
    const auto found = byId(edit.blocks, id);
    if (found == edit.blocks.end()) fail(L"区域已变化，请重新选择。");
    auto block = *found;
    edit.blocks.erase(found);
    edit.blocks.insert(edit.blocks.begin() + position, std::move(block));
    return edit.commit();
}

JsonObject revisePdfRegion(JsonObject const& page, hstring const& id, JsonObject const& changes, JsonObject const& previousPage)
{
    Edit edit(page);
    const auto found = byId(edit.regions, id);
    if (found == edit.regions.end() || !changes) fail(L"区域已变化，请重新选择。");
    const auto node = *found;
    const auto kind = node.GetNamedString(L"kind", L"");
    const bool table = kind == L"table";
    if (changes.HasKey(L"rect")) {
        const auto boundary = rect(changes, L"rect");
        if (!finiteRect(boundary, 3)) fail(L"区域宽高至少 3 点。");
        if (table) fail(L"请调整表格中的单元格边界。");
        const auto parent = node.GetNamedString(L"parent", L"");
        for (auto other = edit.regions.begin(); other != edit.regions.end(); ++other) {
            if (other == found || other->GetNamedString(L"parent", L"") != parent || other->GetNamedString(L"kind", L"") != kind) continue;
            const auto overlap = boundary.intersected(rect(*other, L"rect"));
            if (overlap.width() > .5 && overlap.height() > .5) fail(L"区域与相邻栏或单元格重叠，请先缩小相邻区域。");
        }
        node.Insert(L"rect", rectValue(boundary));
        for (const auto& block : edit.blocks) {
            if (block.GetNamedString(L"container", L"") != id) continue;
            if (!boundary.inset(-1).contains(rect(block, L"rect")))
                fail(L"边界不能切掉原文。需要拆分时，请使用拆分区域并校对两部分原文。");
            auto area = rect(block, L"region");
            const auto old = rect(block, L"containerRect");
            const auto inset = kind == L"cell" ? 2.0 : 0.0;
            if (std::abs(area.right - old.right) < 4) area.right = boundary.right - inset;
            if (std::abs(area.bottom - old.bottom) < 4) area.bottom = boundary.bottom - inset;
            block.Insert(L"region", rectValue(area.intersected(boundary)));
            block.Insert(L"containerRect", node.Lookup(L"rect"));
            resetBackground(block);
        }
    }
    if (changes.HasKey(L"headerRows")) {
        const auto headers = integer(changes, L"headerRows");
        if (!table || headers < 0 || headers > integer(node, L"rows")) fail(L"表头行数无效。");
        number(node, L"headerRows", headers);
        flag(node, L"headersEdited", true);
    }
    if (changes.HasKey(L"previousPage")) {
        if (!table) fail(L"只有表格可以关联表头。");
        const auto previous = integer(changes, L"previousPage", -1);
        flag(node, L"continuationEdited", true);
        remove(node, L"continuedFrom");
        remove(node, L"inheritedHeaders");
        if (previous >= 0) {
            const auto current = integer(edit.page, L"number", -1);
            if (current >= 0 && previous >= current) fail(L"表头只能关联前面的页面。");
            if (!previousPage || integer(previousPage, L"number", previous) != previous)
                fail(L"没有可关联的上一页布局。");
            const auto previousLayout = content(previousPage).GetNamedObject(L"layout", JsonObject{});
            const auto requested = changes.GetNamedString(L"previousTable", L"");
            const auto columns = integer(node, L"columns");
            JsonObject match{nullptr};
            for (const auto& candidate : objects(previousLayout.GetNamedArray(L"regions", nullptr))) {
                if (candidate.GetNamedString(L"kind", L"") == L"table" && integer(candidate, L"columns") == columns
                    && (requested.empty() || candidate.GetNamedString(L"id", L"") == requested)) match = candidate;
            }
            auto headers = match ? match.GetNamedArray(L"headers", nullptr) : JsonArray{nullptr};
            if (match && (!headers || headers.Size() == 0)) headers = match.GetNamedArray(L"inheritedHeaders", nullptr);
            if (!headers || headers.Size() == 0) fail(L"所选页没有列数相同且已识别表头的表格。");
            JsonObject continuation;
            number(continuation, L"page", previous);
            text(continuation, L"table", match.GetNamedString(L"id"));
            node.Insert(L"continuedFrom", continuation);
            node.Insert(L"inheritedHeaders", JsonArray::Parse(headers.Stringify()));
        }
    }
    reindexTables(edit.regions, edit.blocks);
    return edit.commit();
}

JsonObject mergePdfBlocks(JsonObject const& page, std::vector<hstring> const& selection)
{
    const std::set<hstring> ids{selection.begin(), selection.end()};
    if (ids.size() < 2) fail(L"请选择至少两个相邻区域。");
    Edit edit(page);
    std::vector<size_t> indices;
    std::set<hstring> containers;
    std::set<int> uniqueObjects;
    std::vector<hstring> sources, translations;
    JsonArray masks;
    Rect source, area;
    JsonObject merged{nullptr};
    for (size_t i = 0; i < edit.blocks.size(); ++i) {
        const auto& block = edit.blocks[i];
        if (!ids.contains(block.GetNamedString(L"id", L""))) continue;
        if (block.GetNamedBoolean(L"protected", false)) fail(L"原生文字对象不能独立修改，请先用识图处理该页。");
        if (!merged) merged = block;
        const auto table = block.GetNamedString(L"table", L"");
        const auto container = block.GetNamedString(L"container", L"");
        if (table != merged.GetNamedString(L"table", L"") || (table.empty() && container != merged.GetNamedString(L"container", L"")))
            fail(L"只能合并同一栏或同一表格中的相邻区域。");
        indices.push_back(i);
        containers.insert(container);
        sources.push_back(block.GetNamedString(L"source", L""));
        if (const auto translation = block.GetNamedString(L"translation", L""); !translation.empty()) translations.push_back(translation);
        source = source.united(rect(block, L"rect"));
        area = area.united(rect(block, L"region"));
        if (const auto existing = block.GetNamedArray(L"masks", nullptr))
            for (const auto& mask : existing) masks.Append(mask);
        if (const auto existing = block.GetNamedArray(L"objects", nullptr))
            for (const auto& object : existing) {
                const auto value = object.GetNumber();
                if (!std::isfinite(value) || std::trunc(value) != value || value < 0 || value > (std::numeric_limits<int>::max)())
                    fail(L"源文字对象编号无效。");
                uniqueObjects.insert(static_cast<int>(value));
            }
    }
    if (indices.size() != ids.size()) fail(L"部分区域已变化，请重新选择。");
    if (indices.back() - indices.front() + 1 != indices.size()) fail(L"合并区域之间有其他文字，请按阅读顺序选择连续区域。");

    const auto tableId = merged.GetNamedString(L"table", L"");
    if (!tableId.empty() && containers.size() > 1) {
        const auto table = byId(edit.regions, tableId);
        if (table == edit.regions.end() || !table->GetNamedBoolean(L"inferred", false))
            fail(L"这些单元格之间有原始表格线，合并会让文字跨线。可合并无边框表格或同一单元格内的文字。");
        Rect cells;
        double cellArea = 0;
        for (const auto& region : edit.regions) {
            if (!containers.contains(region.GetNamedString(L"id", L""))) continue;
            const auto boundary = rect(region, L"rect");
            cells = cells.united(boundary);
            cellArea += boundary.width() * boundary.height();
        }
        if (cells.empty() || std::abs(cells.width() * cells.height() - cellArea) > cellArea * .02)
            fail(L"请选择能组成矩形的相邻单元格。");
        for (const auto& block : edit.blocks) {
            if (containers.contains(block.GetNamedString(L"container", L"")) && !ids.contains(block.GetNamedString(L"id", L"")))
                fail(L"请选中这些单元格内的全部文字。");
        }
        std::erase_if(edit.regions, [&](const auto& region) { return containers.contains(region.GetNamedString(L"id", L"")); });
        const auto cell = uniqueId(L"cell");
        edit.regions.push_back(cellEntry(cell, tableId, cells));
        text(merged, L"container", cell);
        merged.Insert(L"containerRect", rectValue(cells));
        area = cells.inset(2);
    }

    text(merged, L"id", uniqueId(L"block"));
    text(merged, L"source", joined(sources, L"\n"));
    text(merged, L"draftTranslation", joined(translations, L"\n\n"));
    merged.Insert(L"rect", rectValue(source));
    merged.Insert(L"region", rectValue(area));
    merged.Insert(L"objects", numbers(std::vector<double>(uniqueObjects.begin(), uniqueObjects.end())));
    merged.Insert(L"masks", masks);
    resetTranslation(merged);
    edit.blocks.erase(edit.blocks.begin() + indices.front() + 1, edit.blocks.begin() + indices.back() + 1);
    edit.blocks[indices.front()] = merged;
    reindexTables(edit.regions, edit.blocks);
    return edit.commit();
}

JsonObject splitPdfBlock(JsonObject const& page, hstring const& id, bool vertical, double position,
                         hstring const& firstSource, hstring const& secondSource)
{
    if (!std::isfinite(position) || trim(firstSource).empty() || trim(secondSource).empty())
        fail(L"请设置分界位置，并填写两部分的原文。");
    Edit edit(page);
    const auto found = byId(edit.blocks, id);
    if (found == edit.blocks.end()) fail(L"区域已变化，请重新选择。");
    const auto original = *found;
    if (original.GetNamedBoolean(L"protected", false)) fail(L"请先用识图处理该页，再拆分区域。");
    const bool cell = original.GetNamedString(L"role", L"") == L"cell";
    const auto bounds = rect(original, cell ? L"containerRect" : L"region");
    auto first = bounds;
    auto second = bounds;
    if (vertical) {
        first.right = position;
        second.left = position;
    } else {
        first.bottom = position;
        second.top = position;
    }
    if (!finiteRect(first, 6) || !finiteRect(second, 6) || !bounds.contains(first) || !bounds.contains(second))
        fail(L"分界必须位于区域内，两部分宽高至少 6 点。");
    if (cell) {
        const auto originalCell = original.GetNamedString(L"container", L"");
        for (auto next = edit.blocks.begin(); next != edit.blocks.end(); ++next) {
            if (next != found && next->GetNamedString(L"container", L"") == originalCell)
                fail(L"请先合并单元格内的文字，再拆分。");
        }
        std::erase_if(edit.regions, [&](const auto& region) { return region.GetNamedString(L"id", L"") == originalCell; });
    }

    const auto part = [&](Rect area, hstring const& source) {
        const auto block = JsonObject::Parse(original.Stringify());
        text(block, L"id", uniqueId(L"block"));
        text(block, L"source", hstring(trim(source)));
        text(block, L"draftTranslation", original.GetNamedString(L"translation", L""));
        text(block, L"draftSource", original.GetNamedString(L"source", L""));
        resetTranslation(block);
        flag(block, L"manualSplit", true);
        const auto originalArea = rect(original, L"rect").intersected(area);
        block.Insert(L"rect", rectValue(originalArea.empty() ? area.inset(2) : originalArea));
        block.Insert(L"region", rectValue(cell ? area.inset(2) : area));
        JsonArray masks;
        if (const auto previous = original.GetNamedArray(L"masks", nullptr)) {
            for (const auto& mask : previous) {
                const auto clipped = rect(mask.GetArray()).intersected(area);
                if (!clipped.empty()) masks.Append(rectValue(clipped));
            }
        }
        block.Insert(L"masks", masks);
        if (cell) {
            const auto newId = uniqueId(L"cell");
            text(block, L"container", newId);
            block.Insert(L"containerRect", rectValue(area));
            edit.regions.push_back(cellEntry(newId, block.GetNamedString(L"table", L""), area));
        }
        return block;
    };
    auto firstPart = part(first, firstSource);
    auto secondPart = part(second, secondSource);
    *found = std::move(firstPart);
    edit.blocks.insert(found + 1, std::move(secondPart));
    reindexTables(edit.regions, edit.blocks);
    return edit.commit();
}

}
