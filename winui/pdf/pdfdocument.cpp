#include "pdfdocument.h"
#include "pdfanalysis.h"
#include "pdfmerge.h"
#include "pdfoverlay.h"
#include "pdfscanlayout.h"
#include "pdftaskstore.h"
#include "pdfworkerfiles.h"
#include "../PdfTextChecks.h"

#include <windows.h>
#undef GetObject
#include <bcrypt.h>
#include <wincodec.h>
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_transformpage.h>
#include <objbase.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace yiyi::pdf {
namespace {

using namespace winrt;
using namespace Windows::Data::Json;

constexpr int manifestVersion = 1;
constexpr int maxPreviewPixels = 16'000'000;

[[noreturn]] void fail(wchar_t const* message)
{
    throw hresult_error(E_FAIL, message);
}

void putNumber(JsonObject const& object, hstring const& name, double value)
{
    object.Insert(name, JsonValue::CreateNumberValue(value));
}

void putText(JsonObject const& object, hstring const& name, hstring const& value)
{
    object.Insert(name, JsonValue::CreateStringValue(value));
}

void putFlag(JsonObject const& object, hstring const& name, bool value)
{
    object.Insert(name, JsonValue::CreateBooleanValue(value));
}

void reportProgress(PdfProgress const& progress, hstring const& stage, double completed, double total)
{
    if (!progress) return;
    JsonObject value;
    putText(value, L"stage", stage);
    putNumber(value, L"completed", completed);
    putNumber(value, L"total", total);
    progress(value);
}

int integer(JsonObject const& arguments, hstring const& name, int fallback)
{
    const double value = arguments.GetNamedNumber(name, fallback);
    if (value < (std::numeric_limits<int>::min)() || value > (std::numeric_limits<int>::max)() || std::floor(value) != value)
        fail(L"PDF 处理参数无效。");
    return static_cast<int>(value);
}

std::filesystem::path pathArgument(JsonObject const& arguments, hstring const& name)
{
    const auto value = arguments.GetNamedString(name, L"");
    const std::wstring_view path(value);
    if (path.empty() || path.find(L'\0') != path.npos) fail(L"PDF 处理路径无效。");
    return std::filesystem::path(path);
}

#define PDFIUM_FUNCTIONS(X) \
    X(FPDF_InitLibrary) X(FPDF_DestroyLibrary) X(FPDF_LoadMemDocument64) X(FPDF_CloseDocument) \
    X(FPDF_GetPageCount) X(FPDF_LoadPage) X(FPDF_ClosePage) X(FPDF_GetPageWidthF) X(FPDF_GetPageHeightF) \
    X(FPDFBitmap_CreateEx) X(FPDFBitmap_Destroy) X(FPDF_RenderPageBitmap) \
    X(FPDFPage_GetRotation) X(FPDFPage_GetMediaBox) X(FPDFPage_GetCropBox) X(FPDF_DeviceToPage) \
    X(FPDFPage_CountObjects) X(FPDFPage_GetObject) X(FPDFPageObj_GetType) X(FPDFPageObj_GetBounds) \
    X(FPDFPageObj_GetMatrix) X(FPDFFormObj_CountObjects) X(FPDFFormObj_GetObject) X(FPDFFormObj_RemoveObject) \
    X(FPDFPageObj_GetFillColor) X(FPDFPageObj_SetFillColor) X(FPDFPageObj_GetStrokeColor) X(FPDFPageObj_SetStrokeColor) \
    X(FPDFPath_GetDrawMode) X(FPDFPath_CountSegments) X(FPDFPath_GetPathSegment) \
    X(FPDFPathSegment_GetType) X(FPDFPathSegment_GetPoint) X(FPDFPathSegment_GetClose) \
    X(FPDFPageObj_Transform) X(FPDFPageObj_TransformClipPath) X(FPDFPageObj_Destroy) \
    X(FPDFPage_InsertObject) X(FPDFPage_RemoveObject) X(FPDFPage_GenerateContent) X(FPDF_SaveAsCopy) \
    X(FPDF_NewXObjectFromPage) X(FPDF_CloseXObject) X(FPDF_NewFormObjectFromXObject) \
    X(FPDFPageObj_NewImageObj) X(FPDFImageObj_SetBitmap) X(FPDFPageObj_SetMatrix) \
    X(FPDFPageObj_GetIsActive) X(FPDFPageObj_SetIsActive) \
    X(FPDFText_LoadPage) X(FPDFText_ClosePage) X(FPDFText_CountChars) X(FPDFText_GetUnicode) \
    X(FPDFText_GetCharBox) X(FPDFText_GetCharOrigin) X(FPDFText_GetTextObject) X(FPDFText_GetCharAngle) \
    X(FPDFText_GetFontSize) X(FPDFText_GetFontInfo) X(FPDFText_GetFontWeight) X(FPDFText_GetFillColor)

class Pdfium {
    std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> m_module;

public:
#define DECLARE(name) decltype(&::name) name = nullptr;
    PDFIUM_FUNCTIONS(DECLARE)
#undef DECLARE

    explicit Pdfium(std::filesystem::path const& directory)
        : m_module(LoadLibraryExW((directory / L"pdfium.dll").c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32), FreeLibrary)
    {
        if (!m_module) fail(L"无法加载 PDFium，请运行 scripts/setup-pdf.ps1 或使用完整发行包。");
#define RESOLVE(name) \
    name = std::bit_cast<decltype(name)>(GetProcAddress(m_module.get(), #name)); \
    if (!name) fail(L"PDFium 版本不兼容，请更新 PDF 组件。");
        PDFIUM_FUNCTIONS(RESOLVE)
#undef RESOLVE
        FPDF_InitLibrary();
    }

    ~Pdfium() { FPDF_DestroyLibrary(); }
    Pdfium(Pdfium const&) = delete;
    Pdfium& operator=(Pdfium const&) = delete;
};

#undef PDFIUM_FUNCTIONS

std::wstring canonicalFileName(HANDLE file)
{
    std::wstring name(32768, L'\0');
    const auto count = GetFinalPathNameByHandleW(file, name.data(), static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!count) throw_last_error();
    if (count >= name.size()) fail(L"PDF 路径过长。");
    name.resize(count);
    if (name.starts_with(LR"(\\?\UNC\)"))
        name.replace(0, 8, LR"(\\)");
    else if (name.starts_with(LR"(\\?\)"))
        name.erase(0, 4);
    std::replace(name.begin(), name.end(), L'\\', L'/');
    return name;
}

JsonObject fileStamp(HANDLE file, std::uint64_t size)
{
    FILETIME modified{};
    check_bool(GetFileTime(file, nullptr, nullptr, &modified));
    SYSTEMTIME utc{};
    check_bool(FileTimeToSystemTime(&modified, &utc));
    wchar_t time[32]{};
    swprintf_s(time, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay,
               utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
    JsonObject stamp;
    putNumber(stamp, L"size", static_cast<double>(size));
    putText(stamp, L"modified", time);
    putText(stamp, L"path", hstring(canonicalFileName(file)));
    return stamp;
}

bool sameStamp(JsonObject const& actual, JsonObject const& expected)
{
    if (!expected) return false;
    return actual.GetNamedNumber(L"size") == expected.GetNamedNumber(L"size", -1) &&
        actual.GetNamedString(L"modified") == expected.GetNamedString(L"modified", L"") &&
        CompareStringOrdinal(actual.GetNamedString(L"path").c_str(), -1,
            expected.GetNamedString(L"path", L"").c_str(), -1, TRUE) == CSTR_EQUAL;
}

hstring sha256(unsigned char const* bytes, std::size_t size, PdfProgress const& progress)
{
    struct HashState {
        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_HASH_HANDLE hash{};
        ~HashState()
        {
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } state;
    check_nt(BCryptOpenAlgorithmProvider(&state.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    check_nt(BCryptCreateHash(state.algorithm, &state.hash, nullptr, 0, nullptr, 0, 0));
    for (std::size_t offset = 0; offset < size;) {
        const auto chunk = static_cast<ULONG>(std::min<std::size_t>(size - offset, 4 * 1024 * 1024));
        check_nt(BCryptHashData(state.hash, const_cast<unsigned char*>(bytes + offset), chunk, 0));
        offset += chunk;
        if (progress) {
            JsonObject event;
            putText(event, L"stage", L"fingerprint");
            putNumber(event, L"completed", static_cast<double>(offset));
            putNumber(event, L"total", static_cast<double>(size));
            progress(event);
        }
    }
    std::array<unsigned char, 32> digest{};
    check_nt(BCryptFinishHash(state.hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 0xf];
    }
    return hstring(result);
}

class Document {
    file_handle m_file;
    handle m_mapping;
    std::unique_ptr<void, decltype(&UnmapViewOfFile)> m_view{nullptr, UnmapViewOfFile};

public:
    Pdfium& api;
    FPDF_DOCUMENT handle = nullptr;
    hstring hash;
    JsonObject stamp;
    int pageCount{};

    Document(Pdfium& api, std::filesystem::path const& source, JsonObject const& verified, PdfProgress const& progress)
        : m_file(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)),
          api(api)
    {
        if (!m_file) fail(L"无法读取 PDF，请检查原文件是否存在或正被写入。");
        LARGE_INTEGER length{};
        check_bool(GetFileSizeEx(m_file.get(), &length));
        if (length.QuadPart <= 0 ||
            static_cast<std::uint64_t>(length.QuadPart) > (std::numeric_limits<std::size_t>::max)())
            fail(L"PDF 为空或超出当前系统的地址空间。");
        const auto size = static_cast<std::size_t>(length.QuadPart);
        stamp = fileStamp(m_file.get(), size);
        if (verified) {
            hash = verified.GetNamedString(L"hash", L"");
            if (hash.size() != 64 || !sameStamp(stamp, verified.GetNamedObject(L"stamp", nullptr)))
                fail(L"原 PDF 已变化，请重新导入。");
        }
        m_mapping.attach(CreateFileMappingW(m_file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
        if (!m_mapping) fail(L"无法映射原 PDF。");
        m_view.reset(MapViewOfFile(m_mapping.get(), FILE_MAP_READ, 0, 0, 0));
        if (!m_view) fail(L"无法映射 PDF 文件，请检查可用内存和文件权限。");
        if (!verified) hash = sha256(static_cast<unsigned char const*>(m_view.get()), size, progress);
        handle = api.FPDF_LoadMemDocument64(m_view.get(), size, nullptr);
        if (!handle) fail(L"无法打开此 PDF。请使用未加密、未损坏的 PDF 文件。");
        pageCount = api.FPDF_GetPageCount(handle);
        if (pageCount < 1) {
            api.FPDF_CloseDocument(handle);
            handle = nullptr;
            fail(L"PDF 没有可读取的页面。");
        }
    }

    ~Document() { if (handle) api.FPDF_CloseDocument(handle); }
    Document(Document const&) = delete;
    Document& operator=(Document const&) = delete;

    void checkOutputPath(std::filesystem::path const& destination) const
    {
        const file_handle output(CreateFileW(destination.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!output) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
            throw_last_error();
        }

        FILE_ID_INFO sourceId{}, destinationId{};
        if (GetFileInformationByHandleEx(m_file.get(), FileIdInfo, &sourceId, sizeof(sourceId)) &&
            GetFileInformationByHandleEx(output.get(), FileIdInfo, &destinationId, sizeof(destinationId))) {
            if (sourceId.VolumeSerialNumber == destinationId.VolumeSerialNumber &&
                std::ranges::equal(sourceId.FileId.Identifier, destinationId.FileId.Identifier))
                fail(L"不能覆盖原 PDF，请选择其他路径。");
            return;
        }
        BY_HANDLE_FILE_INFORMATION sourceLegacy{}, destinationLegacy{};
        check_bool(GetFileInformationByHandle(m_file.get(), &sourceLegacy));
        check_bool(GetFileInformationByHandle(output.get(), &destinationLegacy));
        if (sourceLegacy.dwVolumeSerialNumber == destinationLegacy.dwVolumeSerialNumber &&
            sourceLegacy.nFileIndexHigh == destinationLegacy.nFileIndexHigh && sourceLegacy.nFileIndexLow == destinationLegacy.nFileIndexLow)
            fail(L"不能覆盖原 PDF，请选择其他路径。");
    }
};

class Page {
public:
    Pdfium& api;
    FPDF_PAGE handle;
    double width{};
    double height{};

    Page(Document& document, int index)
        : api(document.api), handle(api.FPDF_LoadPage(document.handle, index))
    {
        if (!handle) fail(L"无法读取 PDF 页面。");
        width = api.FPDF_GetPageWidthF(handle);
        height = api.FPDF_GetPageHeightF(handle);
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
            api.FPDF_ClosePage(handle);
            handle = nullptr;
            fail(L"PDF 页面尺寸无效。");
        }
    }

    ~Page() { if (handle) api.FPDF_ClosePage(handle); }
    Page(Page const&) = delete;
    Page& operator=(Page const&) = delete;
};

struct Transform {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    Transform compose(FS_MATRIX const& inner) const
    {
        return {a * inner.a + c * inner.b, b * inner.a + d * inner.b,
                a * inner.c + c * inner.d, b * inner.c + d * inner.d,
                a * inner.e + c * inner.f + e, b * inner.e + d * inner.f + f};
    }

    std::pair<double, double> point(double x, double y) const
    {
        return {a * x + c * y + e, b * x + d * y + f};
    }
};

JsonArray numbers(std::initializer_list<double> values)
{
    JsonArray result;
    for (const auto value : values) result.Append(JsonValue::CreateNumberValue(value));
    return result;
}

JsonArray pageBounds(Transform const& transform, double l, double b, double r, double t, double height)
{
    const auto left = std::min({transform.a * l + transform.c * b, transform.a * l + transform.c * t,
                                transform.a * r + transform.c * b, transform.a * r + transform.c * t}) + transform.e;
    const auto right = std::max({transform.a * l + transform.c * b, transform.a * l + transform.c * t,
                                 transform.a * r + transform.c * b, transform.a * r + transform.c * t}) + transform.e;
    const auto low = std::min({transform.b * l + transform.d * b, transform.b * l + transform.d * t,
                               transform.b * r + transform.d * b, transform.b * r + transform.d * t}) + transform.f;
    const auto high = std::max({transform.b * l + transform.d * b, transform.b * l + transform.d * t,
                                transform.b * r + transform.d * b, transform.b * r + transform.d * t}) + transform.f;
    return numbers({left, height - high, right - left, high - low});
}

struct PageObject {
    FPDF_PAGEOBJECT handle;
    int parent;
    Transform inherited;
};

std::vector<PageObject> pageObjects(Page& page)
{
    auto& api = page.api;
    std::vector<PageObject> objects;
    const auto append = [&](auto&& self, FPDF_PAGEOBJECT object, int parent, Transform const& inherited, int depth) -> void {
        if (!object || depth > 24 || objects.size() >= 20000) fail(L"页面对象过多或嵌套过深，请拆分或简化 PDF。");
        const int index = static_cast<int>(objects.size());
        objects.push_back({object, parent, inherited});
        if (api.FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_FORM) {
            FS_MATRIX matrix{};
            if (!api.FPDFPageObj_GetMatrix(object, &matrix)) fail(L"无法读取嵌套页面对象的坐标。");
            const auto transform = inherited.compose(matrix);
            const int count = api.FPDFFormObj_CountObjects(object);
            for (int i = 0; i < count; ++i)
                self(self, api.FPDFFormObj_GetObject(object, i), index, transform, depth + 1);
        }
    };
    const int count = api.FPDFPage_CountObjects(page.handle);
    for (int i = 0; i < count; ++i) append(append, api.FPDFPage_GetObject(page.handle, i), -1, {}, 0);
    return objects;
}

void describePath(Page& page, PageObject const& item, JsonObject const& node, JsonArray const& rules)
{
    auto& api = page.api;
    const auto object = item.handle;
    int fill{};
    FPDF_BOOL stroke{};
    FS_MATRIX matrix{};
    if (!api.FPDFPath_GetDrawMode(object, &fill, &stroke) || !api.FPDFPageObj_GetMatrix(object, &matrix)) return;
    putFlag(node, L"strokeOnly", stroke && fill == FPDF_FILLMODE_NONE);
    const auto transform = item.inherited.compose(matrix);
    const int count = api.FPDFPath_CountSegments(object);
    JsonArray pathRules;
    std::set<double> xs, ys;
    double startX{}, startY{}, previousX{}, previousY{};
    bool orthogonal = count >= 4 && count <= 5;
    bool closed = false;
    const auto edge = [&](double x, double y) {
        const double dx = std::abs(x - previousX), dy = std::abs(y - previousY);
        if (stroke && ((dx >= 3 && dy < .45) || (dy >= 3 && dx < .45))) {
            auto rule = numbers({previousX, page.height - previousY, x, page.height - y});
            rules.Append(rule);
            pathRules.Append(rule);
        }
    };
    for (int i = 0; i < count && i < 4096; ++i) {
        const auto segment = api.FPDFPath_GetPathSegment(object, i);
        float sx{}, sy{};
        if (!segment || !api.FPDFPathSegment_GetPoint(segment, &sx, &sy) || !std::isfinite(sx) || !std::isfinite(sy)) return;
        const auto [x, y] = transform.point(sx, sy);
        const auto type = api.FPDFPathSegment_GetType(segment);
        if (type == FPDF_SEGMENT_MOVETO) {
            startX = x;
            startY = y;
            if (i != 0) orthogonal = false;
        } else if (type == FPDF_SEGMENT_LINETO && i > 0 &&
                   api.FPDFPathSegment_GetType(api.FPDFPath_GetPathSegment(object, i - 1)) != FPDF_SEGMENT_BEZIERTO) {
            edge(x, y);
            if (x != previousX && y != previousY) orthogonal = false;
        } else {
            orthogonal = false;
        }
        if (orthogonal) {
            xs.insert(x);
            ys.insert(y);
        }
        previousX = x;
        previousY = y;
        if (api.FPDFPathSegment_GetClose(segment)) {
            if (type != FPDF_SEGMENT_BEZIERTO) edge(startX, startY);
            closed = i == count - 1;
            previousX = startX;
            previousY = startY;
        }
    }
    if (pathRules.Size()) node.Insert(L"rules", pathRules);
    unsigned r{}, g{}, b{}, alpha{};
    if (fill != FPDF_FILLMODE_NONE && orthogonal && closed && xs.size() == 2 && ys.size() == 2 &&
        api.FPDFPageObj_GetFillColor(object, &r, &g, &b, &alpha) && alpha == 255)
        node.Insert(L"fillColor", numbers({double(r), double(g), double(b)}));
}

JsonArray describeObjects(Page& page, std::vector<PageObject> const& source, JsonArray const& rules)
{
    JsonArray objects;
    for (auto const& item : source) {
        const auto object = item.handle;
        auto& api = page.api;
        const auto type = api.FPDFPageObj_GetType(object);
        JsonObject node;
        putNumber(node, L"id", objects.Size());
        putNumber(node, L"parent", item.parent);
        putNumber(node, L"type", type);
        float left{}, bottom{}, right{}, top{};
        if (api.FPDFPageObj_GetBounds(object, &left, &bottom, &right, &top) &&
            std::isfinite(left) && std::isfinite(bottom) && std::isfinite(right) && std::isfinite(top)) {
            node.Insert(L"rect", pageBounds(item.inherited, left, bottom, right, top, page.height));
        }
        if (type == FPDF_PAGEOBJ_PATH) describePath(page, item, node, rules);
        objects.Append(node);
    }
    return objects;
}

std::wstring character(unsigned int code)
{
    if (code <= 0xffff) return {static_cast<wchar_t>(code)};
    code -= 0x10000;
    return {static_cast<wchar_t>(0xd800 + (code >> 10)), static_cast<wchar_t>(0xdc00 + (code & 0x3ff))};
}

JsonObject extractPageInput(Page& page)
{
    auto& api = page.api;
    JsonObject result;
    putNumber(result, L"width", page.width);
    putNumber(result, L"height", page.height);
    putNumber(result, L"rotation", api.FPDFPage_GetRotation(page.handle));
    float left{}, bottom{}, right{}, top{};
    if (api.FPDFPage_GetMediaBox(page.handle, &left, &bottom, &right, &top))
        result.Insert(L"mediaBox", numbers({left, bottom, right, top}));
    if (api.FPDFPage_GetCropBox(page.handle, &left, &bottom, &right, &top))
        result.Insert(L"cropBox", numbers({left, bottom, right, top}));
    JsonArray rules;
    const auto objects = pageObjects(page);
    result.Insert(L"objects", describeObjects(page, objects, rules));
    result.Insert(L"rules", rules);
    std::unordered_map<FPDF_PAGEOBJECT, int> textObjects;
    for (int i = 0; i < std::ssize(objects); ++i)
        if (api.FPDFPageObj_GetType(objects[i].handle) == FPDF_PAGEOBJ_TEXT) textObjects.emplace(objects[i].handle, i);

    const std::unique_ptr<std::remove_pointer_t<FPDF_TEXTPAGE>, decltype(api.FPDFText_ClosePage)> textPage(
        api.FPDFText_LoadPage(page.handle), api.FPDFText_ClosePage);
    if (!textPage) fail(L"无法提取页面文字。");
    const auto text = textPage.get();
    const int count = api.FPDFText_CountChars(text);
    if (count < 0) fail(L"无法提取页面文字。");
    if (count > 50000) fail(L"单页文字过多，请简化此页或使用识图处理。");
    std::wstring plain;
    JsonArray glyphs;
    bool incomplete = false;
    bool spaceBefore = false;
    for (int index = 0; index < count; ++index) {
        const auto code = api.FPDFText_GetUnicode(text, index);
        if (code == 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
            incomplete = true;
            continue;
        }
        const auto letter = character(code);
        plain += letter;
        WORD ctype{};
        if (code < 32 || (letter.size() == 1 &&
            GetStringTypeW(CT_CTYPE1, letter.data(), 1, &ctype) && (ctype & C1_SPACE))) {
            spaceBefore = true;
            continue;
        }
        const auto object = api.FPDFText_GetTextObject(text, index);
        const auto found = textObjects.find(object);
        if (found == textObjects.end()) {
            incomplete = true;
            continue;
        }
        double l{}, r{}, b{}, t{}, x{}, y{};
        if (!api.FPDFText_GetCharBox(text, index, &l, &r, &b, &t) ||
            !api.FPDFText_GetCharOrigin(text, index, &x, &y) ||
            !(std::isfinite(l) && std::isfinite(r) && std::isfinite(b) && std::isfinite(t) && std::isfinite(y))) {
            incomplete = true;
            continue;
        }
        double size = api.FPDFText_GetFontSize(text, index);
        FS_MATRIX matrix{};
        if (api.FPDFPageObj_GetMatrix(object, &matrix)) {
            const auto combined = objects[found->second].inherited.compose(matrix);
            size *= std::hypot(combined.c, combined.d);
        }
        if (!std::isfinite(size) || size <= 0 || size > 200) {
            incomplete = true;
            continue;
        }

        JsonObject glyph;
        putText(glyph, L"text", hstring(letter));
        putNumber(glyph, L"textIndex", index);
        putNumber(glyph, L"object", found->second);
        glyph.Insert(L"rect", numbers({l, page.height - t, r - l, t - b}));
        putNumber(glyph, L"baseline", page.height - y);
        putNumber(glyph, L"size", size);
        const auto angle = api.FPDFText_GetCharAngle(text, index);
        putNumber(glyph, L"angle", std::isfinite(angle) ? angle : 0);
        putNumber(glyph, L"fontWeight", api.FPDFText_GetFontWeight(text, index));
        char font[256]{};
        int flags = 0;
        api.FPDFText_GetFontInfo(text, index, font, sizeof(font) - 1, &flags);
        std::wstring fontName;
        for (auto value = font; *value != 0; ++value) fontName += static_cast<unsigned char>(*value);
        putText(glyph, L"fontName", hstring(fontName));
        putNumber(glyph, L"fontFlags", flags);
        unsigned int red = 0, green = 0, blue = 0, alpha = 255;
        api.FPDFText_GetFillColor(text, index, &red, &green, &blue, &alpha);
        glyph.Insert(L"color", numbers({static_cast<double>(red), static_cast<double>(green), static_cast<double>(blue)}));
        glyph.Insert(L"spaceBefore", JsonValue::CreateBooleanValue(spaceBefore));
        spaceBefore = false;
        glyphs.Append(glyph);
    }
    putText(result, L"text", hstring(plain));
    result.Insert(L"glyphs", glyphs);
    result.Insert(L"incompleteText", JsonValue::CreateBooleanValue(incomplete));
    return result;
}

void savePng(std::filesystem::path const& destination, std::vector<BYTE>& data, int width, int height)
{
    com_ptr<IStream> stream;
    check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
    auto factory = create_instance<IWICImagingFactory>(CLSID_WICImagingFactory, CLSCTX_INPROC_SERVER);
    com_ptr<IWICBitmapEncoder> encoder;
    check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    com_ptr<IWICBitmapFrameEncode> frame;
    check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
    check_hresult(frame->Initialize(nullptr));
    check_hresult(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)));
    auto format = GUID_WICPixelFormat32bppBGRA;
    check_hresult(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA) fail(L"无法初始化 PDF 预览的 PNG 编码器。");
    check_hresult(frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(4 * width),
        static_cast<UINT>(data.size()), data.data()));
    check_hresult(frame->Commit());
    check_hresult(encoder->Commit());

    STATSTG stat{};
    check_hresult(stream->Stat(&stat, STATFLAG_NONAME));
    HGLOBAL memory{};
    check_hresult(GetHGlobalFromStream(stream.get(), &memory));
    const auto* bytes = static_cast<char const*>(GlobalLock(memory));
    if (!bytes) throw_last_error();
    try {
        writeWorkerOutput(destination, {bytes, static_cast<std::size_t>(stat.cbSize.QuadPart)});
    } catch (...) {
        GlobalUnlock(memory);
        throw;
    }
    GlobalUnlock(memory);
}

std::vector<BYTE> renderPixels(Page& page, int width, int height, int x, int y, int fullWidth, int fullHeight, int flags)
{
    const auto pixelCount = static_cast<std::int64_t>(width) * height;
    if (pixelCount <= 0 || pixelCount > maxPreviewPixels) fail(L"PDF 预览尺寸过大。");
    std::vector<BYTE> pixels(static_cast<std::size_t>(pixelCount) * 4, 255);
    const auto bitmap = page.api.FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA, pixels.data(), width * 4);
    if (!bitmap) fail(L"PDF 预览内存不足。");
    page.api.FPDF_RenderPageBitmap(bitmap, page.handle, x, y, fullWidth, fullHeight, 0, flags);
    page.api.FPDFBitmap_Destroy(bitmap);
    return pixels;
}

void render(Page& page, std::filesystem::path const& destination, int width, int height,
            int x, int y, int fullWidth, int fullHeight)
{
    auto pixels = renderPixels(page, width, height, x, y, fullWidth, fullHeight, FPDF_ANNOT);
    savePng(destination, pixels, width, height);
}

std::pair<int, int> rasterSize(Page const& page, int pixels)
{
    const auto scale = pixels / std::max(page.width, page.height);
    return {std::max(1, static_cast<int>(std::lround(page.width * scale))),
            std::max(1, static_cast<int>(std::lround(page.height * scale)))};
}

void renderOriginal(Page& page, JsonObject const& arguments, std::filesystem::path const& destination)
{
    const int width = std::clamp(integer(arguments, L"pixels", 1200), 300, 2400);
    const auto height = std::max(1.0, std::round(width * page.height / page.width));
    if (height > maxPreviewPixels / width) fail(L"PDF 预览尺寸过大。");
    render(page, destination, width, static_cast<int>(height), 0, 0, width, static_cast<int>(height));
}

void renderRecognitionRegion(Page& page, JsonObject const& arguments, std::filesystem::path const& destination)
{
    const auto region = arguments.GetNamedArray(L"region", nullptr);
    if (!region || region.Size() != 4) fail(L"识别区域无效。");
    const double x = region.GetNumberAt(0);
    const double y = region.GetNumberAt(1);
    const double w = region.GetNumberAt(2);
    const double h = region.GetNumberAt(3);
    if (x < 0 || y < 0 || w < .005 || h < .005 || x + w > 1 || y + h > 1) fail(L"识别区域无效。");
    const double width = page.width * w;
    const double height = page.height * h;
    const double scale = std::clamp(integer(arguments, L"pixels", 1800), 1000, 2400) / std::max(width, height);
    const auto pixelsWide = std::max(1, static_cast<int>(std::round(width * scale)));
    const auto pixelsHigh = std::max(1, static_cast<int>(std::round(height * scale)));
    const auto fullWidth = static_cast<int>(std::round(pixelsWide / w));
    const auto fullHeight = static_cast<int>(std::round(pixelsHigh / h));
    render(page, destination, pixelsWide, pixelsHigh,
           -static_cast<int>(std::round(x * fullWidth)), -static_cast<int>(std::round(y * fullHeight)), fullWidth, fullHeight);
}

JsonObject pageContent(JsonObject const& data)
{
    return data.GetNamedObject(L"scan", data);
}

bool hasText(hstring const& text)
{
    return std::ranges::any_of(text, [](wchar_t ch) { return !iswspace(ch); });
}

bool wantsTranslation(JsonObject const& block)
{
    return !block.GetNamedBoolean(L"keep", false) && !block.GetNamedBoolean(L"fixed", false) &&
        block.GetNamedString(L"backgroundMode", L"") != L"original" &&
        hasText(block.GetNamedString(L"translation", L""));
}

JsonObject layoutStatus(hstring const& id, hstring const& error, bool backgroundReview = false)
{
    JsonObject status;
    putText(status, L"id", id);
    putText(status, L"error", error);
    putFlag(status, L"fits", error.empty());
    putFlag(status, L"backgroundReview", backgroundReview);
    return status;
}

OverlayText overlayText(JsonObject const& block, hstring const& text)
{
    OverlayText result;
    result.text = text;
    result.source = block.GetNamedString(L"source", L"");
    const auto region = block.GetNamedArray(L"region", nullptr);
    if (region && region.Size() == 4) {
        result.x = region.GetNumberAt(0);
        result.y = region.GetNumberAt(1);
        result.width = region.GetNumberAt(2);
        result.height = region.GetNumberAt(3);
    }
    result.fontSize = block.GetNamedNumber(L"size", 12);
    const auto color = block.GetNamedArray(L"color", nullptr);
    if (color && color.Size() == 3)
        for (uint32_t i = 0; i < color.Size(); ++i)
            result.color[i] = static_cast<uint8_t>(std::clamp(color.GetNumberAt(i), 0.0, 255.0));
    result.bold = block.GetNamedBoolean(L"bold", false);
    result.serif = block.GetNamedBoolean(L"serif", false);
    result.centered = block.GetNamedBoolean(L"centered", false);
    result.code = block.GetNamedBoolean(L"code", false);
    result.fixed = block.GetNamedBoolean(L"fixed", false);
    result.styled = !block.GetNamedString(L"styledSource", L"").empty();
    return result;
}

struct Placement {
    JsonObject block;
    hstring id;
    OverlayLayout layout;
    hstring error;
    bool backgroundReview{};
};

struct PageTranslation {
    std::vector<Placement> placements;
    bool applied{};

    JsonArray statuses() const
    {
        JsonArray output;
        for (const auto& item : placements)
            output.Append(layoutStatus(item.id, item.error, item.backgroundReview));
        return output;
    }
};

void fitBlock(Placement& placement, Page const& page, OverlayRenderer const& renderer,
              hstring const& text, bool scan, bool exporting)
{
    const auto& block = placement.block;
    if (block.GetNamedBoolean(L"stale", false)) {
        placement.error = L"原文已修改，请更新或确认本段译文。";
    } else if (block.GetNamedBoolean(L"protected", false)) {
        placement.error = block.GetNamedString(L"preserveReason", L"此区域暂不能独立替换，请保留原文或使用识图。");
    } else {
        placement.error = ::yiyi::pdfLayoutIssue(block, text);
    }
    if (!placement.error.empty()) return;
    placement.backgroundReview = scan && block.GetNamedString(L"backgroundMode", L"") == L"repair" &&
        !block.GetNamedBoolean(L"backgroundConfirmed", false);
    if (exporting && placement.backgroundReview) {
        placement.error = L"背景修复尚未确认，请检查预览并确认。";
        return;
    }
    placement.layout = renderer.fit(overlayText(block, text), page.width, page.height);
    if (!placement.layout.fits()) placement.error = hstring(placement.layout.error);
}

using Owners = std::unordered_map<int, std::vector<size_t>>;

Owners textOwners(std::vector<JsonObject> const& blocks)
{
    Owners owners;
    for (size_t i = 0; i < blocks.size(); ++i) {
        for (const auto& item : blocks[i].GetNamedArray(L"objects", JsonArray{}))
            owners[static_cast<int>(item.GetNumber())].push_back(i);
    }
    return owners;
}

template <typename Reject>
void enforceSharedObjects(Owners const& owners, std::set<size_t>& fitted, Reject const& reject)
{
    bool changed;
    do {
        changed = false;
        for (const auto& [index, group] : owners) {
            if (std::ranges::all_of(group, [&](size_t i) { return fitted.contains(i); })) continue;
            for (const auto i : group) {
                if (!fitted.erase(i)) continue;
                reject(i);
                changed = true;
            }
        }
    } while (changed);
}

std::vector<JsonObject> pageBlocks(JsonObject const& content)
{
    std::vector<JsonObject> blocks;
    for (const auto& value : content.GetNamedArray(L"blocks", JsonArray{})) blocks.push_back(value.GetObject());
    return blocks;
}

FPDF_PAGEOBJECT checkedTextObject(Page& page, std::vector<PageObject> const& objects, int index)
{
    if (index < 0 || index >= std::ssize(objects) || page.api.FPDFPageObj_GetType(objects[index].handle) != FPDF_PAGEOBJ_TEXT)
        fail(L"PDF 段落索引无效或原文字对象已改变，请重新导入。");
    return objects[index].handle;
}

void replaceTextObjects(Page& page, std::vector<PageObject> const& source, std::set<int> const& removed)
{
    auto& api = page.api;
    std::set<int> expanded;
    for (const auto index : removed) {
        checkedTextObject(page, source, index);
        for (int parent = source[index].parent; parent >= 0; parent = source[parent].parent) expanded.insert(parent);
    }

    if (!expanded.empty()) {
        std::vector<std::vector<int>> children(source.size());
        std::vector<int> top;
        for (int i = 0; i < std::ssize(source); ++i) {
            if (source[i].parent < 0) top.push_back(i);
            else children[source[i].parent].push_back(i);
        }
        const auto promote = [&](auto&& self, int index) -> void {
            const auto& item = source[index];
            if (expanded.contains(index)) {
                for (const auto child : children[index]) {
                    if (!api.FPDFFormObj_RemoveObject(item.handle, source[child].handle))
                        fail(L"无法展开嵌套页面对象。");
                    self(self, child);
                }
                api.FPDFPageObj_Destroy(item.handle);
                return;
            }
            if (item.parent >= 0) {
                const auto& m = item.inherited;
                api.FPDFPageObj_Transform(item.handle, m.a, m.b, m.c, m.d, m.e, m.f);
                api.FPDFPageObj_TransformClipPath(item.handle, m.a, m.b, m.c, m.d, m.e, m.f);
            }
            api.FPDFPage_InsertObject(page.handle, item.handle);
        };
        for (const auto index : top)
            if (!api.FPDFPage_RemoveObject(page.handle, source[index].handle)) fail(L"无法重排页面对象。");
        for (const auto index : top) promote(promote, index);
    }
    for (const auto index : removed) {
        const auto object = source[index].handle;
        if (!api.FPDFPage_RemoveObject(page.handle, object)) fail(L"无法替换原文对象。");
        api.FPDFPageObj_Destroy(object);
    }
    for (int i = 0, count = api.FPDFPage_CountObjects(page.handle); i < count; ++i) {
        const auto object = api.FPDFPage_GetObject(page.handle, i);
        const auto type = api.FPDFPageObj_GetType(object);
        if (type != FPDF_PAGEOBJ_PATH && type != FPDF_PAGEOBJ_TEXT) continue;
        unsigned r{}, g{}, b{}, alpha{};
        if (api.FPDFPageObj_GetFillColor(object, &r, &g, &b, &alpha)) api.FPDFPageObj_SetFillColor(object, r, g, b, alpha);
        if (api.FPDFPageObj_GetStrokeColor(object, &r, &g, &b, &alpha)) api.FPDFPageObj_SetStrokeColor(object, r, g, b, alpha);
    }
}

Transform canvasToPage(Page& page)
{
    const auto point = [&](int x, int y) {
        double pdfX{}, pdfY{};
        check_bool(page.api.FPDF_DeviceToPage(page.handle, 0, 0, 1024, 1024, 0, x, y, &pdfX, &pdfY));
        return std::pair{pdfX, pdfY};
    };
    const auto [x, y] = point(0, 1024);
    const auto [xx, xy] = point(1024, 1024);
    const auto [yx, yy] = point(0, 0);
    return {(xx - x) / page.width, (xy - y) / page.width,
            (yx - x) / page.height, (yy - y) / page.height, x, y};
}

void appendOverlay(Document& doc, Page& page, std::span<const uint8_t> bytes)
{
    auto& api = page.api;
    const auto source = api.FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
    if (!source) fail(L"无法读取译文文字层。");
    const auto xobject = api.FPDF_NewXObjectFromPage(doc.handle, source, 0);
    api.FPDF_CloseDocument(source);
    if (!xobject) fail(L"无法合并译文文字层。");
    const auto form = api.FPDF_NewFormObjectFromXObject(xobject);
    api.FPDF_CloseXObject(xobject);
    if (!form) fail(L"无法创建译文页面对象。");
    const auto m = canvasToPage(page);
    api.FPDFPageObj_Transform(form, m.a, m.b, m.c, m.d, m.e, m.f);
    api.FPDFPage_InsertObject(page.handle, form);
}

void appendBackground(Document& doc, Page& page, std::span<BYTE> pixels, int width, int height)
{
    auto& api = page.api;
    const auto bitmap = api.FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRx, pixels.data(), width * 4);
    if (!bitmap) fail(L"无法创建背景图像。");
    std::unique_ptr<std::remove_pointer_t<FPDF_BITMAP>, decltype(api.FPDFBitmap_Destroy)> scopedBitmap(bitmap, api.FPDFBitmap_Destroy);
    const auto image = api.FPDFPageObj_NewImageObj(doc.handle);
    if (!image) fail(L"无法创建背景对象。");
    std::unique_ptr<std::remove_pointer_t<FPDF_PAGEOBJECT>, decltype(api.FPDFPageObj_Destroy)> scopedImage(image, api.FPDFPageObj_Destroy);
    if (!api.FPDFImageObj_SetBitmap(nullptr, 0, image, bitmap)) fail(L"无法写入背景图像。");
    const auto m = canvasToPage(page);
    api.FPDFPageObj_Transform(image, m.a * page.width, m.b * page.width,
        m.c * page.height, m.d * page.height, m.e, m.f);
    api.FPDFPage_InsertObject(page.handle, scopedImage.release());
}

PageTranslation translatePage(Document& doc, Page& page, JsonObject const& data,
                              OverlayRenderer const& renderer, bool patterned, bool exporting)
{
    PageTranslation result;
    const auto scanData = data.GetNamedObject(L"scan", nullptr);
    const auto blocks = pageBlocks(pageContent(data));
    const bool raster = scanData || patterned || data.GetNamedBoolean(L"rasterBackground", false);
    if (scanData && blocks.empty() && hasText(scanData.GetNamedString(L"translation", L""))) {
        result.placements.push_back({scanData, {}, {}, L"旧版文字草稿需要重新识别文字区域。", false});
        return result;
    }
    std::vector<int> position(blocks.size(), -1);
    std::set<size_t> fitted;
    const auto add = [&](size_t i, hstring const& text) {
        const auto& block = blocks[i];
        Placement placement{block, block.GetNamedString(L"id", L""), {}, {}, false};
        if (exporting && scanData && scanData.GetNamedBoolean(L"stale", false))
            placement.error = L"原文已修改，旧译文需要更新。";
        else
            fitBlock(placement, page, renderer, text, static_cast<bool>(scanData), exporting);
        if (placement.layout.fits()) fitted.insert(i);
        position[i] = static_cast<int>(result.placements.size());
        result.placements.push_back(std::move(placement));
    };
    for (size_t i = 0; i < blocks.size(); ++i)
        if (wantsTranslation(blocks[i])) add(i, blocks[i].GetNamedString(L"translation"));
    if (fitted.empty()) return result;

    const auto owners = textOwners(blocks);
    if (!raster) {
        for (const auto& [object, group] : owners) {
            if (group.size() < 2 || std::ranges::none_of(group, [&](size_t i) { return fitted.contains(i); })) continue;
            for (const auto i : group) {
                const auto& block = blocks[i];
                if (position[i] < 0 && block.GetNamedBoolean(L"fixed", false) && !block.GetNamedBoolean(L"keep", false))
                    add(i, block.GetNamedString(L"source", L""));
            }
        }
        enforceSharedObjects(owners, fitted, [&](size_t i) {
            result.placements[position[i]].error = L"拆分区域共享原文字对象，请完成关联区域的译文后一起替换。";
        });
    }

    if (fitted.empty()) return result;
    const auto source = pageObjects(page);
    std::set<int> removed;
    if (!raster) {
        for (const auto i : fitted)
            for (const auto& value : blocks[i].GetNamedArray(L"objects", JsonArray{}))
                removed.insert(static_cast<int>(value.GetNumber()));
        replaceTextObjects(page, source, removed);
    } else {
        const auto native = scanData ? pageBlocks(data) : blocks;
        const auto nativeOwners = scanData ? textOwners(native) : owners;
        std::unordered_map<hstring, size_t> byId;
        for (size_t i = 0; i < native.size(); ++i) byId.emplace(native[i].GetNamedString(L"id", L""), i);
        std::set<size_t> exact;
        for (const auto i : fitted) {
            const auto& block = blocks[i];
            const auto it = byId.find(block.GetNamedString(L"id", L""));
            if (it != byId.end() && native[it->second].GetNamedString(L"source", L"") == block.GetNamedString(L"source", L"") &&
                native[it->second].GetNamedArray(L"objects", JsonArray{}).Size() &&
                block.GetNamedString(L"backgroundMode", L"auto") == L"auto")
                exact.insert(it->second);
        }
        enforceSharedObjects(nativeOwners, exact, [](size_t) {});
        for (const auto i : exact)
            for (const auto& value : native[i].GetNamedArray(L"objects", JsonArray{}))
                removed.insert(static_cast<int>(value.GetNumber()));

        const auto [width, height] = rasterSize(page, 2200);
        std::vector<BYTE> background;
        {
            struct HiddenText {
                Pdfium& api;
                std::vector<std::pair<FPDF_PAGEOBJECT, FPDF_BOOL>> values;
                ~HiddenText() {
                    for (const auto& [object, active] : values) api.FPDFPageObj_SetIsActive(object, active);
                }
            } hidden{page.api, {}};
            for (const auto index : removed) {
                const auto object = checkedTextObject(page, source, index);
                FPDF_BOOL active{};
                if (!page.api.FPDFPageObj_GetIsActive(object, &active) || !page.api.FPDFPageObj_SetIsActive(object, false))
                    fail(L"无法准备混合页面的文字背景。");
                hidden.values.emplace_back(object, active);
            }
            background = renderPixels(page, width, height, 0, 0, width, height, 0);
        }

        JsonArray bitmapErase;
        for (const auto i : fitted) {
            const auto it = byId.find(blocks[i].GetNamedString(L"id", L""));
            if (it == byId.end() || !exact.contains(it->second)) bitmapErase.Append(blocks[i]);
        }
        const auto failed = eraseScanText(background, width, height, width * 4, page.width, page.height, bitmapErase);
        std::unordered_map<hstring, hstring> rejected;
        for (const auto& item : failed) {
            const auto failure = item.GetObject();
            rejected.emplace(failure.GetNamedString(L"id"), failure.GetNamedString(L"error"));
        }
        std::erase_if(fitted, [&](size_t i) {
            auto& placement = result.placements[position[i]];
            const auto error = rejected.find(placement.id);
            if (error == rejected.end()) return false;
            placement.error = error->second;
            return true;
        });
        if (fitted.empty()) return result;
        for (const auto& item : source) {
            if (item.parent >= 0) continue;
            if (!page.api.FPDFPage_RemoveObject(page.handle, item.handle)) fail(L"无法重建扫描页。");
            page.api.FPDFPageObj_Destroy(item.handle);
        }
        appendBackground(doc, page, background, width, height);
    }

    std::vector<OverlayLayout> layouts;
    for (const auto i : fitted) layouts.push_back(result.placements[position[i]].layout);
    auto overlay = renderer.render(page.width, page.height, layouts);
    if (overlay.empty()) fail(L"未生成可用的 PDF 译文文字层。");
    appendOverlay(doc, page, overlay);
    if (!page.api.FPDFPage_GenerateContent(page.handle)) fail(L"无法生成译文页面。");
    result.applied = true;
    return result;
}

class ExportDirectory {
public:
    ExportDirectory(std::filesystem::path const& destination, hstring const& supplied)
        : m_parent(std::filesystem::canonical(destination.parent_path()))
    {
        if (!supplied.empty()) {
            m_path = std::filesystem::canonical(std::filesystem::path(supplied.c_str()));
            if (m_path.parent_path() != m_parent || !m_path.filename().native().starts_with(L"Yiyi-pdf-") ||
                !std::filesystem::is_empty(m_path))
                fail(L"PDF 导出暂存目录无效。");
            return;
        }
        GUID id{};
        check_hresult(CoCreateGuid(&id));
        m_path = m_parent / (L"Yiyi-pdf-" + std::wstring(to_hstring(guid(id))));
        if (!std::filesystem::create_directory(m_path)) fail(L"无法在导出目录创建暂存文件。");
        m_owned = true;
    }

    ~ExportDirectory()
    {
        if (!m_owned) return;
        std::error_code error;
        const auto resolved = std::filesystem::canonical(m_path, error);
        if (!error && resolved == m_path && resolved.parent_path() == m_parent) std::filesystem::remove_all(resolved, error);
    }

    std::filesystem::path file(std::wstring_view name) const { return m_path / name; }

private:
    std::filesystem::path m_parent;
    std::filesystem::path m_path;
    bool m_owned{};
};

void checkpoint(Document& doc, std::filesystem::path const& destination, PdfProgress const& progress)
{
    struct Writer : FPDF_FILEWRITE {
        file_handle file;
        PdfProgress const& progress;
        std::uint64_t completed{};
        std::exception_ptr error;

        Writer(std::filesystem::path const& path, PdfProgress const& callback)
            : FPDF_FILEWRITE{}, file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)), progress(callback)
        {
            if (!file) throw_last_error();
            version = 1;
            WriteBlock = [](FPDF_FILEWRITE* base, const void* data, unsigned long bytes) -> int {
                auto& self = *static_cast<Writer*>(base);
                try {
                    DWORD written{};
                    if (!WriteFile(self.file.get(), data, bytes, &written, nullptr) || written != bytes) return 0;
                    self.completed += written;
                    reportProgress(self.progress, L"checkpoint", static_cast<double>(self.completed), 0);
                    return 1;
                } catch (...) {
                    self.error = std::current_exception();
                    return 0;
                }
            };
        }
    } writer(destination, progress);
    const bool saved = doc.api.FPDF_SaveAsCopy(doc.handle, &writer, FPDF_NO_INCREMENTAL);
    if (writer.error) std::rethrow_exception(writer.error);
    if (!saved || !FlushFileBuffers(writer.file.get())) fail(L"PDF 导出暂存失败，请检查磁盘空间。");
}

JsonObject exportDocument(Document& doc, std::filesystem::path const& source,
                          std::filesystem::path const& runtime, PdfTaskStore const& task,
                          JsonObject const& request, PdfProgress const& progress)
{
    const auto destination = std::filesystem::absolute(pathArgument(request, L"destination")).lexically_normal();
    doc.checkOutputPath(destination);
    auto reportPath = destination;
    reportPath += L".issues.txt";
    doc.checkOutputPath(reportPath);
    const auto database = pathArgument(request, L"taskDatabase");
    if ((std::filesystem::exists(destination) && std::filesystem::equivalent(destination, database)) ||
        (std::filesystem::exists(reportPath) && std::filesystem::equivalent(reportPath, database)))
        fail(L"导出路径不能覆盖 PDF 任务数据库。");
    const ExportDirectory staging(destination, request.GetNamedString(L"workDirectory", L""));
    const auto patterned = merge::patternPages(runtime, source, 0, doc.pageCount - 1);
    std::vector<std::filesystem::path> parts;
    std::vector<uint8_t> modified(doc.pageCount);
    std::unique_ptr<OverlayRenderer> renderer;
    JsonArray pageStatuses, preserved;
    std::set<int> failedPages;
    std::wstring report = L"PDF 导出检查清单\r\n文件：" + destination.filename().native() +
        L"\r\n未通过检查的区域在导出文件中保留原文，任务中的译文未删除。\r\n\r\n";
    const bool preserveFailed = request.GetNamedBoolean(L"preserveFailed", false);
    constexpr int batch = 16;
    for (int first = 0; first < doc.pageCount; first += batch) {
        const int count = std::min(batch, doc.pageCount - first);
        const auto part = staging.file(L"source-" + std::to_wstring(first) + L".pdf");
        merge::copyPages(runtime, source, part, first, first + count - 1);
        bool changed = false;
        const auto edited = staging.file(L"translated-" + std::to_wstring(first) + L".pdf");
        {
            Document copy(doc.api, part, nullptr, {});
            for (int offset = 0; offset < count; ++offset) {
                const int index = first + offset;
                const auto data = task.page(index);
                if (data && integer(data, L"number", index) != index) fail(L"PDF 任务页码与原文件不一致。");
                if (data && (std::ranges::any_of(pageBlocks(pageContent(data)), wantsTranslation) ||
                     hasText(pageContent(data).GetNamedString(L"translation", L"")))) {
                    if (!renderer) renderer = std::make_unique<OverlayRenderer>(runtime);
                    Page page(copy, offset);
                    auto result = translatePage(copy, page, data, *renderer, patterned.contains(index), true);
                    modified[index] = result.applied ? 1 : 0;
                    changed |= result.applied;
                    JsonObject row;
                    putNumber(row, L"page", index);
                    row.Insert(L"statuses", result.statuses());
                    pageStatuses.Append(row);
                    for (const auto& item : result.placements) {
                        if (item.error.empty()) continue;
                        JsonObject issue;
                        putNumber(issue, L"page", index + 1);
                        putText(issue, L"id", item.id);
                        putText(issue, L"reason", item.error);
                        preserved.Append(issue);
                        failedPages.insert(index);
                        report += L"第 " + std::to_wstring(index + 1) + L" 页 · " + std::wstring(item.id) +
                            L"\r\n原因：" + std::wstring(item.error) + L"\r\n\r\n";
                    }
                }
                reportProgress(progress, L"export", index + 1, doc.pageCount);
            }
            if (changed) checkpoint(copy, edited, progress);
        }
        parts.push_back(changed ? edited : part);
    }

    JsonObject result;
    result.Insert(L"pageStatuses", pageStatuses);
    result.Insert(L"preserved", preserved);
    putNumber(result, L"preservedPages", static_cast<double>(failedPages.size()));
    if (!failedPages.empty() && !preserveFailed) {
        putText(result, L"error", L"导出检查发现需要校对的区域，请查看待校对或选择保留问题原文导出。");
        putNumber(result, L"page", *failedPages.begin());
        return result;
    }
    const auto output = staging.file(L"complete.pdf");
    merge::write(runtime, source, parts, output, doc.pageCount,
        [&](std::string_view stage, int completed, int total) { reportProgress(progress, to_hstring(stage), completed, total); },
        modified);
    doc.checkOutputPath(destination);
    if (!MoveFileExW(output.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fail(L"无法替换导出目标，请关闭占用该 PDF 的程序并检查写入权限。");
    putFlag(result, L"saved", true);
    if (!failedPages.empty()) {
        report += L"合计：" + std::to_wstring(failedPages.size()) + L" 页、" + std::to_wstring(preserved.Size()) +
            L" 个区域保留原文。\r\n";
        try {
            doc.checkOutputPath(reportPath);
            writeWorkerOutput(reportPath, "\xef\xbb\xbf" + to_string(report));
            putText(result, L"report", hstring(reportPath.native()));
        } catch (...) {
            putText(result, L"reportError", L"PDF 已导出，但无法写入检查清单；请在待校对中查看。");
        }
    }
    return result;
}

}

JsonObject executeDocumentRequest(JsonObject const& request, std::filesystem::path const& runtimeDirectory,
                                  PdfProgress const& progress)
{
    const auto action = request.GetNamedString(L"action", L"");
    if (action != L"info" && action != L"renderOriginal" && action != L"recognitionImage" &&
        action != L"extractPageInput" && action != L"inspectPages" && action != L"renderTranslated" &&
        action != L"inspectScan" && action != L"export")
        fail(L"PDF 处理动作无效。");
    auto manifest = request.GetNamedObject(L"document", nullptr);
    std::unique_ptr<PdfTaskStore> store;
    if (!request.GetNamedString(L"taskDatabase", L"").empty()) {
        store = std::make_unique<PdfTaskStore>(pathArgument(request, L"taskDatabase"), true);
        manifest = store->metadata();
        if (!manifest || manifest.Size() == 0) fail(L"PDF 任务信息不存在或损坏。");
    }
    const bool quick = action != L"info" && action != L"export" && manifest && request.GetNamedBoolean(L"quickSourceCheck", false);
    const auto source = pathArgument(request, L"source");
    Pdfium api(runtimeDirectory);
    Document doc(api, source, quick ? manifest : nullptr, progress);
    JsonObject result;
    if (action == L"info") {
        putNumber(result, L"version", manifestVersion);
        putNumber(result, L"pageCount", doc.pageCount);
        putText(result, L"hash", doc.hash);
        result.Insert(L"stamp", doc.stamp);
        return result;
    }

    if (!manifest || integer(manifest, L"version", -1) != manifestVersion ||
        integer(manifest, L"pageCount", -1) != doc.pageCount || manifest.GetNamedString(L"hash", L"") != doc.hash)
        fail(L"原 PDF 已变化，请重新导入。");
    if (action == L"export") {
        if (!store) fail(L"缺少 PDF 导出任务数据。");
        return exportDocument(doc, source, runtimeDirectory, *store, request, progress);
    }
    if (action == L"inspectPages") {
        const int first = integer(request, L"first", -1);
        if (first < 0 || first >= doc.pageCount) fail(L"页码无效。");
        const int count = std::min(std::clamp(integer(request, L"count", 8), 1, 8), doc.pageCount - first);
        JsonArray pages;
        auto previous = request.GetNamedObject(L"preceding", nullptr);
        for (int i = first; i < first + count; ++i) {
            JsonObject data;
            try {
                Page page(doc, i);
                data = analyzePageInput(extractPageInput(page), i);
                if (previous) linkPageTableHeaders(previous, data);
            } catch (...) {
                const auto message = to_message();
                putNumber(data, L"number", i);
                putNumber(data, L"extractionVersion", nativePdfExtractionVersion);
                putFlag(data, L"analyzed", true);
                putText(data, L"analysisError", message);
                putText(data, L"warning", message);
                putText(data, L"kind", L"text");
                data.Insert(L"blocks", JsonArray{});
            }
            pages.Append(data);
            previous = data;
            reportProgress(progress, L"inspect", i + 1, doc.pageCount);
        }
        result.Insert(L"pages", pages);
        putText(result, L"hash", doc.hash);
        putNumber(result, L"nextPage", first + count);
        return result;
    }
    const int index = integer(request, L"page", -1);
    if (index < 0 || index >= doc.pageCount) fail(L"页码无效。");
    if (action == L"renderTranslated") {
        const auto data = store ? store->page(index) : request.GetNamedObject(L"pageData", nullptr);
        const auto destination = pathArgument(request, L"translated");
        doc.checkOutputPath(destination);
        if (data && integer(data, L"number", index) != index) fail(L"PDF 任务页码与原文件不一致。");
        if (data && std::ranges::any_of(pageBlocks(pageContent(data)), wantsTranslation)) {
            GUID id{};
            check_hresult(CoCreateGuid(&id));
            const auto part = std::filesystem::absolute(destination).parent_path() /
                (L"Yiyi-pdf-" + std::wstring(to_hstring(guid(id))) + L".pdf");
            struct RemoveCopy {
                std::filesystem::path path;
                ~RemoveCopy() { std::error_code error; std::filesystem::remove(path, error); }
            } cleanup{part};
            merge::copyPages(runtimeDirectory, source, part, index, index);
            Document copy(api, part, nullptr, {});
            Page page(copy, 0);
            const OverlayRenderer renderer(runtimeDirectory);
            const auto patterned = merge::patternPages(runtimeDirectory, source, index, index);
            const auto translation = translatePage(copy, page, data, renderer, patterned.contains(index), false);
            result.Insert(L"statuses", translation.statuses());
            renderOriginal(page, request, destination);
        } else {
            result.Insert(L"statuses", JsonArray{});
            Page page(doc, index);
            renderOriginal(page, request, destination);
        }
        putFlag(result, L"saved", true);
        return result;
    }
    Page page(doc, index);
    if (action == L"extractPageInput") return extractPageInput(page);
    if (action == L"inspectScan") {
        const auto [width, height] = rasterSize(page, std::clamp(integer(request, L"pixels", 1800), 1000, 2400));
        const auto pixels = renderPixels(page, width, height, 0, 0, width, height, 0);
        return analyzeScanPage(pixels, width, height, width * 4, page.width, page.height, index,
                               runtimeDirectory.parent_path() / L"ocr");
    }
    const auto destination = pathArgument(request, action == L"renderOriginal" ? L"original" : L"image");
    doc.checkOutputPath(destination);
    if (action == L"renderOriginal") renderOriginal(page, request, destination);
    else renderRecognitionRegion(page, request, destination);
    putNumber(result, L"width", page.width);
    putNumber(result, L"height", page.height);
    result.Insert(L"saved", JsonValue::CreateBooleanValue(true));
    return result;
}

}
