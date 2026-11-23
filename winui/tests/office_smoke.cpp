#include "OfficeDocumentNative.h"

#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using yiyi::OfficeDocumentNative;

void check(bool condition, char const* message)
{
    if (!condition) throw std::runtime_error(message);
}

template <typename Work>
void rejected(Work&& work, char const* message)
{
    try {
        work();
    } catch (winrt::hresult_error const&) {
        return;
    }
    throw std::runtime_error(message);
}

bool hasText(OfficeDocumentNative const& doc, std::wstring_view text)
{
    return std::any_of(doc.segments().begin(), doc.segments().end(), [&](yiyi::OfficeSegment const& segment) {
        return std::wstring_view(segment.source) == text;
    });
}

void roundTrip(std::filesystem::path const& source, std::filesystem::path const& destination)
{
    const auto document = OfficeDocumentNative::load(source);
    const bool word = source.extension() == L".DOCX";
    check(document.kind() == (word ? L"docx" : L"pptx") && document.pageCount() == (word ? 1 : 2), "Package type and slide order");
    check(document.segments().size() > 6 && !document.images().empty() && document.hash().size() == 64, "Missing segments, media, or SHA-256");
    check(document.imageBytes(document.images().front().id).starts_with("\x89PNG\r\n"), "Embedded image extraction changed the PNG");
    rejected([&] { document.exportTo(source, {}); }, "Would overwrite the source");
    std::filesystem::path link = destination.parent_path() / (word ? L"hardlink.docx" : L"hardlink.pptx");
    std::filesystem::create_hard_link(source, link);
    rejected([&] { document.exportTo(link, {}); }, "Would overwrite a source hardlink");
    const auto missing = destination.parent_path() / L"missing" / destination.filename();
    rejected([&] { document.exportTo(missing, {}); }, "Would write to nonexistent location");
    if (word) {
        check(hasText(document, L"Internal report") && hasText(document, L"A useful footnote.")
            && hasText(document, L"A useful endnote."), "Header, footnote, or endnote missing");
        check(!hasText(document, L"Before tabAfter tabAfter break"), "Explicit separator was erased");
    } else {
        check(hasText(document, L"Quarterly performance"), "Missing chart title");
    }
    yiyi::OfficeTranslations translations;
    for (const auto& segment : document.segments())
        translations.emplace(segment.id, winrt::hstring(L"译文 🎋：" + std::wstring(segment.source)));
    document.exportTo(destination, translations, {false, 12});
    const auto exported = OfficeDocumentNative::load(destination);
    check(exported.segments().size() == document.segments().size(), "Export changed number of segments");
    for (size_t i = 0; i < document.segments().size(); ++i) {
        check(exported.segments()[i].id == document.segments()[i].id, "Office task ID changed after export");
        check(std::wstring_view(exported.segments()[i].source).find(L"译文 🎋：") != std::wstring_view::npos,
              "Editable translated text or surrogate was lost");
    }
}

}

int wmain(int argc, wchar_t const* argv[])
{
    if (argc != 2) return 2;
    try {
        winrt::init_apartment();
        const std::filesystem::path directory(argv[1]);
        roundTrip(directory / L"原稿 #100%.DOCX", directory / L"译文 #100%.docx");
        roundTrip(directory / L"原稿 #100%.PPTX", directory / L"译文 #100%.pptx");
        const auto fields = OfficeDocumentNative::load(directory / L"fields.docx");
        check(hasText(fields, L"Normal paragraph") && !hasText(fields, L"Generated contents") && !hasText(fields, L"Tracked paragraph"),
              "Protected fields and revisions were exposed as translatable text");
        const auto images = OfficeDocumentNative::load(directory / L"images-only.docx");
        check(images.segments().empty() && !images.images().empty(), "Media-only Office document rejected");
        for (auto const* file : {L"broken.docx", L"spoof.pptx", L"DTD.docx", L"VBA.docx", L"unsafe-path.docx", L"signed.docx"})
            rejected([&] { OfficeDocumentNative::load(directory / file); }, "Unsafe or malformed file was accepted");

        const auto slides = OfficeDocumentNative::load(directory / L"原稿 #100%.PPTX");
        const winrt::hstring longText(std::wstring(1000, L'中'));
        const yiyi::OfficeTranslations translations{{slides.segments().front().id, longText}};
        const auto review = slides.issues(translations, {true, 14});
        check(std::any_of(review.begin(), review.end(), [](yiyi::OfficeNotice const& issue) {
            return std::wstring_view(issue.message).find(L"超出") != std::wstring_view::npos;
        }), "Long slide text did not receive an overflow notice");
        const auto fitted = directory / L"fit.pptx";
        slides.exportTo(fitted, translations, {true, 14});
        const auto exported = OfficeDocumentNative::load(fitted);
        check(exported.segments().front().source == longText, "PPT export discarded long text");
        std::cout << "DOCX/PPTX editable package and media round trip passed.\n";
        return 0;
    } catch (winrt::hresult_error const& error) {
        std::cerr << winrt::to_string(error.message()) << '\n';
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
