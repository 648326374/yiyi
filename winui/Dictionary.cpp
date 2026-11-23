#include "Dictionary.h"

#include <windows.h>
#undef GetObject
#include <winrt/Windows.Data.Text.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace yiyi {
namespace {

using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Data::Text;
using std::wstring_view;

constexpr wstring_view whitespace = L" \t\r\n\f\v\u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005"
    L"\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000";
constexpr uint32_t maximumItems = 12;
constexpr size_t maximumText = 4000;

wstring_view trim(wstring_view text)
{
    auto const first = text.find_first_not_of(whitespace);
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

wstring_view dictionaryContent(wstring_view text)
{
    auto content = trim(text);
    if (content.starts_with(L'\xfeff')) content = trim(content.substr(1));
    if (content.starts_with(L"```")) {
        auto const newline = content.find(L'\n');
        if (newline == content.npos) return {};
        content = trim(content.substr(newline + 1));
        if (content.ends_with(L"```")) content.remove_suffix(3);
    }
    return trim(content);
}

bool equalsIgnoreCase(wstring_view a, wstring_view b)
{
    return a.size() == b.size() && (a.empty()
        || CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL);
}

JsonObject objectValue(IJsonValue const& value)
{
    return value && value.ValueType() == JsonValueType::Object ? value.GetObject() : JsonObject{nullptr};
}

hstring stringValue(IJsonValue const& value)
{
    return value && value.ValueType() == JsonValueType::String ? value.GetString() : hstring{};
}

IJsonValue field(JsonObject const& object, hstring const& name)
{
    return object && object.HasKey(name) ? object.Lookup(name) : IJsonValue{nullptr};
}

hstring textField(JsonObject const& object, hstring const& name)
{
    return stringValue(field(object, name));
}

JsonArray arrayField(JsonObject const& object, hstring const& name)
{
    auto const value = field(object, name);
    return value && value.ValueType() == JsonValueType::Array ? value.GetArray() : JsonArray{nullptr};
}

void setText(JsonObject const& object, hstring const& name, wstring_view text)
{
    object.Insert(name, JsonValue::CreateStringValue(hstring{text}));
}

JsonObject normalizeFull(JsonObject const& object, hstring const& term)
{
    if (!arrayField(object, L"definitions")) return nullptr;
    auto const word = textField(object, L"word");
    if (trim(word).empty() && term.empty()) return nullptr;

    JsonObject normalized;
    setText(normalized, L"word", trim(word).empty() ? wstring_view{term} : trim(word).substr(0, maximumText));
    for (auto const key : {L"phonetic", L"partOfSpeech", L"note"}) {
        setText(normalized, key, trim(textField(object, key)).substr(0, maximumText));
    }

    struct Fields { wchar_t const* name; wchar_t const* first; wchar_t const* second; };
    static constexpr Fields fields[] = {
        {L"definitions", L"meaning", L"detail"}, {L"examples", L"source", L"translation"},
        {L"collocations", L"phrase", L"meaning"},
    };
    for (auto const& [name, first, second] : fields) {
        JsonArray list;
        auto const isDefinition = wstring_view{name} == L"definitions";
        if (auto const values = arrayField(object, name)) {
            for (auto const& value : values) {
                auto const item = objectValue(value);
                auto const principal = item ? textField(item, first) : (isDefinition ? stringValue(value) : hstring{});
                if (trim(principal).empty()) continue;

                JsonObject normalizedItem;
                setText(normalizedItem, first, trim(principal).substr(0, maximumText));
                setText(normalizedItem, second, trim(textField(item, second)).substr(0, maximumText));
                if (auto const pos = field(item, L"partOfSpeech"); isDefinition && pos && pos.ValueType() == JsonValueType::String) {
                    setText(normalizedItem, L"partOfSpeech", trim(pos.GetString()).substr(0, 100));
                }
                list.Append(normalizedItem);
                if (list.Size() == maximumItems) break;
            }
        }
        if (isDefinition && list.Size() == 0) return nullptr;
        normalized.Insert(name, list);
    }

    JsonArray synonyms;
    if (auto const values = arrayField(object, L"synonyms")) {
        for (auto const& value : values) {
            auto const text = stringValue(value);
            if (text.empty()) continue;
            synonyms.Append(JsonValue::CreateStringValue(hstring{wstring_view{text}.substr(0, 100)}));
            if (synonyms.Size() == maximumItems) break;
        }
    }
    normalized.Insert(L"synonyms", synonyms);
    return normalized;
}

JsonObject completedPrefix(wstring_view content, hstring const& term)
{
    if (!content.starts_with(L'{')) return nullptr;
    struct Snapshot { size_t end; std::wstring closing; };
    std::vector<Snapshot> snapshots;
    std::wstring expected;
    bool inString = false;
    bool escaped = false;
    for (size_t i = 0; i < content.size(); ++i) {
        auto const ch = content[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (ch == L'\\') escaped = true;
            else if (ch == L'"') inString = false;
        } else if (ch == L'"') {
            inString = true;
        } else if (ch == L'{' || ch == L'[') {
            if (expected.size() == 64) break;
            expected.push_back(ch == L'{' ? L'}' : L']');
        } else if (ch == L'}' || ch == L']') {
            if (expected.empty() || expected.back() != ch) break;
            expected.pop_back();
            if (expected.size() <= 2) {
                // Preserve early complete entries and the latest snapshot.
                if (snapshots.size() == 64) snapshots.pop_back();
                snapshots.push_back({i + 1, {expected.rbegin(), expected.rend()}});
            }
            if (expected.empty()) break;
        }
    }

    for (auto it = snapshots.rbegin(); it != snapshots.rend(); ++it) {
        auto const prefix = std::wstring{content.substr(0, it->end)} + it->closing;
        JsonObject object{nullptr};
        if (JsonObject::TryParse(hstring{prefix}, object)) {
            if (auto const entry = normalizeFull(object, term)) return entry;
        }
    }
    return nullptr;
}

JsonObject parseFull(wstring_view content, hstring const& term)
{
    if (!content.starts_with(L'{')) return nullptr;
    JsonObject object{nullptr};
    if (JsonObject::TryParse(hstring{content}, object)) return normalizeFull(object, term);
    return completedPrefix(content, term);
}

struct PosPrefix { wstring_view pos; wstring_view rest; };

PosPrefix posPrefix(wstring_view text)
{
    struct Alias { wstring_view token; wstring_view pos; };
    static constexpr Alias aliases[] = {
        {L"noun", L"n."}, {L"verb", L"v."}, {L"adjective", L"adj."}, {L"adverb", L"adv."},
        {L"pronoun", L"pron."}, {L"preposition", L"prep."}, {L"conjunction", L"conj."}, {L"interjection", L"interj."},
        {L"numeral", L"num."}, {L"article", L"art."}, {L"determiner", L"det."},
        {L"vt", L"vt."}, {L"vi", L"vi."}, {L"adj", L"adj."}, {L"adv", L"adv."}, {L"pron", L"pron."},
        {L"prep", L"prep."}, {L"conj", L"conj."}, {L"interj", L"interj."}, {L"num", L"num."},
        {L"art", L"art."}, {L"det", L"det."}, {L"aux", L"aux."}, {L"n", L"n."}, {L"v", L"v."},
        {L"不及物动词", L"vi."}, {L"及物动词", L"vt."}, {L"形容词", L"adj."}, {L"副词", L"adv."},
        {L"代词", L"pron."}, {L"介词", L"prep."}, {L"连词", L"conj."}, {L"叹词", L"interj."},
        {L"数词", L"num."}, {L"冠词", L"art."}, {L"限定词", L"det."}, {L"助动词", L"aux."},
        {L"名词", L"n."}, {L"动词", L"v."},
    };
    auto const delimiter = [](wchar_t ch) {
        return whitespace.find(ch) != whitespace.npos || wstring_view{L".。:：,，/、"}.find(ch) != wstring_view::npos;
    };
    for (auto const& [token, normalized] : aliases) {
        if (!equalsIgnoreCase(text.substr(0, token.size()), token)) continue;
        auto rest = text.substr(token.size());
        if (!rest.empty() && !delimiter(rest.front())) continue;
        while (!rest.empty() && delimiter(rest.front())) rest.remove_prefix(1);
        return {normalized, rest};
    }
    return {};
}

wstring_view normalizePos(wstring_view text)
{
    auto const first = posPrefix(trim(text));
    auto const next = posPrefix(first.rest);
    return !next.pos.empty() && next.pos != first.pos ? wstring_view{} : first.pos;
}

enum class Label { None, Pos, Meaning, Ignored };

Label labelKind(wstring_view label)
{
    static constexpr wstring_view pos[] = {L"词性", L"pos", L"part of speech"};
    static constexpr wstring_view meaning[] = {L"释义", L"词义", L"意思", L"meaning", L"definition", L"translation"};
    static constexpr wstring_view ignore[] = {L"音标", L"ipa", L"phonetic", L"例句", L"example", L"译文", L"note", L"说明"};
    auto const matches = [label](wstring_view candidate) { return equalsIgnoreCase(label, candidate); };
    if (std::any_of(std::begin(pos), std::end(pos), matches)) return Label::Pos;
    if (std::any_of(std::begin(meaning), std::end(meaning), matches)) return Label::Meaning;
    if (std::any_of(std::begin(ignore), std::end(ignore), matches)) return Label::Ignored;
    return Label::None;
}

JsonObject briefEntry(hstring const& term, JsonArray const& definitions)
{
    if (definitions.Size() == 0) return nullptr;
    JsonObject result;
    setText(result, L"word", term);
    setText(result, L"format", L"brief");
    result.Insert(L"definitions", definitions);
    return result;
}

void addBriefMeaning(JsonArray const& list, wstring_view meaning, wstring_view pos)
{
    JsonObject definition;
    setText(definition, L"meaning", meaning);
    if (!pos.empty()) setText(definition, L"partOfSpeech", pos);
    list.Append(definition);
}

JsonObject parseBrief(wstring_view content, hstring const& term)
{
    JsonArray definitions;
    if (content.starts_with(L'{') || content.starts_with(L'[')) {
        auto const object = parseFull(content, term);
        if (!object) return nullptr;
        auto const commonPos = textField(object, L"partOfSpeech");
        for (auto const& value : arrayField(object, L"definitions")) {
            auto const definition = objectValue(value);
            auto const pos = textField(definition, L"partOfSpeech");
            addBriefMeaning(definitions, textField(definition, L"meaning"), normalizePos(pos.empty() ? commonPos : pos));
        }
        return briefEntry(term, definitions);
    }

    auto const headword = trim(term);
    std::vector<std::pair<std::wstring, wstring_view>> known;
    wstring_view currentPos;
    bool ignoreContinuation = false;
    while (!content.empty() && definitions.Size() < maximumItems) {
        auto const newline = content.find_first_of(L"\r\n");
        auto const sourceLine = trim(content.substr(0, newline));
        content.remove_prefix(newline == content.npos ? content.size() : newline + 1);
        if (sourceLine.empty() || sourceLine.starts_with(L"```")) continue;
        std::wstring cleanLine;
        for (size_t i = 0; i < sourceLine.size(); ++i) {
            if (sourceLine[i] == L'*' && i + 1 < sourceLine.size() && sourceLine[i + 1] == L'*') ++i;
            else cleanLine.push_back(sourceLine[i]);
        }
        auto line = trim(cleanLine);
        if ((line.starts_with(L'/') && line.ends_with(L'/')) || equalsIgnoreCase(line, headword)) continue;

        auto const colon = line.find_first_of(L":：");
        auto const label = colon == line.npos ? Label::None : labelKind(trim(line.substr(0, colon)));
        if (label != Label::None) {
            line = trim(line.substr(colon + 1));
            ignoreContinuation = label == Label::Ignored;
            if (label == Label::Pos) {
                currentPos = normalizePos(line);
                continue;
            }
            if (ignoreContinuation) continue;
        } else {
            auto const found = posPrefix(line);
            if (!found.pos.empty()) {
                currentPos = normalizePos(line);
                line = trim(found.rest);
                ignoreContinuation = false;
            } else if (ignoreContinuation) {
                continue;
            }
        }

        // Drop exact echoed headwords only, so language validation still sees stray text.
        if (!headword.empty() && equalsIgnoreCase(line.substr(0, headword.size()), headword)) {
            auto const suffix = trim(line.substr(headword.size()));
            if (suffix.starts_with(L':') || suffix.starts_with(L'：')) line = trim(suffix.substr(1));
        }
        while (!line.empty() && definitions.Size() < maximumItems) {
            auto const separator = line.find_first_of(L";；");
            auto const meaning = trim(line.substr(0, separator)).substr(0, maximumText);
            line.remove_prefix(separator == line.npos ? line.size() : separator + 1);
            if (meaning.empty()) continue;
            auto const duplicate = std::any_of(known.begin(), known.end(), [&](auto const& entry) {
                return entry.first == meaning && entry.second == currentPos;
            });
            if (!duplicate) {
                known.emplace_back(meaning, currentPos);
                addBriefMeaning(definitions, meaning, currentPos);
            }
        }
    }
    return briefEntry(term, definitions);
}

std::wstring unicodeScripts(wstring_view text)
{
    if (text.empty()) return {};
    auto const length = static_cast<int>(text.size());
    auto const capacity = GetStringScripts(GSS_ALLOW_INHERITED_COMMON, text.data(), length, nullptr, 0);
    if (capacity == 0) return {};
    std::wstring scripts(capacity, L'\0');
    auto const written = GetStringScripts(GSS_ALLOW_INHERITED_COMMON, text.data(), length, scripts.data(), capacity);
    scripts.resize(written > 0 ? written - 1 : 0);
    return scripts;
}

bool isEnglishMeaning(wstring_view meaning)
{
    std::wstring letters;
    for (size_t i = 0; i < meaning.size();) {
        auto codepoint = static_cast<uint32_t>(meaning[i]);
        size_t units = 1;
        if (codepoint >= 0xd800 && codepoint <= 0xdbff && i + 1 < meaning.size()
            && meaning[i + 1] >= 0xdc00 && meaning[i + 1] <= 0xdfff) {
            codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (meaning[i + 1] - 0xdc00);
            units = 2;
        }
        if (UnicodeCharacters::GetGeneralCategory(codepoint) <= UnicodeGeneralCategory::OtherLetter) {
            letters.append(meaning.substr(i, units));
        }
        i += units;
    }
    return unicodeScripts(letters) == L"Latn;";
}

bool validMeanings(JsonObject const& dictionary, hstring const& target)
{
    auto const definitions = arrayField(dictionary, L"definitions");
    if (!definitions || definitions.Size() == 0) return false;
    auto const chinese = target == L"简体中文" || target == L"繁體中文";
    auto const english = target == L"英语";
    if (!chinese && !english) return true;
    for (auto const& value : definitions) {
        auto const meaning = textField(objectValue(value), L"meaning");
        if (chinese && unicodeScripts(meaning).find(L"Hani;") == std::wstring::npos) return false;
        if (english && !isEnglishMeaning(meaning)) return false;
    }
    return true;
}

}

JsonObject parseDictionaryResponse(hstring const& result, hstring const& term, hstring const& target, bool detailed)
{
    auto const content = dictionaryContent(result);
    auto entry = detailed ? parseFull(content, term) : JsonObject{nullptr};
    if (!entry) entry = parseBrief(content, term);
    return validMeanings(entry, target) ? entry : nullptr;
}

JsonObject parseDictionaryPreview(hstring const& cumulative, hstring const& term, hstring const& target)
{
    auto const entry = completedPrefix(dictionaryContent(cumulative), term);
    return validMeanings(entry, target) ? entry : nullptr;
}

bool isBriefDictionary(JsonObject const& entry)
{
    if (!entry) return false;
    if (entry.HasKey(L"format")) return textField(entry, L"format") == L"brief";
    return entry.HasKey(L"word") && entry.HasKey(L"definitions") && !entry.HasKey(L"phonetic") && !entry.HasKey(L"examples");
}

hstring formatDictionary(JsonObject const& entry)
{
    std::wstring result = isBriefDictionary(entry) ? std::wstring{} : std::wstring{textField(entry, L"word")};
    auto const append = [&](wstring_view prefix, hstring const& value) {
        if (!value.empty()) result.append(prefix).append(value);
    };
    append(L"  ", textField(entry, L"phonetic"));
    append(L"\n", textField(entry, L"partOfSpeech"));
    if (auto const definitions = arrayField(entry, L"definitions")) {
        for (auto const& value : definitions) {
            auto const item = objectValue(value);
            result += L"\n\n";
            auto const pos = textField(item, L"partOfSpeech");
            if (!pos.empty()) result.append(pos).append(L" ");
            result.append(item ? textField(item, L"meaning") : stringValue(value));
            append(L"\n", textField(item, L"detail"));
        }
    }
    if (auto const examples = arrayField(entry, L"examples")) {
        for (auto const& value : examples) {
            auto const item = objectValue(value);
            result.append(L"\n\n").append(textField(item, L"source")).append(L"\n").append(textField(item, L"translation"));
        }
    }
    if (auto const collocations = arrayField(entry, L"collocations")) {
        for (auto const& value : collocations) {
            auto const item = objectValue(value);
            result.append(L"\n\n").append(textField(item, L"phrase")).append(L" · ").append(textField(item, L"meaning"));
        }
    }
    if (auto const synonyms = arrayField(entry, L"synonyms")) {
        bool first = true;
        for (auto const& value : synonyms) {
            auto const text = stringValue(value);
            if (text.empty()) continue;
            result.append(first ? L"\n\n近义词：" : L", ").append(text);
            first = false;
        }
    }
    append(L"\n\n", textField(entry, L"note"));
    return hstring{trim(result)};
}

}
