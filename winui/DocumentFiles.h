#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace yiyi::documentFiles {

std::filesystem::path sourcePath(std::filesystem::path const& path);
bool hasExtension(std::filesystem::path const& path, std::wstring_view extension);
std::string read(std::filesystem::path const& path, size_t maxBytes);
void requireDifferentOutput(std::filesystem::path const& source, std::filesystem::path const& destination);
void writeAtomically(std::filesystem::path const& path, std::string_view bytes);

class AtomicOutput {
public:
    explicit AtomicOutput(std::filesystem::path const& path);
    ~AtomicOutput();

    AtomicOutput(AtomicOutput const&) = delete;
    AtomicOutput& operator=(AtomicOutput const&) = delete;

    size_t writeAt(uint64_t offset, std::string_view bytes) noexcept;
    void commit();

private:
    struct File;
    std::unique_ptr<File> m_file;
};

}
