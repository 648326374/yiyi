#include "OcrClient.h"

#include "NativePaths.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Security.Cryptography.h>

#include <fstream>
#include <memory>

namespace yiyi {
namespace {

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Security::Cryptography;
using namespace winrt::Windows::Storage::Streams;

class TemporaryImages {
public:
    TemporaryImages()
    {
        auto const id = GuidHelper::CreateNewGuid();
        m_directory = std::filesystem::temp_directory_path() / (L"yiyi-ocr-" + std::wstring{winrt::to_hstring(id)});
        m_input = m_directory / L"input.png";
        m_result = m_directory / L"result.json";
        if (!std::filesystem::create_directory(m_directory)) {
            throw winrt::hresult_error(E_FAIL, L"无法创建识别临时目录，请检查磁盘空间。");
        }
    }

    ~TemporaryImages()
    {
        DeleteFileW(m_input.c_str());
        DeleteFileW(m_result.c_str());
        RemoveDirectoryW(m_directory.c_str());
    }

    std::filesystem::path const& input() const { return m_input; }
    std::filesystem::path const& result() const { return m_result; }

private:
    std::filesystem::path m_directory;
    std::filesystem::path m_input;
    std::filesystem::path m_result;
};

struct WorkerProcess {
    winrt::handle process;

    ~WorkerProcess()
    {
        if (process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) {
            TerminateProcess(process.get(), ERROR_CANCELLED);
            WaitForSingleObject(process.get(), 1500);
        }
    }
};

JsonObject readResult(std::filesystem::path const& path)
{
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    auto const length = static_cast<std::streamoff>(file.tellg());
    if (length <= 0 || length > 2 * 1024 * 1024) return nullptr;
    std::string bytes(static_cast<std::size_t>(length), '\0');
    file.seekg(0);
    if (!file.read(bytes.data(), length)) return nullptr;
    JsonObject result{nullptr};
    JsonObject::TryParse(winrt::to_hstring(bytes), result);
    return result;
}

}

IAsyncOperation<JsonObject> recognizeImageAsync(IBuffer image)
{
    if (!image || image.Length() == 0 || image.Length() > 256 * 1024 * 1024) {
        throw winrt::hresult_invalid_argument(L"截图无效或过大，请缩小范围。");
    }
    auto token = co_await winrt::get_cancellation_token();
    auto cancelled = std::make_shared<winrt::handle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!*cancelled) winrt::throw_last_error();
    token.callback([cancelled] { SetEvent(cancelled->get()); });
    co_await winrt::resume_background();

    winrt::init_apartment();
    struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
    TemporaryImages images;
    {
        winrt::com_array<std::uint8_t> bytes;
        CryptographicBuffer::CopyToByteArray(image, bytes);
        std::ofstream file{images.input(), std::ios::binary | std::ios::trunc};
        file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file) throw winrt::hresult_error(E_FAIL, L"无法写入识别临时图片，请检查磁盘空间。");
    }
    if (WaitForSingleObject(cancelled->get(), 0) == WAIT_OBJECT_0) throw winrt::hresult_canceled();

    auto const directory = executableDirectory();
    auto const worker = directory / L"yiyi_ocr_worker.exe";
    auto command = L"\"" + worker.native() + L"\" \"" + images.input().native() + L"\" \"" + images.result().native() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(worker.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, directory.c_str(), &startup, &info)) {
        throw winrt::hresult_error(E_FAIL, L"无法启动 RapidOCR，请使用包含 OCR 组件的完整发行包。");
    }
    WorkerProcess running{winrt::handle{info.hProcess}};
    winrt::handle thread{info.hThread};
    HANDLE waits[] = {cancelled->get(), running.process.get()};
    auto const status = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, 60000);
    if (status == WAIT_OBJECT_0) throw winrt::hresult_canceled();
    if (status == WAIT_TIMEOUT) throw winrt::hresult_error(E_FAIL, L"本地识别超过 60 秒，已停止。请缩小截图范围后重试。");
    if (status != WAIT_OBJECT_0 + 1) winrt::throw_last_error();

    DWORD exitCode{};
    winrt::check_bool(GetExitCodeProcess(running.process.get(), &exitCode));
    auto const result = readResult(images.result());
    if (exitCode != 0 || !result || result.HasKey(L"code")) {
        throw winrt::hresult_error(E_FAIL,
            L"RapidOCR 组件未能运行。请使用完整发行包并保留 ocr 目录；也可手动切换 AI 识图。");
    }
    if (result.GetNamedString(L"text", L"").empty()) {
        throw winrt::hresult_error(E_FAIL, L"没有识别到文字。请框选清晰的文字区域后重试。");
    }
    co_return result;
}

}
