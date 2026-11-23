#pragma once

#include <winrt/Windows.Data.Json.h>

#include <filesystem>
#include <functional>

namespace yiyi::pdf {

using PdfProgress = std::function<void(winrt::Windows::Data::Json::JsonObject const&)>;

winrt::Windows::Data::Json::JsonObject executeDocumentRequest(
    winrt::Windows::Data::Json::JsonObject const& request,
    std::filesystem::path const& runtimeDirectory,
    PdfProgress const& progress = {});

}
