#pragma once

#include "OfficeTask.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Storage.Streams.h>

namespace yiyi {

struct DocumentWorkspaceHost {
    void* owner{};
    std::filesystem::path profile;
    std::function<std::vector<Provider>()> providers;
    std::function<DocumentSettings()> currentSettings;
    std::function<void(DocumentSettings const&)> settingsChanged;
    std::function<bool()> modelAvailable;
    std::function<void()> openSettings;
    std::function<void(winrt::Windows::Storage::Streams::IBuffer const&)> reviewImage;
};

class DocumentWorkspace : public std::enable_shared_from_this<DocumentWorkspace> {
public:
    static std::shared_ptr<DocumentWorkspace> create(DocumentWorkspaceHost host);
    winrt::Microsoft::UI::Xaml::UIElement root() const;
    winrt::Windows::Foundation::IAsyncAction open(std::filesystem::path path);
    void refresh();
    bool busy() const;
    void close();

private:
    struct Preferences {
        winrt::Microsoft::UI::Xaml::Controls::ComboBox provider{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::ComboBox source{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::ComboBox target{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::ComboBox tone{nullptr};
        std::vector<Provider> providers;
    };

    explicit DocumentWorkspace(DocumentWorkspaceHost host);
    void build();
    void buildText();
    void buildOffice();
    winrt::Microsoft::UI::Xaml::Controls::Grid preferenceBar(Preferences& preferences,
        winrt::Microsoft::UI::Xaml::Controls::StackPanel const& providerPanel);
    void updatePreferences(Preferences& preferences, DocumentSettings const& settings);
    winrt::fire_and_forget preferencesChanged(bool office);
    DocumentSettings selectedSettings(Preferences const& preferences) const;
    void updateText();
    void updateOffice(bool preferences = false);
    void updateOfficeSelection();
    void report(bool office, winrt::hstring const& message, bool isError = false);
    bool available(bool office);
    DocumentCallbacks callbacks(bool office);

    winrt::fire_and_forget pick(bool office);
    winrt::fire_and_forget save(bool office);
    winrt::fire_and_forget translateText();
    winrt::fire_and_forget translateOffice(std::optional<winrt::hstring> id = std::nullopt);
    winrt::fire_and_forget resetText();
    winrt::fire_and_forget resetOffice();
    winrt::fire_and_forget chooseSegment(int selection);
    winrt::Windows::Foundation::IAsyncOperation<bool> resolveDraft(winrt::hstring title);
    winrt::fire_and_forget showIssues();
    winrt::fire_and_forget showImages();
    winrt::fire_and_forget configureOffice();
    winrt::fire_and_forget selectRenderer();
    winrt::fire_and_forget generateLayoutPreview();
    winrt::fire_and_forget openLayoutPreview();
    void copyText();
    void commitDraft();
    void editDraft();
    void keepSegment();

    DocumentWorkspaceHost m_host;
    std::shared_ptr<DocumentTask> m_text;
    std::shared_ptr<OfficeTask> m_office;
    winrt::Microsoft::UI::Xaml::Controls::Grid m_root{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ListView m_tabs{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid m_textPage{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid m_officePage{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueue m_queue{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_draftTimer{nullptr};
    Preferences m_textSettings;
    Preferences m_officeSettings;
    winrt::Microsoft::UI::Xaml::Controls::ComboBox m_encoding{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox m_txtSource{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox m_txtResult{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox m_docSource{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox m_docResult{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ProgressBar m_txtProgress{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ProgressBar m_docProgress{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock m_txtName{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock m_docName{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock m_txtCount{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock m_docCount{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock m_docLocation{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock m_txtStatus{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock m_docStatus{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock m_txtTask{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock m_docTask{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::InfoBar m_txtInfo{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::InfoBar m_docInfo{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ListView m_segmentList{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_textOpen{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_textTranslate{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_textReset{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_textSave{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_textCopy{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_officeOpen{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_officeTranslate{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_officeSave{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_officeIssues{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_officeImages{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_officeOptions{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_officeEdit{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_officeKeep{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button m_officeRetry{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem m_previewOpen{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem m_fitOptions{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem m_officeReset{nullptr};
    std::filesystem::path m_renderer;
    std::shared_ptr<OfficeLayoutPreview const> m_preview;
    int m_segment{-1};
    bool m_updating{};
    bool m_updatingPreferences{};
    bool m_modal{};
    bool m_fileOperation{};
    bool m_modelTask{};
    bool m_closed{};
};

}
