#pragma once

#include <chrono>
#include <functional>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

namespace yiyi {

struct Provider {
    winrt::hstring id;
    winrt::hstring name;
    winrt::hstring protocol;
    winrt::hstring baseUrl;
    winrt::hstring model;
    winrt::hstring apiKey;
    winrt::hstring reasoningEffort;
    winrt::hstring dictionaryMode{L"brief"};
};

void validateProvider(Provider const& provider);

// Preview is cumulative and runs on a background thread.
winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> translateAsync(
    Provider provider, winrt::hstring system, winrt::hstring input,
    std::function<void(winrt::hstring const&)> preview = {}, bool probe = false,
    std::chrono::minutes inactivity = std::chrono::minutes(10));

winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> transcribeImageAsync(
    Provider provider, winrt::Windows::Storage::Streams::IBuffer image,
    std::function<void(winrt::hstring const&)> preview = {},
    std::chrono::minutes inactivity = std::chrono::minutes(10));

}
