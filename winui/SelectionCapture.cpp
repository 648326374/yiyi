#include "SelectionCapture.h"

#include <objbase.h>
#include <ole2.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "gdi32.lib")

namespace yiyi {
namespace {

using Microsoft::WRL::ComPtr;
using Status = SelectionStatus;

constexpr std::size_t textLimit = 24000;
constexpr std::size_t clipboardLimit = 16 * 1024 * 1024;
constexpr ULONGLONG probeTimeout = 1800;
constexpr ULONGLONG copyTimeout = 1200;

struct HandleCloser {
    void operator()(void* handle) const { if (handle) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

struct Reply {
    std::uint32_t status;
    std::uint32_t characters;
};

DWORD processOf(HWND window)
{
    DWORD pid = 0;
    if (window) GetWindowThreadProcessId(window, &pid);
    return pid;
}

GUITHREADINFO foregroundInfo(HWND window)
{
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    const auto thread = GetWindowThreadProcessId(window, nullptr);
    if (!thread || !GetGUIThreadInfo(thread, &info)) return {};
    return info;
}

bool nativePassword(HWND window)
{
    const auto focused = foregroundInfo(window).hwndFocus;
    wchar_t name[64]{};
    const int length = GetClassNameW(focused, name, ARRAYSIZE(name));
    CharLowerBuffW(name, static_cast<DWORD>(length));
    return std::wstring_view(name).find(L"edit") != std::wstring_view::npos
        && (GetWindowLongPtrW(focused, GWL_STYLE) & ES_PASSWORD);
}

std::wstring trim(std::wstring text)
{
    auto space = [](wchar_t c) { return std::iswspace(c) || c == 0x00A0 || c == 0x3000; };
    const auto first = std::find_if_not(text.begin(), text.end(), space);
    const auto last = std::find_if_not(text.rbegin(), text.rend(), space).base();
    return first < last ? std::wstring(first, last) : std::wstring{};
}

bool rangeNearPoint(IUIAutomationTextRange* range, POINT point)
{
    SAFEARRAY* rectangles = nullptr;
    if (FAILED(range->GetBoundingRectangles(&rectangles)) || !rectangles) return false;
    const std::unique_ptr<SAFEARRAY, decltype(&SafeArrayDestroy)> guard(rectangles, SafeArrayDestroy);
    LONG first = 0, last = -1;
    double* values = nullptr;
    if (SafeArrayGetDim(rectangles) != 1 || FAILED(SafeArrayGetLBound(rectangles, 1, &first))
        || FAILED(SafeArrayGetUBound(rectangles, 1, &last))
        || FAILED(SafeArrayAccessData(rectangles, reinterpret_cast<void**>(&values)))) return false;
    bool matches = false;
    const auto count = static_cast<long long>(last) - first + 1;
    for (long long i = 0; i + 3 < count; i += 4) {
        if (values[i + 2] <= 0 || values[i + 3] <= 0) continue;
        if (point.x >= values[i] - 14 && point.x <= values[i] + values[i + 2] + 14
            && point.y >= values[i + 1] - 14 && point.y <= values[i + 1] + values[i + 3] + 14) {
            matches = true;
            break;
        }
    }
    SafeArrayUnaccessData(rectangles);
    return matches;
}

struct ProbeResult {
    Status status = Status::Empty;
    std::wstring text;
};

ProbeResult readElement(IUIAutomation* automation, ComPtr<IUIAutomationElement> current,
                        DWORD pid, POINT point, bool automatic)
{
    ComPtr<IUIAutomationTreeWalker> walker;
    automation->get_ControlViewWalker(walker.GetAddressOf());
    for (int depth = 0; current && depth != 7; ++depth) {
        int elementPid = 0;
        BOOL password = FALSE;
        if (FAILED(current->get_CurrentProcessId(&elementPid)) || static_cast<DWORD>(elementPid) != pid) break;
        if (FAILED(current->get_CurrentIsPassword(&password)) || password) return {Status::Protected, {}};

        ComPtr<IUIAutomationTextPattern> pattern;
        ComPtr<IUIAutomationTextRangeArray> ranges;
        if (SUCCEEDED(current->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(pattern.GetAddressOf()))) && pattern
            && SUCCEEDED(pattern->GetSelection(ranges.GetAddressOf())) && ranges) {
            int count = 0;
            if (SUCCEEDED(ranges->get_Length(&count)) && count > 32) return {Status::TooLong, {}};
            std::wstring text;
            bool matches = !automatic;
            for (int i = 0; i < count; ++i) {
                ComPtr<IUIAutomationTextRange> range;
                if (FAILED(ranges->GetElement(i, range.GetAddressOf())) || !range) continue;
                BSTR value = nullptr;
                const auto status = range->GetText(static_cast<int>(textLimit + 1), &value);
                const std::unique_ptr<wchar_t, decltype(&SysFreeString)> guard(value, SysFreeString);
                if (FAILED(status) || !value || !SysStringLen(value)) continue;
                if (!text.empty()) text += L'\n';
                text.append(value, SysStringLen(value));
                if (text.size() > textLimit) return {Status::TooLong, {}};
                if (!matches) matches = rangeNearPoint(range.Get(), point);
            }
            if (matches) {
                auto value = trim(std::move(text));
                if (!value.empty()) return {Status::Captured, std::move(value)};
            }
        }
        ComPtr<IUIAutomationElement> parent;
        if (walker) walker->GetParentElement(current.Get(), parent.GetAddressOf());
        current = std::move(parent);
    }
    return {};
}

ProbeResult readSelection(HWND source, POINT point, bool automatic)
{
    if (!IsWindow(source) || GetForegroundWindow() != source) return {Status::SourceChanged, {}};
    if (nativePassword(source)) return {Status::Protected, {}};
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.GetAddressOf())))) return {Status::Unavailable, {}};
    const auto pid = processOf(source);
    ComPtr<IUIAutomationElement> element;
    if (automatic) {
        automation->ElementFromPoint(point, element.GetAddressOf());
        return readElement(automation.Get(), std::move(element), pid, point, true);
    }
    if (const auto focused = foregroundInfo(source).hwndFocus) {
        automation->ElementFromHandle(focused, element.GetAddressOf());
        auto result = readElement(automation.Get(), std::move(element), pid, point, false);
        if (result.status != Status::Empty) return result;
    }
    automation->GetFocusedElement(element.GetAddressOf());
    return readElement(automation.Get(), std::move(element), pid, point, false);
}

bool writeAll(HANDLE output, const void* data, DWORD remaining)
{
    auto bytes = static_cast<const char*>(data);
    while (remaining) {
        DWORD written = 0;
        if (!WriteFile(output, bytes, remaining, &written, nullptr) || !written) return false;
        bytes += written;
        remaining -= written;
    }
    return true;
}

struct ClipboardAccess {
    explicit ClipboardAccess(HWND owner) : opened(OpenClipboard(owner) != FALSE) {}
    ~ClipboardAccess() { if (opened) CloseClipboard(); }
    ClipboardAccess(ClipboardAccess const&) = delete;
    ClipboardAccess& operator=(ClipboardAccess const&) = delete;
    bool opened;
};

enum class ClipboardKind { Memory, Bitmap, Metafile, EnhancedMetafile, Palette, Unsupported };

ClipboardKind clipboardKind(UINT format)
{
    if (format == CF_BITMAP || format == CF_DSPBITMAP) return ClipboardKind::Bitmap;
    if (format == CF_METAFILEPICT || format == CF_DSPMETAFILEPICT) return ClipboardKind::Metafile;
    if (format == CF_ENHMETAFILE || format == CF_DSPENHMETAFILE) return ClipboardKind::EnhancedMetafile;
    if (format == CF_PALETTE) return ClipboardKind::Palette;
    if (format == CF_OWNERDISPLAY || (format >= CF_PRIVATEFIRST && format <= CF_PRIVATELAST)
        || (format >= CF_GDIOBJFIRST && format <= CF_GDIOBJLAST)) return ClipboardKind::Unsupported;
    return ClipboardKind::Memory;
}

struct ClipboardCopy {
    ClipboardCopy(UINT format, HANDLE data) : format(format), data(data) {}
    ClipboardCopy(ClipboardCopy&& other) noexcept : format(other.format), data(std::exchange(other.data, nullptr)) {}
    ClipboardCopy(ClipboardCopy const&) = delete;
    ClipboardCopy& operator=(ClipboardCopy const&) = delete;
    ~ClipboardCopy()
    {
        if (!data) return;
        switch (clipboardKind(format)) {
        case ClipboardKind::Bitmap:
        case ClipboardKind::Palette: DeleteObject(data); return;
        case ClipboardKind::EnhancedMetafile: DeleteEnhMetaFile(static_cast<HENHMETAFILE>(data)); return;
        case ClipboardKind::Metafile:
            if (const auto picture = static_cast<const METAFILEPICT*>(GlobalLock(data))) {
                DeleteMetaFile(picture->hMF);
                GlobalUnlock(data);
            }
            break;
        default: break;
        }
        GlobalFree(data);
    }
    UINT format;
    HANDLE data;
};

std::size_t clipboardBytes(HANDLE data, ClipboardKind kind)
{
    switch (kind) {
    case ClipboardKind::Bitmap: {
        BITMAP bitmap{};
        if (!GetObjectW(data, sizeof(bitmap), &bitmap) || bitmap.bmHeight <= 0 || bitmap.bmWidthBytes <= 0) return 0;
        return static_cast<std::size_t>(bitmap.bmHeight) * static_cast<std::size_t>(bitmap.bmWidthBytes);
    }
    case ClipboardKind::Palette:
        return GetPaletteEntries(static_cast<HPALETTE>(data), 0, 0, nullptr) * sizeof(PALETTEENTRY);
    case ClipboardKind::EnhancedMetafile:
        return GetEnhMetaFileBits(static_cast<HENHMETAFILE>(data), 0, nullptr);
    case ClipboardKind::Metafile:
        if (const auto picture = static_cast<const METAFILEPICT*>(GlobalLock(data))) {
            const auto bytes = GetMetaFileBitsEx(picture->hMF, 0, nullptr);
            GlobalUnlock(data);
            return bytes;
        }
        return 0;
    case ClipboardKind::Memory:
        return GlobalSize(data);
    default:
        return 0;
    }
}

struct ClipboardSnapshot {
    bool save(HWND window)
    {
        ClipboardAccess clipboard(window);
        if (!clipboard.opened) return false;
        std::size_t total = 0;
        UINT format = 0;
        for (;;) {
            SetLastError(ERROR_SUCCESS);
            format = EnumClipboardFormats(format);
            if (!format) break;
            const auto kind = clipboardKind(format);
            if (kind == ClipboardKind::Unsupported || format > std::numeric_limits<CLIPFORMAT>::max()) return false;
            const auto data = GetClipboardData(format);
            if (!data) return false;
            const auto bytes = clipboardBytes(data, kind);
            if (!bytes || bytes > clipboardLimit - total) return false;
            total += bytes;
            ClipboardCopy copy(format, OleDuplicateData(data, static_cast<CLIPFORMAT>(format), GMEM_MOVEABLE));
            if (!copy.data) return false;
            formats.push_back(std::move(copy));
        }
        if (GetLastError() != ERROR_SUCCESS) return false;
        sequence = GetClipboardSequenceNumber();
        return true;
    }

    bool restore(HWND window, DWORD expectedSequence, DWORD ownerPid)
    {
        ClipboardAccess clipboard(window);
        if (!clipboard.opened || GetClipboardSequenceNumber() != expectedSequence
            || processOf(GetClipboardOwner()) != ownerPid || !EmptyClipboard()) return false;
        bool success = true;
        for (auto& copy : formats) {
            if (SetClipboardData(copy.format, copy.data)) copy.data = nullptr;
            else success = false;
        }
        return success;
    }

    DWORD sequence = 0;
    std::vector<ClipboardCopy> formats;
};

bool sendCopy()
{
    INPUT keys[4]{};
    for (auto& key : keys) key.type = INPUT_KEYBOARD;
    keys[0].ki.wVk = VK_CONTROL;
    keys[1].ki.wVk = 'C';
    keys[2].ki.wVk = 'C';
    keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
    keys[3].ki.wVk = VK_CONTROL;
    keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
    const UINT sent = SendInput(ARRAYSIZE(keys), keys, sizeof(INPUT));
    if (sent && sent != ARRAYSIZE(keys)) SendInput(2, keys + 2, sizeof(INPUT));
    return sent == ARRAYSIZE(keys);
}

std::wstring clipboardText()
{
    const auto handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) return {};
    const auto characters = std::min(GlobalSize(handle) / sizeof(wchar_t), textLimit + 1);
    const auto value = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!value) return {};
    const auto end = std::find(value, value + characters, L'\0');
    std::wstring copied(value, end);
    GlobalUnlock(handle);
    return copied;
}

bool startProbe(HWND source, POINT point, bool automatic, Handle& process, Handle& reader)
{
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return false;
    executable.resize(length);
    std::wstring commandLine = L"\"" + executable + L"\" --selection-probe "
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(source)) + L" " + std::to_wstring(point.x) + L" "
        + std::to_wstring(point.y) + (automatic ? L" auto" : L" hotkey");

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &security, 0)) return false;
    Handle pipeReader(read), pipeWriter(write);
    if (!SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0)) return false;

    SIZE_T listSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &listSize);
    std::vector<std::byte> listStorage(listSize);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(listStorage.data());
    if (!listSize || !InitializeProcThreadAttributeList(list, 1, 0, &listSize)) return false;
    const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(&DeleteProcThreadAttributeList)>
        attributes(list, DeleteProcThreadAttributeList);
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write, sizeof(write), nullptr, nullptr)) return false;

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = write;
    startup.StartupInfo.hStdError = write;
    startup.lpAttributeList = list;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child)) return false;
    CloseHandle(child.hThread);
    process.reset(child.hProcess);
    reader = std::move(pipeReader);
    return true;
}

bool drainPipe(HANDLE reader, std::vector<char>& bytes)
{
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr)) return GetLastError() == ERROR_BROKEN_PIPE;
        if (!available) return true;
        DWORD read = 0;
        if (!ReadFile(reader, buffer.data(), std::min(available, static_cast<DWORD>(buffer.size())), &read, nullptr) || !read) return false;
        bytes.insert(bytes.end(), buffer.data(), buffer.data() + read);
        if (bytes.size() > sizeof(Reply) + textLimit * sizeof(wchar_t)) return false;
    }
}

ProbeResult decodeReply(const std::vector<char>& bytes)
{
    Reply reply{};
    if (bytes.size() < sizeof(reply)) return {Status::ProbeFailed, {}};
    std::memcpy(&reply, bytes.data(), sizeof(reply));
    if (reply.status > static_cast<std::uint32_t>(Status::TooLong) || reply.characters > textLimit
        || bytes.size() != sizeof(reply) + reply.characters * sizeof(wchar_t)) return {Status::ProbeFailed, {}};
    std::wstring text(reply.characters, L'\0');
    if (!text.empty()) std::memcpy(text.data(), bytes.data() + sizeof(reply), text.size() * sizeof(wchar_t));
    return {static_cast<Status>(reply.status), std::move(text)};
}

}

struct SelectionCapture::State {
    enum class Stage { Idle, Probe, WaitForKeys, WaitForCopy };
    Stage stage = Stage::Idle;
    HWND clipboardOwner = nullptr;
    HWND source = nullptr;
    DWORD sourcePid = 0;
    bool automatic = false;
    ULONGLONG deadline = 0;
    Handle process;
    Handle pipe;
    std::vector<char> output;
    ClipboardSnapshot saved;

    bool sourceIsCurrent() const
    {
        return sourcePid && IsWindow(source) && GetForegroundWindow() == source && processOf(source) == sourcePid;
    }
};

SelectionCapture::SelectionCapture() : m_state(std::make_unique<State>()) {}
SelectionCapture::~SelectionCapture() { cancel(); }

bool SelectionCapture::request(HWND clipboardOwner, HWND source, POINT point, bool automatic)
{
    cancel();
    auto& state = *m_state;
    state.clipboardOwner = clipboardOwner;
    state.source = source;
    state.sourcePid = processOf(source);
    state.automatic = automatic;
    if (!state.sourceIsCurrent() || !startProbe(source, point, automatic, state.process, state.pipe)) return false;
    state.stage = State::Stage::Probe;
    state.deadline = GetTickCount64() + probeTimeout;
    return true;
}

bool SelectionCapture::busy() const { return m_state->stage != State::Stage::Idle; }

void SelectionCapture::cancel()
{
    auto& state = *m_state;
    if (state.stage == State::Stage::WaitForCopy) {
        const auto sequence = GetClipboardSequenceNumber();
        if (sequence != state.saved.sequence) state.saved.restore(state.clipboardOwner, sequence, state.sourcePid);
    }
    if (state.process && WaitForSingleObject(state.process.get(), 0) == WAIT_TIMEOUT) TerminateProcess(state.process.get(), 1);
    state = State{};
}

std::optional<SelectionResult> SelectionCapture::poll()
{
    using Stage = State::Stage;
    auto& state = *m_state;
    if (state.stage == Stage::Idle) return std::nullopt;
    const auto finish = [&](Status status, std::wstring text = {}) -> std::optional<SelectionResult> {
        SelectionResult result{status, std::move(text), state.source, state.automatic};
        cancel();
        return result;
    };
    if (!state.sourceIsCurrent()) return finish(Status::SourceChanged);
    if (GetTickCount64() >= state.deadline) return finish(Status::TimedOut);

    if (state.stage == Stage::Probe) {
        if (!drainPipe(state.pipe.get(), state.output)) return finish(Status::ProbeFailed);
        const auto wait = WaitForSingleObject(state.process.get(), 0);
        if (wait == WAIT_TIMEOUT) return std::nullopt;
        DWORD exitCode = 1;
        if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(state.process.get(), &exitCode) || exitCode
            || !drainPipe(state.pipe.get(), state.output)) return finish(Status::ProbeFailed);
        auto result = decodeReply(state.output);
        state.process.reset();
        state.pipe.reset();
        if (state.automatic || (result.status != Status::Empty && result.status != Status::Unavailable)) {
            return finish(result.status, std::move(result.text));
        }
        state.stage = Stage::WaitForKeys;
        state.deadline = GetTickCount64() + copyTimeout;
    }

    if (state.stage == Stage::WaitForKeys) {
        for (int key : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN, VK_LBUTTON, int('C')}) {
            if (GetAsyncKeyState(key) & 0x8000) return std::nullopt;
        }
        if (nativePassword(state.source)) return finish(Status::Protected);
        if (!state.saved.save(state.clipboardOwner)) return finish(Status::ClipboardUnavailable);
        if (!state.sourceIsCurrent() || state.saved.sequence != GetClipboardSequenceNumber()) return finish(Status::ClipboardChanged);
        if (!sendCopy()) return finish(Status::CopyFailed);
        state.stage = Stage::WaitForCopy;
        state.deadline = GetTickCount64() + copyTimeout;
        return std::nullopt;
    }

    if (GetClipboardSequenceNumber() == state.saved.sequence) return std::nullopt;
    if (processOf(GetClipboardOwner()) != state.sourcePid) return finish(Status::ClipboardChanged);
    std::wstring text;
    DWORD copiedSequence = 0;
    {
        ClipboardAccess clipboard(state.clipboardOwner);
        if (!clipboard.opened) return std::nullopt;
        if (processOf(GetClipboardOwner()) != state.sourcePid) {
            state.stage = Stage::Idle;
            return finish(Status::ClipboardChanged);
        }
        text = clipboardText();
        copiedSequence = GetClipboardSequenceNumber();
    }
    if (copiedSequence != GetClipboardSequenceNumber()) {
        state.stage = Stage::Idle;
        return finish(Status::ClipboardChanged);
    }
    state.stage = Stage::Idle;
    if (!state.saved.restore(state.clipboardOwner, copiedSequence, state.sourcePid)) return finish(Status::ClipboardChanged);
    if (!state.sourceIsCurrent()) return finish(Status::SourceChanged);
    text = trim(std::move(text));
    if (text.empty()) return finish(Status::Empty);
    if (text.size() > textLimit) return finish(Status::TooLong);
    return finish(Status::Captured, std::move(text));
}

int SelectionCapture::runProbe(int argc, wchar_t* const* argv)
{
    if (argc != 6 || (std::wstring_view(argv[5]) != L"auto" && std::wstring_view(argv[5]) != L"hotkey")) return 2;
    wchar_t* end = nullptr;
    errno = 0;
    const auto window = std::wcstoull(argv[2], &end, 10);
    if (errno || end == argv[2] || *end || !window || window > std::numeric_limits<std::uintptr_t>::max()) return 2;
    const auto x = std::wcstol(argv[3], &end, 10);
    if (errno || end == argv[3] || *end) return 2;
    const auto y = std::wcstol(argv[4], &end, 10);
    if (errno || end == argv[4] || *end) return 2;

    ProbeResult result{Status::Unavailable, {}};
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        try {
            result = readSelection(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(window)),
                                   POINT{x, y}, std::wstring_view(argv[5]) == L"auto");
        } catch (...) {
            CoUninitialize();
            return 1;
        }
        CoUninitialize();
    }
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    const Reply reply{static_cast<std::uint32_t>(result.status), static_cast<std::uint32_t>(result.text.size())};
    return output && output != INVALID_HANDLE_VALUE && writeAll(output, &reply, sizeof(reply))
        && writeAll(output, result.text.data(), static_cast<DWORD>(result.text.size() * sizeof(wchar_t))) ? 0 : 1;
}

}
