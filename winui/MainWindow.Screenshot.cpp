#include "pch.h"
#include "MainWindow.xaml.h"
#include "ScreenshotDialog.h"

namespace winrt::Yiyi::WinUI::implementation
{
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;
    using namespace Windows::Storage::Streams;

    void MainWindow::PickScreenshot(IInspectable const&, RoutedEventArgs const&)
    {
        StartScreenshot();
    }

    void MainWindow::StartScreenshot()
    {
        if (!m_closed && !m_screenshot) reviewScreenshot(nullptr);
    }

    void MainWindow::screenshotTargetChanged(hstring const& target)
    {
        auto items = TargetLanguage().Items();
        for (uint32_t index = 0; index < items.Size(); ++index) {
            if (unbox_value<hstring>(items.GetAt(index)) == target) {
                TargetLanguage().SelectedIndex(static_cast<int>(index));
                return;
            }
        }
    }

    fire_and_forget MainWindow::reviewScreenshot(IBuffer image)
    {
        const auto lifetime = get_strong();
        try {
            if (m_desktop) m_desktop->showWindow();
            const auto weak = get_weak();
            ::yiyi::ScreenshotDialogOptions options;
            options.owner = NativeWindowHandle();
            options.root = Workspace().XamlRoot();
            options.theme = Workspace().RequestedTheme();
            options.providers = m_settings.providers;
            options.translationProviderId = m_settings.selectedProviderId;
            options.visionProviderId = m_settings.visionProviderId;
            options.targetLanguage = m_settings.targetLanguage;
            options.visionMode = static_cast<bool>(image) || m_screenshotVision;
            options.aiAvailable = [weak] {
                const auto self = weak.get();
                return self && !self->m_closed && !self->m_busy && !self->documentsBusy() && !self->pdfRequestBusy();
            };
            options.targetChanged = [weak](hstring const& target) {
                if (const auto self = weak.get(); self && !self->m_closed)
                    self->screenshotTargetChanged(target);
            };
            options.visionChanged = [weak](bool vision, hstring const& id) {
                const auto self = weak.get();
                if (!self || self->m_closed) return;
                self->m_screenshotVision = vision;
                if (id == self->m_settings.visionProviderId) return;
                try {
                    auto next = self->m_settings;
                    next.visionProviderId = id;
                    self->applySettings(std::move(next));
                } catch (...) {
                    self->reportError(L"无法保存识图服务。");
                }
            };
            options.useText = [weak](hstring const& source, hstring const& target, bool translate) {
                const auto self = weak.get();
                if (!self || self->m_closed) return;
                const bool busy = self->m_busy || self->documentsBusy() || self->pdfRequestBusy();
                if (busy || source.size() > 24000) {
                    self->notice(busy ? L"请先停止当前请求。" : L"取字结果超过 24,000 个字符，请分段使用。",
                        InfoBarSeverity::Warning);
                    return;
                }
                self->Navigation().SelectedItem(self->TranslateItem());
                self->Clear(nullptr, nullptr);
                self->SourceText().Text(source);
                self->screenshotTargetChanged(target);
                if (translate) self->Translate(nullptr, nullptr);
            };
            ScreenshotButton().IsEnabled(false);
            m_screenshot = ::yiyi::showScreenshotDialogAsync(std::move(options), std::move(image));
            co_await m_screenshot;
        } catch (hresult_canceled const&) {
        } catch (...) {
            if (!m_closed) reportError(L"截图取字失败。");
        }
        m_screenshot = nullptr;
        if (!m_closed) ScreenshotButton().IsEnabled(true);
    }
}
