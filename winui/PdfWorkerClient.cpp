#include "PdfWorkerClient.h"
#include "NativePaths.h"
#include "pdf/pdfworkerfiles.h"

#include <windows.h>
#include <objbase.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace yiyi {
namespace {

using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Storage::Streams;

class JobDirectory {
    std::filesystem::path m_root;
    std::filesystem::path m_path;

public:
    explicit JobDirectory(std::filesystem::path root = std::filesystem::temp_directory_path())
        : m_root(std::filesystem::canonical(root))
    {
        GUID id{};
        check_hresult(CoCreateGuid(&id));
        m_path = m_root / (L"Yiyi-pdf-" + std::wstring(to_hstring(guid(id))));
        if (!std::filesystem::create_directory(m_path))
            throw hresult_error(E_FAIL, L"无法创建 PDF 处理目录。");
    }

    ~JobDirectory()
    {
        std::error_code error;
        const auto resolved = std::filesystem::canonical(m_path, error);
        if (!error && resolved == m_path && resolved.parent_path() == m_root)
            std::filesystem::remove_all(resolved, error);
    }

    JobDirectory(JobDirectory const&) = delete;
    JobDirectory& operator=(JobDirectory const&) = delete;

    std::filesystem::path file(wchar_t const* name) const { return m_path / name; }
    std::filesystem::path const& path() const { return m_path; }
};

void putPath(JsonObject const& request, hstring const& key, std::filesystem::path const& path)
{
    request.Insert(key, JsonValue::CreateStringValue(hstring(path.native())));
}

JsonObject requestFor(std::filesystem::path const& path, hstring const& action)
{
    JsonObject request;
    request.Insert(L"action", JsonValue::CreateStringValue(action));
    putPath(request, L"source", path);
    return request;
}

class WorkerStartup {
public:
    WorkerStartup()
    {
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        HANDLE readHandle{}, writeHandle{};
        check_bool(CreatePipe(&readHandle, &writeHandle, &attributes, 0));
        read.attach(readHandle);
        write.attach(writeHandle);
        check_bool(SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0));
        m_null.attach(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &attributes, OPEN_EXISTING, 0, nullptr));
        if (!m_null) throw_last_error();
        SIZE_T size{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        m_listBuffer.resize(size);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_listBuffer.data());
        check_bool(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size));
        m_inherited = {write.get(), m_null.get()};
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                m_inherited.data(), sizeof(m_inherited), nullptr, nullptr)) {
            const auto error = GetLastError();
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            throw_hresult(HRESULT_FROM_WIN32(error));
        }
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = m_null.get();
        startup.StartupInfo.hStdOutput = write.get();
        startup.StartupInfo.hStdError = m_null.get();
    }

    ~WorkerStartup() { DeleteProcThreadAttributeList(startup.lpAttributeList); }
    WorkerStartup(WorkerStartup const&) = delete;
    WorkerStartup& operator=(WorkerStartup const&) = delete;

    STARTUPINFOEXW startup{};
    handle read;
    handle write;

private:
    file_handle m_null;
    std::array<HANDLE, 2> m_inherited{};
    std::vector<std::byte> m_listBuffer;
};

class RunningWorker {
public:
    explicit RunningWorker(HANDLE process) : handle(process) {}
    ~RunningWorker()
    {
        if (!finished()) {
            TerminateProcess(handle.get(), 1);
            WaitForSingleObject(handle.get(), 3000);
        }
    }
    RunningWorker(RunningWorker const&) = delete;
    RunningWorker& operator=(RunningWorker const&) = delete;
    bool finished() const { return WaitForSingleObject(handle.get(), 0) == WAIT_OBJECT_0; }

    winrt::handle handle;
};

JsonObject invokeWorker(JobDirectory const& job, JsonObject const& request, HANDLE cancel, PdfRequestProgress const& progress = {})
{
    if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) throw hresult_canceled();
    const auto input = job.file(L"request.json");
    const auto output = job.file(L"response.json");
    request.Insert(L"progress", JsonValue::CreateBooleanValue(true));
    pdf::writeWorkerOutput(input, to_string(request.Stringify()));
    const auto directory = executableDirectory();
    const auto worker = directory / L"yiyi_pdf_worker.exe";
    auto arguments = L"\"" + worker.native() + L"\" \"" + input.native() + L"\" \"" + output.native() + L"\"";
    WorkerStartup startup;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(worker.c_str(), arguments.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, directory.c_str(), &startup.startup.StartupInfo, &child))
        throw hresult_error(E_FAIL, L"无法启动 PDF 工作进程，请检查发行包中的 PDF 组件。");
    RunningWorker process(child.hProcess);
    CloseHandle(child.hThread);
    startup.write.close();
    const HANDLE waitHandles[] = {cancel, process.handle.get()};
    auto lastProgress = GetTickCount64();
    std::string lines;
    for (;;) {
        const auto waited = WaitForMultipleObjects(2, waitHandles, FALSE, 75);
        if (waited == WAIT_OBJECT_0) throw hresult_canceled();
        if (waited == WAIT_FAILED) throw hresult_error(E_FAIL, L"无法等待 PDF 工作进程。");
        DWORD available{};
        while (PeekNamedPipe(startup.read.get(), nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            char chunk[4096];
            DWORD read{};
            if (!ReadFile(startup.read.get(), chunk, std::min(available, static_cast<DWORD>(sizeof(chunk))), &read, nullptr) || !read)
                break;
            lines.append(chunk, read);
            std::size_t next{};
            for (auto end = lines.find('\n'); end != std::string::npos; end = lines.find('\n', next)) {
                JsonObject parsed{nullptr};
                if (JsonObject::TryParse(to_hstring(std::string_view(lines).substr(next, end - next)), parsed)) {
                    const auto value = parsed.GetNamedObject(L"progress", nullptr);
                    if (value) {
                        lastProgress = GetTickCount64();
                        if (progress) progress(value);
                    }
                }
                next = end + 1;
            }
            lines.erase(0, next);
            if (lines.size() > 32768) lines.clear();
        }
        if (waited == WAIT_OBJECT_0 + 1) break;
        if (GetTickCount64() - lastProgress >= 90000)
            throw hresult_error(E_FAIL, L"PDF 处理已 90 秒没有进展，已停止。已保存的译文可以继续使用。");
    }
    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.handle.get(), &exitCode) || exitCode != 0)
        throw hresult_error(E_FAIL, L"PDF 工作进程异常退出。");
    JsonObject response{nullptr};
    if (!JsonObject::TryParse(to_hstring(pdf::readWorkerInput(output, 32 * 1024 * 1024)), response))
        throw hresult_error(E_FAIL, L"PDF 工作进程返回的数据无效。");
    const auto error = response.GetNamedString(L"error", L"");
    if (!error.empty() && !(request.GetNamedString(L"action") == L"export" && response.HasKey(L"pageStatuses")))
        throw hresult_error(E_FAIL, error);
    return response;
}

JsonObject requestForPage(std::filesystem::path const& source, JsonObject const& document,
                          hstring const& action, int page, int pixels)
{
    auto request = requestFor(source, action);
    request.Insert(L"document", document);
    request.Insert(L"quickSourceCheck", JsonValue::CreateBooleanValue(true));
    request.Insert(L"page", JsonValue::CreateNumberValue(page));
    request.Insert(L"pixels", JsonValue::CreateNumberValue(pixels));
    return request;
}

IAsyncOperation<JsonObject> queryAsync(JsonObject request, PdfRequestProgress progress = {},
                                       std::filesystem::path destination = {})
{
    auto cancellation = co_await get_cancellation_token();
    auto event = std::make_shared<handle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!*event) throw_last_error();
    cancellation.callback([event] { SetEvent(event->get()); });
    co_await resume_background();
    JobDirectory job;
    std::unique_ptr<JobDirectory> staging;
    if (!destination.empty()) {
        staging = std::make_unique<JobDirectory>(std::filesystem::absolute(destination).parent_path());
        putPath(request, L"workDirectory", staging->path());
    }
    auto response = invokeWorker(job, request, event->get(), progress);
    if (cancellation()) throw hresult_canceled();
    co_return response;
}

IAsyncOperation<IBuffer> workerImageAsync(JsonObject request, hstring outputKey, PdfPreviewStatuses statuses = {})
{
    auto cancellation = co_await get_cancellation_token();
    cancellation.enable_propagation();
    auto event = std::make_shared<handle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!*event) throw_last_error();
    cancellation.callback([event] { SetEvent(event->get()); });
    co_await resume_background();
    JobDirectory job;
    const auto preview = job.file(L"preview.png");
    putPath(request, outputKey, preview);
    const auto response = invokeWorker(job, request, event->get());
    if (!response.GetNamedBoolean(L"saved", false))
        throw hresult_error(E_FAIL, L"PDF 工作进程未生成预览。");
    const auto bytes = pdf::readWorkerInput(preview, 64 * 1024 * 1024);
    if (cancellation()) throw hresult_canceled();
    if (statuses) statuses(response.GetNamedArray(L"statuses", JsonArray{}));
    DataWriter writer;
    const auto first = reinterpret_cast<uint8_t const*>(bytes.data());
    writer.WriteBytes({first, first + bytes.size()});
    co_return writer.DetachBuffer();
}

IAsyncOperation<InMemoryRandomAccessStream> previewAsync(JsonObject request, hstring outputKey, PdfPreviewStatuses statuses = {})
{
    auto cancellation = co_await get_cancellation_token();
    cancellation.enable_propagation();
    auto bytes = co_await workerImageAsync(std::move(request), std::move(outputKey), std::move(statuses));
    InMemoryRandomAccessStream image;
    if (co_await image.WriteAsync(bytes) != bytes.Length())
        throw hresult_error(E_FAIL, L"无法读取 PDF 预览。");
    image.Seek(0);
    co_return image;
}

}

IAsyncOperation<JsonObject> inspectPdfAsync(std::filesystem::path source, PdfRequestProgress progress, hstring officeBookmarkPrefix)
{
    auto request = requestFor(source, L"info");
    if (!officeBookmarkPrefix.empty()) request.Insert(L"officeBookmarkPrefix", JsonValue::CreateStringValue(officeBookmarkPrefix));
    return queryAsync(std::move(request), std::move(progress));
}

IAsyncOperation<JsonObject> analyzePdfPagesAsync(std::filesystem::path source, JsonObject document,
                                                int first, int count, PdfRequestProgress progress, JsonObject preceding)
{
    auto request = requestForPage(source, document, L"inspectPages", first, 1200);
    request.Insert(L"first", JsonValue::CreateNumberValue(first));
    request.Insert(L"count", JsonValue::CreateNumberValue(count));
    if (preceding) request.Insert(L"preceding", preceding);
    return queryAsync(request, std::move(progress));
}

IAsyncOperation<InMemoryRandomAccessStream> renderPdfOriginalAsync(std::filesystem::path source, JsonObject document,
                                                                  int page, int pixels)
{
    return previewAsync(requestForPage(source, document, L"renderOriginal", page, pixels), L"original");
}

IAsyncOperation<InMemoryRandomAccessStream> renderPdfTranslationAsync(std::filesystem::path source, JsonObject document,
                                                                      JsonObject pageData, int page, int pixels,
                                                                      PdfPreviewStatuses statuses)
{
    auto request = requestForPage(source, document, L"renderTranslated", page, pixels);
    request.Insert(L"pageData", pageData);
    return previewAsync(request, L"translated", std::move(statuses));
}

IAsyncOperation<JsonObject> analyzePdfScanAsync(std::filesystem::path source, JsonObject document, int page, int pixels)
{
    return queryAsync(requestForPage(source, document, L"inspectScan", page, pixels));
}

IAsyncOperation<IBuffer> renderPdfCropAsync(std::filesystem::path source, JsonObject document,
                                           int page, int pixels, std::array<double, 4> region)
{
    auto request = requestForPage(source, document, L"recognitionImage", page, pixels);
    JsonArray bounds;
    for (const auto value : region) bounds.Append(JsonValue::CreateNumberValue(value));
    request.Insert(L"region", bounds);
    return workerImageAsync(request, L"image");
}

IAsyncOperation<JsonObject> exportPdfAsync(std::filesystem::path source, std::filesystem::path taskDatabase,
                                          std::filesystem::path destination, bool preserveFailed, PdfRequestProgress progress)
{
    auto request = requestFor(source, L"export");
    putPath(request, L"taskDatabase", taskDatabase);
    putPath(request, L"destination", destination);
    request.Insert(L"preserveFailed", JsonValue::CreateBooleanValue(preserveFailed));
    return queryAsync(request, std::move(progress), std::move(destination));
}

}
