#pragma once

#include <winrt/Windows.Data.Json.h>

namespace yiyi {

winrt::Windows::Data::Json::JsonObject parseDictionaryResponse(winrt::hstring const& result,
    winrt::hstring const& term, winrt::hstring const& target, bool detailed);
winrt::Windows::Data::Json::JsonObject parseDictionaryPreview(winrt::hstring const& cumulative,
    winrt::hstring const& term, winrt::hstring const& target);
winrt::hstring formatDictionary(winrt::Windows::Data::Json::JsonObject const& entry);
bool isBriefDictionary(winrt::Windows::Data::Json::JsonObject const& entry);

}
