#include "DocumentFiles.h"

#include <windows.h>
#include <objbase.h>
#include <winrt/base.h>

#include <limits>

namespace yiyi::documentFiles {
namespace {

bool equalPath(std::wstring_view left, std::wstring_view right)
{
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

}

std::filesystem::path sourcePath(std::filesystem::path const& path)
{
    std::error_code error;
    auto canonical = std::filesystem::canonical(path, error);
    if (error) throw winrt::hresult_error(E_FAIL, L"无法读取文件，请检查文件是否存在及读取权限。");
    return canonical;
}

bool hasExtension(std::filesystem::path const& path, std::wstring_view extension)
{
    return equalPath(path.extension().native(), extension);
}

std::string read(std::filesystem::path const& path, size_t maxBytes)
{
    winrt::file_handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (!file || GetFileType(file.get()) != FILE_TYPE_DISK)
        throw winrt::hresult_error(E_FAIL, L"无法读取文件，请检查文件是否存在及读取权限。");

    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.get(), &length) || length.QuadPart < 0)
        throw winrt::hresult_error(E_FAIL, L"无法读取文件大小。");
    if (static_cast<uint64_t>(length.QuadPart) > maxBytes ||
        static_cast<uint64_t>(length.QuadPart) >= (std::numeric_limits<DWORD>::max)())
        throw winrt::hresult_error(E_FAIL, L"文件超过大小限制，请拆分后导入。");

    std::string bytes(static_cast<size_t>(length.QuadPart) + 1, '\0');
    DWORD received{};
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &received, nullptr)
        || received != length.QuadPart)
        throw winrt::hresult_error(E_FAIL, L"文件读取失败或正在被修改，请重新导入。");
    bytes.resize(received);
    return bytes;
}

void requireDifferentOutput(std::filesystem::path const& source, std::filesystem::path const& destination)
{
    std::error_code error;
    const auto normalized = std::filesystem::weakly_canonical(destination, error);
    if (error) throw winrt::hresult_error(E_FAIL, L"无法验证目标文件路径。");
    const auto output = std::filesystem::absolute(normalized, error);
    if (error) throw winrt::hresult_error(E_FAIL, L"无法验证目标文件路径。");
    const bool equivalent = std::filesystem::equivalent(source, output, error);
    if (equivalent || equalPath(source.native(), output.native()))
        throw winrt::hresult_invalid_argument(L"请选择不同的文件名，避免覆盖原文。");
}

struct AtomicOutput::File {
    std::filesystem::path destination;
    std::filesystem::path temporary;
    winrt::file_handle handle;

    ~File()
    {
        handle.close();
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
    }
};

AtomicOutput::AtomicOutput(std::filesystem::path const& path) : m_file(std::make_unique<File>())
{
    GUID id{};
    winrt::check_hresult(CoCreateGuid(&id));
    auto temporary = path.parent_path() / (L".yiyi-" + std::wstring(winrt::to_hstring(winrt::guid(id))) + L".tmp");
    m_file->destination = path;
    m_file->handle.attach(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!m_file->handle) throw winrt::hresult_error(E_FAIL, L"无法创建导出文件，请检查目录权限。");
    m_file->temporary = std::move(temporary);
}

AtomicOutput::~AtomicOutput() = default;

size_t AtomicOutput::writeAt(uint64_t offset, std::string_view bytes) noexcept
{
    if (offset > static_cast<uint64_t>((std::numeric_limits<LONGLONG>::max)()) ||
        bytes.size() > (std::numeric_limits<DWORD>::max)())
        return 0;
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD written{};
    if (!SetFilePointerEx(m_file->handle.get(), position, nullptr, FILE_BEGIN)
        || !WriteFile(m_file->handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
        return 0;
    return written;
}

void AtomicOutput::commit()
{
    if (!FlushFileBuffers(m_file->handle.get()))
        throw winrt::hresult_error(E_FAIL, L"无法写入导出文件，请检查目录权限和剩余空间。");
    m_file->handle.close();
    if (!MoveFileExW(m_file->temporary.c_str(), m_file->destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw winrt::hresult_error(E_FAIL, L"无法保存导出文件，请关闭占用它的应用并检查目录权限。");
    }
    m_file->temporary.clear();
}

void writeAtomically(std::filesystem::path const& path, std::string_view bytes)
{
    AtomicOutput output(path);
    if (output.writeAt(0, bytes) != bytes.size())
        throw winrt::hresult_error(E_FAIL, L"无法写入导出文件，请检查目录权限和剩余空间。");
    output.commit();
}

}
