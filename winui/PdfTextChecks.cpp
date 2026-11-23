#include "PdfTextChecks.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <regex>
#include <set>
#include <vector>

#pragma comment(lib, "Normaliz.lib")

namespace yiyi {
namespace {

using namespace winrt;
using namespace Windows::Data::Json;

bool digit(wchar_t c)
{
    return c >= L'0' && c <= L'9';
}

bool identifier(wchar_t c)
{
    return digit(c) || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_';
}

std::wstring normalized(std::wstring_view value)
{
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const auto required = NormalizeString(NormalizationKC, value.data(), length, nullptr, 0);
    if (required <= 0) return std::wstring(value);
    std::wstring output(required, L'\0');
    const auto size = NormalizeString(NormalizationKC, value.data(), length, output.data(), required);
    if (size <= 0) return std::wstring(value);
    output.resize(size);
    return output;
}

std::vector<std::wstring> numericTokens(std::wstring_view source, bool pageReferences)
{
    auto value = normalized(source);
    std::replace(value.begin(), value.end(), L'−', L'-');
    if (pageReferences) std::replace(value.begin(), value.end(), L',', L' ');
    std::vector<std::wstring> tokens;
    for (size_t i = 0; i < value.size();) {
        const auto start = i;
        if ((value[i] == L'-' || value[i] == L'+') && i + 1 < value.size() && digit(value[i + 1])) ++i;
        if (!digit(value[i])) { ++i; continue; }
        while (i < value.size() && digit(value[i])) ++i;
        while (i + 1 < value.size() && (value[i] == L'.' || value[i] == L',') && digit(value[i + 1])) {
            ++i;
            while (i < value.size() && digit(value[i])) ++i;
        }
        if (i < value.size() && value[i] == L'%') ++i;
        if (start == 0 || !identifier(value[start - 1])) tokens.push_back(value.substr(start, i - start));
    }
    std::sort(tokens.begin(), tokens.end());
    return tokens;
}

std::wstring_view trim(std::wstring_view value)
{
    while (!value.empty() && iswspace(value.front())) value.remove_prefix(1);
    while (!value.empty() && iswspace(value.back())) value.remove_suffix(1);
    return value;
}

std::vector<std::wstring> codeTokens(std::wstring_view value)
{
    std::wstring code;
    wchar_t quote{};
    bool lineComment = false, blockComment = false, escaped = false;
    for (size_t i = 0; i < value.size(); ++i) {
        const auto c = value[i], next = (i + 1 < value.size() ? value[i + 1] : wchar_t{});
        if (lineComment) {
            if (c == L'\n') { lineComment = false; code += c; }
            continue;
        }
        if (blockComment) {
            if (c == L'\n') code += c;
            if (c == L'*' && next == L'/') { blockComment = false; ++i; }
            continue;
        }
        if (quote) {
            code += c;
            if (escaped) escaped = false;
            else if (c == L'\\') escaped = true;
            else if (c == quote) quote = {};
            continue;
        }
        if (c == L'/' && (next == L'/' || next == L'*')) {
            lineComment = next == L'/';
            blockComment = !lineComment;
            code += L' ';
            ++i;
            continue;
        }
        if (c == L'"' || c == L'\'') quote = c;
        code += c;
    }
    static const std::wregex token(LR"([A-Za-z_]\w*|(?:\d+(?:\.\d*)?|\.\d+)(?:[eEpP][+-]?\d+)?[a-zA-Z0-9_]*|>>=|<<=|->\*|\.\.\.|\+\+|--|->|\.\*|::|<<|>>|<=|>=|==|!=|&&|\|\||[+*/%&|^\-]=|##|\S)");
    std::vector<std::wstring> tokens;
    bool continuedDirective = false;
    for (size_t start = 0; start < code.size();) {
        const auto end = code.find(L'\n', start);
        const auto line = trim(std::wstring_view(code).substr(start, end == code.npos ? end : end - start));
        start = end == code.npos ? code.size() : end + 1;
        if (continuedDirective || line.starts_with(L'#')) {
            tokens.push_back(L"directive:" + std::wstring(line));
            continuedDirective = line.ends_with(L'\\');
            continue;
        }
        for (size_t i = 0; i < line.size();) {
            if (iswspace(line[i])) { ++i; continue; }
            if (line[i] == L'"' || line[i] == L'\'') {
                const auto begin = i;
                const auto delimiter = line[i++];
                bool escape = false;
                while (i < line.size()) {
                    const auto c = line[i++];
                    if (escape) escape = false;
                    else if (c == L'\\') escape = true;
                    else if (c == delimiter) break;
                }
                tokens.emplace_back(line.substr(begin, i - begin));
                continue;
            }
            std::match_results<std::wstring_view::const_iterator> matched;
            if (std::regex_search(line.begin() + i, line.end(), matched, token, std::regex_constants::match_continuous)) {
                tokens.push_back(matched.str());
                i += matched.length();
            } else {
                tokens.emplace_back(line.substr(i++, 1));
            }
        }
    }
    return tokens;
}

std::set<std::wstring> scripts(std::wstring_view text)
{
    const auto length = static_cast<int>(text.size());
    const auto size = GetStringScripts(GSS_ALLOW_INHERITED_COMMON, text.data(), length, nullptr, 0);
    if (size <= 0) return {};
    std::wstring buffer(size, L'\0');
    const auto written = GetStringScripts(GSS_ALLOW_INHERITED_COMMON, text.data(), length, buffer.data(), size);
    if (written <= 0) return {};
    std::set<std::wstring> result;
    auto value = std::wstring_view(buffer.data(), static_cast<size_t>(written - 1));
    for (size_t at; !value.empty() && (at = value.find(L';')) != value.npos;) {
        result.emplace(value.substr(0, at));
        value.remove_prefix(at + 1);
    }
    return result;
}

}

hstring pdfTranslationIssue(JsonObject const& block, hstring const& translation, hstring const& target)
{
    if (trim(translation).empty()) return L"服务未返回译文。";
    if (translation.size() > 16000) return L"当前段落译文过长。";
    const auto original = block.GetNamedString(L"source", L"");
    const std::wstring_view source(original), result(translation);
    if (std::wstring_view(target).find(L"中文") != std::wstring_view::npos || target == L"英语") {
        auto allowed = scripts(source);
        for (const auto name : {L"Zyyy", L"Zinh", L"Latn", L"Grek", L"Hani", L"Bopo"}) allowed.insert(name);
        for (const auto& script : scripts(result))
            if (!allowed.contains(script)) return L"服务返回的译文混入其他文字，请重试当前段落。";
    }
    for (size_t i = 0; i < result.size(); ++i) {
        const auto letter = result[i];
        if (letter >= 0xd800 && letter <= 0xdbff && i + 1 < result.size() &&
            result[i + 1] >= 0xdc00 && result[i + 1] <= 0xdfff) {
            ++i;
            continue;
        }
        WORD type{};
        const bool known = GetStringTypeW(CT_CTYPE1, result.data() + i, 1, &type) && (type & C1_DEFINED);
        const bool invalid = !known || (type & C1_CNTRL && !(type & C1_SPACE)) ||
            letter == 0xfffd || (letter >= 0xd800 && letter <= 0xdfff);
        if (invalid && source.find(letter) == source.npos) return L"服务返回的译文含异常字符，请重试当前段落。";
    }
    const std::wstring formatted(result);
    static const std::wregex markers(LR"(<\|(?:fim_[a-z_]+|im_start|im_end|endoftext|end|start)\|>|\bThe_END\b|%timeout\b|[♀♂]{3,})");
    for (auto it = std::wsregex_iterator(formatted.begin(), formatted.end(), markers); it != std::wsregex_iterator(); ++it)
        if (source.find(it->str()) == source.npos) return L"服务返回的译文混入控制标记或重复符号，请重试当前段落。";
    if (source.find(L"】【") == source.npos) {
        for (auto at = result.find(L"】【"); at != result.npos; at = result.find(L"】【", at + 2)) {
            const auto before = result.substr(0, at), after = result.substr(at + 2);
            const auto left = before.rfind(L'【'), right = before.rfind(L'】');
            const auto close = after.find(L'】'), open = after.find(L'【');
            if (left == before.npos || (right != before.npos && left <= right) || close == after.npos || open < close)
                return L"服务返回的译文含异常结尾标记，请重试当前段落。";
        }
    }
    return pdfLayoutIssue(block, translation);
}

hstring pdfLayoutIssue(JsonObject const& block, hstring const& translation)
{
    const auto original = block.GetNamedString(L"source", L"");
    const std::wstring_view source(original), result(translation);
    if (block.GetNamedBoolean(L"code", false) && codeTokens(source) != codeTokens(result))
        return L"代码中的标识符、常量或语法发生变化，请核对；仅允许翻译注释。";
    const auto role = block.GetNamedString(L"role", L"");
    if (!block.GetNamedBoolean(L"numbersConfirmed", false) && (role == L"cell" || role == L"index") &&
        numericTokens(source, role == L"index") != numericTokens(result, role == L"index"))
        return L"译文数字与原文不一致，请核对后确认。";
    return {};
}

}
