#pragma once

#include <winrt/Windows.Data.Json.h>

#include <vector>

namespace yiyi {

// These edits return a detached page. The caller owns persistence and undo.
winrt::Windows::Data::Json::JsonObject movePdfBlock(winrt::Windows::Data::Json::JsonObject const& page,
    winrt::hstring const& id, int position);
winrt::Windows::Data::Json::JsonObject mergePdfBlocks(winrt::Windows::Data::Json::JsonObject const& page,
    std::vector<winrt::hstring> const& ids);
winrt::Windows::Data::Json::JsonObject splitPdfBlock(winrt::Windows::Data::Json::JsonObject const& page,
    winrt::hstring const& id, bool vertical, double position, winrt::hstring const& firstSource, winrt::hstring const& secondSource);
winrt::Windows::Data::Json::JsonObject revisePdfRegion(winrt::Windows::Data::Json::JsonObject const& page,
    winrt::hstring const& id, winrt::Windows::Data::Json::JsonObject const& changes,
    winrt::Windows::Data::Json::JsonObject const& previousPage = nullptr);

}
