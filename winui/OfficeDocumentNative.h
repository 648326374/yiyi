#pragma once

#include <winrt/base.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace yiyi {

struct OfficeSegment {
    winrt::hstring id;
    winrt::hstring part;
    winrt::hstring source;
    winrt::hstring location;
    winrt::hstring group;
    size_t paragraph{};
    size_t page{};
    std::vector<size_t> nodes;
    bool mixedStyle{};
    double width{};
    double height{};
    double fontSize{18};
};

struct OfficeNotice {
    winrt::hstring id;
    winrt::hstring location;
    winrt::hstring message;
};

struct OfficeImage {
    winrt::hstring id;
    winrt::hstring name;
};

struct OfficeExportOptions {
    bool fitText{true};
    double minimumFont{12};
};

using OfficeTranslations = std::map<winrt::hstring, winrt::hstring>;

class OfficeDocumentNative {
public:
    static OfficeDocumentNative load(std::filesystem::path const& path);

    std::filesystem::path const& path() const { return m_path; }
    winrt::hstring const& kind() const { return m_kind; }
    winrt::hstring const& hash() const { return m_hash; }
    size_t pageCount() const { return m_pages; }
    std::vector<OfficeSegment> const& segments() const { return m_segments; }
    std::vector<OfficeImage> const& images() const { return m_images; }

    std::vector<OfficeNotice> issues(OfficeTranslations const& translations, OfficeExportOptions options = {}) const;
    std::string imageBytes(winrt::hstring const& id) const;
    void exportTo(std::filesystem::path const& path, OfficeTranslations const& translations, OfficeExportOptions options = {}) const;

private:
    std::filesystem::path m_path;
    std::filesystem::path m_sourcePath;
    winrt::hstring m_kind;
    winrt::hstring m_hash;
    size_t m_pages{};
    std::vector<OfficeSegment> m_segments;
    std::vector<OfficeImage> m_images;
    std::vector<OfficeNotice> m_notices;
    std::string m_archive;
};

}
