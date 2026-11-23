#include "OfficeTask.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <stdexcept>

namespace {

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

void cacheAndTranslation(std::filesystem::path const& directory, winrt::hstring const& base,
                         std::filesystem::path const& renderer)
{
    using namespace std::chrono_literals;
    const auto source = directory / L"sample.docx";
    auto task = std::make_shared<yiyi::OfficeTask>(directory / L"profile");
    task->load(source);
    auto state = task->snapshot();
    check(state.completed == 1 && state.draftDirty && state.settings && state.settings->sourceLanguage == L"英语"
        && state.settings->targetLanguage == L"简体中文" && state.settings->provider.apiKey.empty(), "Legacy cache did not restore safely");
    const auto first = state.document->segments()[0].id;
    const auto third = state.document->segments()[2].id;
    check(state.progress.at(first).translation == L"旧译文" && state.draftText == L"本机校对", "Legacy progress and draft did not restore");
    const yiyi::DocumentSettings settings{{L"legacy", L"Model", L"openai", base, L"office-test", L"synthetic-key", L""},
        L"日语", L"法语", L"学术"};
    rejected([&] { task->start(settings); }, "Started with an unsaved draft");
    auto exported = task->suggestedOutput();
    task->exportTo(exported);
    state = task->snapshot();
    check(state.completed == 2 && !state.draftDirty && !state.hasUnexportedResult, "Export did not commit the saved draft");
    const auto exportedDoc = yiyi::OfficeDocumentNative::load(exported);
    check(exportedDoc.segments()[0].source == L"旧译文" && exportedDoc.segments()[1].source == L"本机校对", "Export did not include recovered edits");
    exported += L".issues.txt";
    check(std::filesystem::is_regular_file(exported), "Export omitted the issue list");
    auto preview = task->renderPreview(renderer);
    const auto previewDirectory = preview->path().parent_path();
    check(preview->path().filename() == L"preview.pdf" && std::filesystem::file_size(preview->path()) > 0
        && std::filesystem::is_regular_file(previewDirectory / L"preview.docx"), "Preview or isolated source was not created");
    preview.reset();
    check(!std::filesystem::exists(previewDirectory), "Temporary Office preview was not removed");
    rejected([&] { task->load(directory / "broken.docx"); }, "Opened a broken replacement");
    check(task->snapshot().completed == 2, "Broken replacement erased the current task");

    auto changed = settings;
    changed.provider.model = L"changed";
    rejected([&] { task->retry(third, changed); }, "Resumed a cache with changed model identity");
    std::mutex mutex;
    std::condition_variable arrived;
    bool gotPreview = false;
    auto pending = task->retry(third, settings, {{}, [&](uint64_t generation, size_t index, winrt::hstring const& value) {
        std::lock_guard lock(mutex);
        gotPreview = generation == task->snapshot().generation && index == 3 && value == L"未完成";
        arrived.notify_all();
    }});
    {
        std::unique_lock lock(mutex);
        check(arrived.wait_for(lock, 5s, [&] { return gotPreview; }), "Missing Office preview");
    }
    task->cancel();
    pending.get();
    state = task->snapshot();
    check(!state.translating && !state.progress.contains(third) && state.completed == 2, "Cancelled Office request committed partial text");
    task->retry(third, settings).get();
    state = task->snapshot();
    check(!state.translating && state.completed == 3 && state.progress.at(third).translation == L"重试成功", "Single-section retry failed");
    task->keepOriginal(third);
    task->stageDraft(first, L"下一次继续校对");
    task->flush();
    auto reopened = std::make_shared<yiyi::OfficeTask>(directory / L"profile");
    reopened->load(source);
    state = reopened->snapshot();
    check(state.completed == 3 && state.progress.at(third).kept && state.draftDirty && state.draftText == L"下一次继续校对",
          "Native cache did not persist edits and keep-original");
    reopened->discardDraft();
    reopened->reset();
    state = reopened->snapshot();
    check(state.completed == 0 && !state.settings && !state.hasUnexportedResult && !state.draftDirty, "Task reset did not clear saved progress");

    task->load(directory / L"long.docx");
    task->start(settings).get();
    state = task->snapshot();
    check(state.completed > 0 && state.completed == state.document->segments().size(), "Long Office paragraph did not complete");
}

}

int wmain(int argc, wchar_t const* argv[])
{
    if (argc != 4) return 2;
    try {
        winrt::init_apartment();
        cacheAndTranslation(argv[1], argv[2], argv[3]);
        std::cout << "Office cache, manual draft, cancellation, retry and bounded streaming passed.\n";
        return 0;
    } catch (winrt::hresult_error const& error) {
        std::cerr << winrt::to_string(error.message()) << '\n';
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
