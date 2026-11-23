#pragma once

#include <winrt/Windows.Data.Json.h>

#include <filesystem>
#include <span>

struct sqlite3;

namespace yiyi::pdf {

struct PageUpdate {
    int number;
    winrt::Windows::Data::Json::JsonObject data;
};

class PdfTaskStore {
public:
    explicit PdfTaskStore(std::filesystem::path const& path, bool readOnly = false);
    ~PdfTaskStore();
    PdfTaskStore(PdfTaskStore const&) = delete;
    PdfTaskStore& operator=(PdfTaskStore const&) = delete;

    winrt::Windows::Data::Json::JsonObject metadata() const;
    winrt::Windows::Data::Json::JsonObject page(int number) const;
    winrt::Windows::Data::Json::JsonArray summaries() const;
    void save(winrt::Windows::Data::Json::JsonObject const& metadata, std::span<PageUpdate const> pages);

    static winrt::Windows::Data::Json::JsonObject summary(int number, winrt::Windows::Data::Json::JsonObject const& page);
    static winrt::Windows::Data::Json::JsonArray blockIssues(winrt::Windows::Data::Json::JsonObject const& block);

private:
    sqlite3* m_db{};
};

}
