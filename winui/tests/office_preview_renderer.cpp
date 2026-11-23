#include <filesystem>
#include <fstream>
#include <string_view>

int wmain(int argc, wchar_t const* argv[])
{
    if (argc != 8) return 1;
    const std::wstring_view profile(argv[1]);
    if (!profile.starts_with(L"-env:UserInstallation=file:///") || !profile.ends_with(L"/profile")
        || profile.find(L"%23") == profile.npos || profile.find(L"%25") == profile.npos)
        return 2;
    if (std::wstring_view(argv[2]) != L"--headless" || std::wstring_view(argv[3]) != L"--convert-to"
        || std::wstring_view(argv[4]) != L"pdf" || std::wstring_view(argv[5]) != L"--outdir")
        return 3;
    const std::filesystem::path output(argv[6]);
    const std::filesystem::path input(argv[7]);
    if (input != output / L"preview.docx" || !std::filesystem::is_regular_file(input)) return 4;

    std::ofstream file(output / L"preview.pdf", std::ios::binary);
    file << "%PDF-1.4\n%%EOF\n";
    return file ? 0 : 5;
}
