#include "NativeImage.h"

#include <windows.h>
#include <wincodec.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace yiyi {
namespace {

winrt::com_ptr<IWICImagingFactory> imagingFactory()
{
    return winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory, CLSCTX_INPROC_SERVER);
}

NativeImage readPixels(IWICBitmapSource* source)
{
    NativeImage image;
    winrt::check_hresult(source->GetSize(&image.width, &image.height));
    auto const count = std::uint64_t{image.width} * image.height;
    if (count == 0 || count > maximumImagePixels) {
        throw winrt::hresult_invalid_argument(L"截图无效或过大，请缩小范围。");
    }
    image.pixels.resize(static_cast<std::size_t>(count) * 4);
    winrt::check_hresult(source->CopyPixels(nullptr, image.width * 4, static_cast<UINT>(image.pixels.size()), image.pixels.data()));
    return image;
}

unsigned int gray(std::uint8_t const* pixel)
{
    return (pixel[2] * 11u + pixel[1] * 16u + pixel[0] * 5u) / 32u;
}

void compositeOnWhite(NativeImage& image)
{
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        auto* pixel = image.pixels.data() + i;
        auto const alpha = pixel[3];
        if (alpha == 255) continue;
        for (int channel = 0; channel < 3; ++channel) {
            pixel[channel] = static_cast<std::uint8_t>((pixel[channel] * alpha + 255 * (255 - alpha) + 127) / 255);
        }
        pixel[3] = 255;
    }
}

bool hasDarkPerimeter(NativeImage const& image)
{
    auto const stride = std::size_t{image.width} * 4;
    auto const step = (std::max)(1u, (std::min)(image.width, image.height) / 80);
    std::uint32_t dark{};
    std::uint32_t samples{};
    for (std::uint32_t x = 0; x < image.width; x += step) {
        auto const* pixel = image.pixels.data() + 4 * x;
        dark += (gray(pixel) < 100) + (gray(pixel + (image.height - 1) * stride) < 100);
        samples += 2;
    }
    for (std::uint32_t y = 0; y < image.height; y += step) {
        auto const* pixel = image.pixels.data() + y * stride;
        dark += (gray(pixel) < 100) + (gray(pixel + 4 * (image.width - 1)) < 100);
        samples += 2;
    }
    return std::uint64_t{dark} * 5 > std::uint64_t{samples} * 4;
}

}

NativeImage::operator bool() const noexcept
{
    auto const count = std::uint64_t{width} * height;
    return count > 0 && count <= maximumImagePixels && pixels.size() == count * 4;
}

NativeImage cropImage(NativeImage const& image, int logicalWidth, int logicalHeight, ImageRectangle area)
{
    if (!image || logicalWidth <= 0 || logicalHeight <= 0) return {};
    area.left = std::clamp(area.left, 0, logicalWidth);
    area.right = std::clamp(area.right, 0, logicalWidth);
    area.top = std::clamp(area.top, 0, logicalHeight);
    area.bottom = std::clamp(area.bottom, 0, logicalHeight);
    if (area.left >= area.right || area.top >= area.bottom) return {};

    auto const left = std::uint64_t{image.width} * area.left / logicalWidth;
    auto const top = std::uint64_t{image.height} * area.top / logicalHeight;
    auto const right = (std::uint64_t{image.width} * area.right + logicalWidth - 1) / logicalWidth;
    auto const bottom = (std::uint64_t{image.height} * area.bottom + logicalHeight - 1) / logicalHeight;
    NativeImage result{static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top), {}};
    auto const stride = std::size_t{result.width} * 4;
    result.pixels.resize(stride * result.height);
    for (std::size_t row = 0; row < result.height; ++row) {
        auto const offset = ((top + row) * image.width + left) * 4;
        std::memcpy(result.pixels.data() + row * stride, image.pixels.data() + offset, stride);
    }
    return result;
}

NativeImage decodeImage(std::span<std::uint8_t const> bytes)
{
    if (bytes.empty() || bytes.size() > (std::numeric_limits<DWORD>::max)()) {
        throw winrt::hresult_invalid_argument(L"无法读取图片。");
    }
    auto const factory = imagingFactory();
    winrt::com_ptr<IWICStream> stream;
    winrt::check_hresult(factory->CreateStream(stream.put()));
    winrt::check_hresult(stream->InitializeFromMemory(const_cast<std::uint8_t*>(bytes.data()), static_cast<DWORD>(bytes.size())));
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::check_hresult(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()));
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::check_hresult(decoder->GetFrame(0, frame.put()));
    winrt::com_ptr<IWICFormatConverter> converter;
    winrt::check_hresult(factory->CreateFormatConverter(converter.put()));
    winrt::check_hresult(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
                                               WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    return readPixels(converter.get());
}

std::vector<std::uint8_t> encodePng(NativeImage const& image)
{
    if (!image) throw winrt::hresult_invalid_argument(L"截图无效或过大，请缩小范围。");

    auto const factory = imagingFactory();
    winrt::com_ptr<IStream> stream;
    winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::com_ptr<IPropertyBag2> options;
    winrt::check_hresult(encoder->CreateNewFrame(frame.put(), options.put()));
    winrt::check_hresult(frame->Initialize(options.get()));
    winrt::check_hresult(frame->SetSize(image.width, image.height));
    auto format = GUID_WICPixelFormat32bppBGRA;
    winrt::check_hresult(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA) throw winrt::hresult_error(E_FAIL, L"无法编码截图。");
    winrt::check_hresult(frame->WritePixels(image.height, image.width * 4, static_cast<UINT>(image.pixels.size()),
                                            const_cast<std::uint8_t*>(image.pixels.data())));
    winrt::check_hresult(frame->Commit());
    winrt::check_hresult(encoder->Commit());

    STATSTG stats{};
    winrt::check_hresult(stream->Stat(&stats, STATFLAG_NONAME));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stats.cbSize.QuadPart));
    HGLOBAL allocation{};
    winrt::check_hresult(GetHGlobalFromStream(stream.get(), &allocation));
    auto const data = GlobalLock(allocation);
    if (!data) winrt::throw_last_error();
    std::memcpy(bytes.data(), data, bytes.size());
    GlobalUnlock(allocation);
    return bytes;
}

NativeImage prepareOcrImage(NativeImage image)
{
    if (!image) return {};
    compositeOnWhite(image);
    if (hasDarkPerimeter(image)) {
        for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
            for (int channel = 0; channel < 3; ++channel) image.pixels[i + channel] = 255 - image.pixels[i + channel];
        }
    }

    auto const longest = (std::max)(image.width, image.height);
    if (longest >= 1600) return image;
    auto const scale = (std::min)(2.0, 2048.0 / longest);
    auto const width = static_cast<UINT>(std::lround(image.width * scale));
    auto const height = static_cast<UINT>(std::lround(image.height * scale));
    auto const factory = imagingFactory();
    winrt::com_ptr<IWICBitmap> bitmap;
    winrt::check_hresult(factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppBGRA,
        image.width * 4, static_cast<UINT>(image.pixels.size()), image.pixels.data(), bitmap.put()));
    winrt::com_ptr<IWICBitmapScaler> scaler;
    winrt::check_hresult(factory->CreateBitmapScaler(scaler.put()));
    winrt::check_hresult(scaler->Initialize(bitmap.get(), width, height, WICBitmapInterpolationModeLinear));
    return readPixels(scaler.get());
}

}
