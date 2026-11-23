#pragma once

#include <filesystem>
#include <functional>
#include <cstdint>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace yiyi::pdf::merge {

using Progress = std::function<void(std::string_view stage, int completed, int total)>;

std::set<int> patternPages(const std::filesystem::path& runtimeDirectory, const std::filesystem::path& source,
                           int first, int last);

// Make a bounded editing copy and coalesce content streams before editing with PDFium.
void copyPages(const std::filesystem::path& runtimeDirectory, const std::filesystem::path& source,
               const std::filesystem::path& destination, int first, int last);

// Assemble translated contents and resources onto the original document's page objects.
void write(const std::filesystem::path& runtimeDirectory, const std::filesystem::path& source,
           const std::vector<std::filesystem::path>& parts, const std::filesystem::path& destination,
           int pageCount, const Progress& progress = {}, std::span<const std::uint8_t> modified = {});

}
