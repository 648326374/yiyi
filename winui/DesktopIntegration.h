#pragma once

#include "SelectionCapture.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace yiyi {

struct DesktopPreferences {
    bool closeToTray = true;
    bool shortcutsEnabled = true;
    bool selectionEnabled = true;
    std::wstring showShortcut = L"Ctrl+Alt+Y";
    std::wstring selectionShortcut = L"Ctrl+Alt+D";
};

DesktopPreferences loadDesktopPreferences(const std::filesystem::path& directory);

struct DesktopCallbacks {
    std::function<void()> workspace;
    std::function<void()> settings;
    std::function<void()> screenshot;
    std::function<void(const std::wstring&)> lookup;
    std::function<void(const std::wstring&)> error;
    std::function<void()> preferencesChanged;
    std::function<void(const std::wstring&)> shortcutRecorded;
};

class ProfileInstance {
public:
    enum Result { Primary, ActivatedExisting, Failed };

    ProfileInstance() = default;
    ~ProfileInstance();
    ProfileInstance(ProfileInstance const&) = delete;
    ProfileInstance& operator=(ProfileInstance const&) = delete;

    Result acquire(const std::filesystem::path& directory);
    const std::wstring& error() const { return m_error; }
    const std::wstring& channel() const { return m_channel; }

private:
    HANDLE m_mutex = nullptr;
    HANDLE m_profileLock = nullptr;
    std::wstring m_channel;
    std::wstring m_error;
};

class DesktopIntegration {
public:
    DesktopIntegration(HWND mainWindow, std::filesystem::path directory, std::wstring channel,
                       DesktopCallbacks callbacks);
    ~DesktopIntegration();
    DesktopIntegration(DesktopIntegration const&) = delete;
    DesktopIntegration& operator=(DesktopIntegration const&) = delete;

    bool start();
    bool trayAvailable() const;
    const DesktopPreferences& preferences() const;
    const std::wstring& shortcutError() const;
    const std::wstring& selectionError() const;
    void setCloseToTray(bool enabled);
    void setSelectionEnabled(bool enabled);
    bool applyShortcuts(bool enabled, std::wstring_view show, std::wstring_view selection);
    static std::wstring normalizedShortcut(std::wstring_view text);
    void setRecordingShortcut(bool recording);
    void showWindow();
    void quit();
    void captureSelection();

private:
    struct State;
    std::unique_ptr<State> m_state;
};

}
