#pragma once

#include "App.xaml.g.h"

#include <filesystem>

namespace winrt::Yiyi::WinUI::implementation
{
    struct App : AppT<App>
    {
        App(std::filesystem::path dataDirectory, std::filesystem::path pdfPath, std::filesystem::path documentPath,
            std::wstring desktopChannel, bool smokeTest, int& exitCode);
        fire_and_forget OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        std::filesystem::path m_dataDirectory;
        std::filesystem::path m_pdfPath;
        std::filesystem::path m_documentPath;
        std::wstring m_desktopChannel;
        bool m_smokeTest;
        int& m_exitCode;
        Microsoft::UI::Xaml::Window m_window{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_smokeTimer{ nullptr };
    };
}
