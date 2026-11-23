#include "pdfscanlayout.h"

#include <windows.h>
#undef GetObject

#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace winrt;
using namespace Windows::Data::Json;

namespace {

void check(bool condition, char const* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Image {
    static constexpr int width = 640, height = 420, stride = width * 4;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(stride * height, 255);

    void paint(int left, int top, int right, int bottom, std::array<uint8_t, 3> rgb)
    {
        for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
            auto* pixel = bytes.data() + y * stride + x * 4;
            pixel[0] = rgb[2]; pixel[1] = rgb[1]; pixel[2] = rgb[0];
        }
    }

    std::array<uint8_t, 3> at(int x, int y) const
    {
        const auto* pixel = bytes.data() + y * stride + x * 4;
        return {pixel[2], pixel[1], pixel[0]};
    }

    void text(int x, int y, wchar_t const* value, int size = 23)
    {
        const auto dc = CreateCompatibleDC(nullptr);
        BITMAPINFO format{};
        format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        format.bmiHeader.biWidth = width;
        format.bmiHeader.biHeight = -height;
        format.bmiHeader.biPlanes = 1;
        format.bmiHeader.biBitCount = 32;
        void* bits{};
        const auto bmp = CreateDIBSection(dc, &format, DIB_RGB_COLORS, &bits, nullptr, 0);
        const auto font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Arial");
        if (!dc || !bmp || !font) {
            if (font) DeleteObject(font);
            if (bmp) DeleteObject(bmp);
            if (dc) DeleteDC(dc);
            throw std::runtime_error("Failed to create test text bitmap");
        }
        std::memcpy(bits, bytes.data(), bytes.size());
        const auto oldBmp = SelectObject(dc, bmp), oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(10, 10, 10));
        TextOutW(dc, x, y, value, static_cast<int>(std::wcslen(value)));
        GdiFlush();
        std::memcpy(bytes.data(), bits, bytes.size());
        for (size_t i = 3; i < bytes.size(); i += 4) bytes[i] = 255;
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBmp);
        DeleteObject(font);
        DeleteObject(bmp);
        DeleteDC(dc);
    }

    JsonObject analyze(std::filesystem::path const& runtime = {}) const
    {
        return yiyi::pdf::analyzeScanPage(bytes, width, height, stride, width / 2., height / 2., 7, runtime);
    }

    JsonArray erase(JsonArray const& blocks)
    {
        return yiyi::pdf::eraseScanText(bytes, width, height, stride, width / 2., height / 2., blocks);
    }
};

JsonArray box(double x, double y, double w, double h)
{
    JsonArray result;
    for (auto value : {x / 2, y / 2, w / 2, h / 2}) result.Append(JsonValue::CreateNumberValue(value));
    return result;
}

JsonObject block(JsonArray const& rect, wchar_t const* mode = L"auto")
{
    JsonObject result;
    result.Insert(L"id", JsonValue::CreateStringValue(L"test"));
    result.Insert(L"backgroundMode", JsonValue::CreateStringValue(mode));
    result.Insert(L"rect", rect);
    result.Insert(L"region", rect);
    JsonArray masks;
    masks.Append(rect);
    result.Insert(L"masks", masks);
    return result;
}

JsonArray single(JsonObject const& block)
{
    JsonArray blocks;
    blocks.Append(block);
    return blocks;
}

void geometry(std::filesystem::path const& runtime)
{
    Image image;
    check(image.analyze().GetNamedArray(L"blocks").Size() == 0, "Blank scan produced text");
    image.text(36, 18, L"A scanned passage");
    image.text(36, 47, L"Source text continues");
    constexpr std::array<uint8_t, 3> black{10, 10, 10}, art{21, 120, 192};
    for (const auto y : {100, 158, 216}) image.paint(30, y, 608, y + 1, black);
    for (const auto x : {30, 320, 607}) image.paint(x, 100, x + 1, 217, black);
    image.text(50, 109, L"First heading");
    image.text(340, 109, L"Second heading");
    image.text(50, 170, L"First cell data");
    image.text(340, 170, L"Second cell data");
    image.paint(359, 287, 450, 360, art);

    const auto output = image.analyze(runtime);
    const auto blocks = output.GetNamedArray(L"blocks");
    const auto layout = output.GetNamedObject(L"layout");
    const auto order = layout.GetNamedArray(L"order");
    check(blocks.Size() >= 5 && order.Size() == blocks.Size(), "Scan text or layout order missing");
    int cells = 0;
    for (uint32_t i = 0; i < blocks.Size(); ++i) {
        const auto item = blocks.GetObjectAt(i);
        const auto id = item.GetNamedString(L"id");
        check(std::wstring_view(id).starts_with(L"scan-p7-b") && id == order.GetStringAt(i), "Scan block identity mismatch");
        check(item.GetNamedBoolean(L"scan") && item.GetNamedArray(L"masks").Size() >= 1, "Scan text has no masks");
        check(item.GetNamedObject(L"background").GetNamedString(L"kind") == L"solid", "Plain scan background is not solid");
        if (item.GetNamedString(L"role") == L"cell") ++cells;
    }
    check(cells == 4, "Table assignments did not preserve four scan cells");
    check(image.erase(blocks).Size() == 0, "White scan background could not be erased");
    check(image.at(320, 124) == black && image.at(64, 158) == black, "Clearing text erased a table border");
    check(image.at(400, 300) == art, "Clearing text erased unrelated artwork");
    int remaining = 0;
    for (int y = 111; y < 137; ++y) for (int x = 52; x < 170; ++x)
        if (image.at(x, y)[0] < 230) ++remaining;
    check(remaining <= 3, "Clearing a table cell left source strokes");
}

void backgrounds()
{
    Image image;
    for (int x = 0; x < Image::width; ++x) {
        const auto shade = static_cast<uint8_t>(180 + x / 8);
        image.paint(x, 0, x + 1, Image::height, {shade, static_cast<uint8_t>(shade - 25), shade});
    }
    const auto gradient = image.at(121, 125);
    image.text(80, 101, L"Gradient test here", 35);
    const auto gradientBlock = block(box(76, 100, 330, 43));
    check(image.erase(single(gradientBlock)).Size() == 0, "Gradient erasure failed");
    for (int channel = 0; channel < 3; ++channel)
        check(std::abs(image.at(121, 125)[channel] - gradient[channel]) < 4, "Gradient color not reconstructed");

    Image texture;
    for (int y = 220; y < 370; y += 5) for (int x = 45; x < 583; x += 5) {
        const auto color = static_cast<uint8_t>((x / 5 + y / 5) % 2 ? 144 : 224);
        texture.paint(x, y, x + 5, y + 5, {color, color, color});
    }
    texture.text(40, 52, L"A plain line");
    texture.text(100, 265, L"A textured line", 28);
    const auto plain = box(35, 49, 150, 32), pattern = box(95, 258, 240, 42);
    auto compound = block(plain);
    compound.GetNamedArray(L"masks").Append(pattern);
    const auto before = texture.bytes;
    const auto failed = texture.erase(single(compound));
    check(failed.Size() == 1 && failed.GetObjectAt(0).GetNamedString(L"id") == L"test" && texture.bytes == before,
          "Complex background failure left partial erasure");
    check(texture.erase(single(block(pattern, L"original"))).Size() == 0 && texture.bytes == before,
          "Original-image mode changed the source");
    auto panel = block(pattern, L"panel");
    panel.Insert(L"panelColor", JsonValue::CreateStringValue(L"#234567"));
    check(texture.erase(single(panel)).Size() == 0 && texture.at(102, 267) == std::array<uint8_t, 3>{0x23, 0x45, 0x67},
          "Panel mode did not fill with the selected color");

    Image repair;
    repair.paint(20, 60, 370, 160, {182, 178, 174});
    repair.text(55, 85, L"Repair these strokes", 26);
    const auto previous = repair.bytes;
    check(repair.erase(single(block(box(49, 82, 257, 34), L"repair"))).Size() == 0, "Local repair failed");
    check(repair.bytes != previous, "Local repair left all source strokes");
    int dark = 0;
    for (int y = 85; y < 112; ++y) for (int x = 55; x < 285; ++x)
        if (repair.at(x, y)[0] < 100) ++dark;
    check(dark == 0, "Local repair copied dark strokes into the background");
}

}

int wmain(int argc, wchar_t** argv)
{
    try {
        init_apartment();
        geometry(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
        backgrounds();
        std::cout << "PDF scan regions, table rules, background modes and atomic failure passed.\n";
        return 0;
    } catch (hresult_error const& error) {
        std::cerr << to_string(error.message()) << '\n';
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
