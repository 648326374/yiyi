#include "pdfanalysis.h"

#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace winrt;
using namespace Windows::Data::Json;

namespace {

void check(bool condition, char const* message)
{
    if (!condition) throw std::runtime_error(message);
}

JsonArray numbers(std::initializer_list<double> values)
{
    JsonArray result;
    for (const auto value : values) result.Append(JsonValue::CreateNumberValue(value));
    return result;
}

void number(JsonObject const& object, hstring const& name, double value)
{
    object.Insert(name, JsonValue::CreateNumberValue(value));
}

void text(JsonObject const& object, hstring const& name, hstring const& value)
{
    object.Insert(name, JsonValue::CreateStringValue(value));
}

struct Input {
    JsonObject raw;
    JsonArray glyphs;
    JsonArray objects;
    JsonArray rules;
    std::wstring plain;
    int textIndex{};

    Input()
    {
        number(raw, L"width", 600);
        number(raw, L"height", 800);
        raw.Insert(L"glyphs", glyphs);
        raw.Insert(L"objects", objects);
        raw.Insert(L"rules", rules);
    }

    int addObject(int type, JsonArray const& area)
    {
        JsonObject object;
        const auto id = static_cast<int>(objects.Size());
        number(object, L"id", id);
        number(object, L"type", type);
        object.Insert(L"rect", area);
        objects.Append(object);
        return id;
    }

    double addText(std::wstring_view value, double left, double baseline, double size = 12, hstring const& font = L"Arial",
                   bool spaceBefore = true)
    {
        const auto advance = size * .55;
        const int id = addObject(1, numbers({left, baseline - size * .75, advance * value.size(), size * .82}));
        for (const auto ch : value) {
            if (ch == L' ') {
                spaceBefore = true;
            } else {
                JsonObject glyph;
                text(glyph, L"text", hstring(std::wstring_view(&ch, 1)));
                number(glyph, L"textIndex", textIndex);
                number(glyph, L"object", id);
                number(glyph, L"baseline", baseline);
                number(glyph, L"size", size);
                text(glyph, L"fontName", font);
                glyph.Insert(L"rect", numbers({left, baseline - size * .75, advance * .84, size * .82}));
                glyph.Insert(L"spaceBefore", JsonValue::CreateBooleanValue(spaceBefore));
                glyphs.Append(glyph);
                spaceBefore = false;
            }
            ++textIndex;
            left += advance;
        }
        if (!plain.empty()) plain += L'\n';
        plain += value;
        text(raw, L"text", hstring(plain));
        return left;
    }

    JsonObject analyze(int page = 0)
    {
        return yiyi::pdf::analyzePageInput(raw, page);
    }
};

JsonObject region(JsonObject const& page, hstring const& kind)
{
    for (const auto& value : page.GetNamedObject(L"layout").GetNamedArray(L"regions")) {
        const auto item = value.GetObject();
        if (item.GetNamedString(L"kind") == kind) return item;
    }
    throw std::runtime_error("Requested region is missing");
}

JsonObject block(JsonObject const& page, std::wstring_view search)
{
    for (const auto& value : page.GetNamedArray(L"blocks")) {
        const auto item = value.GetObject();
        const auto source = item.GetNamedString(L"source");
        if (std::wstring_view(source).find(search) != std::wstring_view::npos) return item;
    }
    throw std::runtime_error("Requested block is missing");
}

void flowingText()
{
    Input input;
    input.addText(L"Structure analysis for a full-width document header", 65, 50, 17, L"Arial Bold");
    input.addText(L"First left column paragraph has hyphen-", 45, 115, 11, L"Times");
    input.addText(L"First right column paragraph displays.", 345, 115, 11, L"Times");
    input.addText(L"ation and continues across a text line.", 45, 129, 11, L"Times");
    input.addText(L"Second left column paragraph to translate.", 45, 180, 11, L"Times");
    input.addText(L"Second right column paragraph to translate.", 345, 180, 11, L"Times");
    const auto page = input.analyze();
    const auto first = block(page, L"paragraph has hyphenation");
    check(first.GetNamedArray(L"objects").Size() == 2 && first.GetNamedArray(L"masks").Size() == 2
        && first.GetNamedBoolean(L"serif"), "Paragraph join lost text or geometry");
    const auto left = block(page, L"Second left");
    const auto right = block(page, L"First right");
    check(left.GetNamedNumber(L"order") < right.GetNamedNumber(L"order")
          && left.GetNamedString(L"container") != right.GetNamedString(L"container"), "Column reading order interleaved");
    check(page.GetNamedString(L"baseLayoutHash").size() == 64
          && page.GetNamedString(L"baseLayoutHash") == input.analyze().GetNamedString(L"baseLayoutHash"), "Layout hash is unstable");
    check(region(page, L"column").GetNamedArray(L"children").Size() != 0, "Column children missing");
}

void sourceFeatures()
{
    Input input;
    const auto a = input.addText(L"An important ", 44, 62, 12, L"Arial");
    const auto b = input.addText(L"mixed", a, 62, 12, L"Arial Bold Italic", true);
    input.addText(L" style and color source.", b, 62);
    auto c = input.addText(L"Energy E", 44, 122);
    input.addText(L"2", c - .5, 118, 8, L"Arial", false);
    input.addText(L" relates to mass.", c + 5, 122, 12, L"Arial");
    input.addText(L"12.", 45, 169, 12);
    input.addText(L"List entry stays independent.", 100, 169, 12);
    input.addText(L"float sample(float x) { return x; }", 45, 228, 12, L"Courier");
    input.addText(L"float y; // explain the state", 45, 270, 12, L"Courier");
    input.addText(L"Adjacent art splits this sentence", 45, 330);
    input.addObject(3, numbers({95, 318, 70, 22}));
    input.addText(L"A protected surrounding image caption", 45, 390);
    input.addObject(2, numbers({44, 391, 180, 1}));
    const auto page = input.analyze(3);
    const auto styled = block(page, L"mixed").GetNamedString(L"styledSource");
    check(std::wstring_view(styled).find(L"<b><i>") != std::wstring_view::npos, "Mixed style was lost");
    const auto formula = block(page, L"Energy").GetNamedString(L"source");
    check(std::wstring_view(formula).find(L"E²") != std::wstring_view::npos, "Small script was detached");
    check(block(page, L"12.").GetNamedBoolean(L"fixed"), "List marker is editable");
    check(block(page, L"return x").GetNamedBoolean(L"fixed") && !block(page, L"// explain").GetNamedBoolean(L"fixed"),
          "Code versus comments classification changed");
    check(block(page, L"Adjacent art").GetNamedBoolean(L"protected"), "Intersecting artwork did not protect text");
    check(!block(page, L"surrounding").GetNamedBoolean(L"protected", false), "Link underline blocked a paragraph");
    const auto id = block(page, L"mixed").GetNamedString(L"id");
    check(std::wstring_view(id).starts_with(L"p3-b"), "Block ID has no physical page prefix");
}

void tables()
{
    Input ruled;
    for (const auto y : {560.0, 590.0, 620.0}) ruled.rules.Append(numbers({40, y, 220, y}));
    for (const auto x : {40.0, 130.0, 220.0})
        ruled.rules.Append(numbers({x, x == 130 ? 590.0 : 560.0, x, 620}));
    ruled.addText(L"Merged header", 51, 581, 11, L"Arial Bold");
    ruled.addText(L"Label", 51, 612);
    ruled.addText(L"12", 149, 612);
    auto first = ruled.analyze(4);
    const auto merged = block(first, L"Merged header");
    auto table = region(first, L"table");
    check(!table.GetNamedBoolean(L"inferred") && table.GetNamedNumber(L"rows") == 2
        && merged.GetNamedNumber(L"columnSpan") == 2 && table.GetNamedArray(L"headers").Size() == 1, "Ruled merged header failed");
    Input inferred;
    for (const auto y : {51.0, 81.0, 111.0}) {
        inferred.addText(y == 51 ? L"Category" : L"Samples", 52, y, 11, y == 51 ? L"Arial Bold" : L"Arial");
        inferred.addText(y == 51 ? L"Count" : L"234", 265, y, 11, y == 51 ? L"Arial Bold" : L"Arial");
    }
    const auto detected = inferred.analyze();
    check(region(detected, L"table").GetNamedBoolean(L"inferred") && block(detected, L"Category").GetNamedString(L"role") == L"cell",
          "Borderless data table was not recognized");

    Input following;
    for (const auto y : {25.0, 55.0, 85.0}) following.rules.Append(numbers({40, y, 220, y}));
    for (const auto x : {40.0, 130.0, 220.0}) following.rules.Append(numbers({x, 25, x, 85}));
    following.addText(L"1", 51, 45);
    following.addText(L"2", 141, 45);
    following.addText(L"Values", 51, 75);
    following.addText(L"3", 141, 75);
    auto next = following.analyze(5);
    yiyi::pdf::linkPageTableHeaders(first, next);
    check(region(next, L"table").GetNamedObject(L"continuedFrom").GetNamedNumber(L"page") == 4
        && region(next, L"table").GetNamedArray(L"inheritedHeaders").Size() == 1, "Cross-page headers were not linked");
}

void guards()
{
    Input blank;
    check(blank.analyze().GetNamedString(L"kind") == L"blank", "Blank page was misclassified");
    blank.addObject(3, numbers({0, 0, 600, 800}));
    blank.addText(L"17", 40, 700);
    const auto scan = blank.analyze();
    check(scan.GetNamedString(L"kind") == L"scan" && scan.GetNamedArray(L"blocks").Size() == 0, "Scan with watermark unguarded");
    Input rotated;
    rotated.addText(L"Original page text", 55, 100);
    number(rotated.raw, L"rotation", 1);
    const auto protectedPage = rotated.analyze();
    check(!protectedPage.GetNamedString(L"warning").empty() && protectedPage.GetNamedArray(L"blocks").Size() == 0, "Rotation ignored");
}

}

int main()
{
    try {
        init_apartment();
        flowingText();
        sourceFeatures();
        tables();
        guards();
        std::cout << "PDF text paragraphs, columns, fixed text, ruled/inferred tables and protection passed.\n";
        return 0;
    } catch (hresult_error const& error) {
        std::cerr << to_string(error.message()) << '\n';
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
