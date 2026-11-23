#include "WorkspaceSettings.h"

#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

namespace yiyi {
namespace {

using namespace winrt::Windows::Data::Json;

constexpr auto settingsFile = L"settings.json";
constexpr std::wstring_view credentialPrefix = L"dpapi:";

struct DpapiBuffer {
    DATA_BLOB data{};

    ~DpapiBuffer()
    {
        if (data.pbData) {
            SecureZeroMemory(data.pbData, data.cbData);
            LocalFree(data.pbData);
        }
    }
};

winrt::hstring protectCredential(winrt::hstring const& key)
{
    auto bytes = winrt::to_string(key);
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
    DpapiBuffer encrypted;
    auto success = CryptProtectData(&input, L"Yiyi API credential", nullptr, nullptr, nullptr,
                                    CRYPTPROTECT_UI_FORBIDDEN, &encrypted.data);
    SecureZeroMemory(bytes.data(), bytes.size());
    if (!success) throw winrt::hresult_error(E_FAIL, L"无法使用 Windows 加密 API 密钥。");

    DWORD length{};
    auto const flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    winrt::check_bool(CryptBinaryToStringW(encrypted.data.pbData, encrypted.data.cbData, flags, nullptr, &length));
    std::wstring encoded(length, L'\0');
    winrt::check_bool(CryptBinaryToStringW(encrypted.data.pbData, encrypted.data.cbData, flags, encoded.data(), &length));
    encoded.resize(length);
    return winrt::hstring{std::wstring{credentialPrefix} + encoded};
}

winrt::hstring revealCredential(winrt::hstring const& encoded)
{
    if (encoded.empty()) return {};

    auto const value = std::wstring_view{encoded};
    if (value.substr(0, credentialPrefix.size()) != credentialPrefix) {
        throw winrt::hresult_error(E_INVALIDARG, L"保存的 API 密钥格式无效。");
    }
    auto const base64 = value.substr(credentialPrefix.size());
    auto const flags = CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT;
    DWORD length{};
    winrt::check_bool(CryptStringToBinaryW(base64.data(), static_cast<DWORD>(base64.size()), flags,
                                           nullptr, &length, nullptr, nullptr));
    std::vector<BYTE> bytes(length);
    winrt::check_bool(CryptStringToBinaryW(base64.data(), static_cast<DWORD>(base64.size()), flags,
                                           bytes.data(), &length, nullptr, nullptr));
    DATA_BLOB input{length, bytes.data()};
    DpapiBuffer decrypted;
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &decrypted.data)) {
        throw winrt::hresult_error(E_FAIL, L"无法解密 API 密钥，请使用保存密钥的 Windows 账户。");
    }
    return winrt::to_hstring(std::string_view{reinterpret_cast<char const*>(decrypted.data.pbData), decrypted.data.cbData});
}

void loadProviders(WorkspaceSettings& settings, JsonArray const& providers)
{
    for (auto const& value : providers) {
        auto const object = value.GetObject();
        Provider provider;
        provider.id = object.GetNamedString(L"id");
        provider.name = object.GetNamedString(L"name");
        provider.protocol = object.GetNamedString(L"protocol");
        provider.baseUrl = object.GetNamedString(L"baseUrl");
        provider.model = object.GetNamedString(L"model");
        provider.reasoningEffort = object.GetNamedString(L"reasoningEffort", L"");
        provider.dictionaryMode = object.GetNamedString(L"dictionaryMode", L"brief") == L"detailed" ? L"detailed" : L"brief";
        auto const secret = object.GetNamedString(L"secret", L"");
        try {
            provider.apiKey = revealCredential(secret);
        } catch (winrt::hresult_error const&) {
            auto const hasDpapiPrefix = std::wstring_view{secret}.substr(0, credentialPrefix.size()) == credentialPrefix;
            settings.unreadableCredentials.emplace(provider.id, hasDpapiPrefix ? secret : winrt::hstring{});
        }
        settings.providers.push_back(std::move(provider));
    }
}

JsonObject writeProvider(Provider const& provider, winrt::hstring const& previousSecret)
{
    JsonObject object;
    object.Insert(L"id", JsonValue::CreateStringValue(provider.id));
    object.Insert(L"name", JsonValue::CreateStringValue(provider.name));
    object.Insert(L"protocol", JsonValue::CreateStringValue(provider.protocol));
    object.Insert(L"baseUrl", JsonValue::CreateStringValue(provider.baseUrl));
    object.Insert(L"model", JsonValue::CreateStringValue(provider.model));
    auto const secret = provider.apiKey.empty() ? previousSecret : protectCredential(provider.apiKey);
    object.Insert(L"secret", JsonValue::CreateStringValue(secret));
    object.Insert(L"reasoningEffort", JsonValue::CreateStringValue(provider.reasoningEffort));
    object.Insert(L"dictionaryMode", JsonValue::CreateStringValue(provider.dictionaryMode == L"detailed" ? L"detailed" : L"brief"));
    return object;
}

std::optional<std::string> readFile(std::filesystem::path const& path)
{
    winrt::file_handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file) {
        auto const error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return std::nullopt;
        winrt::throw_hresult(HRESULT_FROM_WIN32(error));
    }
    LARGE_INTEGER length{};
    winrt::check_bool(GetFileSizeEx(file.get(), &length));
    if (length.QuadPart > (std::numeric_limits<DWORD>::max)()) {
        throw winrt::hresult_error(E_INVALIDARG, L"设置文件过大。");
    }
    std::string bytes(static_cast<std::size_t>(length.QuadPart), '\0');
    DWORD read{};
    winrt::check_bool(ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr));
    if (read != bytes.size()) throw winrt::hresult_error(E_FAIL, L"未能完整读取设置。");
    return bytes;
}

std::string_view trimIni(std::string_view value)
{
    auto const first = value.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

std::string decodeIniValue(std::string_view value)
{
    value = trimIni(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }

    std::string decoded;
    for (std::size_t i = 0; i < value.size(); ++i) {
        char ch = value[i];
        if (ch == '\\' && i + 1 < value.size()) {
            ch = value[++i];
            if (ch == 'x') {
                constexpr std::string_view hexDigits = "0123456789abcdefABCDEF";
                unsigned int byte{};
                while (i + 1 < value.size()) {
                    auto const digit = hexDigits.find(value[i + 1]);
                    if (digit == std::string_view::npos) break;
                    byte = byte * 16 + static_cast<unsigned int>(digit < 16 ? digit : digit - 6);
                    if (byte > 255) throw winrt::hresult_error(E_INVALIDARG, L"旧版设置中的十六进制字节无效。");
                    ++i;
                }
                ch = static_cast<char>(byte);
            } else {
                constexpr std::string_view escapes = "0abfnrtv";
                constexpr std::string_view characters{"\0\a\b\f\n\r\t\v", escapes.size()};
                auto const index = escapes.find(ch);
                if (index != std::string_view::npos) ch = characters[index];
            }
        }
        decoded.push_back(ch);
    }
    return decoded;
}

void loadLegacySettings(WorkspaceSettings& settings, std::string_view bytes)
{
    if (bytes.substr(0, 3) == "\xef\xbb\xbf") bytes.remove_prefix(3);
    enum class Section { Other, General, Vision, Pdf };
    auto section = Section::Other;
    while (!bytes.empty()) {
        auto const end = bytes.find('\n');
        auto const line = trimIni(bytes.substr(0, end));
        bytes.remove_prefix(end == std::string_view::npos ? bytes.size() : end + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            if (line == "[General]") section = Section::General;
            else if (line == "[vision]") section = Section::Vision;
            else if (line == "[pdf]") section = Section::Pdf;
            else section = Section::Other;
            continue;
        }
        auto const equals = line.find('=');
        if (section == Section::Other || equals == std::string_view::npos) continue;
        auto const key = trimIni(line.substr(0, equals));
        auto const value = line.substr(equals + 1);
        if (section == Section::Pdf) {
            if (key == "requestTimeoutMinutes") {
                for (const int minutes : {5, 10, 20, 30, 60})
                    if (trimIni(value) == std::to_string(minutes)) settings.pdfRequestTimeout = minutes;
            }
            continue;
        }
        if (section == Section::Vision) {
            if (key == "provider") settings.visionProviderId = winrt::to_hstring(decodeIniValue(value));
            continue;
        }
        if (key == "providers") {
            auto const decoded = decodeIniValue(value);
            constexpr std::string_view marker = "@ByteArray(";
            if (decoded.size() <= marker.size() || decoded.substr(0, marker.size()) != marker || decoded.back() != ')') {
                throw winrt::hresult_error(E_INVALIDARG, L"旧版设置中的 AI 服务列表格式无效。");
            }
            auto const json = std::string_view{decoded}.substr(marker.size(), decoded.size() - marker.size() - 1);
            loadProviders(settings, JsonArray::Parse(winrt::to_hstring(json)));
        } else {
            static constexpr std::pair<std::string_view, winrt::hstring WorkspaceSettings::*> keys[] = {
                {"selectedProvider", &WorkspaceSettings::selectedProviderId},
                {"sourceLanguage", &WorkspaceSettings::sourceLanguage},
                {"targetLanguage", &WorkspaceSettings::targetLanguage},
                {"mode", &WorkspaceSettings::mode},
                {"tone", &WorkspaceSettings::tone},
                {"theme", &WorkspaceSettings::theme},
            };
            for (auto const& [name, preference] : keys) {
                if (key == name) {
                    settings.*preference = winrt::to_hstring(decodeIniValue(value));
                    break;
                }
            }
        }
    }
}

void writeAtomically(std::filesystem::path const& path, std::string const& bytes)
{
    GUID id{};
    winrt::check_hresult(CoCreateGuid(&id));
    auto temporary = path;
    temporary += std::wstring{winrt::to_hstring(winrt::guid{id})} + L".tmp";

    auto const rawFile = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rawFile == INVALID_HANDLE_VALUE) winrt::throw_last_error();

    try {
        winrt::file_handle file{rawFile};
        DWORD written{};
        winrt::check_bool(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
        if (written != bytes.size()) throw winrt::hresult_error(E_FAIL, L"未能完整保存设置。");
        winrt::check_bool(FlushFileBuffers(file.get()));
        file.close();
        winrt::check_bool(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    } catch (...) {
        DeleteFileW(temporary.c_str());
        throw;
    }
}

}

std::filesystem::path defaultDataDirectory()
{
    PWSTR directory{};
    auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &directory);
    auto const allocated = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>{directory, CoTaskMemFree};
    winrt::check_hresult(result);
    return std::filesystem::path{allocated.get()} / L"Yiyi" / L"Yiyi";
}

WorkspaceSettings loadSettings(std::filesystem::path const& directory)
{
    WorkspaceSettings settings;
    if (auto const native = readFile(directory / settingsFile)) {
        JsonObject object{nullptr};
        if (!JsonObject::TryParse(winrt::to_hstring(*native), object)) {
            throw winrt::hresult_error(E_INVALIDARG, L"设置文件的 JSON 格式无效。");
        }
        settings.selectedProviderId = object.GetNamedString(L"selectedProviderId", L"");
        settings.visionProviderId = object.GetNamedString(L"visionProviderId", L"");
        settings.sourceLanguage = object.GetNamedString(L"sourceLanguage", settings.sourceLanguage);
        settings.targetLanguage = object.GetNamedString(L"targetLanguage", settings.targetLanguage);
        settings.mode = object.GetNamedString(L"mode", settings.mode);
        settings.tone = object.GetNamedString(L"tone", settings.tone);
        settings.theme = object.GetNamedString(L"theme", settings.theme);
        settings.pdfRecognitionPixels = object.GetNamedNumber(L"pdfRecognitionPixels", 1800) == 2400 ? 2400 : 1800;
        const auto timeout = object.GetNamedNumber(L"pdfRequestTimeout", 10);
        for (const int minutes : {5, 10, 20, 30, 60})
            if (timeout == minutes) settings.pdfRequestTimeout = minutes;
        loadProviders(settings, object.GetNamedArray(L"providers", JsonArray{}));
    } else if (auto const legacy = readFile(directory / L"settings.ini")) {
        loadLegacySettings(settings, *legacy);
    }
    if (std::none_of(settings.providers.begin(), settings.providers.end(), [&](auto const& provider) {
        return provider.id == settings.selectedProviderId;
    })) {
        settings.selectedProviderId = settings.providers.empty() ? winrt::hstring{} : settings.providers.front().id;
    }
    return settings;
}

void saveSettings(std::filesystem::path const& directory, WorkspaceSettings const& settings)
{
    JsonArray providers;
    for (auto const& provider : settings.providers) {
        auto const previous = settings.unreadableCredentials.find(provider.id);
        providers.Append(writeProvider(provider, previous == settings.unreadableCredentials.end() ? winrt::hstring{} : previous->second));
    }

    JsonObject object;
    object.Insert(L"selectedProviderId", JsonValue::CreateStringValue(settings.selectedProviderId));
    object.Insert(L"visionProviderId", JsonValue::CreateStringValue(settings.visionProviderId));
    object.Insert(L"sourceLanguage", JsonValue::CreateStringValue(settings.sourceLanguage));
    object.Insert(L"targetLanguage", JsonValue::CreateStringValue(settings.targetLanguage));
    object.Insert(L"mode", JsonValue::CreateStringValue(settings.mode));
    object.Insert(L"tone", JsonValue::CreateStringValue(settings.tone));
    object.Insert(L"theme", JsonValue::CreateStringValue(settings.theme));
    object.Insert(L"pdfRecognitionPixels", JsonValue::CreateNumberValue(settings.pdfRecognitionPixels));
    object.Insert(L"pdfRequestTimeout", JsonValue::CreateNumberValue(settings.pdfRequestTimeout));
    object.Insert(L"providers", providers);
    auto const bytes = winrt::to_string(object.Stringify());
    if (bytes.size() > (std::numeric_limits<DWORD>::max)()) {
        throw winrt::hresult_error(E_INVALIDARG, L"设置文件过大。");
    }

    std::filesystem::create_directories(directory);
    writeAtomically(directory / settingsFile, bytes);
}

}
