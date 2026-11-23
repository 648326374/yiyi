#include "OfficeDocumentNative.h"
#include "DocumentFiles.h"
#include "miniz.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <dwrite.h>

#include <winrt/Windows.Data.Text.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "dwrite.lib")

using namespace winrt;
using namespace Windows::Data::Text;
using namespace Windows::Data::Xml::Dom;
using namespace Windows::Storage::Streams;

namespace yiyi {
namespace {

constexpr size_t maxArchive = 512 * 1024 * 1024;
constexpr size_t maxPart = 32 * 1024 * 1024;
constexpr size_t maxEntries = 100000;

[[noreturn]] void invalid(hstring const& message)
{
    throw hresult_invalid_argument(message);
}

[[noreturn]] void fail(hstring const& message)
{
    throw hresult_error(E_FAIL, message);
}

std::wstring_view string(hstring const& value) { return std::wstring_view(value); }

hstring sha256(std::string const& bytes)
{
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        fail(L"无法初始化文档内容校验。");
    std::array<UCHAR, 32> hash{};
    const auto status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                   static_cast<ULONG>(bytes.size()), hash.data(), static_cast<ULONG>(hash.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) fail(L"无法校验文档内容。");
    constexpr std::wstring_view hex = L"0123456789abcdef";
    std::wstring text;
    text.reserve(hash.size() * 2);
    for (const auto byte : hash) {
        text += hex[byte >> 4];
        text += hex[byte & 15];
    }
    return hstring(text);
}

class ZipReader {
public:
    explicit ZipReader(std::string const& archive)
    {
        if (!mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0))
            invalid(L"文件不是有效的 Office 开放文档。加密文档请先解除密码，旧格式请先转换。");
    }
    ~ZipReader() { mz_zip_reader_end(&zip); }
    ZipReader(ZipReader const&) = delete;
    ZipReader& operator=(ZipReader const&) = delete;

    mz_zip_archive_file_stat stat(mz_uint index)
    {
        mz_zip_archive_file_stat result{};
        if (!mz_zip_reader_file_stat(&zip, index, &result))
            invalid(L"文档目录损坏。");
        return result;
    }

    std::string read(mz_uint index)
    {
        const auto item = stat(index);
        if (item.m_uncomp_size > maxPart || item.m_is_encrypted)
            invalid(L"文档部件过大、已加密或损坏。");
        std::string data(static_cast<size_t>(item.m_uncomp_size), '\0');
        if (!mz_zip_reader_extract_to_mem(&zip, index, data.data(), data.size(), 0))
            invalid(L"无法读取文档部件，文件可能损坏。");
        return data;
    }

    mz_zip_archive zip{};
};

class ZipWriter {
public:
    explicit ZipWriter(documentFiles::AtomicOutput& file)
    {
        zip.m_pIO_opaque = &file;
        zip.m_pWrite = [](void* opaque, mz_uint64 offset, const void* bytes, size_t length) -> size_t {
            auto& target = *static_cast<documentFiles::AtomicOutput*>(opaque);
            return target.writeAt(offset, std::string_view(static_cast<char const*>(bytes), length));
        };
        if (!mz_zip_writer_init(&zip, 0)) fail(L"无法初始化 Office 导出。");
    }
    ~ZipWriter() { mz_zip_writer_end(&zip); }
    ZipWriter(ZipWriter const&) = delete;
    ZipWriter& operator=(ZipWriter const&) = delete;

    mz_zip_archive zip{};
};

bool validName(std::string_view path)
{
    if (path.empty() || path.front() == '/' || path.find_first_of("\\:") != path.npos) return false;
    while (!path.empty()) {
        const auto slash = path.find('/');
        const auto unit = path.substr(0, slash);
        if (unit.empty() || unit == "." || unit == "..") return false;
        if (slash == path.npos) break;
        path.remove_prefix(slash + 1);
    }
    return true;
}

std::wstring relationPart(std::wstring_view part)
{
    const auto slash = part.find_last_of(L'/') + 1;
    return std::wstring(part.substr(0, slash)) + L"_rels/" + std::wstring(part.substr(slash)) + L".rels";
}

std::wstring resolve(std::wstring_view base, std::wstring_view target)
{
    if (target.find_first_of(L"\\:?#") != target.npos || target.empty())
        invalid(L"文档部件关系包含无效路径。");
    std::wstring path(target.starts_with(L'/') ? target.substr(1) : base.substr(0, base.find_last_of(L'/') + 1));
    if (!target.starts_with(L'/')) path += target;
    std::vector<std::wstring_view> components;
    for (std::wstring_view rest(path); !rest.empty();) {
        const auto slash = rest.find(L'/');
        const auto component = rest.substr(0, slash);
        if (component == L"..") {
            if (components.empty()) invalid(L"文档部件关系超出文档包。");
            components.pop_back();
        } else if (!component.empty() && component != L".") {
            components.push_back(component);
        }
        if (slash == rest.npos) break;
        rest.remove_prefix(slash + 1);
    }
    std::wstring result;
    for (const auto component : components) {
        if (!result.empty()) result += L'/';
        result += component;
    }
    if (result.empty()) invalid(L"文档部件关系缺少目标。");
    return result;
}

XmlDocument parseXml(std::string_view bytes, hstring const& part)
{
    if (bytes.empty()) invalid(L"文档包含空 XML 部件：" + part);
    Buffer buffer{static_cast<uint32_t>(bytes.size())};
    std::memcpy(buffer.data(), bytes.data(), bytes.size());
    buffer.Length(static_cast<uint32_t>(bytes.size()));
    XmlLoadSettings options;
    options.ProhibitDtd(true);
    options.ResolveExternals(false);
    options.ValidateOnParse(false);
    options.ElementContentWhiteSpace(true);
    options.MaxElementDepth(256);
    XmlDocument document;
    try {
        document.LoadXmlFromBuffer(buffer, options);
    } catch (hresult_error const&) {
        invalid(L"无法解析文档 XML，部件损坏、嵌套过深或包含 DTD：" + part);
    }
    return document;
}

hstring local(IXmlNode const& node) { return unbox_value_or<hstring>(node.LocalName(), {}); }
hstring ns(IXmlNode const& node) { return unbox_value_or<hstring>(node.NamespaceUri(), {}); }

template <typename Visitor>
void eachElement(IXmlNode const& parent, Visitor const& visit)
{
    for (auto node = parent.FirstChild(); node; node = node.NextSibling()) {
        if (node.NodeType() == NodeType::ElementNode) visit(node.as<XmlElement>());
    }
}

void descendants(XmlElement const& root, hstring const& name, std::vector<XmlElement>& output)
{
    if (local(root) == name) output.push_back(root);
    eachElement(root, [&](XmlElement const& child) { descendants(child, name, output); });
}

std::vector<XmlElement> named(XmlElement const& root, hstring const& name)
{
    std::vector<XmlElement> output;
    if (root) descendants(root, name, output);
    return output;
}

XmlElement ancestor(IXmlNode const& node) { return node.ParentNode().try_as<XmlElement>(); }

hstring attribute(XmlElement const& node, hstring const& name)
{
    if (const auto value = node.GetAttribute(name); !value.empty()) return value;
    const auto attributes = node.Attributes();
    for (uint32_t i = 0; i < attributes.Length(); ++i) {
        const auto item = attributes.Item(i);
        if (local(item) == name) return item.InnerText();
    }
    return {};
}

std::vector<XmlElement> paragraphs(XmlDocument const& doc)
{
    auto output = named(doc.DocumentElement(), L"p");
    std::erase_if(output, [](XmlElement const& element) {
        const auto name = ns(element);
        const auto family = string(name);
        return !family.ends_with(L"/wordprocessingml/2006/main") && !family.ends_with(L"/wordprocessingml/main")
            && !family.ends_with(L"/drawingml/2006/main") && !family.ends_with(L"/drawingml/main");
    });
    return output;
}

bool textElement(XmlElement const& element)
{
    if (local(element) != L"t") return false;
    const auto family = ns(element);
    return string(family).find(L"wordprocessingml") != std::wstring_view::npos
        || string(family).find(L"drawingml") != std::wstring_view::npos;
}

bool protectedElement(std::wstring_view name)
{
    return name == L"fldChar" || name == L"fldSimple" || name == L"fld" || name == L"ins"
        || name == L"del" || name == L"moveFrom" || name == L"moveTo";
}

struct ParagraphText {
    std::vector<XmlElement> nodes;
    std::vector<std::vector<size_t>> pieces;
};

ParagraphText paragraphText(XmlElement const& paragraph)
{
    ParagraphText result;
    std::vector<size_t> piece;
    const auto flush = [&] {
        if (!piece.empty()) result.pieces.push_back(std::exchange(piece, {}));
    };
    const auto walk = [&](auto&& self, XmlElement const& parent) -> void {
        eachElement(parent, [&](XmlElement const& child) {
            const auto name = local(child);
            if (name == L"p") return;
            if (textElement(child)) {
                piece.push_back(result.nodes.size());
                result.nodes.push_back(child);
            } else if (name == L"br" || name == L"tab" || name == L"cr" || name == L"footnoteReference" || name == L"endnoteReference") {
                flush();
            } else {
                self(self, child);
            }
        });
    };
    walk(walk, paragraph);
    flush();
    return result;
}

std::wstring style(XmlElement const& element)
{
    std::wstring result;
    for (auto parent = ancestor(element); parent; parent = ancestor(parent)) {
        const auto name = local(parent);
        if (name == L"p") break;
        if (name == L"hyperlink") result += L"link:" + std::wstring(attribute(parent, L"id"));
        if (name == L"r") eachElement(parent, [&](XmlElement const& property) {
            if (local(property) == L"rPr") result += string(property.GetXml());
        });
    }
    return result;
}

bool hasLetters(std::wstring_view text)
{
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t code = text[i];
        if (code >= 0xd800 && code <= 0xdbff && i + 1 < text.size()) {
            const auto low = text[i + 1];
            if (low >= 0xdc00 && low <= 0xdfff) {
                code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                ++i;
            }
        }
        const auto category = UnicodeCharacters::GetGeneralCategory(code);
        if (category >= UnicodeGeneralCategory::UppercaseLetter && category <= UnicodeGeneralCategory::OtherLetter) return true;
    }
    return false;
}

double number(hstring const& text, double fallback = 0)
{
    wchar_t* end{};
    const auto value = std::wcstod(text.c_str(), &end);
    return end != text.c_str() && std::isfinite(value) ? value : fallback;
}

std::wstring pageLabel(size_t page) { return L"第 " + std::to_wstring(page) + L" 页"; }
std::wstring paragraphLabel(size_t index) { return L" · 段落 " + std::to_wstring(index); }

void describeShape(OfficeSegment& segment, XmlElement const& paragraph, std::wstring const& label, size_t ordinal)
{
    for (auto shape = ancestor(paragraph); shape; shape = ancestor(shape)) {
        if (local(shape) != L"sp") continue;
        const auto properties = named(shape, L"cNvPr");
        const auto id = properties.empty() ? hstring(std::to_wstring(segment.paragraph)) : attribute(properties.front(), L"id");
        const auto name = properties.empty() ? hstring() : attribute(properties.front(), L"name");
        segment.group = hstring(std::wstring(segment.part) + L":" + std::wstring(id));
        if (!name.empty()) segment.location = hstring(label + L" · " + std::wstring(name) + paragraphLabel(ordinal));
        for (const auto& extent : named(shape, L"ext")) {
            const auto width = extent.GetAttribute(L"cx");
            const auto height = extent.GetAttribute(L"cy");
            if (width.empty() || height.empty()) continue;
            segment.width = number(width) / 12700;
            segment.height = number(height) / 12700;
            break;
        }
        const auto bodies = named(shape, L"bodyPr");
        const auto margin = [&](hstring const& name, double fallback) {
            return bodies.empty() ? fallback : number(bodies.front().GetAttribute(name), fallback * 12700) / 12700;
        };
        segment.width -= margin(L"lIns", 7.2) + margin(L"rIns", 7.2);
        segment.height -= margin(L"tIns", 3.6) + margin(L"bIns", 3.6);
        for (const auto& property : named(paragraph, L"rPr")) {
            const auto size = property.GetAttribute(L"sz");
            if (size.empty()) continue;
            segment.fontSize = std::clamp(number(size, 1800) / 100, 1.0, 400.0);
            break;
        }
        break;
    }
}

void extractSegments(XmlDocument const& doc, std::wstring const& part, std::wstring const& label, size_t page,
                     std::vector<OfficeSegment>& segments, std::vector<OfficeNotice>& notices)
{
    const auto items = paragraphs(doc);
    size_t ordinal = 0;
    size_t fieldDepth = 0;
    for (size_t p = 0; p < items.size(); ++p) {
        const auto& paragraph = items[p];
        bool protect = fieldDepth > 0;
        for (const auto& field : named(paragraph, L"fldChar")) {
            protect = true;
            const auto kind = attribute(field, L"fldCharType");
            if (kind == L"begin") ++fieldDepth;
            else if (kind == L"end" && fieldDepth) --fieldDepth;
        }
        for (auto parent = ancestor(paragraph); parent && !protect; parent = ancestor(parent))
            protect = protectedElement(string(local(parent)));
        const auto text = paragraphText(paragraph);
        std::wstring source;
        for (const auto& node : text.nodes) source += string(node.InnerText());
        if (!hasLetters(source)) continue;
        constexpr const wchar_t* protectedNodes[] = {L"fldSimple", L"fld", L"ins", L"del", L"moveFrom", L"moveTo"};
        for (auto name : protectedNodes) protect = protect || !named(paragraph, name).empty();
        if (protect) {
            notices.push_back({{}, hstring(label), hstring(L"含自动域或修订的段落保留原文：" + source.substr(0, 70))});
            continue;
        }
        OfficeSegment segment;
        segment.part = hstring(part);
        segment.paragraph = p;
        segment.id = hstring(part + L":" + std::to_wstring(p));
        segment.page = page;
        segment.location = hstring(label + paragraphLabel(++ordinal));
        describeShape(segment, paragraph, label, ordinal);
        if (part.find(L"notesSlides/") != part.npos) {
            segment.width = segment.height = 0;
            segment.location = hstring(label + paragraphLabel(ordinal));
        }
        size_t piece = 0;
        for (auto const& nodes : text.pieces) {
            auto unit = segment;
            std::wstring value;
            for (const auto index : nodes) value += string(text.nodes[index].InnerText());
            if (!hasLetters(value)) continue;
            unit.source = hstring(value);
            unit.nodes = nodes;
            if (text.pieces.size() > 1) {
                unit.id = hstring(std::wstring(segment.id) + L":" + std::to_wstring(piece));
                unit.location = hstring(std::wstring(segment.location) + L" · 片段 " + std::to_wstring(piece + 1));
            }
            ++piece;
            std::set<std::wstring> styles;
            for (const auto index : nodes) styles.insert(style(text.nodes[index]));
            unit.mixedStyle = styles.size() > 1;
            if (unit.mixedStyle)
                notices.push_back({unit.id, unit.location, L"段内有多种样式或链接，译文样式位置按长度映射，请校对强调范围。"});
            segments.push_back(std::move(unit));
        }
    }
    if (part.find(L"/charts/") != part.npos && !named(doc.DocumentElement(), L"strRef").empty())
        notices.push_back({{}, hstring(label), L"图表标题可翻译；与嵌入工作簿关联的系列名、坐标标签保留原文。"});
}

com_ptr<IDWriteFactory> const& textFactory()
{
    static const auto factory = [] {
        com_ptr<IDWriteFactory> result;
        check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown**>(result.put())));
        return result;
    }();
    return factory;
}

com_ptr<IDWriteTextLayout> layoutText(std::wstring_view text, double pointSize, double width)
{
    const auto& factory = textFactory();
    com_ptr<IDWriteTextFormat> format;
    check_hresult(factory->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(pointSize * 96 / 72), L"zh-CN", format.put()));
    com_ptr<IDWriteTextLayout> layout;
    check_hresult(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.get(),
        static_cast<float>(std::clamp(width * 96 / 72, 1.0, 100000.0)), 100000.0f, layout.put()));
    return layout;
}

double textHeight(hstring const& text, double size, double width)
{
    if (text.empty()) return 0;
    DWRITE_TEXT_METRICS metrics{};
    check_hresult(layoutText(string(text), size, width)->GetMetrics(&metrics));
    return metrics.height * 72 / 96;
}

std::vector<size_t> characterBoundaries(std::wstring_view text)
{
    if (text.empty()) return {0};
    auto layout = layoutText(text, 12, 100000);
    std::vector<DWRITE_CLUSTER_METRICS> clusters(text.size());
    UINT32 count{};
    check_hresult(layout->GetClusterMetrics(clusters.data(), static_cast<UINT32>(clusters.size()), &count));
    std::vector<size_t> offsets{0};
    for (size_t i = 0; i < count; ++i) offsets.push_back(offsets.back() + clusters[i].length);
    if (offsets.back() != text.size()) fail(L"无法识别译文字符边界。");
    return offsets;
}

using Groups = std::map<hstring, std::vector<OfficeSegment const*>>;

Groups fitGroups(std::vector<OfficeSegment> const& segments)
{
    Groups groups;
    for (const auto& segment : segments) {
        if (!segment.group.empty() && segment.width > 0 && segment.height > 0)
            groups[segment.group].push_back(&segment);
    }
    return groups;
}

double fontSize(double size, double scale, double minimum)
{
    return std::max(std::min(minimum, size), size * scale);
}

double fitHeight(std::vector<OfficeSegment const*> const& group, OfficeTranslations const& translations,
                 double scale, OfficeExportOptions const& options)
{
    double height = 0;
    for (const auto* item : group) {
        const auto translation = translations.find(item->id);
        const auto size = fontSize(item->fontSize, scale, options.minimumFont);
        height += textHeight(translation == translations.end() ? item->source : translation->second, size, item->width) * 1.12;
    }
    return height;
}

double fitScale(std::vector<OfficeSegment const*> const& group, OfficeTranslations const& translations, OfficeExportOptions const& options)
{
    const auto available = group.front()->height;
    if (!options.fitText || fitHeight(group, translations, 1, options) <= available) return 1;
    double low = 0.05;
    double high = 1;
    for (int i = 0; i < 16; ++i) {
        const auto middle = (low + high) / 2;
        if (fitHeight(group, translations, middle, options) <= available) low = middle;
        else high = middle;
    }
    return low;
}

std::wstring_view trim(std::wstring_view text)
{
    constexpr std::wstring_view spaces = L" \t\r\n\f\v\u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005"
                                         L"\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000";
    const auto first = text.find_first_not_of(spaces);
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(spaces) + 1 - first);
}

std::wstring withWhitespace(hstring const& source, hstring const& target)
{
    auto const text = string(source);
    auto const content = trim(text);
    if (content.empty()) return std::wstring(trim(string(target)));
    const auto start = static_cast<size_t>(content.data() - text.data());
    return std::wstring(text.substr(0, start)) + std::wstring(trim(string(target))) + std::wstring(text.substr(start + content.size()));
}

void replaceText(XmlDocument const& document, XmlElement const& element, hstring const& replacement)
{
    for (auto child = element.FirstChild(); child; child = element.FirstChild()) element.RemoveChild(child);
    if (!replacement.empty()) element.AppendChild(document.CreateTextNode(replacement));
    element.SetAttributeNS(box_value(hstring(L"http://www.w3.org/XML/1998/namespace")), L"xml:space", L"preserve");
}

void scaleParagraph(XmlDocument const& doc, XmlElement const& paragraph, double defaultSize, double scale, OfficeExportOptions const& options)
{
    const auto prefix = unbox_value_or<hstring>(paragraph.Prefix(), {});
    const auto name = prefix.empty() ? hstring(L"rPr") : prefix + L":rPr";
    const auto namespaceUri = paragraph.NamespaceUri();
    eachElement(paragraph, [&](XmlElement const& run) {
        const auto type = local(run);
        if (type != L"r" && type != L"fld") return;
        XmlElement properties{nullptr};
        eachElement(run, [&](XmlElement const& element) {
            if (!properties && local(element) == L"rPr") properties = element;
        });
        if (!properties) {
            properties = doc.CreateElementNS(namespaceUri, name);
            if (const auto first = run.FirstChild()) run.InsertBefore(properties, first);
            else run.AppendChild(properties);
        }
        const auto original = std::clamp(number(properties.GetAttribute(L"sz"), defaultSize * 100) / 100, 1.0, 400.0);
        properties.SetAttribute(L"sz", hstring(std::to_wstring(std::lround(fontSize(original, scale, options.minimumFont) * 100))));
    });
}

std::string utf8Xml(XmlDocument const& document)
{
    const auto xml = document.GetXml();
    auto text = string(xml);
    if (text.starts_with(L"<?xml")) {
        const auto end = text.find(L"?>");
        if (end == text.npos) fail(L"无法导出文档 XML。");
        text.remove_prefix(end + 2);
    }
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>" + to_string(text);
}

}

OfficeDocumentNative OfficeDocumentNative::load(std::filesystem::path const& path)
{
    OfficeDocumentNative result;
    const bool word = documentFiles::hasExtension(path, L".docx");
    if (!word && !documentFiles::hasExtension(path, L".pptx"))
        invalid(L"请选择 .docx 或 .pptx，旧版 .doc / .ppt 请先另存为新格式。");
    result.m_path = std::filesystem::absolute(path).lexically_normal();
    result.m_sourcePath = documentFiles::sourcePath(result.m_path);
    result.m_archive = documentFiles::read(result.m_sourcePath, maxArchive);
    result.m_hash = sha256(result.m_archive);
    ZipReader zip(result.m_archive);
    const auto count = mz_zip_reader_get_num_files(&zip.zip);
    if (count > maxEntries) invalid(L"文档包含过多部件。");
    std::map<std::wstring, mz_uint, std::less<>> entries;
    for (mz_uint i = 0; i < count; ++i) {
        const auto item = zip.stat(i);
        const std::string_view name(item.m_filename);
        if (mz_zip_reader_get_filename(&zip.zip, i, nullptr, 0) != name.size() + 1
            || !validName(name) || item.m_is_encrypted)
            invalid(L"文档包含不规范的部件路径，或部件已加密。");
        const auto fullName = to_hstring(name);
        if (!entries.emplace(std::wstring(fullName), i).second)
            invalid(L"文档包含重复部件。");
        if (name.starts_with("_xmlsignatures/"))
            invalid(L"带数字签名的文档请先保存为未签名副本，再进行翻译。");
        if (fullName.size() >= 14 && CompareStringOrdinal(fullName.c_str() + fullName.size() - 14, 14, L"vbaProject.bin", 14, TRUE) == CSTR_EQUAL)
            invalid(L"请先另存为不含宏的 DOCX / PPTX 文档。");
        if (name.find("/media/") != name.npos && !item.m_is_directory) {
            const auto file = name.substr(name.find_last_of('/') + 1);
            result.m_images.push_back({fullName, to_hstring(file)});
        }
        if (name.find("/embeddings/") != name.npos)
            result.m_notices.push_back({{}, fullName, L"嵌入文件保留原样，未翻译内部内容。"});
    }
    const auto xml = [&](std::wstring_view pathName) {
        const auto entry = entries.find(pathName);
        if (entry == entries.end()) invalid(L"文档缺少必要部件：" + hstring(pathName));
        return parseXml(zip.read(entry->second), hstring(pathName));
    };
    std::wstring main;
    const auto rootRels = xml(L"_rels/.rels");
    for (const auto& relation : named(rootRels.DocumentElement(), L"Relationship")) {
        if (string(relation.GetAttribute(L"Type")).ends_with(L"/officeDocument") && relation.GetAttribute(L"TargetMode") != L"External")
            main = resolve({}, string(relation.GetAttribute(L"Target")));
    }
    if (main.empty() || !entries.contains(L"[Content_Types].xml"))
        invalid(L"文档缺少 Office 类型或主文档关系。");
    const auto mainDoc = xml(main);
    if (local(mainDoc.DocumentElement()) != (word ? L"document" : L"presentation"))
        invalid(L"文件内容与扩展名不一致。");
    result.m_kind = word ? L"docx" : L"pptx";

    struct Part {
        std::wstring name;
        std::wstring label;
        size_t page{};
    };
    std::vector<Part> parts;
    if (word) {
        result.m_pages = 1;
        parts.push_back({main, L"正文", 0});
    } else {
        const auto relationships = xml(relationPart(main));
        std::map<hstring, std::wstring> slides;
        for (const auto& relation : named(relationships.DocumentElement(), L"Relationship")) {
            if (string(relation.GetAttribute(L"Type")).ends_with(L"/slide") && relation.GetAttribute(L"TargetMode") != L"External")
                slides.emplace(relation.GetAttribute(L"Id"), resolve(main, string(relation.GetAttribute(L"Target"))));
        }
        const auto transitional = box_value(hstring(L"http://schemas.openxmlformats.org/officeDocument/2006/relationships"));
        const auto strict = box_value(hstring(L"http://purl.oclc.org/ooxml/officeDocument/relationships"));
        for (const auto& id : named(mainDoc.DocumentElement(), L"sldId")) {
            auto reference = id.GetAttributeNS(transitional, L"id");
            if (reference.empty()) reference = id.GetAttributeNS(strict, L"id");
            const auto slide = slides.find(reference);
            if (slide == slides.end()) invalid(L"幻灯片关系缺失。");
            const auto page = ++result.m_pages;
            const auto location = pageLabel(page);
            parts.push_back({slide->second, location, page});
            const auto relName = relationPart(slide->second);
            if (!entries.contains(relName)) continue;
            const auto slideRels = xml(relName);
            for (const auto& relation : named(slideRels.DocumentElement(), L"Relationship")) {
                if (relation.GetAttribute(L"TargetMode") == L"External") continue;
                const auto type = relation.GetAttribute(L"Type");
                std::wstring label;
                if (string(type).ends_with(L"/notesSlide")) label = L"备注";
                else if (string(type).ends_with(L"/chart")) label = L"图表";
                else if (string(type).ends_with(L"/diagramData") || string(type).ends_with(L"/diagramDrawing")) label = L"SmartArt";
                if (!label.empty())
                    parts.push_back({resolve(slide->second, string(relation.GetAttribute(L"Target"))), location + L" · " + label, page});
            }
        }
    }

    for (const auto& [name, index] : entries) {
        std::wstring label;
        if (word && name.starts_with(L"word/") && name.ends_with(L".xml")) {
            const auto file = std::wstring_view(name).substr(name.find_last_of(L'/') + 1);
            if (file.starts_with(L"header")) label = L"页眉";
            else if (file.starts_with(L"footer")) label = L"页脚";
            else if (name.ends_with(L"/footnotes.xml")) label = L"脚注";
            else if (name.ends_with(L"/endnotes.xml")) label = L"尾注";
            else if (name.find(L"/charts/") != name.npos) label = L"图表";
            else if (name.find(L"/diagrams/") != name.npos) label = L"SmartArt";
        }
        if (!word && name.starts_with(L"ppt/diagrams/") && name.ends_with(L".xml")) label = L"SmartArt";
        if (!label.empty()) parts.push_back({name, std::move(label), 0});
    }
    std::set<std::wstring> seen;
    for (const auto& part : parts) {
        if (!seen.emplace(part.name).second) continue;
        extractSegments(part.name == main ? mainDoc : xml(part.name), part.name, part.label, part.page, result.m_segments, result.m_notices);
    }
    if (!result.m_images.empty()) result.m_notices.push_back({{}, L"图片",
        hstring(L"保留 " + std::to_wstring(result.m_images.size()) + L" 个媒体资源，图片内文字未改写。")});
    if (result.m_segments.empty() && result.m_images.empty())
        invalid(L"没有找到可翻译的文字或可查看的图片。");
    return result;
}

std::string OfficeDocumentNative::imageBytes(hstring const& id) const
{
    const auto image = std::find_if(m_images.begin(), m_images.end(), [&](OfficeImage const& entry) { return entry.id == id; });
    if (image == m_images.end()) invalid(L"图片不存在。");
    ZipReader zip(m_archive);
    const auto index = mz_zip_reader_locate_file(&zip.zip, to_string(id).c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (index < 0) fail(L"无法读取文档图片。");
    return zip.read(static_cast<mz_uint>(index));
}

std::vector<OfficeNotice> OfficeDocumentNative::issues(OfficeTranslations const& translations, OfficeExportOptions options) const
{
    auto result = m_notices;
    options.minimumFont = std::clamp(options.minimumFont, 6.0, 36.0);
    for (auto const& [group, items] : fitGroups(m_segments)) {
        const auto scale = fitScale(items, translations, options);
        const auto* item = items.front();
        if (fitHeight(items, translations, scale, options) > item->height)
            result.push_back({item->id, item->location, L"文字可能超出文本框，请在 Office 中检查字号与换行。此处为估算。"});
        else if (scale < 0.99)
            result.push_back({item->id, item->location, L"导出时将在字号下限内缩小文字，建议检查实际版式。"});
    }
    return result;
}

void OfficeDocumentNative::exportTo(std::filesystem::path const& path, OfficeTranslations const& translations, OfficeExportOptions options) const
{
    const auto extension = m_kind == L"docx" ? L".docx" : L".pptx";
    if (!documentFiles::hasExtension(path, extension))
        invalid(hstring(std::wstring(L"请另存为不同文件名的 ") + extension + L" 文件。"));
    documentFiles::requireDifferentOutput(m_path, path);
    documentFiles::requireDifferentOutput(m_sourcePath, path);
    options.minimumFont = std::clamp(options.minimumFont, 6.0, 36.0);

    std::map<hstring, double> scales;
    if (m_kind == L"pptx" && options.fitText) {
        for (const auto& [group, items] : fitGroups(m_segments))
            scales.emplace(group, fitScale(items, translations, options));
    }
    std::map<hstring, std::vector<OfficeSegment const*>> byPart;
    for (const auto& segment : m_segments) {
        if (translations.contains(segment.id))
            byPart[segment.part].push_back(&segment);
    }
    const auto updatedPart = [&](XmlDocument const& doc, std::vector<OfficeSegment const*> const& segments) {
        const auto items = paragraphs(doc);
        std::set<size_t> scaled;
        for (const auto* segment : segments) {
            if (segment->paragraph >= items.size()) fail(L"文档段落映射发生变化。");
            const auto& paragraph = items[segment->paragraph];
            const auto texts = paragraphText(paragraph).nodes;
            const auto target = withWhitespace(segment->source, translations.at(segment->id));
            const auto boundaries = characterBoundaries(target);
            size_t consumed = 0;
            size_t sourceConsumed = 0;
            for (size_t i = 0; i < segment->nodes.size(); ++i) {
                const auto index = segment->nodes[i];
                if (index >= texts.size()) fail(L"文档文字映射发生变化。");
                sourceConsumed += texts[index].InnerText().size();
                const auto ratio = static_cast<double>(sourceConsumed) / std::max<size_t>(1, segment->source.size());
                const size_t end = i + 1 == segment->nodes.size() ? target.size() :
                    std::clamp(static_cast<size_t>(std::llround(ratio * target.size())), consumed, target.size());
                const auto boundary = *std::lower_bound(boundaries.begin(), boundaries.end(), end);
                replaceText(doc, texts[index], hstring(std::wstring_view(target).substr(consumed, boundary - consumed)));
                consumed = boundary;
            }
            const auto scale = scales.find(segment->group);
            if (scale != scales.end() && scale->second < 0.99 && scaled.emplace(segment->paragraph).second)
                scaleParagraph(doc, paragraph, segment->fontSize, scale->second, options);
        }
        return utf8Xml(doc);
    };
    ZipReader reader(m_archive);
    documentFiles::AtomicOutput file(path);
    ZipWriter writer(file);
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&reader.zip); ++i) {
        const auto partInfo = reader.stat(i);
        const auto part = to_hstring(partInfo.m_filename);
        const auto edit = byPart.find(part);
        bool written;
        if (edit == byPart.end()) {
            written = mz_zip_writer_add_from_zip_reader(&writer.zip, &reader.zip, i);
        } else {
            const auto bytes = updatedPart(parseXml(reader.read(i), part), edit->second);
            written = mz_zip_writer_add_mem(&writer.zip, partInfo.m_filename, bytes.data(), bytes.size(), MZ_BEST_SPEED);
        }
        if (!written) fail(L"导出失败，请检查磁盘空间和目录权限。");
    }
    if (!mz_zip_writer_finalize_archive(&writer.zip)) fail(L"无法完成 Office 导出。");
    file.commit();
}

}
