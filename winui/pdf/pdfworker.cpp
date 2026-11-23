#include "../NativePaths.h"
#include "pdfdocument.h"
#include "pdfworkerfiles.h"

#include <winrt/Windows.Foundation.Collections.h>

#include <chrono>
#include <cstdio>

int wmain(int argc, wchar_t** argv)
{
    using namespace winrt;
    using namespace Windows::Data::Json;
    if (argc != 3) return 2;
    init_apartment(apartment_type::multi_threaded);
    JsonObject request{nullptr};
    try {
        if (!JsonObject::TryParse(to_hstring(yiyi::pdf::readWorkerInput(argv[1], 32 * 1024 * 1024)), request)) return 3;
    } catch (...) {
        return 3;
    }

    JsonObject result{nullptr};
    try {
        const auto progressEnabled = request.GetNamedBoolean(L"progress", false);
        using Clock = std::chrono::steady_clock;
        auto last = Clock::time_point::min();
        const auto progress = [&](JsonObject const& value) {
            const auto now = Clock::now();
            if (!progressEnabled) return;
            if (last != Clock::time_point::min() && now - last < std::chrono::milliseconds(100) &&
                value.GetNamedNumber(L"completed") != value.GetNamedNumber(L"total"))
                return;
            last = now;
            JsonObject event;
            event.Insert(L"progress", value);
            const auto line = to_string(event.Stringify()) + '\n';
            std::fwrite(line.data(), 1, line.size(), stdout);
            std::fflush(stdout);
        };
        result = yiyi::pdf::executeDocumentRequest(request, yiyi::executableDirectory() / L"pdf", progress);
    } catch (...) {
        result = JsonObject{};
        result.Insert(L"error", JsonValue::CreateStringValue(to_message()));
    }

    try {
        yiyi::pdf::writeWorkerOutput(argv[2], to_string(result.Stringify()));
    } catch (...) {
        return 4;
    }
    return 0;
}
