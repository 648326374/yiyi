#pragma once

#include <winrt/Windows.Data.Json.h>

#include <cstdint>
#include <filesystem>
#include <span>

namespace yiyi::pdf {

winrt::Windows::Data::Json::JsonObject analyzeScanPage(
    std::span<const uint8_t> bgra, int pixelWidth, int pixelHeight, int stride,
    double pageWidth, double pageHeight, int pageNumber, std::filesystem::path const& ocrRuntime = {});

// Writes only accepted text masks. The return value contains {id,error} for
// regions whose background could not safely be reconstructed.
winrt::Windows::Data::Json::JsonArray eraseScanText(
    std::span<uint8_t> bgra, int pixelWidth, int pixelHeight, int stride,
    double pageWidth, double pageHeight, winrt::Windows::Data::Json::JsonArray const& blocks);

}
