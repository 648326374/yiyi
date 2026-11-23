#include "TextClient.h"

#include <winrt/Windows.Security.Cryptography.h>

#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3 || argc > 4) return 4;
    const auto mode = argc == 4 ? std::wstring_view(argv[3]) : std::wstring_view();
    winrt::init_apartment();
    yiyi::Provider provider{L"test", L"Test", argv[1], argv[2], L"test-model", L"private-test-secret", L""};
    if (provider.protocol == L"openai" || provider.protocol == L"responses") provider.reasoningEffort = L"medium";
    if (mode == L"invalid-key") provider.apiKey = L"private-test-secret\r\nX-Injected: bad";
    try {
        auto const preview = [](winrt::hstring const& result) {
            std::cout << "preview " << winrt::to_string(result) << std::endl;
        };
        winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> request{nullptr};
        if (mode == L"vision" || mode == L"oversize-vision") {
            auto image = winrt::Windows::Security::Cryptography::CryptographicBuffer::DecodeFromBase64String(
                L"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jP1sAAAAASUVORK5CYII=");
            if (mode == L"oversize-vision") image.data()[18] = 0x20; // IHDR width exceeds 4096
            request = yiyi::transcribeImageAsync(provider, image, preview);
        } else {
            request = yiyi::translateAsync(provider, L"System prompt", L"User text", preview, mode == L"probe");
        }
        if (mode == L"cancel") {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            request.Cancel();
        }
        const auto result = request.get();
        std::cout << "result " << winrt::to_string(result) << std::endl;
        return 0;
    } catch (winrt::hresult_canceled const&) {
        std::cout << "cancelled" << std::endl;
        return 2;
    } catch (winrt::hresult_error const& error) {
        std::cout << "error " << winrt::to_string(error.message()) << std::endl;
        return 3;
    }
}
