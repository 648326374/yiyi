#include "pch.h"
#include "MainWindow.xaml.h"
#include "DocumentWorkspace.h"

namespace winrt::Yiyi::WinUI::implementation
{
    using namespace Microsoft::UI::Xaml::Controls;

    void MainWindow::showDocuments()
    {
        try {
            if (!m_documentWorkspace) {
                const auto weak = get_weak();
                ::yiyi::DocumentWorkspaceHost host;
                host.owner = NativeWindowHandle();
                host.profile = m_dataDirectory;
                host.providers = [weak] {
                    if (const auto self = weak.get()) return self->m_settings.providers;
                    return std::vector<::yiyi::Provider>{};
                };
                host.currentSettings = [weak] {
                    ::yiyi::DocumentSettings settings;
                    if (const auto self = weak.get()) {
                        for (const auto& provider : self->m_settings.providers) {
                            if (provider.id == self->m_settings.selectedProviderId) {
                                settings.provider = provider;
                                break;
                            }
                        }
                        settings.sourceLanguage = self->m_settings.sourceLanguage;
                        settings.targetLanguage = self->m_settings.targetLanguage;
                        settings.tone = self->m_settings.tone;
                    }
                    return settings;
                };
                host.modelAvailable = [weak] {
                    const auto self = weak.get();
                    return self && !self->m_closed && !self->m_busy && !self->m_screenshot && !self->pdfRequestBusy();
                };
                host.settingsChanged = [weak](::yiyi::DocumentSettings const& settings) {
                    const auto self = weak.get();
                    if (!self || self->m_closed) return;
                    auto next = self->m_settings;
                    next.selectedProviderId = settings.provider.id;
                    next.sourceLanguage = settings.sourceLanguage;
                    next.targetLanguage = settings.targetLanguage;
                    next.tone = settings.tone;
                    self->applySettings(std::move(next));

                    const auto selectText = [](ComboBox const& picker, hstring const& value) {
                        const auto items = picker.Items();
                        for (uint32_t i = 0; i < items.Size(); ++i) {
                            if (unbox_value<hstring>(items.GetAt(i)) == value) {
                                picker.SelectedIndex(static_cast<int>(i));
                                break;
                            }
                        }
                    };
                    self->m_updating = true;
                    selectText(self->SourceLanguage(), settings.sourceLanguage);
                    selectText(self->TargetLanguage(), settings.targetLanguage);
                    selectText(self->TonePicker(), settings.tone);
                    self->m_updating = false;
                };
                host.openSettings = [weak] {
                    if (const auto self = weak.get()) self->Navigation().SelectedItem(self->SettingsItem());
                };
                host.reviewImage = [weak](Windows::Storage::Streams::IBuffer const& image) {
                    if (const auto self = weak.get(); self && !self->m_closed && !self->m_screenshot)
                        self->reviewScreenshot(image);
                };
                m_documentWorkspace = ::yiyi::DocumentWorkspace::create(std::move(host));
                DocumentsHost().Content(m_documentWorkspace->root());
            } else {
                refreshDocuments();
            }
        } catch (...) {
            reportError(L"无法打开文档工作区。");
        }
    }

    Windows::Foundation::IAsyncAction MainWindow::OpenDocumentAsync(std::filesystem::path path)
    {
        const auto lifetime = get_strong();
        if (m_closed) co_return;
        Navigation().SelectedItem(DocumentsItem());
        if (!m_documentWorkspace) showDocuments();
        if (const auto workspace = m_documentWorkspace) co_await workspace->open(std::move(path));
    }

    void MainWindow::refreshDocuments()
    {
        if (m_documentWorkspace) m_documentWorkspace->refresh();
    }

    void MainWindow::closeDocuments()
    {
        if (m_documentWorkspace) m_documentWorkspace->close();
    }

    bool MainWindow::documentsBusy() const
    {
        return m_documentWorkspace && m_documentWorkspace->busy();
    }
}
