#include "../pdf/pdfmerge.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 7) return 2;
    const std::filesystem::path runtime = argv[1], source = argv[2], partOne = argv[3], partTwo = argv[4],
        merged = argv[5], pageCopy = argv[6];
    using namespace yiyi::pdf::merge;
    try {
        require(patternPages(runtime, source, 0, 1) == std::set<int>{0}, "nested inherited PDF pattern resources");
        copyPages(runtime, source, pageCopy, 0, 0);

        bool incomplete = false;
        auto rejected = merged;
        rejected += L".incomplete";
        try {
            write(runtime, source, {partOne}, rejected, 2);
        } catch (const std::runtime_error&) {
            incomplete = true;
        }
        require(incomplete && !std::filesystem::exists(rejected), "incomplete merge created an output file");

        int mergedPages = 0;
        int assembled = -1;
        write(runtime, source, {partOne, partTwo}, merged, 2, [&](std::string_view stage, int completed, int total) {
            if (stage == "merge") {
                require(total == 2 && completed == ++mergedPages, "merging progress order");
            } else {
                require(stage == "assembling" && total == 100 && completed >= assembled, "assembling progress order");
                assembled = completed;
            }
        });
        require(mergedPages == 2 && assembled >= 0 && assembled <= 100, "merge progress reporting");

        bool callbackFailed = false;
        rejected = merged;
        rejected += L".callback";
        try {
            write(runtime, source, {partOne, partTwo}, rejected, 2, [](std::string_view stage, int, int) {
                if (stage == "assembling") throw std::logic_error("controlled progress exception");
            });
        } catch (const std::logic_error& error) {
            callbackFailed = std::string_view(error.what()) == "controlled progress exception";
        }
        require(callbackFailed, "exception crossed the foreign qpdf ABI or was swallowed");
        std::cout << "PDF merge operations passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
