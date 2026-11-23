#include "PdfTask.h"
#include "PdfWorkerClient.h"

#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace winrt;
using namespace Windows::Data::Json;
using namespace yiyi;

void require(bool value, char const* reason)
{
    if (!value) throw std::runtime_error(reason);
}

JsonObject block(PdfTask const& task, int page)
{
    const auto data = task.page(page);
    require(static_cast<bool>(data), "page was not saved");
    const auto blocks = data.GetNamedArray(L"blocks");
    require(blocks.Size() == 1, "unexpected number of text blocks");
    return blocks.GetObjectAt(0);
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4) return 2;
    winrt::init_apartment();
    try {
        const std::filesystem::path source = argv[1], profile = argv[2];
        const auto info = inspectPdfAsync(source).get();
        {
            const auto legacy = std::make_shared<PdfTask>(profile / L"legacy");
            legacy->load(source, info);
            legacy->analyze(0).get();
            require(block(*legacy, 0).GetNamedString(L"translation") == L"旧版第一页", "pre-SQLite task was not reconciled");
            auto originalCache = legacy->databasePath();
            require(std::filesystem::exists(originalCache.replace_extension(L".json")), "legacy cache was removed during migration");
        }
        auto task = std::make_shared<PdfTask>(profile);
        task->load(source, info);
        task->analyze(-1).get();
        if (!task->snapshot().error.empty()) throw std::runtime_error(to_string(task->snapshot().error));
        require(task->snapshot().analyzedPages == 2, "both pages should be analyzed");
        require(task->snapshot().totalBlocks == 2, "both text blocks should be available");

        DocumentSettings settings;
        settings.provider.id = L"fixture";
        settings.provider.name = L"Local fixture";
        settings.provider.protocol = L"openai";
        settings.provider.baseUrl = argv[3];
        settings.provider.model = L"fixture-model";
        settings.provider.apiKey = L"fixture-secret";
        auto first = task->start(settings, -1);
        auto snapshot = task->snapshot();
        for (int tries = 0; tries < 250 && snapshot.completedBlocks == 0 && snapshot.busy; ++tries) {
            std::this_thread::sleep_for(20ms);
            snapshot = task->snapshot();
        }
        require(snapshot.completedBlocks == 1, "first translation should be durable while the second request is in flight");
        std::this_thread::sleep_for(250ms);
        task->cancel();
        first.get();
        require(task->snapshot().state == PdfTaskState::Paused, "cancellation should pause the workflow");
        require(block(*task, 0).GetNamedString(L"translation") == L"第一页", "first page translation was lost");

        task = std::make_shared<PdfTask>(profile);
        task->load(source, info);
        require(task->snapshot().taskPending && task->snapshot().target == L"简体中文", "pending task settings were lost");
        auto wrongModel = settings.provider;
        wrongModel.model = L"other";
        try {
            task->resume(wrongModel).get();
            throw std::runtime_error("resume accepted another model");
        } catch (hresult_invalid_argument const&) {
        }

        task->resume(settings.provider).get();
        require(task->snapshot().state == PdfTaskState::Completed, "resume did not complete the task");
        require(task->snapshot().completedBlocks == 2, "resume did not translate the remaining block");
        require(block(*task, 1).GetNamedString(L"translation") == L"第二页", "incorrect resumed translation");

        const auto id = block(*task, 0).GetNamedString(L"id");
        const auto originalPage = task->page(0);
        auto tablePage = JsonObject::Parse(originalPage.Stringify());
        const auto cell = tablePage.GetNamedArray(L"blocks").GetObjectAt(0);
        cell.Insert(L"role", JsonValue::CreateStringValue(L"cell"));
        cell.Insert(L"table", JsonValue::CreateStringValue(L"context-test-table"));
        cell.Insert(L"row", JsonValue::CreateNumberValue(1));
        cell.Insert(L"column", JsonValue::CreateNumberValue(1));
        const auto table = JsonObject::Parse(LR"json({"id":"context-test-table","headerRows":0,"inheritedHeaders":[{"column":0,"columnSpan":2,"text":"Inherited column heading"}]})json");
        tablePage.GetNamedObject(L"layout").GetNamedArray(L"regions").Append(table);
        require(task->replacePageStructure(0, originalPage, tablePage), "table prompt test fixture was not saved");
        task->retryBlock(0, id, settings).get();
        require(task->snapshot().state == PdfTaskState::Failed, "invalid replacement should fail validation");
        require(block(*task, 0).GetNamedString(L"translation") == L"第一页", "invalid replacement altered committed translation");
        task->discardTask();

        auto retry = task->retryBlock(0, id, settings);
        const auto marker = source.parent_path() / L"retry-sent";
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(20ms);
        require(std::filesystem::exists(marker), "cancellation probe did not reach the local endpoint");
        std::this_thread::sleep_for(200ms);
        const auto canceledAt = std::chrono::steady_clock::now();
        task->cancel();
        retry.get();
        require(std::chrono::steady_clock::now() - canceledAt < 650ms, "cancel did not interrupt retry delay promptly");
        require(task->snapshot().state == PdfTaskState::Paused, "cancel during retry did not pause the task");
        task->discardTask();
        require(task->replacePageStructure(0, task->page(0), originalPage), "table fixture could not be restored");

        const auto before = task->page(0);
        task->editBlock(0, id, L"校对后的第一页");
        JsonObject status;
        status.Insert(L"id", JsonValue::CreateStringValue(id));
        status.Insert(L"error", JsonValue::CreateStringValue(L"当前区域排版失败"));
        JsonArray statuses;
        statuses.Append(status);
        require(!task->recordPreviewStatuses(0, statuses, before), "stale preview overwrote an edited page");
        require(task->recordPreviewStatuses(0, statuses, task->page(0)), "current preview review was not saved");
        require(task->issueSummaries().Size() == 1, "current preview issue was missing");

        task->editBlock(0, id, L"第一页");
        const auto output = source.parent_path() / L"完整译稿.pdf";
        const auto fullExport = exportPdfAsync(source, task->databasePath(), output, false).get();
        if (!fullExport.GetNamedBoolean(L"saved", false))
            throw std::runtime_error(to_string(fullExport.Stringify()));
        require(fullExport.GetNamedNumber(L"preservedPages") == 0, "intact export unexpectedly retained original blocks");

        JsonObject revision;
        revision.Insert(L"source", JsonValue::CreateStringValue(L"Edited second page"));
        task->reviseBlock(1, block(*task, 1).GetNamedString(L"id"), revision);
        const auto partialPath = source.parent_path() / L"部分译稿.pdf";
        const auto strict = exportPdfAsync(source, task->databasePath(), partialPath, false).get();
        require(!strict.GetNamedBoolean(L"saved", false) && strict.GetNamedNumber(L"preservedPages") == 1,
                "strict export accepted stale source edit");
        require(!std::filesystem::exists(partialPath), "strict export created a destination after rejection");
        const auto partial = exportPdfAsync(source, task->databasePath(), partialPath, true).get();
        require(partial.GetNamedBoolean(L"saved") && partial.GetNamedNumber(L"preservedPages") == 1,
                "partial export should preserve and report the stale page");
        require(partial.GetNamedArray(L"preserved").GetObjectAt(0).GetNamedNumber(L"page") == 2,
                "partial report did not locate the stale text");
        require(std::filesystem::exists(std::filesystem::path(partial.GetNamedString(L"report").c_str())),
                "partial export report was not created");

        const auto alias = source.parent_path() / L"源文件硬链接.pdf";
        std::filesystem::create_hard_link(source, alias);
        for (const auto& path : {source, alias}) {
            try {
                exportPdfAsync(source, task->databasePath(), path, true).get();
                throw std::runtime_error("export accepted source or source hardlink as destination");
            } catch (hresult_error const&) {
            }
        }

        task->keepOriginal(0, id);
        require(task->snapshot().preservedBlocks == 1, "keep original did not update summary");
        task->resetTranslationsAsync({[&] {
            if (task->snapshot().state == PdfTaskState::Resetting) task->cancel();
        }}).get();
        require(task->snapshot().resetCursor == 0, "cancel did not persist the reset cursor");
        task = std::make_shared<PdfTask>(profile);
        task->load(source, info);
        require(task->snapshot().resetCursor == 0, "reopened task lost its reset cursor");
        task->resetTranslationsAsync().get();
        require(task->snapshot().resetCursor < 0 && task->snapshot().completedBlocks == 0 && !task->snapshot().taskPending,
                "resumed reset left translated content");
        require(block(*task, 0).GetNamedBoolean(L"keep"), "reset discarded a preservation edit");
        return 0;
    } catch (hresult_error const& error) {
        std::cerr << to_string(error.message()) << '\n';
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
