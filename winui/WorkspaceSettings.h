#pragma once

#include "TextClient.h"

#include <filesystem>
#include <map>
#include <vector>

namespace yiyi {

struct WorkspaceSettings {
    std::vector<Provider> providers;
    winrt::hstring selectedProviderId;
    winrt::hstring visionProviderId;
    winrt::hstring sourceLanguage{L"自动检测"};
    winrt::hstring targetLanguage{L"简体中文"};
    winrt::hstring mode{L"auto"};
    winrt::hstring tone{L"自然"};
    winrt::hstring theme{L"system"};
    int pdfRecognitionPixels = 1800;
    int pdfRequestTimeout = 10;
    std::map<winrt::hstring, winrt::hstring> unreadableCredentials;
};

std::filesystem::path defaultDataDirectory();
WorkspaceSettings loadSettings(std::filesystem::path const& directory);
void saveSettings(std::filesystem::path const& directory, WorkspaceSettings const& settings);

}
