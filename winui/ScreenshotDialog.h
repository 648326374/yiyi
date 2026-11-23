#pragma once

#include "ScreenshotCapture.h"
#include "TextClient.h"

#undef GetCurrentTime
#include <winrt/Microsoft.UI.Xaml.h>

#include <functional>
#include <vector>

namespace yiyi {

struct ScreenshotDialogOptions {
    HWND owner{};
    winrt::Microsoft::UI::Xaml::XamlRoot root{nullptr};
    std::vector<Provider> providers;
    winrt::hstring translationProviderId;
    winrt::hstring visionProviderId;
    winrt::hstring targetLanguage{L"简体中文"};
    winrt::Microsoft::UI::Xaml::ElementTheme theme{winrt::Microsoft::UI::Xaml::ElementTheme::Default};
    bool visionMode{};
    std::function<bool()> aiAvailable;
    std::function<void(winrt::hstring const&)> targetChanged;
    std::function<void(bool, winrt::hstring const&)> visionChanged;
    std::function<void(winrt::hstring const&, winrt::hstring const&, bool)> useText;
};

winrt::Windows::Foundation::IAsyncAction showScreenshotDialogAsync(
    ScreenshotDialogOptions options, winrt::Windows::Storage::Streams::IBuffer image = nullptr);

}
