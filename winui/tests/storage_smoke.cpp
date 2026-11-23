#include "HistoryStore.h"
#include "WorkspaceSettings.h"

#include <winrt/Windows.Foundation.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace {

class TestDirectory {
public:
    TestDirectory()
    {
        auto const id = winrt::Windows::Foundation::GuidHelper::CreateNewGuid();
        m_path = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
            (L"yiyi-storage-test-" + std::wstring{winrt::to_hstring(id)});
        if (!std::filesystem::create_directory(m_path)) throw std::runtime_error("Cannot create test directory");
    }

    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    std::filesystem::path const& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

void check(bool passed, char const* message)
{
    if (!passed) throw std::runtime_error(message);
}

std::string readFile(std::filesystem::path const& path)
{
    std::ifstream file{path, std::ios::binary};
    check(static_cast<bool>(file), "Cannot read fixture");
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void checkSettings(std::filesystem::path const& directory, std::filesystem::path const& fixture, bool hasDpapiSample)
{
    auto const empty = yiyi::loadSettings(directory);
    check(empty.providers.empty() && empty.mode == L"auto", "Wrong defaults in empty profile");

    auto const legacyPath = directory / L"settings.ini";
    std::filesystem::copy_file(fixture, legacyPath);
    auto settings = yiyi::loadSettings(directory);
    check(!std::filesystem::exists(directory / L"settings.json"), "Loading the legacy profile wrote to disk");
    check(settings.sourceLanguage == L"简体中文" && settings.targetLanguage == L"英语" && settings.mode == L"dictionary"
        && settings.theme == L"dark" && settings.tone == L"正式", "Legacy preferences were not imported");
    check(settings.selectedProviderId == L"something" && settings.visionProviderId == L"broken" && settings.providers.size() == 2,
        "Legacy providers were lost");

    auto const& imported = settings.providers.front();
    check(imported.name == L"老模型 😃, \"QSettings\" foo " && imported.baseUrl == L"https://example.test/model?q=x,y&a=b"
        && imported.model == L"测试=a" && imported.reasoningEffort == L"high", "Legacy Qt escapes were decoded incorrectly");
    check(imported.dictionaryMode == L"detailed" && settings.providers.back().dictionaryMode == L"brief",
        "Legacy dictionary mode did not migrate or default to brief");
    check(hasDpapiSample ? imported.apiKey == L"fake-key-é" : imported.apiKey.empty(), "Legacy credential was not decoded");
    check(settings.unreadableCredentials.size() == 1 && settings.unreadableCredentials.at(L"broken") == L"dpapi:AAECAwQ="
        && settings.providers.back().apiKey.empty(), "Unreadable credential discarded other provider data");

    settings.providers.front().apiKey = L"synthetic-native-credential-é";
    settings.mode = L"translation";
    settings.visionProviderId = L"something";
    yiyi::saveSettings(directory, settings);
    settings = yiyi::loadSettings(directory);
    check(settings.mode == L"translation" && settings.visionProviderId == L"something"
        && settings.providers.front().apiKey == L"synthetic-native-credential-é" && settings.providers.front().dictionaryMode == L"detailed",
        "Native settings did not take precedence or failed DPAPI round-trip");
    check(settings.unreadableCredentials.at(L"broken") == L"dpapi:AAECAwQ=", "Save discarded unreadable encrypted credential");
    check(readFile(directory / L"settings.json").find("synthetic-native-credential") == std::string::npos, "Saved plaintext credential");

    settings.tone = L"自然";
    settings.unreadableCredentials.erase(L"broken");
    yiyi::saveSettings(directory, settings);
    settings = yiyi::loadSettings(directory);
    check(settings.tone == L"自然" && settings.unreadableCredentials.empty(), "Replacing settings or clearing unreadable key failed");
    check(readFile(legacyPath) == readFile(fixture), "Native settings write changed the legacy profile");
}

void checkHistory(std::filesystem::path const& directory)
{
    yiyi::HistoryStore history{directory};
    yiyi::HistoryRecord record;
    record.input = L"Hello_ % 世界 ПРИВЕТ";
    record.output = L"Bonjour. À L'ÉCOLE";
    record.source = L"英语";
    record.target = L"法语";
    record.provider = L"Test provider";
    record.model = L"Test model";

    auto const firstId = history.add(record);
    auto const secondId = history.add(record);
    auto const retrieved = history.get(firstId);
    check(retrieved && retrieved->input == record.input && !retrieved->created.empty(), "Reading the saved record failed");
    check(history.list(true).empty() && history.setFavorite(firstId, true), "Favorites update failed");
    check(history.list(true, L"helLO_").size() == 1 && history.list(false, L"世界").size() == 2,
        "Literal and Unicode searches failed");
    check(history.list(true, L"привет").size() == 1 && history.list(false, L"à l'école").size() == 2
        && history.list(false, L"a l'ecole").empty(), "Unicode case-insensitive search failed");
    auto const byOutput = history.list(false, L"Bonjour.");
    check(byOutput.size() == 2 && byOutput.front().id == secondId && history.list(false, L"missing").empty(),
        "Output search or newest-first order failed");

    {
        yiyi::HistoryStore reopened{directory};
        auto const persisted = reopened.get(firstId);
        check(persisted && persisted->favorite && reopened.remove(firstId), "History did not persist across connections");
    }
    check(!history.get(firstId) && !history.remove(firstId) && history.list().size() == 1, "Deleted history entry remains");
}

}

int wmain(int argc, wchar_t* argv[])
{
    try {
        check(argc == 2 || argc == 3, "Specify a Qt settings.ini fixture");
        winrt::init_apartment();
        TestDirectory directory;
        auto const defaultDirectory = yiyi::defaultDataDirectory();
        check(defaultDirectory.filename() == L"Yiyi" && defaultDirectory.parent_path().filename() == L"Yiyi",
            "Default path is incompatible with the Qt profile");
        checkSettings(directory.path(), argv[1], argc == 3 && std::wstring_view{argv[2]} == L"--dpapi-fixture");
        checkHistory(directory.path());
        std::cout << "Legacy preferences, DPAPI and SQLite history passed.\n";
    } catch (winrt::hresult_error const& error) {
        std::cerr << winrt::to_string(error.message()) << '\n';
        return 1;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
