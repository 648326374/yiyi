#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace yiyi::pdf {

std::string readWorkerInput(std::filesystem::path const& path, std::size_t maxBytes);
void writeWorkerOutput(std::filesystem::path const& path, std::string_view bytes);

}
