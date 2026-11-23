#include "DocumentTask.h"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {

using yiyi::DocumentEncoding;
using yiyi::DocumentState;

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

std::string read(std::filesystem::path const& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read test file");
    return {std::istreambuf_iterator<char>(stream), {}};
}

void encodingAndSplits()
{
    winrt::hstring encoding;
    const auto decode = [&](std::string_view bytes, DocumentEncoding selected = DocumentEncoding::Auto) {
        return yiyi::decodeDocumentText(bytes, selected, encoding);
    };
    check(decode("\xef\xbb\xbf" "hello") == L"hello" && encoding == L"UTF-8", "UTF-8 BOM");
    check(decode("\xff\xfe\x60\x4f") == L"你" && encoding == L"UTF-16 LE", "UTF-16 LE BOM");
    check(decode("\xfe\xff\x4f\x60") == L"你" && encoding == L"UTF-16 BE", "UTF-16 BE BOM");
    check(decode("\x60\x4f", DocumentEncoding::Utf16Le) == L"你", "UTF-16 LE without BOM");
    check(decode("\xc4\xe3\xba\xc3", DocumentEncoding::Gb18030) == L"你好" && encoding == L"GB18030 / GBK", "GB18030/GBK");
    rejected([&] { decode("\xc4\xe3\xba\xc3"); }, "Auto accepted non-UTF-8");
    rejected([&] { decode(std::string_view("\xff\xfe\0\0", 4)); }, "UTF-32 BOM was accepted");
    rejected([&] { decode("\xff\xfe\x60"); }, "Truncated UTF-16 was accepted");
    rejected([&] { decode(std::string_view("\xff\xfe\0\xd8", 4)); }, "Unpaired surrogate was accepted");
    rejected([&] { decode(std::string_view("text\0data", 9)); }, "Binary content was accepted");

    const std::wstring mixed = L" \t" + std::wstring(15, L'x') + L"🎋y!\n\n ";
    const auto parts = yiyi::splitDocumentText(mixed, 16);
    std::wstring reconstructed;
    for (auto const& part : parts) {
        reconstructed += std::wstring_view(part.text);
        if (part.translate) check(part.text.size() <= 16, "Oversized chunk");
    }
    check(reconstructed == mixed && parts.size() == 4 && parts[1].text.size() == 15
        && parts[2].text == L"🎋y!" && parts[2].continuation, "Surrogate, literal indentation or continuation lost");
}

void workflow(std::filesystem::path const& source, winrt::hstring const& server)
{
    using namespace std::chrono_literals;
    const auto original = read(source);
    auto document = std::make_shared<yiyi::DocumentTask>();
    document->load(source);
    auto state = document->snapshot();
    check(state.state == DocumentState::Ready && state.source == L"  First \n\n\tSecond\n" && state.total == 2,
          "Document layout or segment count changed on import");
    rejected([&] { document->exportTo(document->suggestedOutput()); }, "Exported without translated segments");

    yiyi::DocumentSettings settings;
    settings.provider = {L"test", L"Model", L"openai", server, L"test-model", L"private-test-secret"};
    settings.sourceLanguage = L"英语";
    document->start(settings).get();
    state = document->snapshot();
    check(state.state == DocumentState::Failed && state.completed == 1 && state.hasUnexportedResult &&
          std::wstring_view(state.error).find(L"第 2 / 2 段") != std::wstring_view::npos, "Failed part did not preserve progress");
    rejected([&] { document->exportTo(source); }, "Export overwrote source file");
    const auto linkedSource = source.parent_path() / L"hardlink.txt";
    std::filesystem::create_hard_link(source, linkedSource);
    rejected([&] { document->exportTo(linkedSource); }, "Export overwrote a source hardlink");
    const auto partial = document->suggestedOutput();
    check(partial.filename() == L"input.partial.txt", "Wrong partial output suggestion");
    document->exportTo(partial);
    check(read(partial) == "  第一 \r\n\r\n\t" && !document->snapshot().hasUnexportedResult, "Partial UTF-8 / CRLF export failed");

    std::mutex mutex;
    std::condition_variable arrived;
    bool gotPreview = false;
    auto resumed = document->resume({{}, [&](uint64_t generation, size_t segment, winrt::hstring const& preview) {
        std::lock_guard lock(mutex);
        gotPreview = generation == document->snapshot().generation && segment == 2 && preview == L"临时内容";
        arrived.notify_all();
    }});
    {
        std::unique_lock lock(mutex);
        check(arrived.wait_for(lock, 5s, [&] { return gotPreview; }), "Missing current segment preview");
    }
    document->cancel();
    resumed.get();
    state = document->snapshot();
    check(state.state == DocumentState::Paused && state.completed == 1 && state.result == L"  第一 \n\n\t",
          "Cancelled preview was committed");

    document->resume().get();
    state = document->snapshot();
    check(state.state == DocumentState::Completed && state.completed == 2 && state.hasUnexportedResult
          && state.taskDescription == L"Model · 英语 → 简体中文 · 自然", "Resume lost completed segments or task snapshot");
    const auto complete = document->suggestedOutput();
    check(complete.filename() == L"input.translated.txt", "Wrong complete output suggestion");
    document->exportTo(complete);
    check(read(complete) == "  第一 \r\n\r\n\t第二\r\n" && read(source) == original, "Full export changed layout or source file");
    document->reset();
    state = document->snapshot();
    check(state.state == DocumentState::Ready && !state.hasSnapshot && state.completed == 0 && state.result.empty(),
          "Reset did not release progress and model settings");
}

}

int wmain(int argc, wchar_t const* argv[])
{
    if (argc != 3) return 2;
    try {
        winrt::init_apartment();
        encodingAndSplits();
        workflow(argv[1], argv[2]);
        std::cout << "TXT encoding, splitting, pause, resume and export passed.\n";
        return 0;
    } catch (winrt::hresult_error const& error) {
        std::cerr << winrt::to_string(error.message()) << '\n';
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
