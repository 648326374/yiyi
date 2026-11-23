#include "../pdf/pdfoverlay.h"

#include <windows.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

#define PDFIUM_FUNCTIONS(X) \
    X(FPDF_InitLibrary) X(FPDF_DestroyLibrary) X(FPDF_LoadMemDocument64) X(FPDF_CloseDocument) \
    X(FPDF_LoadPage) X(FPDF_ClosePage) X(FPDF_GetPageWidthF) X(FPDF_GetPageHeightF) \
    X(FPDFText_LoadPage) X(FPDFText_ClosePage) X(FPDFText_CountChars) X(FPDFText_GetText) \
    X(FPDFBitmap_Create) X(FPDFBitmap_Destroy) X(FPDFBitmap_GetBuffer) X(FPDFBitmap_GetStride) \
    X(FPDFBitmap_FillRect) X(FPDF_RenderPageBitmap) X(FPDFPage_CountObjects) X(FPDFPage_GetObject) \
    X(FPDFTextObj_GetFont) X(FPDFFont_GetIsEmbedded) X(FPDF_CreateNewDocument) X(FPDFPage_New) \
    X(FPDF_NewXObjectFromPage) X(FPDF_NewFormObjectFromXObject) X(FPDF_CloseXObject) \
    X(FPDFPage_InsertObject) X(FPDFPage_GenerateContent) X(FPDF_SaveAsCopy)

struct Pdfium {
    HMODULE library = nullptr;
#define DECLARE(name) decltype(&::name) name = nullptr;
    PDFIUM_FUNCTIONS(DECLARE)
#undef DECLARE

    explicit Pdfium(const std::filesystem::path& dir)
    {
        library = LoadLibraryExW(std::filesystem::absolute(dir / L"pdfium.dll").c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(library != nullptr, "could not load test PDFium library");
#define LOAD(name) name = std::bit_cast<decltype(name)>(GetProcAddress(library, #name)); require(name != nullptr, #name);
        PDFIUM_FUNCTIONS(LOAD)
#undef LOAD
        FPDF_InitLibrary();
    }
    ~Pdfium()
    {
        FPDF_DestroyLibrary();
        FreeLibrary(library);
    }
};

#undef PDFIUM_FUNCTIONS

struct Output : FPDF_FILEWRITE {
    std::vector<std::uint8_t> bytes;
    Output() : FPDF_FILEWRITE{1, [](FPDF_FILEWRITE* object, const void* data, unsigned long size) -> int {
        auto& bytes = static_cast<Output*>(object)->bytes;
        const auto* first = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), first, first + size);
        return 1;
    }} {}
};

std::wstring text(Pdfium& api, FPDF_PAGE page)
{
    const auto extracted = api.FPDFText_LoadPage(page);
    require(extracted != nullptr, "could not extract text");
    const auto count = api.FPDFText_CountChars(extracted);
    require(count > 0, "PDF text layer is empty");
    std::vector<unsigned short> utf16(static_cast<size_t>(count) * 2 + 1);
    const int units = api.FPDFText_GetText(extracted, 0, count, utf16.data());
    api.FPDFText_ClosePage(extracted);
    require(units > 0, "PDF text UTF-16 extraction failed");
    return std::wstring(utf16.begin(), utf16.begin() + units - 1);
}

void assertVisibleInk(Pdfium& api, FPDF_PAGE page, double pageWidth, double pageHeight)
{
    constexpr int zoom = 4;
    const int width = static_cast<int>(pageWidth * zoom), height = static_cast<int>(pageHeight * zoom);
    const auto bitmap = api.FPDFBitmap_Create(width, height, 0);
    require(bitmap != nullptr, "could not allocate overlay raster");
    api.FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xffffffff);
    api.FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, 0);
    const auto* pixels = static_cast<const std::uint8_t*>(api.FPDFBitmap_GetBuffer(bitmap));
    const auto stride = api.FPDFBitmap_GetStride(bitmap);
    int firstLineInk = 0, secondLineInk = 0;
    bool contained = true;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            const auto pixel = pixels + row * stride + 4 * col;
            if (pixel[0] > 235 && pixel[1] > 235 && pixel[2] > 235) continue;
            // Permit one antialiased pixel around the translated rectangular regions.
            if (col < 28 * zoom - 1 || col > 248 * zoom + 1) contained = false;
            if (row >= 33 * zoom - 1 && row <= 61 * zoom + 1) ++firstLineInk;
            else if (row >= 74 * zoom - 1 && row <= 99 * zoom + 1) ++secondLineInk;
            else contained = false;
        }
    }
    api.FPDFBitmap_Destroy(bitmap);
    require(contained && firstLineInk > 400 && secondLineInk > 100, "embedded glyphs missing or overflowed their source bounds");
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) return 2;
    try {
        using namespace yiyi::pdf;
        const std::filesystem::path runtime = argv[1], artifact = argv[2];
        constexpr double width = 300.50, height = 200.75;
        const OverlayRenderer renderer(runtime);
        OverlayText primary;
        primary.text = L"简体中文／—\U0002000B";
        primary.x = 28;
        primary.y = 33;
        primary.width = 220;
        primary.height = 28;
        primary.fontSize = 21;
        primary.color = {15, 40, 130};
        const auto chinese = renderer.fit(primary, width, height);
        require(chinese.fits(), "CJK and supplementary-plane translation did not fit");

        OverlayText styled;
        styled.text = L"<b>Italic</b> <i>词条</i> H<sub>2</sub>O";
        styled.styled = true;
        styled.serif = true;
        styled.centered = true;
        styled.x = 28;
        styled.y = 74;
        styled.width = 220;
        styled.height = 25;
        styled.fontSize = 18;
        const auto formatted = renderer.fit(styled, width, height);
        require(formatted.fits(), "formatted serif text did not fit");

        auto cramped = primary;
        cramped.text.assign(100, L'长');
        cramped.width = 5;
        cramped.height = 5;
        const auto noFit = renderer.fit(cramped, width, height);
        require(!noFit.fits() && !noFit.error.empty(), "overflowing translation was accepted");
        cramped.x = -50;
        const auto offPage = renderer.fit(cramped, width, height);
        require(!offPage.fits(), "off-page area was accepted");
        auto unsupported = primary;
        unsupported.text = L"\U0010FFFF";
        require(!renderer.fit(unsupported, width, height).fits(), "unencodable character was accepted");
        require(renderer.render(width, height, {noFit, offPage}).empty(), "rejected layout generated visible content");

        const auto bytes = renderer.render(width, height, {chinese, noFit, formatted});
        require(!bytes.empty() && bytes.size() < 100000, "overlay did not subset its embedded fonts");
        Pdfium api(runtime);
        const auto source = api.FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
        require(source != nullptr, "PDFium refused the overlay PDF");
        const auto page = api.FPDF_LoadPage(source, 0);
        require(page && std::abs(api.FPDF_GetPageWidthF(page) - width) < .01 &&
            std::abs(api.FPDF_GetPageHeightF(page) - height) < .01, "overlay page dimensions changed");
        require(api.FPDFPage_CountObjects(page) > 1, "missing translated page objects");
        const auto font = api.FPDFTextObj_GetFont(api.FPDFPage_GetObject(page, 0));
        require(font && api.FPDFFont_GetIsEmbedded(font) == 1, "overlay font is not embedded");
        const auto searchable = text(api, page);
        require(searchable.find(primary.text) != std::wstring::npos, "CJK, punctuation, or supplementary Unicode changed");
        require(searchable.find(L"Italic") != std::wstring::npos && searchable.find(L"词条") != std::wstring::npos &&
            searchable.find(L'长') == std::wstring::npos, "styled or rejected text extraction mismatch");
        assertVisibleInk(api, page, width, height);
        api.FPDF_ClosePage(page);

        const auto target = api.FPDF_CreateNewDocument();
        const auto destination = api.FPDFPage_New(target, 0, width, height);
        const auto overlay = api.FPDF_NewXObjectFromPage(target, source, 0);
        require(destination && overlay, "PDFium could not import the overlay page");
        api.FPDF_CloseDocument(source);
        const auto form = api.FPDF_NewFormObjectFromXObject(overlay);
        api.FPDF_CloseXObject(overlay);
        require(form != nullptr, "PDFium could not create the overlay form");
        api.FPDFPage_InsertObject(destination, form);
        require(api.FPDFPage_GenerateContent(destination), "could not write PDF overlay form into page");
        Output saved;
        require(api.FPDF_SaveAsCopy(target, &saved, 0), "could not save PDF containing overlay");
        api.FPDF_ClosePage(destination);
        api.FPDF_CloseDocument(target);

        const auto reopened = api.FPDF_LoadMemDocument64(saved.bytes.data(), saved.bytes.size(), nullptr);
        const auto reopenedPage = reopened ? api.FPDF_LoadPage(reopened, 0) : nullptr;
        require(reopenedPage && text(api, reopenedPage).find(primary.text) != std::wstring::npos,
            "imported PDF is no longer searchable");
        assertVisibleInk(api, reopenedPage, width, height);
        api.FPDF_ClosePage(reopenedPage);
        api.FPDF_CloseDocument(reopened);
        std::ofstream output(artifact, std::ios::binary);
        output.write(reinterpret_cast<const char*>(saved.bytes.data()), static_cast<std::streamsize>(saved.bytes.size()));
        require(static_cast<bool>(output), "could not write overlay test artifact");
        std::cout << "Searchable embedded PDF overlay fits bounds after PDFium round trip (" << bytes.size() << " bytes)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
