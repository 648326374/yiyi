#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace yiyi {

inline constexpr std::uint64_t maximumImagePixels = 64000000;

struct NativeImage {
    std::uint32_t width{};
    std::uint32_t height{};
    // Top-down, straight-alpha BGRA with no padding between rows.
    std::vector<std::uint8_t> pixels;

    explicit operator bool() const noexcept;
};

struct ImageRectangle {
    int left{};
    int top{};
    int right{};
    int bottom{};
};

NativeImage cropImage(NativeImage const& image, int logicalWidth, int logicalHeight, ImageRectangle selection);
NativeImage decodeImage(std::span<std::uint8_t const> bytes);
std::vector<std::uint8_t> encodePng(NativeImage const& image);
NativeImage prepareOcrImage(NativeImage image);

}
