#include "NativeImage.h"
#include "NativePaths.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <bit>
#include <cmath>
#include <fstream>

namespace {

using namespace winrt::Windows::Data::Json;
using Recognize = int (*)(std::uint8_t const*, int, char*, int, float*);

std::vector<std::uint8_t> readImage(std::filesystem::path const& path)
{
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    auto const length = static_cast<std::streamoff>(file.tellg());
    if (length <= 0 || length > 256 * 1024 * 1024) throw winrt::hresult_invalid_argument();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), length)) throw winrt::hresult_error(E_FAIL);
    return bytes;
}

JsonObject error(winrt::hstring const& code)
{
    JsonObject result;
    result.Insert(L"code", JsonValue::CreateStringValue(code));
    return result;
}

JsonObject recognize(std::filesystem::path const& input)
{
    auto const runtime = yiyi::executableDirectory() / L"ocr";
    auto const libraryName = runtime / L"YiyiOcr.dll";
    auto const library = LoadLibraryExW(libraryName.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) return error(L"rapid_missing");

    // The isolated worker exits after one request. Keeping the DLL loaded lets
    // Windows close its dependent runtime only once, at process exit.
    auto const entry = GetProcAddress(library, "YiyiOcr");
    if (!entry || !SetCurrentDirectoryW(runtime.c_str())) return error(L"rapid_missing");
    auto const run = std::bit_cast<Recognize>(entry);

    try {
        auto const bytes = readImage(input);
        auto const image = yiyi::prepareOcrImage(yiyi::decodeImage(bytes));
        auto const png = yiyi::encodePng(image);
        std::string output(1024 * 1024, '\0');
        float confidence{};
        auto const length = run(png.data(), static_cast<int>(png.size()), output.data(), static_cast<int>(output.size()), &confidence);
        if (length < 0 || static_cast<std::size_t>(length) >= output.size()) return error(L"rapid_failed");
        output.resize(length);
        auto const first = output.find_first_not_of(" \t\r\n\f\v");
        if (first == std::string::npos) output.clear();
        else output = output.substr(first, output.find_last_not_of(" \t\r\n\f\v") - first + 1);

        JsonObject result;
        result.Insert(L"text", JsonValue::CreateStringValue(winrt::to_hstring(output)));
        result.Insert(L"confidence", JsonValue::CreateNumberValue(std::isfinite(confidence) ? confidence : 0));
        return result;
    } catch (...) {
        return error(L"rapid_failed");
    }
}

}

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 3) return 2;
    try {
        winrt::init_apartment();
        auto const input = std::filesystem::absolute(argv[1]);
        auto const output = std::filesystem::absolute(argv[2]);
        auto const result = recognize(input);
        auto const json = winrt::to_string(result.Stringify());
        std::ofstream file{output, std::ios::binary | std::ios::trunc};
        if (!file.write(json.data(), static_cast<std::streamsize>(json.size()))) return 4;
        file.close();
        if (!file) return 5;
        return result.HasKey(L"code") ? 1 : 0;
    } catch (...) {
        return 3;
    }
}
