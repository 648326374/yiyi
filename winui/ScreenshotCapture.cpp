#include "ScreenshotCapture.h"

#include "NativeImage.h"

#include <windowsx.h>
#include <winrt/Windows.Security.Cryptography.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "user32.lib")

namespace yiyi {
namespace {

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Security::Cryptography;
using namespace winrt::Windows::Storage::Streams;

struct ScreenDc {
    HDC value{GetDC(nullptr)};
    ~ScreenDc() { if (value) ReleaseDC(nullptr, value); }
};

struct MemoryDc {
    HDC value;
    ~MemoryDc() { if (value) DeleteDC(value); }
};

struct GdiObject {
    HGDIOBJ value;
    ~GdiObject() { if (value) DeleteObject(value); }
};

struct SelectedObject {
    HDC dc;
    HGDIOBJ previous;
    ~SelectedObject() { if (previous && previous != HGDI_ERROR) SelectObject(dc, previous); }
};

struct DpiScope {
    DPI_AWARENESS_CONTEXT previous{SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)};
    ~DpiScope() { if (previous) SetThreadDpiAwarenessContext(previous); }
};

struct WindowVisibility {
    HWND owner;
    bool visible;

    explicit WindowVisibility(HWND window) : owner(window), visible(window && IsWindowVisible(window))
    {
        if (visible) ShowWindowAsync(owner, SW_HIDE);
    }

    ~WindowVisibility()
    {
        if (visible && IsWindow(owner)) {
            ShowWindowAsync(owner, SW_SHOW);
            SetForegroundWindow(owner);
        }
    }
};

BITMAPINFO bitmapInfo(std::uint32_t width, std::uint32_t height)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return info;
}

NativeImage readMonitor(HDC screen, RECT const& bounds)
{
    auto const width = bounds.right - bounds.left;
    auto const height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) * height > maximumImagePixels) return {};
    auto const info = bitmapInfo(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    void* bits{};
    GdiObject bitmap{CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0)};
    MemoryDc destination{CreateCompatibleDC(screen)};
    if (!bitmap.value || !destination.value) return {};
    SelectedObject selected{destination.value, SelectObject(destination.value, bitmap.value)};
    if (!selected.previous || selected.previous == HGDI_ERROR) return {};
    if (!BitBlt(destination.value, 0, 0, width, height, screen, bounds.left, bounds.top, SRCCOPY | CAPTUREBLT)) return {};
    GdiFlush();

    NativeImage image{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), {}};
    auto const* start = static_cast<std::uint8_t const*>(bits);
    image.pixels.assign(start, start + static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 3; i < image.pixels.size(); i += 4) image.pixels[i] = 255;
    return image;
}

struct MonitorOverlay;

struct Selection {
    MonitorOverlay* monitor{};
    ImageRectangle region{};
    bool done{};
};

struct MonitorOverlay {
    RECT bounds;
    NativeImage image;
    Selection& selection;
    HWND window{};
    POINT start{};
    POINT current{};
    bool dragging{};

    ~MonitorOverlay() { if (window) DestroyWindow(window); }

    int width() const { return bounds.right - bounds.left; }
    int height() const { return bounds.bottom - bounds.top; }

    POINT bounded(LPARAM position) const
    {
        return { std::clamp<LONG>(GET_X_LPARAM(position), 0, width() - 1),
                 std::clamp<LONG>(GET_Y_LPARAM(position), 0, height() - 1) };
    }

    RECT area() const
    {
        return { (std::min)(start.x, current.x), (std::min)(start.y, current.y),
                 (std::max)(start.x, current.x) + 1, (std::max)(start.y, current.y) + 1 };
    }

    void paint() const
    {
        PAINTSTRUCT painting{};
        auto const dc = BeginPaint(window, &painting);
        if (!dc) {
            EndPaint(window, &painting);
            return;
        }
        auto const info = bitmapInfo(image.width, image.height);
        auto const drawImage = [&] {
            StretchDIBits(dc, 0, 0, width(), height(), 0, 0, width(), height(),
                          image.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        };
        drawImage();
        MemoryDc dim{CreateCompatibleDC(dc)};
        GdiObject black{CreateCompatibleBitmap(dc, 1, 1)};
        if (dim.value && black.value) {
            SelectedObject selected{dim.value, SelectObject(dim.value, black.value)};
            PatBlt(dim.value, 0, 0, 1, 1, BLACKNESS);
            AlphaBlend(dc, 0, 0, width(), height(), dim.value, 0, 0, 1, 1, BLENDFUNCTION{AC_SRC_OVER, 0, 95, 0});
        }
        if (dragging) {
            auto rectangle = area();
            auto const saved = SaveDC(dc);
            if (saved) {
                IntersectClipRect(dc, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
                drawImage();
                RestoreDC(dc, saved);
            }
            GdiObject outline{CreateSolidBrush(RGB(128, 216, 164))};
            FrameRect(dc, &rectangle, static_cast<HBRUSH>(outline.value));
            if (InflateRect(&rectangle, -1, -1)) FrameRect(dc, &rectangle, static_cast<HBRUSH>(outline.value));
        }

        static constexpr wchar_t hint[] = L"拖动框选文字区域 · 松开完成截图 · Esc / 右键取消";
        auto const dpi = static_cast<int>(GetDpiForWindow(window));
        GdiObject font{CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI")};
        {
            SelectedObject selectedFont{dc, font.value ? SelectObject(dc, font.value) : nullptr};
            SIZE extent{};
            GetTextExtentPoint32W(dc, hint, ARRAYSIZE(hint) - 1, &extent);
            auto const hintWidth = (std::min)(width(), static_cast<int>(extent.cx) + MulDiv(32, dpi, 96));
            auto const top = MulDiv(24, dpi, 96);
            RECT text{(width() - hintWidth) / 2, top, (width() + hintWidth) / 2, top + MulDiv(44, dpi, 96)};
            GdiObject green{CreateSolidBrush(RGB(30, 43, 36))};
            FillRect(dc, &text, static_cast<HBRUSH>(green.value));
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, hint, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        }
        EndPaint(window, &painting);
    }
};

LRESULT CALLBACK overlayProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto const* params = reinterpret_cast<CREATESTRUCTW const*>(lParam);
        auto* overlay = static_cast<MonitorOverlay*>(params->lpCreateParams);
        overlay->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(overlay));
    }
    auto* overlay = reinterpret_cast<MonitorOverlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!overlay) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_NCDESTROY:
        overlay->window = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        overlay->paint();
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(window);
        overlay->start = overlay->current = overlay->bounded(lParam);
        overlay->dragging = true;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (overlay->dragging) {
            overlay->current = overlay->bounded(lParam);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        overlay->dragging = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (overlay->dragging) {
            overlay->current = overlay->bounded(lParam);
            overlay->dragging = false;
            ReleaseCapture();
            auto const rectangle = overlay->area();
            auto const minimum = MulDiv(4, static_cast<int>(GetDpiForWindow(window)), 96);
            if (rectangle.right - rectangle.left >= minimum && rectangle.bottom - rectangle.top >= minimum) {
                overlay->selection = { overlay, {rectangle.left, rectangle.top, rectangle.right, rectangle.bottom}, true };
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam != VK_ESCAPE) break;
        [[fallthrough]];
    case WM_RBUTTONDOWN:
    case WM_CLOSE:
        overlay->selection.done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

constexpr wchar_t overlayClass[] = L"YiyiWinUIScreenshotOverlay";

void registerOverlayClass()
{
    static std::once_flag registered;
    std::call_once(registered, [] {
        WNDCLASSW type{};
        type.lpfnWndProc = overlayProcedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32515)); // IDC_CROSS
        type.lpszClassName = overlayClass;
        if (RegisterClassW(&type) == 0) winrt::throw_last_error();
    });
}

NativeImage selectMonitorRegion(HANDLE cancelled)
{
    DpiScope dpi;
    if (!dpi.previous) winrt::throw_last_error();

    struct MonitorList {
        std::vector<RECT> rectangles;
        std::exception_ptr error;
    } monitors;
    auto const enumerated = EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, RECT* rect, LPARAM context) -> BOOL {
        auto& list = *reinterpret_cast<MonitorList*>(context);
        try {
            list.rectangles.push_back(*rect);
            return TRUE;
        } catch (...) {
            list.error = std::current_exception();
            return FALSE;
        }
    }, reinterpret_cast<LPARAM>(&monitors));
    if (monitors.error) std::rethrow_exception(monitors.error);
    if (!enumerated) winrt::throw_last_error();

    Selection selection;
    std::vector<std::unique_ptr<MonitorOverlay>> overlays;
    {
        ScreenDc screen;
        for (auto const& bounds : monitors.rectangles) {
            if (WaitForSingleObject(cancelled, 0) == WAIT_OBJECT_0) return {};
            auto image = screen.value ? readMonitor(screen.value, bounds) : NativeImage{};
            if (!image) continue;
            overlays.emplace_back(new MonitorOverlay{bounds, std::move(image), selection});
        }
    }
    if (overlays.empty()) throw winrt::hresult_error(E_FAIL, L"无法截取当前屏幕，请确认桌面可见且未锁定。");

    registerOverlayClass();
    POINT cursor{};
    GetCursorPos(&cursor);
    HWND active{};
    for (auto const& overlay : overlays) {
        auto const window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, overlayClass,
            L"截图框选：拖动选择区域，Esc 或右键取消", WS_POPUP,
            overlay->bounds.left, overlay->bounds.top, overlay->width(), overlay->height(), nullptr, nullptr,
            GetModuleHandleW(nullptr), overlay.get());
        if (!window) winrt::throw_last_error();
        ShowWindow(window, SW_SHOWNOACTIVATE);
        if (!active || PtInRect(&overlay->bounds, cursor)) active = window;
    }
    SetForegroundWindow(active);
    SetActiveWindow(active);
    SetFocus(active);

    while (!selection.done) {
        auto const ready = MsgWaitForMultipleObjects(1, &cancelled, FALSE, INFINITE, QS_ALLINPUT);
        if (ready == WAIT_OBJECT_0) return {};
        if (ready != WAIT_OBJECT_0 + 1) winrt::throw_last_error();
        MSG message{};
        while (!selection.done && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return {};
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (!selection.monitor) return {};
    auto const& monitor = *selection.monitor;
    return cropImage(monitor.image, monitor.width(), monitor.height(), selection.region);
}

}

IAsyncOperation<IBuffer> captureScreenRegionAsync(HWND owner)
{
    auto token = co_await winrt::get_cancellation_token();
    auto signal = std::make_shared<winrt::handle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!*signal) winrt::throw_last_error();
    token.callback([signal] { SetEvent(signal->get()); });
    co_await winrt::resume_background();

    static std::mutex captureMutex;
    std::unique_lock lock{captureMutex, std::try_to_lock};
    if (!lock) throw winrt::hresult_error(E_FAIL, L"截图已经开始。");
    WindowVisibility restore{owner};
    auto const ready = WaitForSingleObject(signal->get(), 220);
    if (ready == WAIT_OBJECT_0) co_return nullptr;
    if (ready != WAIT_TIMEOUT) winrt::throw_last_error();

    auto const image = selectMonitorRegion(signal->get());
    if (!image) co_return nullptr;

    winrt::init_apartment();
    struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
    auto const png = encodePng(image);
    co_return CryptographicBuffer::CreateFromByteArray(png);
}

IAsyncOperation<InMemoryRandomAccessStream> imageStreamAsync(IBuffer image)
{
    InMemoryRandomAccessStream stream;
    DataWriter writer{stream};
    writer.WriteBuffer(image);
    co_await writer.StoreAsync();
    writer.DetachStream();
    stream.Seek(0);
    co_return stream;
}

}
