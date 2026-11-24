#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>

int wmain(int argc, wchar_t const* argv[])
{
    if (argc < 8 || argc > 9) return 1;
    const std::wstring_view profile(argv[1]);
    if (!profile.starts_with(L"-env:UserInstallation=file:///") || !profile.ends_with(L"/profile")
        || profile.find(L"%23") == profile.npos || profile.find(L"%25") == profile.npos)
        return 2;
    if (std::wstring_view(argv[2]) != L"--headless" || std::wstring_view(argv[3]) != L"--convert-to"
        || std::wstring_view(argv[4]) != L"pdf" || std::wstring_view(argv[5]) != L"--outdir")
        return 3;
    const std::filesystem::path output(argv[6]);
    if (std::filesystem::exists(std::filesystem::path(argv[0]).parent_path() / L"hold-preview"))
        std::this_thread::sleep_for(std::chrono::seconds(20));
    for (int i = 7; i < argc; ++i) {
        const std::filesystem::path input(argv[i]);
        if (input.parent_path() != output || !std::filesystem::is_regular_file(input) ||
            (input.filename() != L"original.docx" && input.filename() != L"preview.docx" && input.filename() != L"preview.pptx"))
            return 4;

        auto pdf = input;
        pdf.replace_extension(L".pdf");
        std::ofstream file(pdf, std::ios::binary);
        file << "%PDF-1.4\n%%EOF\n";
        if (!file) return 5;
    }
    return 0;
}
