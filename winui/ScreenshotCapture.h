#pragma once

#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

namespace yiyi {

// Returns the selected physical pixels as PNG, or a null buffer if the user cancels.
winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::Streams::IBuffer> captureScreenRegionAsync(HWND owner);

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::Streams::InMemoryRandomAccessStream>
imageStreamAsync(winrt::Windows::Storage::Streams::IBuffer image);

}
