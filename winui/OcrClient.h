#pragma once

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

namespace yiyi {

// Runs the adjacent native worker in an isolated, cancelable process.
// A successful result has the recognized text and average confidence (0–1).
winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Data::Json::JsonObject>
recognizeImageAsync(winrt::Windows::Storage::Streams::IBuffer image);

}
