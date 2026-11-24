#include "DesktopIntegration.h"
#include "../assets/resource.h"

#include <bcrypt.h>
#include <commctrl.h>
#include <objbase.h>
#include <sddl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cwctype>
#include <fstream>
#include <map>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace yiyi {
namespace {

constexpr wchar_t brokerClass[] = L"Yiyi.WinUI.DesktopBroker.1";
constexpr wchar_t bubbleClass[] = L"Yiyi.WinUI.SelectionBubble.1";
constexpr wchar_t groupName[] = L"desktop";
constexpr std::wstring_view channelPrefix = L"Yiyi.WinUI.";
constexpr UINT activateMessage = WM_APP + 1;
constexpr UINT trayMessage = WM_APP + 2;
constexpr UINT mouseDownMessage = WM_APP + 3;
constexpr UINT mouseUpMessage = WM_APP + 4;
constexpr UINT mouseDismissMessage = WM_APP + 5;
constexpr LRESULT activatedReply = 0x59495949;
constexpr UINT trayId = 1;
constexpr UINT_PTR selectDelayTimer = 1;
constexpr UINT_PTR selectPollTimer = 2;
constexpr UINT_PTR bubbleTimer = 3;
constexpr UINT_PTR closeSubclassId = 1;
constexpr wchar_t bubbleCaption[] = L"查询选中文字";
constexpr int bubbleWidth = 118;
constexpr int bubbleHeight = 34;
constexpr int bubbleRadius = 8;

enum MenuCommand : UINT_PTR { OpenWorkspace = 1, Screenshot, Settings, ToggleSelection, Quit, AcceptSelection };

std::wstring_view trim(std::wstring_view text)
{
    while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
    while (!text.empty() && std::iswspace(text.back())) text.remove_suffix(1);
    return text;
}

struct Shortcut {
    UINT modifiers = 0;
    UINT key = 0;
    std::wstring text;
};

std::optional<Shortcut> parseShortcut(std::wstring_view text)
{
    Shortcut shortcut;
    for (auto separator = text.find(L'+'); separator != std::wstring_view::npos; separator = text.find(L'+')) {
        auto name = std::wstring(trim(text.substr(0, separator)));
        CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
        UINT modifier = 0;
        if (name == L"ctrl" || name == L"control") modifier = MOD_CONTROL;
        else if (name == L"alt") modifier = MOD_ALT;
        else if (name == L"shift") modifier = MOD_SHIFT;
        else if (name == L"meta" || name == L"win") modifier = MOD_WIN;
        if (!modifier || (shortcut.modifiers & modifier)) return std::nullopt;
        shortcut.modifiers |= modifier;
        text.remove_prefix(separator + 1);
    }
    if (!(shortcut.modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN))) return std::nullopt;
    auto key = std::wstring(trim(text));
    CharUpperBuffW(key.data(), static_cast<DWORD>(key.size()));
    if (key.size() == 1 && ((key[0] >= L'A' && key[0] <= L'Z') || (key[0] >= L'0' && key[0] <= L'9'))) {
        shortcut.key = key[0];
    } else if ((key.size() == 2 || key.size() == 3) && key[0] == L'F' && key[1] >= L'1' && key[1] <= L'9') {
        int number = key[1] - L'0';
        if (key.size() == 3 && key[2] >= L'0' && key[2] <= L'9') number = number * 10 + key[2] - L'0';
        else if (key.size() == 3) return std::nullopt;
        if (number > 24 || number == 12) return std::nullopt;
        shortcut.key = static_cast<UINT>(VK_F1 + number - 1);
    } else return std::nullopt;
    if (shortcut.modifiers & MOD_CONTROL) shortcut.text += L"Ctrl+";
    if (shortcut.modifiers & MOD_ALT) shortcut.text += L"Alt+";
    if (shortcut.modifiers & MOD_SHIFT) shortcut.text += L"Shift+";
    if (shortcut.modifiers & MOD_WIN) shortcut.text += L"Meta+";
    shortcut.text += key;
    return shortcut;
}

std::wstring readPreference(const std::filesystem::path& path, PCWSTR key, PCWSTR fallback)
{
    wchar_t text[128]{};
    const auto length = GetPrivateProfileStringW(groupName, key, fallback, text, ARRAYSIZE(text), path.c_str());
    return std::wstring(text, length);
}

bool readBool(const std::filesystem::path& path, PCWSTR key)
{
    auto value = readPreference(path, key, L"true");
    CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value != L"false" && value != L"0";
}

bool savePreferences(const std::filesystem::path& directory, const DesktopPreferences& preferences)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    GUID id{};
    wchar_t suffix[39]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, suffix, ARRAYSIZE(suffix))) return false;
    const auto path = directory / L"desktop.ini";
    const auto temporary = directory / (std::wstring(L"desktop-") + suffix + L".tmp");
    std::ofstream output(temporary, std::ios::binary);
    if (!output) return false;
    const auto value = [](bool enabled) { return enabled ? "true" : "false"; };
    const auto ascii = [](const std::wstring& text) {
        std::string bytes;
        bytes.reserve(text.size());
        for (wchar_t character : text) bytes += static_cast<char>(character);
        return bytes;
    };
    output << "[desktop]\ncloseToTray=" << value(preferences.closeToTray)
           << "\nselectionEnabled=" << value(preferences.selectionEnabled)
           << "\nshortcutsEnabled=" << value(preferences.shortcutsEnabled)
           << "\nshowShortcut=" << ascii(preferences.showShortcut)
           << "\nselectionShortcut=" << ascii(preferences.selectionShortcut) << '\n';
    output.close();
    if (output && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    std::filesystem::remove(temporary, error);
    return false;
}

std::string utf8(std::wstring_view value)
{
    const auto length = static_cast<int>(value.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring profileChannel(const std::filesystem::path& path)
{
    struct Provider {
        BCRYPT_ALG_HANDLE handle = nullptr;
        ~Provider() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    } algorithm;
    std::wstring identity = path.generic_wstring();
    CharLowerBuffW(identity.data(), static_cast<DWORD>(identity.size()));
    auto bytes = utf8(identity);
    std::array<UCHAR, 32> digest{};
    if (bytes.empty() || !BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
        || !BCRYPT_SUCCESS(BCryptHash(algorithm.handle, nullptr, 0, reinterpret_cast<PUCHAR>(bytes.data()),
            static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())))) return {};
    std::wstring name(channelPrefix);
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (auto byte : digest) {
        name += hex[byte >> 4];
        name += hex[byte & 15];
    }
    return name;
}

struct HandleCloser {
    void operator()(void* handle) const { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

std::wstring legacyPipe(std::wstring_view channel)
{
    return L"\\\\.\\pipe\\Yiyi-" + std::wstring(channel.substr(channelPrefix.size(), 32));
}

bool awaitPipe(HANDLE pipe, OVERLAPPED& operation, DWORD& transferred, HANDLE stop, DWORD milliseconds)
{
    const HANDLE events[] = {operation.hEvent, stop};
    if (WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, milliseconds) == WAIT_OBJECT_0) {
        return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
    }
    CancelIoEx(pipe, &operation);
    GetOverlappedResult(pipe, &operation, &transferred, TRUE);
    return false;
}

DWORD transferPipe(HANDLE pipe, char* data, DWORD size, bool write, HANDLE stop, DWORD milliseconds)
{
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return 0;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD transferred = 0;
    const BOOL completed = write ? WriteFile(pipe, data, size, &transferred, &operation)
                                 : ReadFile(pipe, data, size, &transferred, &operation);
    if (completed || (GetLastError() == ERROR_IO_PENDING && awaitPipe(pipe, operation, transferred, stop, milliseconds))) {
        return transferred;
    }
    return 0;
}

DWORD lockPid(HANDLE file)
{
    char data[32]{};
    DWORD read = 0;
    if (!ReadFile(file, data, sizeof(data), &read, nullptr)) return 0;
    const char* end = std::find(data, data + read, '\n');
    if (end == data + read) return 0;
    if (end > data && end[-1] == '\r') --end;
    DWORD pid = 0;
    const auto parsed = std::from_chars(data, end, pid);
    return parsed.ec == std::errc{} && parsed.ptr == end ? pid : 0;
}

bool activateLegacy(const std::filesystem::path& lock, const std::wstring& pipeName)
{
    Handle pipe(CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, nullptr));
    if (pipe.get() == INVALID_HANDLE_VALUE) return false;
    Handle file(CreateFileW(lock.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() != INVALID_HANDLE_VALUE) {
        if (const auto pid = lockPid(file.get())) AllowSetForegroundWindow(pid);
    }
    char command[] = "activate";
    char reply[2]{};
    return transferPipe(pipe.get(), command, 8, true, nullptr, 250) == 8
        && transferPipe(pipe.get(), reply, 2, false, nullptr, 300) == 2 && std::string_view(reply, 2) == "ok";
}

void clearStaleLock(const std::filesystem::path& path)
{
    Handle lock(CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (lock.get() == INVALID_HANDLE_VALUE) return;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(lock.get(), &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    const auto pid = lockPid(lock.get());
    if (!pid) return;
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(lock.get(), beginning, nullptr, FILE_BEGIN)) return;
    char data[64]{};
    DWORD read = 0;
    if (!ReadFile(lock.get(), data, sizeof(data), &read, nullptr)) return;
    const std::string_view contents(data, read);
    const auto first = contents.find('\n');
    if (first == std::string_view::npos) return;
    const auto name = contents.substr(first + 1);
    if (!name.starts_with("Yiyi\n") && !name.starts_with("Yiyi\r\n")) return;
    const Handle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
    const bool ended = process ? WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0 : GetLastError() == ERROR_INVALID_PARAMETER;
    if (!ended) return;
    FILE_DISPOSITION_INFO disposition{TRUE};
    SetFileInformationByHandle(lock.get(), FileDispositionInfo, &disposition, sizeof(disposition));
}

HANDLE createProfileLock(const std::filesystem::path& path)
{
    Handle lock(CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    if (lock.get() == INVALID_HANDLE_VALUE) return nullptr;
    wchar_t computer[256]{};
    DWORD length = ARRAYSIZE(computer);
    if (!GetComputerNameW(computer, &length)) length = 0;
    std::wstring executable(32768, L'\0');
    const auto exeLength = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!exeLength || exeLength >= executable.size()) return nullptr;
    executable.resize(exeLength);
    const auto appName = utf8(std::filesystem::path(executable).stem().native());
    const auto data = std::to_string(GetCurrentProcessId()) + '\n' + appName + '\n' + utf8({computer, length}) + "\n\n\n";
    DWORD written = 0;
    if (!WriteFile(lock.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr) || written != data.size()) return nullptr;
    return lock.release();
}

PSECURITY_DESCRIPTOR userPipeSecurity()
{
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return nullptr;
    const Handle token(raw);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (!size) return nullptr;
    std::vector<unsigned char> buffer(size);
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) return nullptr;
    const auto user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid)) return nullptr;
    const std::unique_ptr<wchar_t, decltype(&LocalFree)> sidText(sid, LocalFree);
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)
        ? descriptor : nullptr;
}

class LegacyActivation {
public:
    bool listen(HWND broker, const std::wstring& name)
    {
        const std::unique_ptr<void, decltype(&LocalFree)> descriptor(userPipeSecurity(), LocalFree);
        if (!descriptor) return false;
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
        m_pipe.reset(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 64, 64, 0, &security));
        if (m_pipe.get() == INVALID_HANDLE_VALUE) return false;
        m_stop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!m_stop) return false;
        try {
            m_worker = std::thread([this, broker] { serve(broker); });
            return true;
        } catch (const std::system_error&) {
            stop();
            return false;
        }
    }

    void stop()
    {
        if (m_worker.joinable()) {
            SetEvent(m_stop.get());
            m_worker.join();
        }
        m_stop.reset();
        m_pipe.reset();
    }

    ~LegacyActivation() { stop(); }

private:
    void serve(HWND broker)
    {
        const Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) return;
        const auto pipe = m_pipe.get();
        const auto stop = m_stop.get();
        while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
            ResetEvent(event.get());
            OVERLAPPED operation{};
            operation.hEvent = event.get();
            DWORD count = 0;
            if (!ConnectNamedPipe(pipe, &operation)) {
                const auto error = GetLastError();
                if (error != ERROR_PIPE_CONNECTED && (error != ERROR_IO_PENDING || !awaitPipe(pipe, operation, count, stop, INFINITE))) break;
            }
            char command[8]{};
            count = 0;
            while (count != sizeof(command)) {
                const auto read = transferPipe(pipe, command + count, static_cast<DWORD>(sizeof(command)) - count, false, stop, 500);
                if (!read) break;
                count += read;
            }
            if (count == sizeof(command) && std::string_view(command, count) == "activate") {
                PostMessageW(broker, activateMessage, 0, 0);
                char reply[] = "ok";
                if (transferPipe(pipe, reply, 2, true, stop, 250) == 2) {
                    // Give the client time to read and disconnect before reusing the pipe.
                    transferPipe(pipe, command, 1, false, stop, 1000);
                }
            }
            DisconnectNamedPipe(pipe);
        }
    }

    Handle m_pipe;
    Handle m_stop;
    std::thread m_worker;
};

bool ownWindow(HWND window)
{
    DWORD pid = 0;
    if (window) GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId();
}

long long pointDistance(POINT first, POINT second)
{
    return std::abs(static_cast<long long>(first.x) - second.x) + std::abs(static_cast<long long>(first.y) - second.y);
}

std::wstring selectionMessage(SelectionStatus status)
{
    switch (status) {
    case SelectionStatus::Protected: return L"此输入框不允许取词。";
    case SelectionStatus::TooLong: return L"选中文字超过 24,000 字符，请缩短选择范围。";
    case SelectionStatus::SourceChanged: return L"取词时窗口已切换，请重新选择。";
    case SelectionStatus::TimedOut: return L"取词超时。可以复制文字后在译意中粘贴查询。";
    case SelectionStatus::ClipboardChanged: return L"剪贴板已变化，请重试。";
    case SelectionStatus::ClipboardUnavailable: return L"无法完整保存剪贴板内容，请手动复制后粘贴查询。";
    case SelectionStatus::CopyFailed: return L"无法向此应用发送复制操作，请手动复制后粘贴查询。";
    default: return L"未读取到选中文字。请确认文字可复制，或手动复制后粘贴查询。";
    }
}

}

DesktopPreferences loadDesktopPreferences(const std::filesystem::path& directory)
{
    auto path = std::filesystem::absolute(directory) / L"desktop.ini";
    if (!std::filesystem::exists(path)) path = path.parent_path() / L"settings.ini";
    DesktopPreferences preferences;
    preferences.closeToTray = readBool(path, L"closeToTray");
    preferences.shortcutsEnabled = readBool(path, L"shortcutsEnabled");
    preferences.selectionEnabled = readBool(path, L"selectionEnabled");
    if (auto shortcut = parseShortcut(readPreference(path, L"showShortcut", preferences.showShortcut.c_str()))) {
        preferences.showShortcut = std::move(shortcut->text);
    }
    if (auto shortcut = parseShortcut(readPreference(path, L"selectionShortcut", preferences.selectionShortcut.c_str()))) {
        preferences.selectionShortcut = std::move(shortcut->text);
    }
    return preferences;
}

ProfileInstance::~ProfileInstance()
{
    if (m_profileLock) CloseHandle(m_profileLock);
    if (m_mutex) {
        ReleaseMutex(m_mutex);
        CloseHandle(m_mutex);
    }
}

ProfileInstance::Result ProfileInstance::acquire(const std::filesystem::path& directory)
{
    if (m_mutex) return Primary;
    m_error.clear();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto canonical = error ? std::filesystem::path{} : std::filesystem::canonical(directory, error);
    if (error || (m_channel = profileChannel(canonical)).empty()) {
        m_error = L"无法打开数据目录，请检查目录权限。";
        return Failed;
    }
    const auto name = L"Local\\" + m_channel;
    const HANDLE mutex = CreateMutexW(nullptr, TRUE, name.c_str());
    const DWORD state = GetLastError();
    if (!mutex) {
        m_error = L"无法创建该工作区的单实例锁。";
        return Failed;
    }
    const auto deadline = GetTickCount64() + 2500;
    bool ownsMutex = state != ERROR_ALREADY_EXISTS;
    const auto lockPath = canonical / L"desktop.lock";
    const auto pipe = legacyPipe(m_channel);
    do {
        if (!ownsMutex) {
            const auto ownership = WaitForSingleObject(mutex, 25);
            ownsMutex = ownership == WAIT_OBJECT_0 || ownership == WAIT_ABANDONED;
            if (ownership == WAIT_FAILED) break;
        }
        if (ownsMutex) {
            m_profileLock = createProfileLock(lockPath);
            if (m_profileLock) {
                m_mutex = mutex;
                return Primary;
            }
            if (activateLegacy(lockPath, pipe)) {
                ReleaseMutex(mutex);
                CloseHandle(mutex);
                return ActivatedExisting;
            }
            clearStaleLock(lockPath);
            Sleep(30);
        } else if (const HWND broker = FindWindowW(brokerClass, m_channel.c_str())) {
            DWORD pid = 0;
            GetWindowThreadProcessId(broker, &pid);
            if (pid) AllowSetForegroundWindow(pid);
            DWORD_PTR reply = 0;
            if (SendMessageTimeoutW(broker, activateMessage, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &reply)
                && reply == activatedReply) {
                CloseHandle(mutex);
                return ActivatedExisting;
            }
        }
    } while (GetTickCount64() < deadline);
    if (ownsMutex) ReleaseMutex(mutex);
    CloseHandle(mutex);
    m_error = L"译意正在启动或退出，请稍后重试。若问题持续，请检查数据目录。";
    return Failed;
}

struct DesktopIntegration::State {
    State(HWND window, std::filesystem::path directory, std::wstring channel, DesktopCallbacks callbacks)
        : window(window), directory(std::move(directory)), channel(std::move(channel)), callbacks(std::move(callbacks)),
          preferences(loadDesktopPreferences(this->directory)) {}
    ~State();

    bool start();
    void addTrayIcon();
    void removeTrayIcon();
    void showMenu();
    void showWindow();
    void requestQuit();
    bool hideWindow();
    bool registerShortcuts(bool enabled, std::wstring_view show, std::wstring_view selection);
    void updateSelectionHook();
    void save();
    void setSelectionEnabled(bool enabled);
    void captureSelection();
    void requestSelection(HWND source, POINT point, bool automatic);
    void processMouse(UINT event, POINT point);
    void pollSelection();
    void showBubble(const SelectionResult& result);
    void paintBubbleButton(const DRAWITEMSTRUCT& item) const;
    void dismissBubble();
    void selectBubble();
    void onTimer(UINT_PTR timer);
    LRESULT dispatch(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    static LRESULT CALLBACK brokerProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK bubbleProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK bubbleButtonProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR reference);
    static LRESULT CALLBACK closeProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR state);
    static LRESULT CALLBACK mouseProc(int code, WPARAM event, LPARAM pointer);
    static thread_local State* hookOwner;

    HWND window = nullptr;
    HWND broker = nullptr;
    HWND bubble = nullptr;
    HWND bubbleButton = nullptr;
    COLORREF bubbleBackground = GetSysColor(COLOR_HIGHLIGHT);
    COLORREF bubbleHover = GetSysColor(COLOR_HIGHLIGHT);
    COLORREF bubbleForeground = GetSysColor(COLOR_HIGHLIGHTTEXT);
    std::filesystem::path directory;
    std::wstring channel;
    DesktopCallbacks callbacks;
    DesktopPreferences preferences;
    std::wstring shortcutError;
    std::wstring selectionError;
    SelectionCapture capture;
    LegacyActivation legacyActivation;
    std::map<int, Shortcut> hotkeys;
    NOTIFYICONDATAW tray{};
    UINT taskbarMessage = 0;
    int showId = 0;
    int selectionId = 0;
    bool trayReady = false;
    bool trayVersion4 = false;
    bool exiting = false;
    bool systemClosing = false;
    bool closeNoticeShown = false;
    bool recording = false;
    bool wasMaximized = false;
    HHOOK hook = nullptr;
    HWND selectionSource = nullptr;
    HWND bubbleSource = nullptr;
    POINT mousePress{};
    POINT lastPress{};
    POINT release{};
    ULONGLONG lastPressTime = 0;
    ULONGLONG bubbleDeadline = 0;
    bool pressed = false;
    bool doubleClick = false;
    bool bubbleHovered = false;
    std::wstring bubbleText;
};

thread_local DesktopIntegration::State* DesktopIntegration::State::hookOwner = nullptr;

DesktopIntegration::State::~State()
{
    exiting = true;
    if (window) RemoveWindowSubclass(window, closeProc, closeSubclassId);
    if (hook) UnhookWindowsHookEx(hook);
    if (hookOwner == this) hookOwner = nullptr;
    capture.cancel();
    legacyActivation.stop();
    if (bubble) DestroyWindow(bubble);
    if (broker) {
        for (const auto& pair : hotkeys) UnregisterHotKey(broker, pair.first);
        removeTrayIcon();
        DestroyWindow(broker);
    }
}

bool DesktopIntegration::State::start()
{
    if (broker) return true;
    if (!window || channel.empty()) return false;
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = brokerProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = brokerClass;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    broker = CreateWindowExW(WS_EX_TOOLWINDOW, brokerClass, channel.c_str(), WS_POPUP, 0, 0, 0, 0,
                             nullptr, nullptr, instance, this);
    if (!broker) return false;
    if (!SetWindowSubclass(window, closeProc, closeSubclassId, reinterpret_cast<DWORD_PTR>(this))) {
        DestroyWindow(broker);
        broker = nullptr;
        return false;
    }
    taskbarMessage = RegisterWindowMessageW(L"TaskbarCreated");
    legacyActivation.listen(broker, legacyPipe(channel));
    addTrayIcon();
    registerShortcuts(preferences.shortcutsEnabled, preferences.showShortcut, preferences.selectionShortcut);
    updateSelectionHook();
    return true;
}

void DesktopIntegration::State::addTrayIcon()
{
    trayReady = false;
    trayVersion4 = false;
    const auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_YIYI), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (!icon) return;

    tray = {};
    tray.cbSize = sizeof(tray);
    tray.hWnd = broker;
    tray.uID = trayId;
    tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    tray.uCallbackMessage = trayMessage;
    tray.hIcon = icon;
    wcscpy_s(tray.szTip, L"译意 · AI 翻译词典");
    trayReady = Shell_NotifyIconW(NIM_ADD, &tray) != FALSE;
    DestroyIcon(icon);
    tray.hIcon = nullptr;
    tray.uVersion = NOTIFYICON_VERSION_4;
    trayVersion4 = trayReady && Shell_NotifyIconW(NIM_SETVERSION, &tray);
}

void DesktopIntegration::State::removeTrayIcon()
{
    if (trayReady) Shell_NotifyIconW(NIM_DELETE, &tray);
    trayReady = false;
}

bool DesktopIntegration::State::hideWindow()
{
    if (!window || exiting || systemClosing || !trayReady || !preferences.closeToTray) return false;
    dismissBubble();
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    wasMaximized = IsZoomed(window) != FALSE || (GetWindowPlacement(window, &placement)
        && (placement.flags & WPF_RESTORETOMAXIMIZED) != 0);
    ShowWindow(window, SW_HIDE);
    if (!closeNoticeShown) {
        closeNoticeShown = true;
        auto notice = tray;
        notice.uFlags = NIF_INFO;
        notice.dwInfoFlags = NIIF_INFO;
        wcscpy_s(notice.szInfoTitle, L"译意仍在运行");
        wcscpy_s(notice.szInfo, L"点击托盘图标可打开窗口，右键托盘菜单可退出。");
        Shell_NotifyIconW(NIM_MODIFY, &notice);
    }
    return true;
}

void DesktopIntegration::State::showWindow()
{
    dismissBubble();
    if (!window) return;
    if (!IsWindowVisible(window)) ShowWindow(window, wasMaximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    else if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
}

void DesktopIntegration::State::requestQuit()
{
    exiting = true;
    dismissBubble();
    capture.cancel();
    removeTrayIcon();
    if (window) PostMessageW(window, WM_CLOSE, 0, 0);
}

void DesktopIntegration::State::showMenu()
{
    const HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, OpenWorkspace, L"打开译意");
    AppendMenuW(menu, callbacks.screenshot ? MF_STRING : MF_STRING | MF_GRAYED, Screenshot, L"截图取字");
    AppendMenuW(menu, callbacks.settings ? MF_STRING : MF_STRING | MF_GRAYED, Settings, L"偏好设置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (preferences.selectionEnabled ? MF_CHECKED : 0), ToggleSelection, L"划词浮标");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Quit, L"退出译意");
    SetMenuDefaultItem(menu, OpenWorkspace, FALSE);
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(broker);
    const auto command = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD, point.x, point.y, broker, nullptr);
    PostMessageW(broker, WM_NULL, 0, 0);
    DestroyMenu(menu);
    switch (command) {
    case OpenWorkspace: showWindow(); break;
    case Screenshot: showWindow(); if (callbacks.screenshot) callbacks.screenshot(); break;
    case Settings: showWindow(); if (callbacks.settings) callbacks.settings(); break;
    case ToggleSelection: setSelectionEnabled(!preferences.selectionEnabled); break;
    case Quit: requestQuit(); break;
    default: break;
    }
}

bool DesktopIntegration::State::registerShortcuts(bool enabled, std::wstring_view show, std::wstring_view selection)
{
    shortcutError.clear();
    const auto first = parseShortcut(show), second = parseShortcut(selection);
    if (enabled && (!first || !second || first->text == second->text)) {
        shortcutError = L"请设置两个不同的组合键，使用 Ctrl、Alt 或 Win 配合字母、数字或 F1–F24（F12 除外）。";
        return false;
    }
    if (!broker) return true;
    std::map<int, Shortcut> next;
    int assigned[2]{};
    if (enabled) {
        int index = 0;
        for (const auto& shortcut : {*first, *second}) {
            const auto previous = std::find_if(hotkeys.begin(), hotkeys.end(), [&](const auto& item) {
                return item.second.text == shortcut.text;
            });
            int id = previous == hotkeys.end() ? 0x5900 : previous->first;
            if (previous == hotkeys.end()) {
                while (hotkeys.contains(id) || next.contains(id)) ++id;
                if (!RegisterHotKey(broker, id, shortcut.modifiers | MOD_NOREPEAT, shortcut.key)) {
                    for (const auto& pair : next) if (!hotkeys.contains(pair.first)) UnregisterHotKey(broker, pair.first);
                    shortcutError = shortcut.text + L" 已被占用或被系统保留，请换一个组合键。";
                    return false;
                }
            }
            next.emplace(id, shortcut);
            assigned[index++] = id;
        }
    }
    for (const auto& pair : hotkeys) if (!next.contains(pair.first)) UnregisterHotKey(broker, pair.first);
    hotkeys = std::move(next);
    showId = assigned[0];
    selectionId = assigned[1];
    return true;
}

void DesktopIntegration::State::save()
{
    if (!savePreferences(directory, preferences) && callbacks.error) {
        callbacks.error(L"桌面偏好设置未能写入磁盘，请检查数据目录权限。");
    }
    if (callbacks.preferencesChanged) callbacks.preferencesChanged();
}

void DesktopIntegration::State::setSelectionEnabled(bool enabled)
{
    if (preferences.selectionEnabled == enabled) return;
    preferences.selectionEnabled = enabled;
    updateSelectionHook();
    save();
}

void DesktopIntegration::State::updateSelectionHook()
{
    selectionError.clear();
    if (broker) KillTimer(broker, selectDelayTimer);
    capture.cancel();
    dismissBubble();
    pressed = false;
    if (hook) UnhookWindowsHookEx(hook);
    hook = nullptr;
    if (hookOwner == this) hookOwner = nullptr;
    if (broker && preferences.selectionEnabled) {
        if (!hookOwner) hook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
        if (hook) hookOwner = this;
        else selectionError = L"划词监听未能启动，可使用全局取词快捷键。关闭再打开此开关可重试。";
    }
}

void DesktopIntegration::State::captureSelection()
{
    if (capture.busy() || !broker) return;
    KillTimer(broker, selectDelayTimer);
    dismissBubble();
    const auto source = GetForegroundWindow();
    if (!source || ownWindow(source)) {
        if (callbacks.error) callbacks.error(L"请先在其他应用中选中文字，再按取词快捷键。");
        return;
    }
    POINT point{};
    GetCursorPos(&point);
    requestSelection(source, point, false);
}

void DesktopIntegration::State::requestSelection(HWND source, POINT point, bool automatic)
{
    if (capture.request(broker, source, point, automatic)) {
        if (SetTimer(broker, selectPollTimer, 25, nullptr)) return;
        capture.cancel();
    }
    if (!automatic && callbacks.error) callbacks.error(L"未能启动取词。请手动复制后粘贴查询。");
}

void DesktopIntegration::State::processMouse(UINT event, POINT point)
{
    if (!hook || !preferences.selectionEnabled) return;
    const auto underPointer = WindowFromPoint(point);
    if (ownWindow(underPointer)) return;
    if (event != mouseUpMessage) {
        dismissBubble();
        KillTimer(broker, selectDelayTimer);
        capture.cancel();
    }
    if (event == mouseDownMessage) {
        const auto now = GetTickCount64();
        doubleClick = lastPressTime && now - lastPressTime <= GetDoubleClickTime() && pointDistance(lastPress, point) <= 8;
        lastPressTime = now;
        lastPress = mousePress = point;
        pressed = true;
        selectionSource = GetAncestor(underPointer, GA_ROOT);
    } else if (event == mouseUpMessage && pressed) {
        pressed = false;
        if ((pointDistance(point, mousePress) < 6 && !doubleClick) || selectionSource != GetForegroundWindow()) return;
        release = point;
        SetTimer(broker, selectDelayTimer, 180, nullptr);
    } else if (event == mouseDismissMessage) pressed = false;
}

void DesktopIntegration::State::pollSelection()
{
    auto result = capture.poll();
    if (!capture.busy()) KillTimer(broker, selectPollTimer);
    if (!result) return;
    if (result->automatic) {
        if (result->status == SelectionStatus::Captured && preferences.selectionEnabled) showBubble(*result);
    } else {
        showWindow();
        if (result->status == SelectionStatus::Captured) {
            if (callbacks.lookup) callbacks.lookup(result->text);
        } else if (callbacks.error) callbacks.error(selectionMessage(result->status));
    }
}

void DesktopIntegration::State::showBubble(const SelectionResult& result)
{
    if (GetForegroundWindow() != result.source || ownWindow(result.source)) return;
    if (!bubble) {
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW klass{};
        klass.lpfnWndProc = bubbleProc;
        klass.hInstance = instance;
        klass.hCursor = LoadCursor(nullptr, IDC_HAND);
        klass.lpszClassName = bubbleClass;
        if (!RegisterClassW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        bubble = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, bubbleClass, L"译意划词",
            WS_POPUP, release.x, release.y, bubbleWidth, bubbleHeight, nullptr, nullptr, instance, this);
        if (!bubble) return;
        bubbleButton = CreateWindowExW(0, L"BUTTON", bubbleCaption, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, bubbleWidth, bubbleHeight, bubble, reinterpret_cast<HMENU>(AcceptSelection), instance, nullptr);
        if (!bubbleButton) {
            DestroyWindow(bubble);
            bubble = nullptr;
            return;
        }
        SetWindowSubclass(bubbleButton, bubbleButtonProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromPoint(release, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    SetWindowPos(bubble, HWND_TOPMOST, release.x, release.y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
    const auto dpi = static_cast<int>(GetDpiForWindow(bubble));
    const int width = MulDiv(bubbleWidth, dpi, 96), height = MulDiv(bubbleHeight, dpi, 96);
    const auto x = std::clamp(release.x + MulDiv(12, dpi, 96), monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width));
    const auto y = std::clamp(release.y + MulDiv(18, dpi, 96), monitor.rcWork.top, std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
    const int diameter = MulDiv(2 * bubbleRadius, dpi, 96);
    if (const auto region = CreateRoundRectRgn(0, 0, width, height, diameter, diameter)) {
        if (!SetWindowRgn(bubble, region, FALSE)) DeleteObject(region);
    }
    MoveWindow(bubbleButton, 0, 0, width, height, TRUE);
    bubbleSource = result.source;
    bubbleText = result.text;
    bubbleDeadline = GetTickCount64() + 8000;
    SetWindowPos(bubble, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(broker, bubbleTimer, 150, nullptr);
}

void DesktopIntegration::State::paintBubbleButton(const DRAWITEMSTRUCT& item) const
{
    const auto dc = item.hDC;
    const auto saved = SaveDC(dc);
    if (!saved) return;
    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON);
    const bool hover = bubbleHovered && !(item.itemState & ODS_SELECTED);
    SetDCBrushColor(dc, highContrast ? GetSysColor(COLOR_HIGHLIGHT) : hover ? bubbleHover : bubbleBackground);
    FillRect(dc, &item.rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : bubbleForeground);

    const int dpi = static_cast<int>(GetDpiForWindow(item.hwndItem));
    const auto font = [dpi](int size, int weight, const wchar_t* family) {
        return CreateFontW(-MulDiv(size, dpi, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
    };
    const auto iconFont = font(14, FW_NORMAL, L"Segoe MDL2 Assets");
    const auto textFont = font(12, FW_SEMIBOLD, L"Microsoft YaHei UI");
    auto label = item.rcItem;
    label.left += MulDiv(10, dpi, 96);
    if (iconFont) {
        SelectObject(dc, iconFont);
        RECT icon{label.left, label.top, label.left + MulDiv(16, dpi, 96), label.bottom};
        DrawTextW(dc, L"\uE8C1", 1, &icon, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        label.left = icon.right + MulDiv(5, dpi, 96);
    }
    SelectObject(dc, textFont ? textFont : GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(dc, bubbleCaption, -1, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    RestoreDC(dc, saved);
    if (iconFont) DeleteObject(iconFont);
    if (textFont) DeleteObject(textFont);
}

void DesktopIntegration::State::dismissBubble()
{
    if (broker) KillTimer(broker, bubbleTimer);
    if (bubble) ShowWindow(bubble, SW_HIDE);
    bubbleHovered = false;
    bubbleSource = nullptr;
    bubbleText.clear();
}

void DesktopIntegration::State::selectBubble()
{
    auto text = std::move(bubbleText);
    dismissBubble();
    if (text.empty() || !callbacks.lookup) return;
    showWindow();
    callbacks.lookup(text);
}

void DesktopIntegration::State::onTimer(UINT_PTR timer)
{
    if (timer == selectDelayTimer) {
        KillTimer(broker, selectDelayTimer);
        if (preferences.selectionEnabled) requestSelection(selectionSource, release, true);
    } else if (timer == selectPollTimer) pollSelection();
    else if (timer == bubbleTimer && (GetTickCount64() >= bubbleDeadline || GetForegroundWindow() != bubbleSource
        || (GetAsyncKeyState(VK_ESCAPE) & 0x8000))) dismissBubble();
}

LRESULT DesktopIntegration::State::dispatch(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == taskbarMessage && taskbarMessage) {
        addTrayIcon();
        if (callbacks.preferencesChanged) callbacks.preferencesChanged();
        return 0;
    }
    switch (message) {
    case activateMessage: showWindow(); return activatedReply;
    case trayMessage:
        switch (trayVersion4 ? LOWORD(lparam) : lparam) {
        case WM_CONTEXTMENU: case WM_RBUTTONUP: showMenu(); break;
        case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: showWindow(); break;
        default: break;
        }
        return 0;
    case WM_HOTKEY: {
        const auto key = hotkeys.find(static_cast<int>(wparam));
        if (key == hotkeys.end()) return 0;
        if (recording) {
            if (callbacks.shortcutRecorded) callbacks.shortcutRecorded(key->second.text);
        } else if (key->first == showId) {
            showWindow();
            if (callbacks.workspace) callbacks.workspace();
        } else if (key->first == selectionId) captureSelection();
        return 0;
    }
    case mouseDownMessage: case mouseUpMessage: case mouseDismissMessage:
        processMouse(message, POINT{static_cast<LONG>(wparam), static_cast<LONG>(lparam)});
        return 0;
    case WM_TIMER: onTimer(wparam); return 0;
    default: return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

LRESULT CALLBACK DesktopIntegration::State::brokerProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        const auto info = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(info->lpCreateParams));
    }
    const auto state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state) {
        try { return state->dispatch(hwnd, message, wparam, lparam); }
        catch (...) { OutputDebugStringW(L"Yiyi desktop callback failed.\n"); }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK DesktopIntegration::State::bubbleProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        const auto info = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(info->lpCreateParams));
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_DRAWITEM && wparam == AcceptSelection) {
        const auto state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (state) state->paintBubbleButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == AcceptSelection && HIWORD(wparam) == BN_CLICKED) {
        const auto state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (state) {
            try { state->selectBubble(); }
            catch (...) { OutputDebugStringW(L"Yiyi selection callback failed.\n"); }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK DesktopIntegration::State::bubbleButtonProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                                             UINT_PTR id, DWORD_PTR reference)
{
    const auto state = reinterpret_cast<State*>(reference);
    switch (message) {
    case WM_ERASEBKGND:
        return TRUE;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    case WM_MOUSEMOVE:
        if (!state->bubbleHovered) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
            if (TrackMouseEvent(&tracking)) {
                state->bubbleHovered = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        break;
    case WM_MOUSELEAVE:
        state->bubbleHovered = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, bubbleButtonProc, id);
        state->bubbleButton = nullptr;
        break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK DesktopIntegration::State::closeProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                                       UINT_PTR id, DWORD_PTR reference)
{
    const auto state = reinterpret_cast<State*>(reference);
    if (message == WM_CLOSE && state->hideWindow()) return 0;
    if (message == WM_QUERYENDSESSION) state->systemClosing = true;
    if (message == WM_ENDSESSION && !wparam) state->systemClosing = false;
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, closeProc, id);
        state->window = nullptr;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK DesktopIntegration::State::mouseProc(int code, WPARAM event, LPARAM pointer)
{
    if (code == HC_ACTION && hookOwner) {
        UINT message = 0;
        if (event == WM_LBUTTONDOWN) message = mouseDownMessage;
        else if (event == WM_LBUTTONUP) message = mouseUpMessage;
        else if (event == WM_RBUTTONDOWN || event == WM_MOUSEWHEEL || event == WM_MOUSEHWHEEL) message = mouseDismissMessage;
        if (message) {
            const auto point = reinterpret_cast<const MSLLHOOKSTRUCT*>(pointer)->pt;
            PostMessageW(hookOwner->broker, message, static_cast<WPARAM>(point.x), static_cast<LPARAM>(point.y));
        }
    }
    return CallNextHookEx(nullptr, code, event, pointer);
}

DesktopIntegration::DesktopIntegration(HWND mainWindow, std::filesystem::path directory, std::wstring channel,
                                       DesktopCallbacks callbacks)
    : m_state(std::make_unique<State>(mainWindow, std::move(directory), std::move(channel), std::move(callbacks))) {}
DesktopIntegration::~DesktopIntegration() = default;

bool DesktopIntegration::start() { return m_state->start(); }
bool DesktopIntegration::trayAvailable() const { return m_state->trayReady; }
const DesktopPreferences& DesktopIntegration::preferences() const { return m_state->preferences; }
const std::wstring& DesktopIntegration::shortcutError() const { return m_state->shortcutError; }
const std::wstring& DesktopIntegration::selectionError() const { return m_state->selectionError; }

void DesktopIntegration::setCloseToTray(bool enabled)
{
    if (m_state->preferences.closeToTray == enabled) return;
    m_state->preferences.closeToTray = enabled;
    m_state->save();
}

void DesktopIntegration::setSelectionEnabled(bool enabled) { m_state->setSelectionEnabled(enabled); }

void DesktopIntegration::setSelectionBubbleColors(COLORREF background, COLORREF hover, COLORREF foreground)
{
    m_state->bubbleBackground = background;
    m_state->bubbleHover = hover;
    m_state->bubbleForeground = foreground;
    if (m_state->bubbleButton) InvalidateRect(m_state->bubbleButton, nullptr, FALSE);
}

bool DesktopIntegration::applyShortcuts(bool enabled, std::wstring_view show, std::wstring_view selection)
{
    if (!m_state->registerShortcuts(enabled, show, selection)) return false;
    m_state->preferences.shortcutsEnabled = enabled;
    if (auto shortcut = parseShortcut(show)) m_state->preferences.showShortcut = std::move(shortcut->text);
    if (auto shortcut = parseShortcut(selection)) m_state->preferences.selectionShortcut = std::move(shortcut->text);
    m_state->save();
    return true;
}

std::wstring DesktopIntegration::normalizedShortcut(std::wstring_view text)
{
    const auto shortcut = parseShortcut(text);
    return shortcut ? shortcut->text : std::wstring{};
}

void DesktopIntegration::setRecordingShortcut(bool recording) { m_state->recording = recording; }
void DesktopIntegration::showWindow() { m_state->showWindow(); }
void DesktopIntegration::quit() { m_state->requestQuit(); }
void DesktopIntegration::captureSelection() { m_state->captureSelection(); }

}
