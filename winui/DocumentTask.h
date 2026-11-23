#pragma once

#include "TextClient.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace yiyi {

enum class DocumentEncoding { Auto, Utf8, Utf16Le, Utf16Be, Gb18030 };
enum class DocumentState { Empty, Ready, Translating, Paused, Failed, Completed };

struct DocumentPart {
    winrt::hstring text;
    bool translate{};
    bool continuation{};
};

struct DocumentSettings {
    Provider provider;
    winrt::hstring sourceLanguage{L"自动检测"};
    winrt::hstring targetLanguage{L"简体中文"};
    winrt::hstring tone{L"自然"};
};

struct DocumentSnapshot {
    std::filesystem::path path;
    winrt::hstring source;
    winrt::hstring result;
    winrt::hstring encoding;
    winrt::hstring error;
    winrt::hstring taskDescription;
    DocumentState state{DocumentState::Empty};
    size_t completed{};
    size_t total{};
    uint64_t generation{};
    bool hasUnexportedResult{};
    bool hasSnapshot{};
};

struct DocumentCallbacks {
    // State changes are delivered on the apartment which starts the request.
    std::function<void()> changed;
    // Preview is cumulative for the active segment and starts on a background thread.
    std::function<void(uint64_t generation, size_t segment, winrt::hstring const&)> preview;
};

std::vector<DocumentPart> splitDocumentText(std::wstring_view text, size_t limit = 1000);
winrt::hstring decodeDocumentText(std::string_view bytes, DocumentEncoding encoding, winrt::hstring& detected);

class DocumentTask : public std::enable_shared_from_this<DocumentTask> {
public:
    void load(std::filesystem::path const& path, DocumentEncoding encoding = DocumentEncoding::Auto);
    void reset();
    DocumentSnapshot snapshot() const;
    std::filesystem::path suggestedOutput() const;
    void exportTo(std::filesystem::path const& path);

    winrt::Windows::Foundation::IAsyncAction start(DocumentSettings settings, DocumentCallbacks callbacks = {});
    winrt::Windows::Foundation::IAsyncAction resume(DocumentCallbacks callbacks = {});
    void cancel();

private:
    winrt::Windows::Foundation::IAsyncAction run(uint64_t generation, DocumentCallbacks callbacks);
    uint64_t beginLocked();
    void resetLocked();

    mutable std::mutex m_mutex;
    std::vector<DocumentPart> m_parts;
    std::filesystem::path m_path;
    std::filesystem::path m_sourcePath;
    winrt::hstring m_source;
    std::wstring m_result;
    winrt::hstring m_encoding;
    winrt::hstring m_error;
    DocumentState m_state{DocumentState::Empty};
    DocumentSettings m_settings;
    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> m_request{nullptr};
    size_t m_index{};
    size_t m_completed{};
    size_t m_exportedCompleted{};
    size_t m_total{};
    uint64_t m_generation{};
    bool m_hasSnapshot{};
    bool m_crlf{};
};

}
