#pragma once

#include <winrt/Windows.Data.Json.h>

namespace yiyi::pdf {

inline constexpr int nativePdfExtractionVersion = 15;

// Input rectangles and rules use PDF points measured from the top-left of the page.
winrt::Windows::Data::Json::JsonObject analyzePageInput(
    winrt::Windows::Data::Json::JsonObject const& raw, int pageNumber);

// Carry headers to a geometrically matching table on the immediately following page.
void linkPageTableHeaders(
    winrt::Windows::Data::Json::JsonObject const& previous,
    winrt::Windows::Data::Json::JsonObject const& current);

// Orders located scan lines before they have an OCR transcript.
winrt::Windows::Data::Json::JsonObject analyzeScanRows(
    winrt::Windows::Data::Json::JsonArray const& rows, winrt::Windows::Data::Json::JsonArray const& rules,
    double pageWidth, double pageHeight, int pageNumber);

}
