#include "NativeImage.h"

#include <winrt/base.h>

#include <iostream>

int main()
{
    try {
        winrt::init_apartment();
        yiyi::NativeImage source{301, 151, std::vector<std::uint8_t>(301 * 151 * 4, 255)};
        for (std::uint32_t y = 0; y < source.height; ++y) {
            for (std::uint32_t x = 0; x < source.width; ++x) {
                auto const pixel = (y * source.width + x) * 4;
                source.pixels[pixel] = static_cast<std::uint8_t>(x);
                source.pixels[pixel + 1] = static_cast<std::uint8_t>(y);
            }
        }

        // At fractional scale the top-left rounds down, and the far edge rounds up.
        auto const crop = yiyi::cropImage(source, 200, 100, {50, 20, 151, 61});
        if (!crop || crop.width != 153 || crop.height != 63 || crop.pixels[0] != 75 || crop.pixels[1] != 30
            || crop.pixels[crop.pixels.size() - 4] != 227 || crop.pixels[crop.pixels.size() - 3] != 92) {
            std::cerr << "Wrong physical pixels in fractional-scale crop.\n";
            return 1;
        }
        auto const clipped = yiyi::cropImage(source, 200, 100, {-5, -4, 1, 1});
        if (clipped.width != 2 || clipped.height != 2 || yiyi::cropImage(source, 200, 100, {2, 2, 1, 1})) {
            std::cerr << "Screen-edge clipping or empty selection failed.\n";
            return 2;
        }
        auto const png = yiyi::encodePng(crop);
        auto const decoded = yiyi::decodeImage(png);
        if (decoded.width != crop.width || decoded.height != crop.height || decoded.pixels != crop.pixels) {
            std::cerr << "Native PNG encoding did not preserve selected pixels.\n";
            return 3;
        }

        yiyi::NativeImage dark{400, 100, std::vector<std::uint8_t>(400 * 100 * 4, 0)};
        for (std::size_t i = 3; i < dark.pixels.size(); i += 4) dark.pixels[i] = 255;
        auto const prepared = yiyi::prepareOcrImage(dark);
        if (prepared.width != 800 || prepared.height != 200 || prepared.pixels[0] != 255 || dark.pixels[0] != 0) {
            std::cerr << "OCR dark-background inversion or small-text scaling failed.\n";
            return 4;
        }
        std::cout << "Native fractional crop, edge clipping, PNG round-trip and OCR scaling passed.\n";
        return 0;
    } catch (winrt::hresult_error const& error) {
        std::cerr << winrt::to_string(error.message()) << '\n';
        return 5;
    }
}
