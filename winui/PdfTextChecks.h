#pragma once

#include <winrt/Windows.Data.Json.h>

namespace yiyi {

winrt::hstring pdfTranslationIssue(winrt::Windows::Data::Json::JsonObject const& block,
                                   winrt::hstring const& translation, winrt::hstring const& target);

winrt::hstring pdfLayoutIssue(winrt::Windows::Data::Json::JsonObject const& block,
                              winrt::hstring const& translation);

}
