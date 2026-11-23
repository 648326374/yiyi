#pragma once

#include <winrt/base.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

struct sqlite3;

namespace yiyi {

struct HistoryRecord {
    std::int64_t id{};
    winrt::hstring input;
    winrt::hstring output;
    winrt::hstring kind{L"translation"};
    winrt::hstring dictionary{L"{}"};
    winrt::hstring source;
    winrt::hstring target;
    winrt::hstring provider;
    winrt::hstring model;
    winrt::hstring created;
    bool favorite{};
};

class HistoryStore {
public:
    explicit HistoryStore(std::filesystem::path const& directory);
    ~HistoryStore();

    HistoryStore(HistoryStore const&) = delete;
    HistoryStore& operator=(HistoryStore const&) = delete;

    std::int64_t add(HistoryRecord const& record);
    std::vector<HistoryRecord> list(bool favoritesOnly = false, winrt::hstring const& search = {}) const;
    std::size_t count(bool favoritesOnly = false) const;
    std::optional<HistoryRecord> get(std::int64_t id) const;
    bool setFavorite(std::int64_t id, bool favorite);
    bool remove(std::int64_t id);

private:
    sqlite3* m_db{};
    mutable std::mutex m_mutex;
};

}
