#pragma once

#include <windows.h>
#include <winrt/base.h>

#include <filesystem>
#include <string>

namespace yiyi {

inline std::filesystem::path executableDirectory()
{
    std::wstring filename(32768, L'\0');
    auto const length = GetModuleFileNameW(nullptr, filename.data(), static_cast<DWORD>(filename.size()));
    if (length == 0 || length >= filename.size()) winrt::throw_last_error();
    filename.resize(length);
    return std::filesystem::path{filename}.parent_path();
}

}
