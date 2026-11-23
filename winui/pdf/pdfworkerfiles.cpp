#include "pdfworkerfiles.h"

#include <windows.h>
#include <objbase.h>
#include <winrt/base.h>

#include <algorithm>

namespace yiyi::pdf {

std::string readWorkerInput(std::filesystem::path const& path, std::size_t maxBytes)
{
    winrt::file_handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) winrt::throw_last_error();
    LARGE_INTEGER size{};
    winrt::check_bool(GetFileSizeEx(file.get(), &size));
    if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > maxBytes)
        throw winrt::hresult_error(E_INVALIDARG, L"PDF 处理请求过大。");

    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 4 * 1024 * 1024));
        DWORD read = 0;
        winrt::check_bool(ReadFile(file.get(), bytes.data() + offset, chunk, &read, nullptr));
        if (read == 0) throw winrt::hresult_error(E_FAIL, L"PDF 请求文件读取不完整。");
        offset += read;
    }
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0, 3);
    return bytes;
}

void writeWorkerOutput(std::filesystem::path const& path, std::string_view bytes)
{
    GUID id{};
    winrt::check_hresult(CoCreateGuid(&id));
    auto temporary = path;
    temporary += std::wstring(winrt::to_hstring(winrt::guid(id))) + L".tmp";
    winrt::file_handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) winrt::throw_last_error();
    try {
        while (!bytes.empty()) {
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 4 * 1024 * 1024));
            DWORD written = 0;
            winrt::check_bool(WriteFile(file.get(), bytes.data(), chunk, &written, nullptr));
            if (written != chunk) throw winrt::hresult_error(E_FAIL, L"PDF 处理结果写入不完整。");
            bytes.remove_prefix(written);
        }
        winrt::check_bool(FlushFileBuffers(file.get()));
        file.close();
        winrt::check_bool(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    } catch (...) {
        file.close();
        DeleteFileW(temporary.c_str());
        throw;
    }
}

}
