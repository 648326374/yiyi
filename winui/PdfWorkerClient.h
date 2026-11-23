#pragma once

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <array>
#include <filesystem>
#include <functional>

namespace yiyi {

using PdfRequestProgress = std::function<void(winrt::Windows::Data::Json::JsonObject const&)>;
using PdfPreviewStatuses = std::function<void(winrt::Windows::Data::Json::JsonArray const&)>;

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Data::Json::JsonObject>
inspectPdfAsync(std::filesystem::path source, PdfRequestProgress progress = {});

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Data::Json::JsonObject>
analyzePdfPagesAsync(std::filesystem::path source, winrt::Windows::Data::Json::JsonObject document,
                     int first, int count, PdfRequestProgress progress = {},
                     winrt::Windows::Data::Json::JsonObject preceding = nullptr);

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::Streams::InMemoryRandomAccessStream>
renderPdfOriginalAsync(std::filesystem::path source, winrt::Windows::Data::Json::JsonObject document,
                       int page, int pixels);

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::Streams::InMemoryRandomAccessStream>
renderPdfTranslationAsync(std::filesystem::path source, winrt::Windows::Data::Json::JsonObject document,
                          winrt::Windows::Data::Json::JsonObject pageData, int page, int pixels,
                          PdfPreviewStatuses statuses = {});

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Data::Json::JsonObject>
analyzePdfScanAsync(std::filesystem::path source, winrt::Windows::Data::Json::JsonObject document, int page, int pixels);

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::Streams::IBuffer>
renderPdfCropAsync(std::filesystem::path source, winrt::Windows::Data::Json::JsonObject document,
                   int page, int pixels, std::array<double, 4> region);

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Data::Json::JsonObject>
exportPdfAsync(std::filesystem::path source, std::filesystem::path taskDatabase, std::filesystem::path destination,
               bool preserveFailed, PdfRequestProgress progress = {});

}
