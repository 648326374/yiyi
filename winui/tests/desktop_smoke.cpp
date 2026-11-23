#include "../DesktopIntegration.h"
#include "../SelectionCapture.h"

#include <richedit.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Window {
    HWND handle;
    ~Window() { if (IsWindow(handle)) DestroyWindow(handle); }
};

bool pumpUntil(auto predicate, ULONGLONG timeout)
{
    const auto deadline = GetTickCount64() + timeout;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (predicate()) return true;
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
    return false;
}

DWORD secondaryProcess(const std::filesystem::path& directory)
{
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    require(length && length < executable.size(), "could not locate desktop test executable");
    executable.resize(length);
    auto command = L"\"" + executable + L"\" --secondary \"" + directory.native() + L"\"";
    STARTUPINFOW info{};
    info.cb = sizeof(info);
    PROCESS_INFORMATION child{};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &info, &child) != FALSE, "could not start secondary workspace test process");
    CloseHandle(child.hThread);
    const bool terminated = pumpUntil([&] { return WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0; }, 4000);
    DWORD exitCode = 255;
    if (terminated) GetExitCodeProcess(child.hProcess, &exitCode);
    else TerminateProcess(child.hProcess, 254);
    CloseHandle(child.hProcess);
    require(terminated, "secondary workspace process timed out");
    return exitCode;
}

void verifyDesktop(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory);
    std::ofstream file(directory / L"settings.ini", std::ios::binary);
    file << "[desktop]\ncloseToTray=false\nselectionEnabled=false\nshortcutsEnabled=false\nshowShortcut=Alt+Shift+F24\nselectionShortcut=Ctrl+Alt+D\n";
    file.close();
    auto preferences = yiyi::loadDesktopPreferences(directory);
    require(!preferences.closeToTray && !preferences.selectionEnabled && !preferences.shortcutsEnabled, "legacy desktop preferences");
    require(preferences.showShortcut == L"Alt+Shift+F24" && preferences.selectionShortcut == L"Ctrl+Alt+D", "legacy shortcuts");
    require(yiyi::DesktopIntegration::normalizedShortcut(L" alt + CTRL + shift + a ") == L"Ctrl+Alt+Shift+A", "shortcut normalization");
    require(yiyi::DesktopIntegration::normalizedShortcut(L"Win+F24") == L"Meta+F24", "Windows modifier normalization");
    for (const auto invalid : {L"Shift+A", L"Ctrl+F12", L"Ctrl+F25", L"Ctrl+Ctrl+A", L"Ctrl+A, Ctrl+B"}) {
        require(yiyi::DesktopIntegration::normalizedShortcut(invalid).empty(), "invalid shortcut accepted");
    }

    yiyi::ProfileInstance primary;
    require(primary.acquire(directory) == yiyi::ProfileInstance::Primary, "profile primary startup");
    Window fixture{CreateWindowExW(0, L"STATIC", L"Yiyi desktop test", WS_OVERLAPPEDWINDOW,
                                  70, 70, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
    require(fixture.handle != nullptr, "create desktop fixture window");
    yiyi::DesktopIntegration desktop(fixture.handle, directory, primary.channel(), {});
    require(desktop.start(), "initialize desktop broker");
    require(secondaryProcess(directory / L".") == yiyi::ProfileInstance::ActivatedExisting, "profile alias did not activate existing broker");
    require(secondaryProcess(directory / L"other-profile") == yiyi::ProfileInstance::Primary, "another profile was blocked");
    const auto legacyPipe = L"\\\\.\\pipe\\Yiyi-" + primary.channel().substr(std::wstring_view(L"Yiyi.WinUI.").size(), 32);
    require(WaitNamedPipeW(legacyPipe.c_str(), 1000), "Qt compatibility activation pipe is unavailable");
    const auto pipe = CreateFileW(legacyPipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    require(pipe != INVALID_HANDLE_VALUE, "Qt compatibility activation pipe could not be opened");
    DWORD copied = 0;
    const bool sent = WriteFile(pipe, "acti", 4, &copied, nullptr) && copied == 4
        && WriteFile(pipe, "vate", 4, &copied, nullptr) && copied == 4;
    char reply[2]{};
    const bool acknowledged = sent && ReadFile(pipe, reply, sizeof(reply), &copied, nullptr) && copied == sizeof(reply)
        && std::string_view(reply, 2) == "ok";
    CloseHandle(pipe);
    require(acknowledged, "Qt compatibility activation command was not acknowledged");

    require(desktop.applyShortcuts(true, L"Ctrl+Alt+Shift+F23", L"Ctrl+Alt+Shift+F24"), "two real hotkeys failed registration");
    require(RegisterHotKey(fixture.handle, 0x7111, MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F22), "could not reserve hotkey for conflict test");
    require(!desktop.applyShortcuts(true, L"Ctrl+Alt+Shift+F21", L"Ctrl+Alt+Shift+F22"), "occupied hotkey was accepted");
    require(desktop.preferences().showShortcut == L"Ctrl+Alt+Shift+F23", "failed shortcut save replaced working preference");
    require(RegisterHotKey(fixture.handle, 0x7112, MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F21), "partially registered hotkey was not reverted");
    UnregisterHotKey(fixture.handle, 0x7111);
    UnregisterHotKey(fixture.handle, 0x7112);
    require(desktop.applyShortcuts(false, {}, {}), "hotkey disable failed");

    if (desktop.trayAvailable()) {
        desktop.setCloseToTray(true);
        desktop.showWindow();
        SendMessageW(fixture.handle, WM_CLOSE, 0, 0);
        require(IsWindow(fixture.handle) && !IsWindowVisible(fixture.handle), "close did not hide window to system tray");
        desktop.showWindow();
        require(IsWindowVisible(fixture.handle), "restored window is not visible");
        desktop.quit();
        require(pumpUntil([&] { return !IsWindow(fixture.handle); }, 2000), "tray quit did not close the window");
    } else std::cout << "SKIP close-to-tray (system tray unavailable)\n";

    preferences = yiyi::loadDesktopPreferences(directory);
    require(!preferences.shortcutsEnabled && preferences.showShortcut == L"Ctrl+Alt+Shift+F23", "desktop changes were not saved");
    std::ifstream legacy(directory / L"settings.ini");
    const std::string original{std::istreambuf_iterator<char>(legacy), {}};
    require(original.find("showShortcut=Alt+Shift+F24") != std::string::npos, "legacy profile was changed");
}

void verifySelection()
{
    const auto richEdit = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(richEdit != nullptr, "RichEdit fixture is unavailable");
    Window fixture{CreateWindowExW(0, L"STATIC", L"Yiyi selection test", WS_OVERLAPPEDWINDOW,
                                  80, 80, 360, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
    require(fixture.handle != nullptr, "create selection fixture");
    const auto edit = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"native selection fixture",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE, 10, 10, 300, 60, fixture.handle, nullptr, GetModuleHandleW(nullptr), nullptr);
    const auto password = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"dummy password",
        WS_CHILD | WS_VISIBLE | ES_PASSWORD, 10, 90, 300, 25, fixture.handle, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(edit && password, "create selection controls");
    ShowWindow(fixture.handle, SW_SHOW);
    SetWindowPos(fixture.handle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(fixture.handle);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, 16);
    require(pumpUntil([&] { return GetForegroundWindow() == fixture.handle && GetFocus() == edit; }, 700), "fixture did not get focus");
    POINT point{20, 10};
    ClientToScreen(edit, &point);
    require(GetAncestor(WindowFromPoint(point), GA_ROOT) == fixture.handle, "selection fixture pointer is covered");

    yiyi::SelectionCapture capture;
    std::optional<yiyi::SelectionResult> result;
    const DWORD clipboardSequence = GetClipboardSequenceNumber();
    require(capture.request(fixture.handle, fixture.handle, point, true), "could not start isolated UIA probe");
    require(pumpUntil([&] { return bool(result = capture.poll()); }, 3500), "UIA probe did not finish");
    if (result->status != yiyi::SelectionStatus::Captured || result->text != L"native selection") {
        std::wcerr << L"UIA status " << static_cast<unsigned>(result->status) << L", text " << result->text << L'\n';
        throw std::runtime_error("UIA did not read the expected selection");
    }
    require(clipboardSequence == GetClipboardSequenceNumber(), "automatic selection changed clipboard");

    SetFocus(password);
    SendMessageW(password, EM_SETSEL, 0, -1);
    require(capture.request(fixture.handle, fixture.handle, point, false), "could not start password test probe");
    require(pumpUntil([&] { return bool(result = capture.poll()); }, 3500), "password probe did not finish");
    require(result->status == yiyi::SelectionStatus::Protected && result->text.empty(), "password selection was not rejected");
    require(clipboardSequence == GetClipboardSequenceNumber(), "password probe changed clipboard");
    DestroyWindow(fixture.handle);
    FreeLibrary(richEdit);
}

}

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc > 1 && std::wstring_view(argv[1]) == L"--selection-probe") return yiyi::SelectionCapture::runProbe(argc, argv);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--secondary") {
        yiyi::ProfileInstance instance;
        return instance.acquire(argv[2]);
    }
    if (argc != 2 && argc != 3) return 2;
    try {
        verifyDesktop(argv[1]);
        if (argc == 3 && std::wstring_view(argv[2]) == L"--interactive") verifySelection();
        else std::cout << "SKIP UI Automation selection (pass --interactive)\n";
        std::cout << "Desktop integration passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
