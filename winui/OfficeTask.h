#pragma once

#include "DocumentTask.h"
#include "OfficeDocumentNative.h"

#include <optional>

namespace yiyi {

struct OfficeProgress {
    winrt::hstring translation;
    winrt::hstring failure;
    bool done{};
    bool kept{};
};

struct OfficeTaskSnapshot {
    std::shared_ptr<OfficeDocumentNative const> document;
    std::map<winrt::hstring, OfficeProgress> progress;
    std::optional<DocumentSettings> settings;
    winrt::hstring error;
    winrt::hstring draftId;
    winrt::hstring draftText;
    OfficeExportOptions exportOptions;
    std::optional<size_t> activeSegment;
    size_t completed{};
    uint64_t generation{};
    uint64_t revision{};
    bool translating{};
    bool processing{};
    bool draftDirty{};
    bool hasUnexportedResult{};
};

class OfficeLayoutPreview {
public:
    ~OfficeLayoutPreview();
    std::filesystem::path const& path() const { return m_translation.empty() ? m_original : m_translation; }
    std::filesystem::path const& original() const { return m_original; }
    std::filesystem::path const& translation() const { return m_translation; }
    winrt::hstring const& bookmarkPrefix() const { return m_bookmarkPrefix; }
    bool matches(OfficeTaskSnapshot const& state) const { return m_generation == state.generation && m_revision == state.revision; }

private:
    friend class OfficeTask;
    explicit OfficeLayoutPreview(std::filesystem::path directory);
    std::filesystem::path m_directory;
    std::filesystem::path m_original;
    std::filesystem::path m_translation;
    winrt::hstring m_bookmarkPrefix;
    uint64_t m_generation{};
    uint64_t m_revision{};
};

std::filesystem::path installedWord();

class OfficeTask : public std::enable_shared_from_this<OfficeTask> {
public:
    explicit OfficeTask(std::filesystem::path profile);

    // These operations perform disk IO and may be called from a background apartment.
    void load(std::filesystem::path const& path);
    void exportTo(std::filesystem::path const& path);
    std::shared_ptr<OfficeLayoutPreview const> renderPreview(std::filesystem::path const& renderer, void* cancel = nullptr);
    std::vector<OfficeNotice> issues() const;
    OfficeTaskSnapshot snapshot() const;
    std::filesystem::path suggestedOutput() const;

    void edit(winrt::hstring const& id, winrt::hstring const& text);
    void keepOriginal(winrt::hstring const& id);
    void stageDraft(winrt::hstring const& id, winrt::hstring text);
    void discardDraft();
    void flush();
    void reset();
    void setExportOptions(OfficeExportOptions options);

    winrt::Windows::Foundation::IAsyncAction start(DocumentSettings settings, DocumentCallbacks callbacks = {});
    winrt::Windows::Foundation::IAsyncAction retry(winrt::hstring const& id, DocumentSettings settings, DocumentCallbacks callbacks = {});
    void cancel();

private:
    struct Data {
        std::shared_ptr<OfficeDocumentNative const> document;
        std::map<winrt::hstring, OfficeProgress> progress;
        std::optional<DocumentSettings> settings;
        winrt::hstring draftId;
        winrt::hstring draftText;
        OfficeExportOptions exportOptions;
        uint64_t revision{};
        uint64_t exportedRevision{};
        bool providerResolved{};
        bool cacheWritable{true};
    };

    Data restore(std::shared_ptr<OfficeDocumentNative const> document) const;
    void persistLocked();
    void requireLoadedLocked() const;
    void requireIdleLocked() const;
    size_t findLocked(winrt::hstring const& id) const;
    bool draftDirtyLocked() const;
    void applyEditLocked(winrt::hstring const& id, winrt::hstring text, bool kept);
    uint64_t beginLocked(DocumentSettings settings);
    winrt::Windows::Foundation::IAsyncAction run(uint64_t generation, std::optional<size_t> segment, DocumentCallbacks callbacks);
    std::filesystem::path cachePath(OfficeDocumentNative const& document) const;

    std::filesystem::path m_profile;
    mutable std::mutex m_mutex;
    Data m_data;
    winrt::hstring m_error;
    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> m_request{nullptr};
    std::optional<size_t> m_active;
    uint64_t m_generation{};
    bool m_translating{};
    bool m_processing{};
};

}
