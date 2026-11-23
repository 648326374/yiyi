#include "Dictionary.h"

#include <winrt/Windows.Foundation.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace {

void check(bool condition, char const* message)
{
    if (!condition) throw std::runtime_error(message);
}

void checkBrief()
{
    struct Case {
        wchar_t const* input;
        wchar_t const* term;
        wchar_t const* target;
        wchar_t const* expected;
    };
    constexpr Case cases[] = {
        {L"look\n词性：v. 动词\n释义：看；注视\n音标：/lʊk/\n例句：Look at me.\n译文：看着我。", L"look", L"简体中文", L"v. 看\n\nv. 注视"},
        {L"n. 书籍\nv. 预订", L"book", L"繁體中文", L"n. 书籍\n\nv. 预订"},
        {L"词性：副词\n释义：迅速地", L"quickly", L"简体中文", L"adv. 迅速地"},
        {L"POS: adjective\nTRANSLATION: 有趣的：interesting; interesting", L"有趣的", L"英语", L"adj. interesting"},
        {L"POS: n./v./adj./adv.等实际词性\nTRANSLATION: book", L"书", L"英语", L"book"},
        {L"词性：light.\n释义：光", L"light", L"简体中文", L"光"},
        {L"POS: n.\nTRANSLATION: café", L"咖啡馆", L"英语", L"n. café"},
        {L"US; us\nn. study\nv. study", L"研究", L"英语", L"US\n\nus\n\nn. study\n\nv. study"},
        {L"C++: 一种编程语言", L"C++", L"简体中文", L"一种编程语言"},
        {L"POS: adv.\nNOTE: no translation\nwrapped continuation", L"快速地", L"英语", nullptr},
        {L"TRANSLATION: funny（有趣的）", L"有趣的", L"英语", nullptr},
        {L"有趣的: funny\n幽默的/滑稽的", L"有趣的", L"英语", nullptr},
        {L"book книга", L"书", L"英语", nullptr},
        {L"light", L"光", L"简体中文", nullptr},
        {L"100%", L"比例", L"英语", nullptr},
        {L"𠀀", L"han", L"简体中文", L"𠀀"},
        {L"こんにちは", L"hello", L"日语", L"こんにちは"},
    };
    for (auto const& entry : cases) {
        auto const parsed = yiyi::parseDictionaryResponse(entry.input, entry.term, entry.target, false);
        check(static_cast<bool>(parsed) == (entry.expected != nullptr), "Wrong brief validation result");
        if (entry.expected) {
            check(yiyi::isBriefDictionary(parsed), "Lost brief dictionary format");
            check(yiyi::formatDictionary(parsed) == entry.expected, "Wrong brief dictionary display");
        }
    }
}

void checkDetailed(std::filesystem::path const& fixture)
{
    std::ifstream stream{fixture, std::ios::binary};
    check(static_cast<bool>(stream), "Cannot open legacy malformed optional field fixture");
    auto const malformed = winrt::to_hstring(std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}});
    auto const detailed = yiyi::parseDictionaryResponse(malformed, L"hello", L"简体中文", true);
    check(detailed && !yiyi::isBriefDictionary(detailed) && detailed.GetNamedString(L"note").empty(), "Malformed note lost complete fields");
    auto const display = yiyi::formatDictionary(detailed);
    check(std::wstring_view{display}.starts_with(L"hello  /həˈləʊ/\ninterjection · 感叹词\n\n你好；喂")
        && std::wstring_view{display}.find(L"say hello · 打招呼\n\n近义词：hi, hey") != std::wstring_view::npos,
        "Detailed dictionary display omitted fields");
    auto const brief = yiyi::parseDictionaryResponse(malformed, L"hello", L"简体中文", false);
    check(brief && yiyi::formatDictionary(brief) == L"interj. 你好；喂", "Structured reply did not support brief display");

    auto const fallback = yiyi::parseDictionaryResponse(L"interj. 你好；喂", L"hello", L"简体中文", true);
    check(fallback && yiyi::isBriefDictionary(fallback), "Detailed plain-text fallback failed");
    auto const missingHeadword = yiyi::parseDictionaryResponse(LR"({"definitions":[3,{"meaning":"你好"},"喂"]})", L"hello", L"简体中文", true);
    check(missingHeadword && missingHeadword.GetNamedString(L"word") == L"hello", "Meaning normalization failed");
    check(!yiyi::parseDictionaryResponse(LR"({"definitions":[{"meaning" "坏结构"}]})", L"hello", L"简体中文", true), "Malformed definition passed");
    check(!yiyi::parseDictionaryResponse(LR"({"definitions":["greeting"]})", L"hello", L"简体中文", true), "Wrong-language structure passed");

    auto const prefix = LR"({"word":"relationship","partOfSpeech":"n.","definitions":[{"meaning":"关系；联系"})";
    auto const preview = yiyi::parseDictionaryPreview(prefix, L"relationship", L"简体中文");
    check(preview && std::wstring_view{yiyi::formatDictionary(preview)}.ends_with(L"关系；联系"), "Complete definition preview failed");
    check(!yiyi::parseDictionaryPreview(L"{\"definitions\":[{\"meaning\":\"未完成\\u59", L"book", L"简体中文"), "Unfinished escape entered preview");
    check(!yiyi::parseDictionaryPreview(LR"({"definitions":[{"meaning":"book"})", L"书", L"简体中文"), "Wrong-language preview passed");
    auto const fenced = yiyi::parseDictionaryResponse(L"\xfeff```json\n{\"word\":\"book\",\"definitions\":[\"书\"]}\n```", L"book", L"简体中文", true);
    check(fenced && yiyi::formatDictionary(fenced) == L"book\n\n书", "Fenced output failed");
    auto const oldBrief = winrt::Windows::Data::Json::JsonObject::Parse(LR"({"word":"book","definitions":[{"meaning":"书"}]})");
    check(yiyi::isBriefDictionary(oldBrief) && yiyi::formatDictionary(oldBrief) == L"书", "Legacy brief record display changed");
}

}

int wmain(int argc, wchar_t* argv[])
{
    try {
        check(argc == 2, "Specify legacy malformed optional field fixture");
        winrt::init_apartment();
        checkBrief();
        checkDetailed(argv[1]);
        std::cout << "Dictionary brief and detailed formats, preview and script validation passed.\n";
    } catch (winrt::hresult_error const& error) {
        std::cerr << winrt::to_string(error.message()) << '\n';
        return 1;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
