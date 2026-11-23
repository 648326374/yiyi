#pragma once

#include "MainWindow.g.h"
#include "DesktopIntegration.h"
#include "HistoryStore.h"
#include "PdfTask.h"
#include "PdfWorkerClient.h"
#include "WorkspaceSettings.h"

#include <filesystem>
#include <map>
#include <memory>

namespace yiyi { class DocumentWorkspace; }

namespace winrt::Yiyi::WinUI::implementation
{
    struct ProviderEditor;

    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        explicit MainWindow(std::filesystem::path dataDirectory);

        Windows::Foundation::IAsyncAction OpenPdfAsync(std::filesystem::path path);
        Windows::Foundation::IAsyncAction OpenDocumentAsync(std::filesystem::path path);
        HWND NativeWindowHandle();
        void StartDesktop(std::wstring channel);
        void ShowDesktopWorkspace();
        void ShowDesktopSettings();
        void StartScreenshot();
        void NavigationChanged(Microsoft::UI::Xaml::Controls::NavigationView const&,
            Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
        void ActiveProviderChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void PreferencesChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void LibrarySelectionChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void SearchChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        void WorkbenchTextChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        void OpenLibraryItem(IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);

        void Translate(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Cancel(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Clear(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Copy(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SwapLanguages(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ApplySidebarTheme(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenWorkbenchSettings(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget PasteWorkbenchText(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void LoadWorkbenchExample(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void NewLibraryQuery(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void NewProvider(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenHistorySelection(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ToggleHistoryFavorite(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ToggleResultFavorite(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget DeleteHistory(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget PickPdf(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PreviousPdfPage(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void NextPdfPage(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CommitPdfPage(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PdfPageNumberKey(IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
        void PdfZoomChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void PdfViewportChanged(IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void TogglePdfReading(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget OpenPdfSettings(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PdfTargetChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void PdfScopeChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void TranslatePdf(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RetranslatePdfPage(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void TogglePdfAnalysis(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void DiscardPdfTask(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget ResetPdfTranslations(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget ExportPdf(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget OpenPdfIssues(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget ReviewPdfBlocks(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget OpenPdfStructure(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void TogglePdfTextView(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PdfScanTabClicked(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CopyPdfScanText(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget OpenPdfRecognitionDraft(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget RecognizePdfPage(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget RecognizePdfRange(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget RecognizePdfCrop(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ImportDragOver(IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        fire_and_forget ImportDrop(IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        void DesktopPreferencesChanged(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ApplyDesktopShortcuts(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ResetDesktopShortcuts(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void DesktopShortcutKeyDown(IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
        void DesktopShortcutFocusChanged(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PickScreenshot(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        void fillPreferences();
        void refreshProviders();
        void selectProvider(hstring const& id);
        fire_and_forget showProviderEditor(hstring id, int preset);
        fire_and_forget removeProvider(hstring id);
        fire_and_forget probeProvider(std::shared_ptr<ProviderEditor> editor, bool vision);
        void applySettings(::yiyi::WorkspaceSettings next);
        void updateTheme();
        void setBusy(bool busy, bool probe = false);
        void notice(hstring const& message, Microsoft::UI::Xaml::Controls::InfoBarSeverity severity =
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational);
        void reportError(hstring const& fallback);
        void refreshLibrary(int64_t selection = 0);
        ::yiyi::HistoryRecord const* selectedRecord();
        ::yiyi::HistoryRecord const* recordForAction(IInspectable const& sender);
        void updateHistoryActions();
        void updateResultActions();
        void updateWorkbenchChrome();
        void updateWorkbenchCharacterCount();
        void setFavorite(int64_t id, bool favorite);
        fire_and_forget runRequest(::yiyi::Provider provider, hstring system, hstring input,
            ::yiyi::HistoryRecord record);
        Windows::Foundation::IAsyncAction renderPdfPage(int page);
        Windows::Foundation::IAsyncAction editPdfBlock(int page, Windows::Data::Json::JsonObject block);
        void updatePdfNavigation();
        void updatePdfTaskUi();
        void updatePdfBlockOverlay();
        void showPdfError(hstring const& error);
        void closePdf();
        bool pdfRequestBusy() const;
        ::yiyi::PdfTaskCallbacks pdfCallbacks();
        ::yiyi::Provider pdfProvider(hstring const& id) const;
        ::yiyi::DocumentSettings pdfSettings() const;
        fire_and_forget runPdfOperation(Windows::Foundation::IAsyncAction operation, bool refreshPreview);
        void setPdfReading(bool reading);
        int pdfPageCount() const;
        void refreshDesktopSettings();
        void recordDesktopShortcut(std::wstring const&);
        void lookupDesktopSelection(std::wstring const&);
        fire_and_forget reviewScreenshot(Windows::Storage::Streams::IBuffer image);
        void screenshotTargetChanged(hstring const&);
        void showDocuments();
        void closeDocuments();
        void refreshDocuments();
        bool documentsBusy() const;
        void showDictionaryEntry(Windows::Data::Json::JsonObject const& entry);

        std::filesystem::path m_dataDirectory;
        ::yiyi::WorkspaceSettings m_settings;
        std::unique_ptr<::yiyi::HistoryStore> m_history;
        std::unique_ptr<::yiyi::DesktopIntegration> m_desktop;
        std::shared_ptr<::yiyi::DocumentWorkspace> m_documentWorkspace;
        std::vector<::yiyi::HistoryRecord> m_library;
        Windows::Foundation::IAsyncOperation<hstring> m_request{ nullptr };
        Windows::Foundation::IAsyncAction m_screenshot{nullptr};
        hstring m_dictionaryViewSignature;
        hstring m_workbenchProvider;
        hstring m_workbenchTiming;
        hstring m_historyFilter;
        hstring m_favoritesFilter;
        bool m_workbenchExample = false;
        bool m_workbenchRequest = false;
        std::filesystem::path m_pdfSource;
        std::shared_ptr<::yiyi::PdfTask> m_pdfTask;
        Windows::Data::Json::JsonObject m_pdfDocument{nullptr};
        Windows::Data::Json::JsonObject m_pdfDisplayedPage{nullptr};
        std::map<int, Windows::Data::Json::JsonObject> m_pdfStructureUndo;
        Windows::Foundation::IAsyncOperation<Windows::Data::Json::JsonObject> m_pdfInspect{nullptr};
        Windows::Foundation::IAsyncOperation<Windows::Storage::Streams::InMemoryRandomAccessStream> m_pdfRender{nullptr};
        Windows::Foundation::IAsyncOperation<Windows::Data::Json::JsonObject> m_pdfExport{nullptr};
        Windows::Foundation::IAsyncInfo m_pdfCrop{nullptr};
        Windows::Foundation::IAsyncAction m_pdfOperation{nullptr};
        uint64_t m_pdfGeneration = 0;
        uint64_t m_pdfOperationGeneration = 0;
        int m_pdfPage = 0;
        bool m_pdfOpening = false;
        bool m_pdfReading = false;
        bool m_pdfTextView = false;
        bool m_pdfScanTranslation = false;
        bool m_pdfTargetUpdating = false;
        bool m_updatingDesktop = false;
        bool m_screenshotVision = false;
        uint64_t m_requestSerial = 0;
        int64_t m_resultId = 0;
        bool m_resultIsFavorite = false;
        bool m_favoritesOnly = false;
        bool m_updating = true;
        bool m_settingsLoaded = false;
        bool m_busy = false;
        bool m_closed = false;
    };
}

namespace winrt::Yiyi::WinUI::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
