#include "pdfoverlay.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#pragma comment(lib, "dwrite.lib")

namespace yiyi::pdf {
namespace {

using Microsoft::WRL::ComPtr;
constexpr float cffEm = 1000;
constexpr double italicShear = .20;
constexpr double fitTolerance = .025;
constexpr std::size_t maxTextUnits = 16000;

struct Style {
    bool bold = false;
    bool italic = false;
    int script = 0;
};

struct Glyph {
    std::uint32_t code = 0;
    std::uint16_t index = 0;
    std::uint8_t face = 0;
    double advance = 0;
    double inkLeft = 0, inkTop = 0, inkRight = 0, inkBottom = 0;
    bool hasInk = false;
};

struct Character {
    std::uint32_t code;
    Style style;
    Glyph glyph{};
};

struct PlacedGlyph {
    Glyph glyph;
    Style style;
    double size, x, baseline, scaleX;
};

bool space(std::uint32_t code)
{
    return code == 0x00a0 || code == 0x3000 || (code <= 0xffff && std::iswspace(static_cast<wchar_t>(code)));
}

bool lineBreak(std::uint32_t code) { return code == '\n' || code == 0x2028 || code == 0x2029; }

std::wstring normalizeLines(std::wstring_view text)
{
    std::wstring result;
    result.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            if (index + 1 < text.size() && text[index + 1] == '\n') ++index;
            result += '\n';
        } else result += text[index];
    }
    return result;
}

std::vector<std::wstring_view> splitLines(std::wstring_view text)
{
    std::vector<std::wstring_view> lines;
    for (;;) {
        const auto separator = text.find(L'\n');
        lines.push_back(text.substr(0, separator));
        if (separator == std::wstring_view::npos) return lines;
        text.remove_prefix(separator + 1);
    }
}

std::wstring codeIndentation(std::wstring_view text, std::wstring_view source)
{
    const auto original = normalizeLines(source);
    const auto inputLines = splitLines(text), sourceLines = splitLines(original);
    if (inputLines.size() != sourceLines.size()) return std::wstring(text);
    std::wstring result;
    for (size_t index = 0; index < inputLines.size(); ++index) {
        if (index) result += L'\n';
        const auto indent = sourceLines[index].find_first_not_of(L" \t");
        result += sourceLines[index].substr(0, indent);
        auto content = inputLines[index];
        while (!content.empty() && space(content.front())) content.remove_prefix(1);
        while (!content.empty() && space(content.back())) content.remove_suffix(1);
        result += content;
    }
    return result;
}

bool simpleCharacter(std::wstring_view value)
{
    WORD types[2]{};
    if (!GetStringTypeW(CT_CTYPE2, value.data(), static_cast<int>(value.size()), types)) return false;
    for (size_t index = 0; index < value.size(); ++index) if (types[index] == C2_RIGHTTOLEFT) return false;
    if (!GetStringTypeW(CT_CTYPE3, value.data(), static_cast<int>(value.size()), types)) return false;
    for (size_t index = 0; index < value.size(); ++index) if (types[index] & (C3_NONSPACING | C3_VOWELMARK)) return false;
    return true;
}

std::wstring unsupported(std::uint32_t code)
{
    wchar_t hex[16]{};
    swprintf_s(hex, L"U+%04X", static_cast<unsigned>(code));
    return std::wstring(L"译文含当前排版不支持的字符（") + hex + L"）";
}

bool charactersFor(const OverlayText& block, std::vector<Character>& characters, std::wstring& error)
{
    auto text = normalizeLines(block.text);
    if (block.code) text = codeIndentation(text, block.source);
    if (text.size() > maxTextUnits) return false;
    int bold = 0, italic = 0, superscript = 0, subscript = 0;
    for (size_t index = 0; index < text.size();) {
        if (block.styled && !block.code && text[index] == '<') {
            const auto end = text.find(L'>', index + 1);
            if (end != std::wstring::npos && end - index <= 5) {
                auto tag = std::wstring_view(text).substr(index + 1, end - index - 1);
                const bool closing = tag.starts_with(L"/");
                if (closing) tag.remove_prefix(1);
                int* depth = tag == L"b" ? &bold : tag == L"i" ? &italic :
                    tag == L"sup" ? &superscript : tag == L"sub" ? &subscript : nullptr;
                if (depth) {
                    *depth = closing ? std::max(0, *depth - 1) : *depth + 1;
                    index = end + 1;
                    continue;
                }
            }
        }
        const size_t start = index;
        std::uint32_t code = text[index++];
        if (code >= 0xd800 && code <= 0xdbff && index < text.size() && text[index] >= 0xdc00 && text[index] <= 0xdfff) {
            code = 0x10000 + ((code - 0xd800) << 10) + (text[index++] - 0xdc00);
        }
        if ((code >= 0xd800 && code <= 0xdfff) ||
            (!space(code) && !lineBreak(code) && (code < 0x20 || !simpleCharacter(std::wstring_view(text).substr(start, index - start))))) {
            error = unsupported(code);
            return false;
        }
        const Style style{block.bold || bold > 0, italic > 0, superscript ? 1 : subscript ? -1 : 0};
        if (code == '\t') {
            for (int cell = 0; cell < 4; ++cell) characters.push_back({L' ', style});
        } else characters.push_back({code, style});
    }
    return true;
}

struct GlyphPlacement {
    double size, rise, width, scaleX;
};

GlyphPlacement measure(const Character& character, double size, bool code)
{
    const double scriptSize = character.style.script ? size * .66 : size;
    const double rise = character.style.script > 0 ? -size * .36 : character.style.script < 0 ? size * .20 : 0;
    const double advance = character.glyph.advance * scriptSize / cffEm;
    const double width = code && advance > 0 ? (character.code < 0x2e80 ? .6 : 1.0) * scriptSize : advance;
    return {scriptSize, rise, width, advance > 0 ? width / advance : 1.0};
}

struct Line {
    size_t first, last;
    double advance = 0;
    double left = 0, top = 0, right = 0, bottom = 0;
    bool hasInk = false;
};

Line inkBounds(const std::vector<Character>& characters, size_t first, size_t last, double size, bool code)
{
    Line result{first, last};
    for (size_t index = first; index < last; ++index) {
        const auto& ch = characters[index];
        const auto placement = measure(ch, size, code);
        if (ch.glyph.hasInk) {
            const auto scale = placement.size / cffEm;
            const auto stroke = ch.style.bold ? placement.size * .016 : 0;
            const auto shear = ch.style.italic ? italicShear : 0;
            const double top = placement.rise + ch.glyph.inkTop * scale - stroke;
            const double bottom = placement.rise + ch.glyph.inkBottom * scale + stroke;
            const double left = result.advance + (ch.glyph.inkLeft * placement.scaleX - ch.glyph.inkBottom * shear) * scale - stroke;
            const double right = result.advance + (ch.glyph.inkRight * placement.scaleX - ch.glyph.inkTop * shear) * scale + stroke;
            if (result.hasInk) {
                result.left = std::min(result.left, left);
                result.right = std::max(result.right, right);
                result.top = std::min(result.top, top);
                result.bottom = std::max(result.bottom, bottom);
            } else {
                result.left = left;
                result.right = right;
                result.top = top;
                result.bottom = bottom;
                result.hasInk = true;
            }
        }
        result.advance += placement.width;
    }
    return result;
}

bool breakBetween(std::uint32_t before, std::uint32_t after)
{
    if (space(before) || before == '-' || before == 0x2010) return true;
    if (before < 0x2e80 && after < 0x2e80) return false;
    const std::wstring_view opening = L"([{（【《「『“‘";
    const std::wstring_view closing = L",.!?;:)]}、。，？！：；）】》」』”’";
    return (before > 0xffff || opening.find(static_cast<wchar_t>(before)) == std::wstring_view::npos)
        && (after > 0xffff || closing.find(static_cast<wchar_t>(after)) == std::wstring_view::npos);
}

std::vector<Line> wrap(const std::vector<Character>& text, double size, double width, bool code, bool fixed)
{
    std::vector<Line> lines;
    for (size_t paragraph = 0; paragraph < text.size();) {
        auto end = paragraph;
        while (end < text.size() && !lineBreak(text[end].code)) ++end;
        if (fixed || paragraph == end) lines.push_back(inkBounds(text, paragraph, end, size, code));
        else for (auto start = paragraph; start < end;) {
            size_t stop = start, opportunity = start;
            double advance = 0;
            while (stop < end) {
                const auto next = measure(text[stop], size, code).width;
                if (stop != start && advance + next > width) break;
                advance += next;
                ++stop;
                if (stop < end && breakBetween(text[stop - 1].code, text[stop].code)) opportunity = stop;
            }
            if (stop < end && opportunity > start) stop = opportunity;
            auto included = stop;
            if (!code) while (included > start && space(text[included - 1].code)) --included;
            lines.push_back(inkBounds(text, start, included, size, code));
            start = stop;
            if (!code) while (start < end && space(text[start].code)) ++start;
        }
        paragraph = end + 1;
    }
    return lines;
}

std::vector<PlacedGlyph> place(const std::vector<Character>& text, const std::vector<Line>& lines,
                               double x, double y, double width, double height, double size,
                               double step, bool centered, bool code)
{
    double baseline = 0;
    double inkBottom = 0;
    size_t blank = 0;
    std::vector<PlacedGlyph> glyphs;
    for (const auto& line : lines) {
        if (!line.hasInk) {
            ++blank;
            continue;
        }
        const auto left = std::min(0.0, line.left);
        const auto right = std::max(line.advance, line.right);
        if (right - left > width + fitTolerance) return {};
        baseline = glyphs.empty() ? -line.top + blank * step
            : std::max(baseline + (blank + 1) * step, inkBottom + size * .08 - line.top);
        blank = 0;
        inkBottom = baseline + line.bottom;
        if (inkBottom > height + fitTolerance) return {};
        double position = x - left + (centered ? (width - (right - left)) / 2 : 0);
        for (size_t index = line.first; index < line.last; ++index) {
            const auto& ch = text[index];
            const auto placement = measure(ch, size, code);
            glyphs.push_back({ch.glyph, ch.style, placement.size, position, y + baseline + placement.rise, placement.scaleX});
            position += placement.width;
        }
    }
    return glyphs;
}

class Face {
public:
    void load(IDWriteFactory& factory, const std::filesystem::path& path)
    {
        ComPtr<IDWriteFontFile> file;
        if (FAILED(factory.CreateFontFileReference(path.c_str(), nullptr, file.GetAddressOf()))) return;
        BOOL supported = FALSE;
        DWRITE_FONT_FILE_TYPE fileType{};
        DWRITE_FONT_FACE_TYPE faceType{};
        UINT32 count = 0;
        if (FAILED(file->Analyze(&supported, &fileType, &faceType, &count)) || !supported || !count) return;
        IDWriteFontFile* files[] = {file.Get()};
        if (SUCCEEDED(factory.CreateFontFace(faceType, 1, files, 0, DWRITE_FONT_SIMULATIONS_NONE, m_face.GetAddressOf()))) {
            m_face->GetMetrics(&m_metrics);
            if (!m_metrics.designUnitsPerEm) m_face.Reset();
        }
    }

    explicit operator bool() const { return static_cast<bool>(m_face); }
    IDWriteFontFace& native() const { return *m_face.Get(); }
    double lineHeight() const { return (m_metrics.ascent + m_metrics.descent + m_metrics.lineGap) / double(m_metrics.designUnitsPerEm); }

    std::optional<Glyph> glyph(std::uint32_t code, std::uint8_t face) const
    {
        if (!m_face) return std::nullopt;
        if (const auto cached = m_cache.find(code); cached != m_cache.end()) return cached->second;
        UINT16 index = 0;
        DWRITE_GLYPH_METRICS metrics{};
        if (FAILED(m_face->GetGlyphIndices(&code, 1, &index)) || !index ||
            FAILED(m_face->GetDesignGlyphMetrics(&index, 1, &metrics, FALSE))) return std::nullopt;
        const double scale = cffEm / m_metrics.designUnitsPerEm;
        Glyph value{code, index, face, metrics.advanceWidth * scale,
            metrics.leftSideBearing * scale, (metrics.topSideBearing - metrics.verticalOriginY) * scale,
            (double(metrics.advanceWidth) - metrics.rightSideBearing) * scale,
            (double(metrics.advanceHeight) - metrics.bottomSideBearing - metrics.verticalOriginY) * scale, false};
        value.hasInk = value.inkRight > value.inkLeft && value.inkBottom > value.inkTop;
        m_cache.emplace(code, value);
        return value;
    }

private:
    ComPtr<IDWriteFontFace> m_face;
    DWRITE_FONT_METRICS m_metrics{};
    mutable std::map<std::uint32_t, Glyph> m_cache;
};

void big16(std::string& bytes, std::uint16_t value)
{
    bytes += static_cast<char>(value >> 8);
    bytes += static_cast<char>(value);
}

void big32(std::string& bytes, std::uint32_t value)
{
    big16(bytes, static_cast<std::uint16_t>(value >> 16));
    big16(bytes, static_cast<std::uint16_t>(value));
}

void type2Number(std::string& bytes, double value)
{
    if (!std::isfinite(value) || value < -32768.0 || value > 32767.0) throw std::runtime_error("字体轮廓超出 PDF 字形范围。");
    const auto fixed = static_cast<std::int32_t>(std::llround(value * 65536));
    if (fixed % 65536) {
        bytes += static_cast<char>(255);
        big32(bytes, static_cast<std::uint32_t>(fixed));
        return;
    }
    const auto number = fixed / 65536;
    if (number >= -107 && number <= 107) bytes += static_cast<char>(number + 139);
    else if (number >= 108 && number <= 1131) {
        bytes += static_cast<char>((number - 108) / 256 + 247);
        bytes += static_cast<char>((number - 108) % 256);
    } else if (number <= -108 && number >= -1131) {
        bytes += static_cast<char>((-number - 108) / 256 + 251);
        bytes += static_cast<char>((-number - 108) % 256);
    } else {
        bytes += static_cast<char>(28);
        big16(bytes, static_cast<std::uint16_t>(number));
    }
}

// DWrite supplies the actual outlines of the placed glyphs. Relative Type 2
// paths let us embed just those glyphs without carrying the 65,535-glyph OTF.
class Outline : public IDWriteGeometrySink {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) noexcept override
    {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IDWriteGeometrySink)) return E_NOINTERFACE;
        *value = static_cast<IDWriteGeometrySink*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }
    void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) noexcept override {}
    void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) noexcept override {}
    void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F start, D2D1_FIGURE_BEGIN) noexcept override
    {
        attempt([&] { delta(start); m_bytes += static_cast<char>(21); });
    }
    void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* points, UINT32 count) noexcept override
    {
        attempt([&] {
            for (UINT32 index = 0; index < count; ++index) {
                delta(points[index]);
                m_bytes += static_cast<char>(5);
            }
        });
    }
    void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* curves, UINT32 count) noexcept override
    {
        attempt([&] {
            for (UINT32 index = 0; index < count; ++index) {
                delta(curves[index].point1);
                delta(curves[index].point2);
                delta(curves[index].point3);
                m_bytes += static_cast<char>(8);
            }
        });
    }
    void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END) noexcept override {}
    HRESULT STDMETHODCALLTYPE Close() noexcept override { return m_error ? E_FAIL : S_OK; }

    std::string charstring(const Face& face, const Glyph& glyph)
    {
        type2Number(m_bytes, glyph.advance);
        const auto status = face.native().GetGlyphRunOutline(cffEm, &glyph.index, nullptr, nullptr, 1, FALSE, FALSE, this);
        if (m_error) std::rethrow_exception(m_error);
        if (FAILED(status)) throw std::runtime_error("无法读取译文字体的字形轮廓。");
        m_bytes += static_cast<char>(14);
        return std::move(m_bytes);
    }

private:
    void attempt(auto operation) noexcept
    {
        if (m_error) return;
        try { operation(); } catch (...) { m_error = std::current_exception(); }
    }

    void delta(D2D1_POINT_2F next)
    {
        // Use the same fixed-point values for each endpoint and the next delta.
        const double x = std::round(double(next.x) * 65536) / 65536;
        const double y = std::round(-double(next.y) * 65536) / 65536;
        type2Number(m_bytes, x - m_x);
        type2Number(m_bytes, y - m_y);
        m_x = x;
        m_y = y;
    }

    double m_x = 0, m_y = 0;
    std::string m_bytes;
    std::exception_ptr m_error;
};

std::string cffIndex(const std::vector<std::string>& values)
{
    if (values.size() > 65535) throw std::runtime_error("译文字形数量超出 PDF 字体限制。");
    std::string bytes;
    big16(bytes, static_cast<std::uint16_t>(values.size()));
    if (values.empty()) return bytes;
    bytes += '\4';
    std::uint32_t offset = 1;
    big32(bytes, offset);
    for (const auto& value : values) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max() - offset) throw std::runtime_error("译文字体数据过大。");
        offset += static_cast<std::uint32_t>(value.size());
        big32(bytes, offset);
    }
    for (const auto& value : values) bytes += value;
    return bytes;
}

void dictNumber(std::string& bytes, std::int32_t number)
{
    bytes += static_cast<char>(29);
    big32(bytes, static_cast<std::uint32_t>(number));
}

std::string topDictionary(int charset, int fdSelect, int charStrings, int fdArray, int count,
                          const std::array<int, 4>& bounds)
{
    std::string dict;
    dictNumber(dict, 391);
    dictNumber(dict, 392);
    dictNumber(dict, 0);
    dict.append("\x0c\x1e", 2); // ROS (Adobe, Identity, 0)
    for (int coordinate : bounds) dictNumber(dict, coordinate);
    dict += '\5'; // FontBBox
    dictNumber(dict, charset);
    dict += static_cast<char>(15);
    dictNumber(dict, charStrings);
    dict += '\21';
    dictNumber(dict, count);
    dict.append("\x0c\x22", 2); // CIDCount
    dictNumber(dict, fdArray);
    dict.append("\x0c\x24", 2);
    dictNumber(dict, fdSelect);
    dict.append("\x0c\x25", 2);
    return cffIndex({std::move(dict)});
}

std::string fontDictionary(int privateOffset)
{
    std::string dict;
    dictNumber(dict, 0);
    dictNumber(dict, privateOffset);
    dict += '\22'; // Empty Private DICT uses default width values of zero.
    return cffIndex({std::move(dict)});
}

std::string embedCff(const std::string& name, const std::vector<Glyph>& used, const Face& face,
                     const std::array<int, 4>& bounds)
{
    const auto names = cffIndex({name});
    const auto strings = cffIndex({"Adobe", "Identity"});
    const auto globalSubrs = cffIndex({});
    const auto count = static_cast<int>(used.size() + 1);
    std::string charset(1, '\0'); // Format 0, identity CIDs for the subset.
    for (int cid = 1; cid < count; ++cid) big16(charset, static_cast<std::uint16_t>(cid));
    const std::string fdSelect(1 + count, '\0'); // Format 0, every glyph uses FD 0.
    std::vector<std::string> glyphs{std::string(1, '\16')}; // .notdef
    glyphs.reserve(count);
    for (const auto& glyph : used) glyphs.push_back(Outline().charstring(face, glyph));
    const auto charStrings = cffIndex(glyphs);
    const auto topSize = topDictionary(0, 0, 0, 0, count, bounds).size();
    const auto charsetOffset = 4 + names.size() + topSize + strings.size() + globalSubrs.size();
    const auto fdSelectOffset = charsetOffset + charset.size();
    const auto charStringsOffset = fdSelectOffset + fdSelect.size();
    const auto fdArrayOffset = charStringsOffset + charStrings.size();
    const auto privateOffset = fdArrayOffset + fontDictionary(0).size();
    if (privateOffset > std::numeric_limits<int>::max()) throw std::runtime_error("译文字体数据超出 PDF 限制。");
    const auto top = topDictionary(static_cast<int>(charsetOffset), static_cast<int>(fdSelectOffset),
        static_cast<int>(charStringsOffset), static_cast<int>(fdArrayOffset), count, bounds);
    return std::string("\x01\x00\x04\x04", 4) + names + top + strings + globalSubrs + charset + fdSelect +
        charStrings + fontDictionary(static_cast<int>(privateOffset));
}

void hex16(std::string& output, std::uint16_t value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned shift = 12;; shift -= 4) {
        output += hex[(value >> shift) & 15];
        if (!shift) break;
    }
}

std::string toUnicode(const std::vector<Glyph>& used)
{
    std::string mapping = "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /YiyiToUnicode def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    for (size_t start = 0; start < used.size(); start += 100) {
        const size_t end = std::min(start + 100, used.size());
        mapping += std::to_string(end - start) + " beginbfchar\n";
        for (size_t index = start; index < end; ++index) {
            mapping += '<';
            hex16(mapping, static_cast<std::uint16_t>(index + 1));
            mapping += "> <";
            auto code = used[index].code;
            if (code <= 0xffff) hex16(mapping, static_cast<std::uint16_t>(code));
            else {
                code -= 0x10000;
                hex16(mapping, static_cast<std::uint16_t>(0xd800 + (code >> 10)));
                hex16(mapping, static_cast<std::uint16_t>(0xdc00 + (code & 0x3ff)));
            }
            mapping += ">\n";
        }
        mapping += "endbfchar\n";
    }
    return mapping + "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
}

std::string number(double value)
{
    char text[48]{};
    const auto conversion = std::to_chars(text, text + sizeof(text), value, std::chars_format::fixed, 4);
    if (conversion.ec != std::errc{}) throw std::runtime_error("PDF 文字层坐标无效。");
    return std::string(text, conversion.ptr);
}

class Pdf {
public:
    int add(std::string value)
    {
        m_objects.push_back(std::move(value));
        return static_cast<int>(m_objects.size());
    }

    int stream(std::string_view content, std::string_view attributes = {})
    {
        auto object = "<< /Length " + std::to_string(content.size()) + ' ' + std::string(attributes) + " >>\nstream\n";
        object += content;
        object += "\nendstream";
        return add(std::move(object));
    }

    std::vector<std::uint8_t> write() const
    {
        std::string file = "%PDF-1.7\n%\xe2\xe3\xcf\xd3\n";
        std::vector<size_t> offsets;
        for (size_t index = 0; index < m_objects.size(); ++index) {
            offsets.push_back(file.size());
            file += std::to_string(index + 1) + " 0 obj\n" + m_objects[index] + "\nendobj\n";
        }
        const auto xref = std::to_string(file.size());
        const auto count = std::to_string(m_objects.size() + 1);
        file += "xref\n0 " + count + "\n0000000000 65535 f \n";
        for (const auto offset : offsets) {
            const auto value = std::to_string(offset);
            if (value.size() > 10) throw std::runtime_error("PDF 文字层体积过大。");
            file += std::string(10 - value.size(), '0') + value + " 00000 n \n";
        }
        file += "trailer\n<< /Size " + count + " /Root 1 0 R >>\nstartxref\n" + xref + "\n%%EOF\n";
        return std::vector<std::uint8_t>(file.begin(), file.end());
    }

    std::string& object(int id) { return m_objects[id - 1]; }

private:
    std::vector<std::string> m_objects;
};

std::string ref(int id) { return std::to_string(id) + " 0 R"; }

struct Subset {
    void insert(const Glyph& glyph)
    {
        if (symbols.contains(glyph.code)) return;
        if (used.size() >= 65534) throw std::runtime_error("译文字形数量超出 PDF 字体限制。");
        used.push_back(glyph);
        symbols.emplace(glyph.code, static_cast<std::uint16_t>(used.size()));
    }

    int embed(Pdf& pdf, const Face& face, bool serif) const
    {
        std::uint64_t fingerprint = 14695981039346656037ULL;
        for (const auto& glyph : used) {
            fingerprint = (fingerprint ^ glyph.code) * 1099511628211ULL;
            fingerprint = (fingerprint ^ glyph.index) * 1099511628211ULL;
        }
        auto tag = fingerprint;
        std::string name(6, 'A');
        for (char& character : name) {
            character += static_cast<char>(tag % 26);
            tag /= 26;
        }
        name += serif ? "+YiyiPdfSerif-Regular-" : "+YiyiPdfSans-Regular-";
        name += std::to_string(fingerprint);
        std::array<int, 4> bounds{};
        std::string widths;
        for (const auto& glyph : used) {
            widths += number(glyph.advance) + ' ';
            bounds[0] = std::min(bounds[0], static_cast<int>(std::floor(glyph.inkLeft)));
            bounds[1] = std::min(bounds[1], static_cast<int>(std::floor(-glyph.inkBottom)));
            bounds[2] = std::max(bounds[2], static_cast<int>(std::ceil(glyph.inkRight)));
            bounds[3] = std::max(bounds[3], static_cast<int>(std::ceil(-glyph.inkTop)));
        }
        const auto embedded = pdf.stream(embedCff(name, used, face, bounds), "/Subtype /CIDFontType0C");
        const auto colorSpace = serif ? " /Flags 6" : " /Flags 4";
        std::string box;
        for (int coordinate : bounds) box += std::to_string(coordinate) + ' ';
        const auto descriptor = pdf.add("<< /Type /FontDescriptor /FontName /" + name + colorSpace +
            " /FontBBox [" + box + "] /ItalicAngle 0 /Ascent 880 /Descent -120 /CapHeight 700 /StemV 80 /FontFile3 " +
            ref(embedded) + " >>");
        const auto descendant = pdf.add("<< /Type /Font /Subtype /CIDFontType0 /BaseFont /" + name +
            " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor " + ref(descriptor) +
            " /DW 1000 /W [1 [" + widths + "]] >>");
        const auto mapping = pdf.stream(toUnicode(used));
        return pdf.add("<< /Type /Font /Subtype /Type0 /BaseFont /" + name + " /Encoding /Identity-H /DescendantFonts [" +
            ref(descendant) + "] /ToUnicode " + ref(mapping) + " >>");
    }

    std::vector<Glyph> used;
    std::map<std::uint32_t, std::uint16_t> symbols;
};

void emit(std::string& content, const std::vector<PlacedGlyph>& glyphs, const std::array<Subset, 2>& fonts, double height)
{
    for (size_t start = 0; start < glyphs.size();) {
        const auto& first = glyphs[start];
        const auto face = first.glyph.face;
        content += "BT /F" + std::to_string(face) + ' ' + number(first.size) + " Tf ";
        content += first.style.bold ? number(first.size * .032) + " w 2 Tr " : "0 Tr ";
        content += number(first.scaleX) + " 0 " + number(first.style.italic ? italicShear : 0) + " 1 " +
            number(first.x) + ' ' + number(height - first.baseline) + " Tm <";
        double position = first.x;
        size_t end = start;
        while (end < glyphs.size()) {
            const auto& glyph = glyphs[end];
            if (glyph.glyph.face != face || glyph.size != first.size || glyph.baseline != first.baseline ||
                glyph.scaleX != first.scaleX || glyph.style.bold != first.style.bold || glyph.style.italic != first.style.italic ||
                std::abs(glyph.x - position) > .001) break;
            hex16(content, fonts[face].symbols.at(glyph.glyph.code));
            position += glyph.glyph.advance * glyph.size * glyph.scaleX / cffEm;
            ++end;
        }
        content += "> Tj ET\n";
        start = end;
    }
}

bool pageSize(double width, double height)
{
    return std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0 && width <= 1e6 && height <= 1e6;
}

}

struct OverlayLayoutData {
    std::array<std::uint8_t, 3> color;
    std::vector<PlacedGlyph> glyphs;
};

struct OverlayRenderer::Fonts {
    ComPtr<IDWriteFactory> factory;
    std::array<Face, 2> faces;
};

OverlayRenderer::OverlayRenderer(const std::filesystem::path& runtimeDirectory) : m_fonts(std::make_unique<Fonts>())
{
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                    reinterpret_cast<IUnknown**>(m_fonts->factory.GetAddressOf())))) {
        throw std::runtime_error("无法初始化 Windows PDF 字体排版。");
    }
    const auto directory = std::filesystem::absolute(runtimeDirectory);
    m_fonts->faces[0].load(*m_fonts->factory.Get(), directory / L"YiyiPdfSans-Regular.otf");
    if (!m_fonts->faces[0]) throw std::runtime_error("缺少 PDF 导出字体或字体无法读取，请使用完整发行包。");
    m_fonts->faces[1].load(*m_fonts->factory.Get(), directory / L"YiyiPdfSerif-Regular.otf");
}

OverlayRenderer::~OverlayRenderer() = default;

OverlayLayout OverlayRenderer::fit(const OverlayText& block, double pageWidth, double pageHeight) const
{
    OverlayLayout result;
    if (!pageSize(pageWidth, pageHeight) || !std::isfinite(block.x) || !std::isfinite(block.y) ||
        !std::isfinite(block.width) || !std::isfinite(block.height) || !std::isfinite(block.fontSize) ||
        block.fontSize < 1 || block.fontSize > 512 || block.text.size() > maxTextUnits) {
        result.error = L"译文或区域超出限制";
        return result;
    }
    const auto x = std::max(0.0, block.x), y = std::max(0.0, block.y);
    const auto width = std::min(pageWidth, block.x + block.width) - x;
    const auto height = std::min(pageHeight, block.y + block.height) - y;
    if (width < 3 || height < 3) {
        result.error = L"译文或区域超出限制";
        return result;
    }
    std::vector<Character> text;
    if (!charactersFor(block, text, result.error)) {
        if (result.error.empty()) result.error = L"译文超出长度限制";
        return result;
    }
    const std::uint8_t preferred = block.serif && !block.code && m_fonts->faces[1] ? 1 : 0;
    for (auto& character : text) {
        if (lineBreak(character.code)) continue;
        auto glyph = m_fonts->faces[preferred].glyph(character.code, preferred);
        const auto other = static_cast<std::uint8_t>(1 - preferred);
        if (!glyph) glyph = m_fonts->faces[other].glyph(character.code, other);
        if (!glyph) {
            result.error = unsupported(character.code);
            return result;
        }
        character.glyph = *glyph;
    }
    const auto original = block.fontSize;
    const auto minimum = std::min(original, std::max(original < 8 ? original * .8 : 6.0, original * .5));
    const bool fixed = block.fixed && block.text == block.source;
    for (auto size = original;; size = std::max(minimum, size - .25)) {
        const auto lines = wrap(text, size, width * .97, block.code, fixed);
        const auto step = size * m_fonts->faces[preferred].lineHeight() * 1.04;
        auto glyphs = place(text, lines, x, y, width, height, size, block.code ? size * 1.2 : step, block.centered, block.code);
        if (glyphs.empty() && !block.code) {
            glyphs = place(text, lines, x, y, width, height, size, size * 1.12, block.centered, false);
        }
        if (!glyphs.empty()) {
            result.m_data = std::make_shared<OverlayLayoutData>(OverlayLayoutData{block.color, std::move(glyphs)});
            return result;
        }
        if (size <= minimum) break;
    }
    result.error = L"译文超出区域，请调整区域、字号或译文后排版";
    return result;
}

std::vector<std::uint8_t> OverlayRenderer::render(double pageWidth, double pageHeight,
                                                 const std::vector<OverlayLayout>& layouts) const
{
    if (!pageSize(pageWidth, pageHeight)) throw std::runtime_error("PDF 文字层页面尺寸无效。");
    std::array<Subset, 2> fonts;
    for (const auto& layout : layouts) if (layout.m_data) {
        for (const auto& placement : layout.m_data->glyphs) fonts[placement.glyph.face].insert(placement.glyph);
    }
    if (fonts[0].used.empty() && fonts[1].used.empty()) return {};

    Pdf pdf;
    pdf.add("<< /Type /Catalog /Pages 2 0 R >>");
    pdf.add("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    const auto page = pdf.add({});
    std::string resources;
    for (std::uint8_t face = 0; face < fonts.size(); ++face) if (!fonts[face].used.empty()) {
        resources += "/F" + std::to_string(face) + ' ' + ref(fonts[face].embed(pdf, m_fonts->faces[face], face == 1)) + ' ';
    }

    std::string content;
    for (const auto& layout : layouts) if (layout.m_data) {
        const auto& data = *layout.m_data;
        std::string color;
        for (const auto component : data.color) color += number(component / 255.0) + ' ';
        content += "q " + color + "rg " + color + "RG\n";
        emit(content, data.glyphs, fonts, pageHeight);
        content += "Q\n";
    }
    const auto contents = pdf.stream(content);
    pdf.object(page) = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + number(pageWidth) + ' ' + number(pageHeight) +
        "] /Resources << /Font << " + resources + ">> >> /Contents " + ref(contents) + " >>";
    return pdf.write();
}

}
