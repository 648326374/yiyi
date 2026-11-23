#pragma once

#include "DocumentTask.h"
#include "pdf/pdftaskstore.h"

#include <winrt/Windows.Data.Json.h>

namespace yiyi {

enum class PdfTaskState { Empty, Ready, Analyzing, Recognizing, Translating, Resetting, Paused, Failed, Completed };

struct PdfTaskSnapshot {
    std::filesystem::path path;
    winrt::Windows::Data::Json::JsonObject document{nullptr};
    PdfTaskState state{PdfTaskState::Empty};
    int pageCount{};
    int analyzedPages{};
    int recognizedPages{};
    int totalBlocks{};
    int completedBlocks{};
    int preservedBlocks{};
    int activePage{-1};
    int firstPage{-1};
    int lastPage{-1};
    winrt::hstring providerId;
    winrt::hstring target;
    winrt::hstring model;
    winrt::hstring visionProviderId;
    winrt::hstring taskDescription;
    winrt::hstring taskScope;
    winrt::hstring operation;
    winrt::hstring error;
    uint64_t generation{};
    bool taskPending{};
    bool busy{};
    int scanPages{};
    int resetCursor{-1};
};

struct PdfTaskCallbacks {
    // Invoked on the caller's apartment, with no task lock held.
    std::function<void()> changed;
};

class PdfTask : public std::enable_shared_from_this<PdfTask> {
public:
    explicit PdfTask(std::filesystem::path profile);

    void load(std::filesystem::path path, winrt::Windows::Data::Json::JsonObject inspectedDocument);
    PdfTaskSnapshot snapshot() const;
    winrt::Windows::Data::Json::JsonObject page(int number) const;
    winrt::Windows::Data::Json::JsonArray issueSummaries() const;
    std::filesystem::path databasePath() const;

    winrt::Windows::Foundation::IAsyncAction analyze(int page, PdfTaskCallbacks callbacks = {});
    winrt::Windows::Foundation::IAsyncAction start(DocumentSettings settings, int page, PdfTaskCallbacks callbacks = {});
    winrt::Windows::Foundation::IAsyncAction retranslatePage(int page, DocumentSettings settings, PdfTaskCallbacks callbacks = {});
    winrt::Windows::Foundation::IAsyncAction retryBlock(int page, winrt::hstring id, DocumentSettings settings,
                                                       PdfTaskCallbacks callbacks = {});
    winrt::Windows::Foundation::IAsyncAction resume(Provider provider, PdfTaskCallbacks callbacks = {});
    winrt::Windows::Foundation::IAsyncAction recognizePage(int page, Provider vision, int pixels, PdfTaskCallbacks callbacks = {});
    winrt::Windows::Foundation::IAsyncAction recognizeRange(int first, int last, Provider vision, int pixels,
                                                          PdfTaskCallbacks callbacks = {});
    void configureRecognition(Provider vision, int pixels, std::chrono::minutes timeout);
    void cancel();
    void discardTask();
    winrt::Windows::Foundation::IAsyncAction resetTranslationsAsync(PdfTaskCallbacks callbacks = {});

    void editBlock(int page, winrt::hstring const& id, winrt::hstring const& translation);
    void keepOriginal(int page, winrt::hstring const& id);
    void reviseBlock(int page, winrt::hstring const& id, winrt::Windows::Data::Json::JsonObject edits);
    bool recordPreviewStatuses(int page, winrt::Windows::Data::Json::JsonArray statuses,
                               winrt::Windows::Data::Json::JsonObject expectedPage);
    void recordExportStatuses(winrt::Windows::Data::Json::JsonArray statuses);
    bool replacePageStructure(int page, winrt::Windows::Data::Json::JsonObject expectedPage,
                              winrt::Windows::Data::Json::JsonObject newPage);

private:
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Action = winrt::Windows::Foundation::IAsyncAction;
    using AsyncInfo = winrt::Windows::Foundation::IAsyncInfo;

    void requireIdleLocked(bool allowReset = false) const;
    int pageCountLocked() const;
    void checkPageLocked(int page) const;
    void checkGenerationLocked(uint64_t generation) const;
    uint64_t beginLocked(PdfTaskState state, int page);
    void setRequest(uint64_t generation, AsyncInfo const& request);
    void saveLocked(Object metadata, std::span<pdf::PageUpdate const> pages = {});
    int beginResetLocked();
    int resetBatchLocked(int first);
    void finishResetLocked();
    winrt::Windows::Foundation::IAsyncOperation<int> resetBatchAsync(uint64_t generation, int first);
    void finish(uint64_t generation, winrt::hstring const& error = {});
    Action analyzeRange(uint64_t generation, int first, int last, PdfTaskCallbacks callbacks);
    Action beginTranslation(DocumentSettings settings, int page, bool replace, PdfTaskCallbacks callbacks,
                            winrt::hstring blockId = {});
    Action translateRange(uint64_t generation, DocumentSettings settings, PdfTaskCallbacks callbacks);
    Action beginRecognition(int first, int last, Provider vision, int pixels, bool force, PdfTaskCallbacks callbacks);
    Action recognize(uint64_t generation, int page, Provider vision, int pixels, PdfTaskCallbacks callbacks);

    const std::filesystem::path m_profile;
    mutable std::mutex m_mutex;
    std::filesystem::path m_path;
    std::filesystem::path m_database;
    std::unique_ptr<pdf::PdfTaskStore> m_store;
    Object m_document{nullptr};
    Object m_metadata{nullptr};
    std::vector<Object> m_summaries;
    AsyncInfo m_request{nullptr};
    PdfTaskState m_state{PdfTaskState::Empty};
    winrt::hstring m_error;
    uint64_t m_generation{};
    int m_activePage{-1};
    int m_analyzedPages{};
    int m_scanPages{};
    int m_recognizedPages{};
    int m_totalBlocks{};
    int m_completedBlocks{};
    int m_preservedBlocks{};
    Provider m_vision;
    int m_pixels{1800};
    std::chrono::minutes m_timeout{10};
};

}
