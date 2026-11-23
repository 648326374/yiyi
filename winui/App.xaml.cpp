#include "pch.h"
#include "App.xaml.h"
#include "DesktopIntegration.h"
#include "MainWindow.xaml.h"

#include <shellapi.h>
#include <WindowsAppSDK-VersionInfo.h>
#include <MddBootstrap.h>

#include <chrono>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{
    struct LaunchOptions
    {
        std::filesystem::path dataDirectory;
        std::filesystem::path pdfPath;
        std::filesystem::path documentPath;
        bool smokeTest = false;
        bool noDesktop = false;
    };

    void reportStartupError(std::wstring_view message, bool smokeTest)
    {
        const std::wstring text(message);
        OutputDebugStringW(text.c_str());
        std::fputs(winrt::to_string(message).c_str(), stderr);
        std::fputc('\n', stderr);
        if (!smokeTest) {
            MessageBoxW(nullptr, text.c_str(), L"Yiyi", MB_OK | MB_ICONERROR);
        }
    }

    LaunchOptions readLaunchOptions(int argc, wchar_t* const* args)
    {
        LaunchOptions options;
        for (int index = 1; index < argc; ++index) {
            const std::wstring_view argument(args[index]);
            if (argument == L"--smoke-test") {
                options.smokeTest = true;
            } else if (argument == L"--no-desktop") {
                options.noDesktop = true;
            } else if ((argument == L"--data-dir" || argument == L"--pdf" || argument == L"--document") && index + 1 < argc
                && *args[index + 1] && !std::wstring_view(args[index + 1]).starts_with(L"--")) {
                auto path = std::filesystem::absolute(args[++index]);
                if (argument == L"--data-dir") options.dataDirectory = std::move(path);
                else if (argument == L"--pdf") options.pdfPath = std::move(path);
                else options.documentPath = std::move(path);
            } else {
                throw std::invalid_argument("Usage: Yiyi.WinUI.exe [--data-dir PATH] [--pdf PATH] [--document PATH] [--no-desktop] [--smoke-test]");
            }
        }
        if (options.smokeTest && options.dataDirectory.empty()) {
            GUID id;
            winrt::check_hresult(CoCreateGuid(&id));
            wchar_t guid[39]{};
            StringFromGUID2(id, guid, ARRAYSIZE(guid));
            options.dataDirectory = std::filesystem::temp_directory_path() / (std::wstring(L"yiyi-winui-") + guid);
        }
        return options;
    }

    struct RuntimeLifetime
    {
        ~RuntimeLifetime()
        {
            MddBootstrapShutdown();
        }
    };
}

namespace winrt::Yiyi::WinUI::implementation
{
    App::App(std::filesystem::path dataDirectory, std::filesystem::path pdfPath, std::filesystem::path documentPath,
             std::wstring desktopChannel, bool smokeTest, int& exitCode)
        : m_dataDirectory(std::move(dataDirectory)), m_pdfPath(std::move(pdfPath)), m_documentPath(std::move(documentPath)),
          m_desktopChannel(std::move(desktopChannel)),
          m_smokeTest(smokeTest), m_exitCode(exitCode)
    {
        InitializeComponent();
    }

    fire_and_forget App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        const auto lifetime = get_strong();
        try {
            auto mainWindow = winrt::make_self<MainWindow>(m_dataDirectory);
            m_window = *mainWindow;
            mainWindow->StartDesktop(m_desktopChannel);
            m_window.Activate();
            if (!m_pdfPath.empty()) co_await mainWindow->OpenPdfAsync(m_pdfPath);
            else if (!m_documentPath.empty()) co_await mainWindow->OpenDocumentAsync(m_documentPath);
            if (m_smokeTest) {
                m_smokeTimer = m_window.DispatcherQueue().CreateTimer();
                m_smokeTimer.Interval(std::chrono::milliseconds(200));
                m_smokeTimer.IsRepeating(false);
                m_smokeTimer.Tick([this](auto const&, auto const&) {
                    m_window.Close();
                    m_exitCode = 0;
                    Exit();
                });
                m_smokeTimer.Start();
            }
        } catch (...) {
            m_exitCode = 1;
            reportStartupError(winrt::to_message(), m_smokeTest);
            Exit();
        }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    LaunchOptions options;
    try {
        int argc = 0;
        const std::unique_ptr<wchar_t*, decltype(&LocalFree)> args(
            CommandLineToArgvW(GetCommandLineW(), &argc), LocalFree);
        if (!args) winrt::throw_last_error();
        if (argc > 1 && std::wstring_view(args.get()[1]) == L"--selection-probe") {
            return ::yiyi::SelectionCapture::runProbe(argc, args.get());
        }
        options = readLaunchOptions(argc, args.get());
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        if (options.dataDirectory.empty()) options.dataDirectory = ::yiyi::defaultDataDirectory();
        ::yiyi::ProfileInstance instance;
        if (!options.smokeTest && !options.noDesktop) {
            const auto registration = instance.acquire(options.dataDirectory);
            if (registration == ::yiyi::ProfileInstance::ActivatedExisting) return 0;
            if (registration == ::yiyi::ProfileInstance::Failed) {
                reportStartupError(instance.error(), false);
                return 1;
            }
        }
        const HRESULT status = MddBootstrapInitialize2(
            WINDOWSAPPSDK_RELEASE_MAJORMINOR, WINDOWSAPPSDK_RELEASE_VERSION_TAG_W,
            PACKAGE_VERSION{ WINDOWSAPPSDK_RUNTIME_VERSION_UINT64 }, MddBootstrapInitializeOptions_None);
        if (FAILED(status)) {
            wchar_t code[11]{};
            swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(status));
            const auto message = std::wstring(L"无法初始化 Windows App Runtime（") + code +
                L"）。请安装完整的 Windows App Runtime 2.4 或更高的 2.x 版本（x64），然后重新启动译意。\n"
                L"https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads/";
            reportStartupError(message, options.smokeTest);
            return 2;
        }
        const RuntimeLifetime runtime;
        int exitCode = options.smokeTest ? 1 : 0;
        winrt::Microsoft::UI::Xaml::Application::Start([&](auto const&) {
            try {
                winrt::make<winrt::Yiyi::WinUI::implementation::App>(options.dataDirectory, options.pdfPath,
                    options.documentPath, instance.channel(), options.smokeTest, exitCode);
            } catch (...) {
                exitCode = 1;
                reportStartupError(winrt::to_message(), options.smokeTest);
                PostQuitMessage(exitCode);
            }
        });
        return exitCode;
    } catch (...) {
        reportStartupError(winrt::to_message(), options.smokeTest);
        return 1;
    }
}
