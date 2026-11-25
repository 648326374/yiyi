#define NOMINMAX
#include <windows.h>
#include <comdef.h>
#include <oleacc.h>
#include <wrl/client.h>

#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

void require(HRESULT result)
{
    if (FAILED(result)) _com_issue_error(result);
}

_variant_t invoke(IDispatch& object, wchar_t const* member, WORD flags,
                  std::initializer_list<_variant_t> values = {})
{
    auto name = const_cast<wchar_t*>(member);
    DISPID id{};
    require(object.GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &id));
    std::vector<VARIANTARG> args(std::rbegin(values), std::rend(values));
    DISPID property = DISPID_PROPERTYPUT;
    const bool assignments = flags == DISPATCH_PROPERTYPUT;
    DISPPARAMS parameters{args.data(), assignments ? &property : nullptr, static_cast<UINT>(args.size()),
                         assignments ? 1u : 0u};
    EXCEPINFO error{};
    _variant_t result;
    const auto status = object.Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &parameters, &result, &error, nullptr);
    SysFreeString(error.bstrSource);
    SysFreeString(error.bstrDescription);
    SysFreeString(error.bstrHelpFile);
    require(status);
    return result;
}

ComPtr<IDispatch> dispatch(_variant_t const& value)
{
    if (value.vt != VT_DISPATCH || !value.pdispVal) _com_issue_error(E_NOINTERFACE);
    return ComPtr<IDispatch>(value.pdispVal);
}

struct WordWindow {
    DWORD pid;
    HWND document{};
};

BOOL CALLBACK findDocument(HWND window, LPARAM argument)
{
    auto& state = *reinterpret_cast<WordWindow*>(argument);
    wchar_t name[32]{};
    if (GetClassNameW(window, name, static_cast<int>(std::size(name))) && wcscmp(name, L"_WwG") == 0) {
        state.document = window;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK findWordFrame(HWND window, LPARAM argument)
{
    auto& state = *reinterpret_cast<WordWindow*>(argument);
    DWORD owner{};
    GetWindowThreadProcessId(window, &owner);
    wchar_t name[32]{};
    if (owner == state.pid && GetClassNameW(window, name, static_cast<int>(std::size(name))) && wcscmp(name, L"OpusApp") == 0)
        EnumChildWindows(window, findDocument, argument);
    return state.document == nullptr;
}

class WordProcess {
public:
    WordProcess(std::filesystem::path const& executable, std::wstring& desktop)
    {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.lpDesktop = desktop.data();
        PROCESS_INFORMATION child{};
        // /w creates an independent instance. /a bypasses add-ins and the shared normal template.
        auto command = L"\"" + executable.native() + L"\" /w /a /m /q";
        const auto working = executable.parent_path();
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            working.c_str(), &startup, &child))
            _com_issue_error(HRESULT_FROM_WIN32(GetLastError()));
        CloseHandle(child.hThread);
        m_process = child.hProcess;
        m_pid = child.dwProcessId;
    }

    ~WordProcess()
    {
        if (WaitForSingleObject(m_process, 3000) == WAIT_TIMEOUT) {
            TerminateProcess(m_process, 1);
            WaitForSingleObject(m_process, 3000);
        }
        CloseHandle(m_process);
    }

    WordProcess(WordProcess const&) = delete;
    WordProcess& operator=(WordProcess const&) = delete;

    ComPtr<IDispatch> application() const
    {
        const auto start = GetTickCount64();
        do {
            WordWindow state{m_pid};
            EnumWindows(findWordFrame, reinterpret_cast<LPARAM>(&state));
            if (state.document) {
                ComPtr<IDispatch> window;
                if (SUCCEEDED(AccessibleObjectFromWindow(state.document, static_cast<DWORD>(OBJID_NATIVEOM),
                        IID_PPV_ARGS(window.GetAddressOf()))) && window)
                    return dispatch(invoke(*window.Get(), L"Application", DISPATCH_PROPERTYGET));
            }
        } while (WaitForSingleObject(m_process, 50) == WAIT_TIMEOUT && GetTickCount64() - start < 15000);
        _com_issue_error(E_FAIL);
    }

private:
    HANDLE m_process{};
    DWORD m_pid{};
};

void renderDocument(IDispatch& documents, std::filesystem::path const& input)
{
    auto output = input;
    output.replace_extension(L".pdf");
    const _variant_t missing(DISP_E_PARAMNOTFOUND, VT_ERROR);
    auto document = dispatch(invoke(documents, L"Open", DISPATCH_METHOD,
        {input.c_str(), false, true, false, missing, missing, missing, missing, missing, missing, missing, false}));
    constexpr long pdf = 17; // wdExportFormatPDF
    constexpr long wordBookmarks = 2; // wdExportCreateWordBookmarks
    invoke(*document.Get(), L"ExportAsFixedFormat", DISPATCH_METHOD,
        {output.c_str(), pdf, false, missing, missing, missing, missing, missing, missing, missing, wordBookmarks});
    invoke(*document.Get(), L"Close", DISPATCH_METHOD, {0L});
}

void render(std::filesystem::path const& executable, std::filesystem::path const& directory, std::wstring& desktop)
{
    WordProcess process(executable, desktop);
    auto word = process.application();
    invoke(*word.Get(), L"Visible", DISPATCH_PROPERTYPUT, {false});
    invoke(*word.Get(), L"DisplayAlerts", DISPATCH_PROPERTYPUT, {0L});
    invoke(*word.Get(), L"AutomationSecurity", DISPATCH_PROPERTYPUT, {3L}); // msoAutomationSecurityForceDisable
    auto options = dispatch(invoke(*word.Get(), L"Options", DISPATCH_PROPERTYGET));
    for (const auto* setting : {L"UpdateLinksAtOpen", L"UpdateFieldsAtPrint", L"UpdateLinksAtPrint"})
        invoke(*options.Get(), setting, DISPATCH_PROPERTYPUT, {false});
    auto documents = dispatch(invoke(*word.Get(), L"Documents", DISPATCH_PROPERTYGET));
    for (const auto* name : {L"original.docx", L"preview.docx"}) {
        const auto input = directory / name;
        if (std::filesystem::is_regular_file(input)) renderDocument(*documents.Get(), input);
    }
    invoke(*word.Get(), L"Quit", DISPATCH_METHOD, {0L}); // wdDoNotSaveChanges
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) return 2;
    auto name = L"YiyiWordPreview-" + std::to_wstring(GetCurrentProcessId());
    const auto previous = GetThreadDesktop(GetCurrentThreadId());
    // Keep Word's window creation and any prompts off the user's interactive desktop.
    const auto desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) return 1;
    int result = 1;
    if (SetThreadDesktop(desktop)) {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
            try {
                render(argv[1], argv[2], name);
                result = 0;
            } catch (...) {
            }
            CoUninitialize();
        }
        SetThreadDesktop(previous);
    }
    CloseDesktop(desktop);
    return result;
}
