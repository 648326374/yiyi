#include "pch.h"
#include "MainWindow.xaml.h"

#include <microsoft.ui.xaml.window.h>
#include <shobjidl_core.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <cwchar>
#include <cmath>

namespace winrt::Yiyi::WinUI::implementation
{
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;
    using namespace Windows::ApplicationModel::DataTransfer;
    using namespace Windows::Data::Json;
    using namespace Windows::Foundation;
    using namespace Windows::Storage;

    namespace {
        constexpr std::wstring_view pdfTargets[] = {L"简体中文", L"繁體中文", L"英语"};

        JsonArray visibleBlocks(JsonObject const& page)
        {
            return page ? page.GetNamedObject(L"scan", page).GetNamedArray(L"blocks", JsonArray{}) : JsonArray{};
        }

        hstring pageText(JsonObject const& page, wchar_t const* key)
        {
            if (!page) return {};
            const auto content = page.GetNamedObject(L"scan", page);
            const auto whole = content.GetNamedString(key, L"");
            if (!whole.empty()) return whole;
            std::wstring text;
            for (const auto& item : visibleBlocks(page)) {
                const auto part = item.GetObject().GetNamedString(key, L"");
                if (part.empty()) continue;
                if (!text.empty()) text += L"\r\n\r\n";
                text += std::wstring_view(part);
            }
            return hstring(text);
        }
    }

    HWND MainWindow::NativeWindowHandle()
    {
        HWND handle = nullptr;
        check_hresult(get_strong().as<::IWindowNative>()->get_WindowHandle(&handle));
        return handle;
    }

    IAsyncAction MainWindow::OpenPdfAsync(std::filesystem::path path)
    {
        const auto lifetime = get_strong();
        if (m_closed || m_pdfOpening || pdfRequestBusy() || m_pdfExport) co_return;
        Navigation().SelectedItem(PdfItem());
        if (m_pdfTask) m_pdfTask->cancel();
        m_pdfOpening = true;
        showPdfError({});
        ++m_pdfGeneration;
        if (m_pdfRender) {
            m_pdfRender.Cancel();
            m_pdfRender = nullptr;
        }
        PdfLoading().IsActive(true);
        PdfActivityStatus().Text(L"正在读取 PDF…");
        updatePdfTaskUi();
        try {
            m_pdfInspect = ::yiyi::inspectPdfAsync(path);
            const auto document = co_await m_pdfInspect;
            if (m_closed) co_return;
            auto next = std::make_shared<::yiyi::PdfTask>(m_dataDirectory);
            next->load(path, document);
            m_pdfTask = std::move(next);
            m_pdfSource = std::move(path);
            m_pdfDocument = document;
            m_pdfPage = 0;
            m_pdfDisplayedPage = nullptr;
            m_pdfStructureUndo.clear();
            m_pdfTextView = false;
            PdfScanTabClicked(PdfScanSourceTab(), nullptr);
            PdfFileName().Text(hstring(m_pdfSource.filename().native()));
            PdfViewer().Visibility(Visibility::Visible);
            PdfPreviewLabels().Visibility(Visibility::Visible);
            PdfTextPanel().Visibility(Visibility::Collapsed);
            PdfEmptyHint().Visibility(Visibility::Collapsed);
            PdfZoom().SelectedIndex(0);
        } catch (...) {
            if (!m_closed) showPdfError(to_message());
            m_pdfInspect = nullptr;
            m_pdfOpening = false;
            if (!m_closed) {
                PdfLoading().IsActive(false);
                updatePdfTaskUi();
            }
            co_return;
        }
        m_pdfInspect = nullptr;
        m_pdfOpening = false;
        updatePdfTaskUi();
        const auto task = m_pdfTask;
        co_await renderPdfPage(0);
        if (!m_closed && !m_pdfOpening && task && task == m_pdfTask) {
            const auto state = task->snapshot();
            if (state.resetCursor >= 0 && !state.busy)
                runPdfOperation(task->resetTranslationsAsync(pdfCallbacks()), true);
            else if (!state.busy && !state.taskPending && state.analyzedPages < state.pageCount)
                runPdfOperation(task->analyze(-1, pdfCallbacks()), true);
        }
    }

    fire_and_forget MainWindow::PickPdf(IInspectable const&, RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        if (m_pdfOpening || pdfRequestBusy() || m_pdfExport) co_return;
        try {
            Pickers::FileOpenPicker picker;
            picker.FileTypeFilter().Append(L".pdf");
            check_hresult(picker.as<::IInitializeWithWindow>()->Initialize(NativeWindowHandle()));
            const auto file = co_await picker.PickSingleFileAsync();
            if (file && !m_closed) co_await OpenPdfAsync(std::filesystem::path(file.Path().c_str()));
        } catch (...) {
            if (!m_closed) showPdfError(to_message());
        }
    }

    IAsyncAction MainWindow::renderPdfPage(int page)
    {
        const auto lifetime = get_strong();
        if (m_closed || m_pdfOpening || page < 0 || page >= pdfPageCount()) co_return;
        if (m_pdfRender) m_pdfRender.Cancel();
        const auto generation = ++m_pdfGeneration;
        const bool pageChanged = m_pdfPage != page;
        m_pdfPage = page;
        if (pageChanged) m_pdfDisplayedPage = nullptr;
        updatePdfNavigation();
        if (pageChanged) {
            PdfPageNumber().Text(to_hstring(page + 1));
            PdfOriginalImage().Source(nullptr);
            PdfTranslatedImage().Source(nullptr);
            PdfBlockOverlay().Children().Clear();
        }
        PdfLoading().IsActive(true);
        try {
            const auto task = m_pdfTask;
            const auto data = task ? task->page(page) : JsonObject{nullptr};
            const auto statuses = std::make_shared<JsonArray>(nullptr);
            m_pdfRender = ::yiyi::renderPdfOriginalAsync(m_pdfSource, m_pdfDocument, page, 1500);
            const auto stream = co_await m_pdfRender;
            if (m_closed || generation != m_pdfGeneration) co_return;
            Media::Imaging::BitmapImage preview;
            co_await preview.SetSourceAsync(stream);
            if (m_closed || generation != m_pdfGeneration) co_return;
            PdfOriginalImage().Source(preview);
            PdfTextOriginalImage().Source(preview);
            if (pageChanged) PdfZoomChanged(nullptr, nullptr);
            PdfTranslatedImage().Source(preview);
            if (data && std::ranges::any_of(visibleBlocks(data), [](auto const& value) {
                    return !value.GetObject().GetNamedString(L"translation", L"").empty();
                })) {
                m_pdfRender = ::yiyi::renderPdfTranslationAsync(m_pdfSource, m_pdfDocument, data, page, 1500,
                    [statuses](JsonArray const& value) { *statuses = value; });
                const auto translatedStream = co_await m_pdfRender;
                if (m_closed || generation != m_pdfGeneration) co_return;
                Media::Imaging::BitmapImage translated;
                co_await translated.SetSourceAsync(translatedStream);
                if (m_closed || generation != m_pdfGeneration) co_return;
                PdfTranslatedImage().Source(translated);
                if (*statuses) task->recordPreviewStatuses(page, *statuses, data);
            }
            m_pdfDisplayedPage = task ? task->page(page) : nullptr;
            updatePdfBlockOverlay();
        } catch (...) {
            if (m_closed || generation != m_pdfGeneration) co_return;
            showPdfError(to_message());
        }
        if (generation == m_pdfGeneration) {
            m_pdfRender = nullptr;
            PdfLoading().IsActive(false);
            updatePdfTaskUi();
        }
    }

    int MainWindow::pdfPageCount() const
    {
        return m_pdfDocument ? static_cast<int>(m_pdfDocument.GetNamedNumber(L"pageCount")) : 0;
    }

    void MainWindow::updatePdfNavigation()
    {
        const int count = pdfPageCount();
        PdfOpenButton().IsEnabled(!m_pdfOpening && !pdfRequestBusy() && !m_pdfExport);
        PdfPageNumber().IsEnabled(count > 0 && !m_pdfOpening);
        PdfPrevious().IsEnabled(m_pdfPage > 0 && !m_pdfOpening);
        PdfNext().IsEnabled(m_pdfPage + 1 < count && !m_pdfOpening);
        PdfZoom().IsEnabled(count > 0 && !m_pdfOpening);
        if (PdfPageNumber().FocusState() == FocusState::Unfocused)
            PdfPageNumber().Text(to_hstring(m_pdfPage + 1));
        PdfPageCount().Text(hstring(L"/ " + std::to_wstring(count)));
    }

    ::yiyi::Provider MainWindow::pdfProvider(hstring const& id) const
    {
        const auto found = std::ranges::find(m_settings.providers, id, &::yiyi::Provider::id);
        return found == m_settings.providers.end() ? ::yiyi::Provider{} : *found;
    }

    ::yiyi::DocumentSettings MainWindow::pdfSettings() const
    {
        const auto snapshot = m_pdfTask->snapshot();
        return {pdfProvider(snapshot.providerId.empty() ? m_settings.selectedProviderId : snapshot.providerId),
                m_settings.sourceLanguage, m_settings.targetLanguage, m_settings.tone};
    }

    bool MainWindow::pdfRequestBusy() const
    {
        if (m_pdfCrop) return true;
        if (!m_pdfTask) return false;
        const auto state = m_pdfTask->snapshot().state;
        return state == ::yiyi::PdfTaskState::Translating || state == ::yiyi::PdfTaskState::Recognizing;
    }

    void MainWindow::showPdfError(hstring const& error)
    {
        PdfError().Message(error);
        PdfError().IsOpen(!error.empty());
    }

    void MainWindow::updatePdfTaskUi()
    {
        if (!PdfTranslate() || m_closed) return;
        updatePdfNavigation();
        using ::yiyi::PdfTaskState;
        const auto state = m_pdfTask ? m_pdfTask->snapshot() : ::yiyi::PdfTaskSnapshot{};
        const bool loaded = state.pageCount > 0;
        const bool analyzing = state.state == PdfTaskState::Analyzing;
        const bool working = state.busy || m_pdfExport || m_pdfCrop;
        const bool resetPending = state.resetCursor >= 0;
        const bool idle = loaded && !m_pdfOpening && !working && !resetPending;
        const bool canRequest = !m_busy && !m_screenshot && !documentsBusy();
        const auto page = m_pdfDisplayedPage;
        const auto blocks = visibleBlocks(page);
        const auto summary = page ? ::yiyi::pdf::PdfTaskStore::summary(m_pdfPage, page) : JsonObject{};
        const bool scan = summary.GetNamedBoolean(L"scan", false);
        const bool whole = PdfScope().SelectedIndex() == 0;
        const auto total = whole ? state.totalBlocks : static_cast<int>(summary.GetNamedNumber(L"total", 0));
        const auto completed = whole ? state.completedBlocks : static_cast<int>(summary.GetNamedNumber(L"completed", 0));
        const bool scopeReady = whole ? state.analyzedPages == state.pageCount : static_cast<bool>(page);
        const bool pageNeedsRecognition = scan && !summary.GetNamedBoolean(L"recognized", false);
        const bool needsRecognition = whole ? state.scanPages > state.recognizedPages : pageNeedsRecognition;

        const auto provider = pdfProvider(state.providerId.empty() ? m_settings.selectedProviderId : state.providerId);
        const auto model = state.state == PdfTaskState::Recognizing ? pdfProvider(m_settings.visionProviderId).model
            : state.model.empty() ? provider.model : state.model;
        PdfModel().Text(hstring(std::wstring(state.state == PdfTaskState::Recognizing ? L"识图 · " : L"模型 · ") +
            (model.empty() ? L"未选择" : model.c_str())));
        PdfSettingsButton().IsEnabled(!m_pdfOpening && !working);
        PdfExport().IsEnabled(idle && state.completedBlocks > 0);
        PdfReview().IsEnabled(idle && blocks.Size() > 0);
        PdfStructure().IsEnabled(idle && blocks.Size() > 0);
        PdfTextView().IsEnabled(loaded && scan);
        PdfRecognize().IsEnabled(idle && canRequest);
        PdfRecognizeRange().IsEnabled(idle && canRequest);
        PdfCrop().IsEnabled(idle && canRequest && PdfOriginalImage().Source());
        PdfRetranslate().IsEnabled(idle && canRequest && !state.taskPending && summary.GetNamedNumber(L"total", 0) > 0);
        PdfAnalyze().Text(resetPending ? L"继续清除" : analyzing ? L"暂停解析" : L"继续解析");
        PdfAnalyze().IsEnabled(analyzing || (resetPending && !working) ||
            (idle && state.analyzedPages < state.pageCount));
        PdfDiscard().IsEnabled(idle && state.taskPending);
        PdfReset().IsEnabled(idle && (!state.providerId.empty() || state.completedBlocks > 0));
        PdfIssues().IsEnabled(loaded);
        PdfScope().IsEnabled(idle && !state.taskPending);
        PdfTarget().IsEnabled(idle && state.providerId.empty());
        const auto target = std::ranges::find(pdfTargets, std::wstring_view(state.target.empty() ? m_settings.targetLanguage : state.target));
        m_pdfTargetUpdating = true;
        PdfTarget().SelectedIndex(target == std::end(pdfTargets) ? -1 : static_cast<int>(target - std::begin(pdfTargets)));
        m_pdfTargetUpdating = false;
        PdfTranslate().Content(box_value(working ? (analyzing || m_pdfExport || resetPending ? L"取消处理" : L"暂停")
            : resetPending ? L"继续清除" : state.taskPending ? L"继续" : needsRecognition ? L"识别并翻译" : !scopeReady || !loaded ? L"翻译"
            : total == 0 ? L"无可译内容" : total == completed ? L"已完成" : L"翻译"));
        PdfTranslate().IsEnabled(working || resetPending || (idle && canRequest &&
            (state.taskPending || needsRecognition || !scopeReady || completed < total)));
        if (m_pdfOpening) return;

        std::wstring pageStatus = L"尚未解析";
        if (page) {
            if (pageNeedsRecognition) pageStatus = L"扫描页待识别";
            else if (summary.GetNamedBoolean(L"scan", false) && blocks.Size() == 0) pageStatus = L"当前页无文字区域";
            else {
                const auto warning = page.GetNamedString(L"warning", L"");
                const auto pageTotal = static_cast<int>(summary.GetNamedNumber(L"total", 0));
                const auto pageCompleted = static_cast<int>(summary.GetNamedNumber(L"completed", 0));
                if (!warning.empty() && !scan) pageStatus = warning;
                else if (pageTotal == 0) pageStatus = L"无可译内容";
                else pageStatus = std::to_wstring(pageCompleted) + L" / " + std::to_wstring(pageTotal) + L" 段";
            }
        }
        PdfTranslatedLabel().Content(box_value(hstring(L"译文 · " + pageStatus)));
        if (!m_pdfExport && !m_pdfCrop) {
            const std::wstring activity = !state.operation.empty() ? std::wstring(state.operation)
                : !loaded ? L"打开 PDF，保留原版式翻译" : L"第 " + std::to_wstring(m_pdfPage + 1) + L" / " +
                    std::to_wstring(state.pageCount) + L" 页 · " + pageStatus;
            PdfActivityStatus().Text(hstring(activity));
            PdfProgress().Visibility(state.busy ? Visibility::Visible : Visibility::Collapsed);
            PdfProgress().IsIndeterminate(state.busy && !analyzing && !resetPending && state.totalBlocks == 0);
            PdfProgress().Value(resetPending && state.pageCount > 0 ? static_cast<double>(state.resetCursor) / state.pageCount
                : analyzing && state.pageCount > 0 ? static_cast<double>(state.analyzedPages) / state.pageCount
                : state.totalBlocks > 0 ? static_cast<double>(state.completedBlocks) / state.totalBlocks : 0);
        }
        if (!state.error.empty()) showPdfError(state.error);
        if (m_pdfTextView) {
            if (page && !scan) {
                TogglePdfTextView(nullptr, nullptr);
                return;
            }
            PdfPageSource().Text(pageText(page, L"source"));
            PdfPageTranslation().Text(pageText(page, L"translation"));
            const auto recognized = page ? page.GetNamedObject(L"scan", nullptr) : nullptr;
            PdfDraftButton().Visibility(recognized && (!recognized.GetNamedString(L"draftSource", L"").empty() ||
                !recognized.GetNamedString(L"draftTranslation", L"").empty()) ? Visibility::Visible : Visibility::Collapsed);
        }
    }

    ::yiyi::PdfTaskCallbacks MainWindow::pdfCallbacks()
    {
        return {[weak = get_weak()] {
            const auto self = weak.get();
            if (!self || self->m_closed || self->m_pdfOpening) return;
            if (self->m_pdfTask) self->m_pdfDisplayedPage = self->m_pdfTask->page(self->m_pdfPage);
            self->updatePdfTaskUi();
        }};
    }

    fire_and_forget MainWindow::runPdfOperation(IAsyncAction operation, bool refreshPreview)
    {
        const auto lifetime = get_strong();
        const auto generation = ++m_pdfOperationGeneration;
        m_pdfOperation = operation;
        updatePdfTaskUi();
        try {
            co_await operation;
        } catch (hresult_canceled const&) {
        } catch (...) {
            if (!m_closed && generation == m_pdfOperationGeneration) showPdfError(to_message());
        }
        if (m_closed || generation != m_pdfOperationGeneration) co_return;
        m_pdfOperation = nullptr;
        if (m_pdfTask) m_pdfDisplayedPage = m_pdfTask->page(m_pdfPage);
        updatePdfTaskUi();
        if (refreshPreview) co_await renderPdfPage(m_pdfPage);
    }

    void MainWindow::closePdf()
    {
        if (m_pdfTask) m_pdfTask->cancel();
        if (m_pdfInspect) m_pdfInspect.Cancel();
        if (m_pdfRender) m_pdfRender.Cancel();
        if (m_pdfExport) m_pdfExport.Cancel();
        if (m_pdfCrop) m_pdfCrop.Cancel();
    }

    void MainWindow::PdfTargetChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (!m_settingsLoaded || m_pdfTargetUpdating || !m_pdfTask || m_pdfTask->snapshot().busy) return;
        const auto selected = PdfTarget().SelectedIndex();
        if (selected < 0 || selected >= std::ssize(pdfTargets)) return;
        auto next = m_settings;
        next.targetLanguage = hstring(pdfTargets[selected]);
        applySettings(std::move(next));
        const auto items = TargetLanguage().Items();
        m_updating = true;
        for (uint32_t i = 0; i < items.Size(); ++i)
            if (unbox_value<hstring>(items.GetAt(i)) == m_settings.targetLanguage) TargetLanguage().SelectedIndex(i);
        m_updating = false;
    }

    void MainWindow::PdfScopeChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_settingsLoaded) updatePdfTaskUi();
    }

    void MainWindow::TranslatePdf(IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_pdfTask || m_pdfOpening) return;
        if (m_pdfExport) { m_pdfExport.Cancel(); return; }
        if (m_pdfCrop) { m_pdfCrop.Cancel(); return; }
        try {
            const auto state = m_pdfTask->snapshot();
            if (state.busy) {
                m_pdfTask->cancel();
                updatePdfTaskUi();
                return;
            }
            if (state.resetCursor >= 0) {
                runPdfOperation(m_pdfTask->resetTranslationsAsync(pdfCallbacks()), true);
                return;
            }
            if (m_busy || m_screenshot || documentsBusy()) return;
            showPdfError({});
            const auto visionId = state.visionProviderId.empty() ? m_settings.visionProviderId : state.visionProviderId;
            m_pdfTask->configureRecognition(pdfProvider(visionId), m_settings.pdfRecognitionPixels,
                                            std::chrono::minutes(m_settings.pdfRequestTimeout));
            const int page = PdfScope().SelectedIndex() == 0 ? -1 : m_pdfPage;
            const auto settings = pdfSettings();
            runPdfOperation(state.taskPending ? m_pdfTask->resume(settings.provider, pdfCallbacks())
                : m_pdfTask->start(settings, page, pdfCallbacks()), true);
        } catch (...) {
            showPdfError(to_message());
        }
    }

    void MainWindow::RetranslatePdfPage(IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_pdfTask || m_busy || m_screenshot || documentsBusy() || m_pdfExport) return;
        try {
            showPdfError({});
            m_pdfTask->configureRecognition(pdfProvider(m_settings.visionProviderId), m_settings.pdfRecognitionPixels,
                                            std::chrono::minutes(m_settings.pdfRequestTimeout));
            runPdfOperation(m_pdfTask->retranslatePage(m_pdfPage, pdfSettings(), pdfCallbacks()), true);
        } catch (...) {
            showPdfError(to_message());
        }
    }

    void MainWindow::TogglePdfAnalysis(IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_pdfTask || m_pdfExport) return;
        try {
            const auto state = m_pdfTask->snapshot();
            if (state.state == ::yiyi::PdfTaskState::Analyzing) m_pdfTask->cancel();
            else if (!state.busy && state.resetCursor >= 0)
                runPdfOperation(m_pdfTask->resetTranslationsAsync(pdfCallbacks()), true);
            else if (!state.busy) runPdfOperation(m_pdfTask->analyze(-1, pdfCallbacks()), true);
            updatePdfTaskUi();
        } catch (...) {
            showPdfError(to_message());
        }
    }

    void MainWindow::DiscardPdfTask(IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_pdfTask) return;
        try {
            m_pdfTask->discardTask();
            showPdfError({});
            updatePdfTaskUi();
        } catch (...) {
            showPdfError(to_message());
        }
    }

    void MainWindow::PreviousPdfPage(IInspectable const&, RoutedEventArgs const&)
    {
        renderPdfPage(m_pdfPage - 1);
    }

    void MainWindow::NextPdfPage(IInspectable const&, RoutedEventArgs const&)
    {
        renderPdfPage(m_pdfPage + 1);
    }

    void MainWindow::CommitPdfPage(IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_pdfDocument || m_pdfOpening) return;
        const auto text = PdfPageNumber().Text();
        wchar_t* end = nullptr;
        const auto page = std::wcstol(text.c_str(), &end, 10);
        if (end != text.c_str() && *end == L'\0' && page > 0 && page <= pdfPageCount() && page - 1 != m_pdfPage) {
            PdfPageNumber().Text(to_hstring(static_cast<int>(page)));
            renderPdfPage(page - 1);
            return;
        }
        PdfPageNumber().Text(to_hstring(m_pdfPage + 1));
    }

    void MainWindow::PdfPageNumberKey(IInspectable const&, Input::KeyRoutedEventArgs const& args)
    {
        if (args.Key() == Windows::System::VirtualKey::Enter) {
            args.Handled(true);
            CommitPdfPage(nullptr, nullptr);
        }
    }

    void MainWindow::PdfZoomChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        constexpr float zoom[] = {1.0f, 1.25f, 1.5f, 2.0f};
        const auto selected = std::clamp(PdfZoom().SelectedIndex(), 0, 3);
        const auto viewer = PdfViewer();
        if (viewer && m_pdfDocument) {
            const auto origin = box_value(0.0).as<IReference<double>>();
            viewer.ChangeView(origin, origin, box_value(zoom[selected]).as<IReference<float>>(), true);
        }
    }

    void MainWindow::PdfViewportChanged(IInspectable const&, SizeChangedEventArgs const& args)
    {
        const auto size = args.NewSize();
        const auto width = std::max(120.0, (static_cast<double>(size.Width) - 12) / 2);
        if (const auto preview = PdfOriginalImage()) preview.Width(width);
        if (const auto preview = PdfTranslatedImage()) preview.Width(width);
        updatePdfBlockOverlay();
    }

    void MainWindow::updatePdfBlockOverlay()
    {
        if (!PdfBlockOverlay()) return;
        const auto children = PdfBlockOverlay().Children();
        children.Clear();
        if (!m_pdfDisplayedPage) return;
        const auto pageWidth = m_pdfDisplayedPage.GetNamedNumber(L"width", 0);
        const auto width = PdfTranslatedImage().Width();
        if (pageWidth <= 0 || !std::isfinite(width)) return;
        const auto scale = width / pageWidth;
        const auto page = m_pdfPage;
        for (const auto& value : visibleBlocks(m_pdfDisplayedPage)) {
            const auto block = value.GetObject();
            if (block.GetNamedBoolean(L"fixed", false)) continue;
            const auto bounds = block.GetNamedArray(L"region", nullptr);
            if (!bounds || bounds.Size() != 4) continue;
            Button area;
            area.Padding({});
            area.MinWidth(0);
            area.MinHeight(0);
            area.Width(std::max(1.0, bounds.GetNumberAt(2) * scale));
            area.Height(std::max(1.0, bounds.GetNumberAt(3) * scale));
            area.Background(Media::SolidColorBrush(Microsoft::UI::Colors::Transparent()));
            const auto issues = ::yiyi::pdf::PdfTaskStore::blockIssues(block);
            area.BorderThickness(issues.Size() ? Thickness{1, 1, 1, 1} : Thickness{});
            if (issues.Size()) area.BorderBrush(Media::SolidColorBrush(Microsoft::UI::ColorHelper::FromArgb(255, 191, 130, 50)));
            const auto hint = issues.Size() ? issues.GetObjectAt(0).GetNamedString(L"reason") : hstring(L"点击校对这一段");
            ToolTipService::SetToolTip(area, box_value(hint));
            Automation::AutomationProperties::SetName(area, hint);
            Automation::AutomationProperties::SetAutomationId(area, hstring(L"pdfBlock_" + std::wstring(block.GetNamedString(L"id"))));
            Canvas::SetLeft(area, bounds.GetNumberAt(0) * scale);
            Canvas::SetTop(area, bounds.GetNumberAt(1) * scale);
            area.Click([weak = get_weak(), page, block](auto&&, auto&&) {
                if (const auto self = weak.get(); self && self->m_pdfTask && !self->m_pdfTask->snapshot().busy && !self->m_pdfExport)
                    self->editPdfBlock(page, block);
            });
            children.Append(area);
        }
    }

    void MainWindow::TogglePdfTextView(IInspectable const&, RoutedEventArgs const&)
    {
        m_pdfTextView = !m_pdfTextView;
        PdfTextView().Text(m_pdfTextView ? L"版式对照" : L"整页文字");
        PdfTextPanel().Visibility(m_pdfTextView ? Visibility::Visible : Visibility::Collapsed);
        PdfViewer().Visibility(m_pdfTextView ? Visibility::Collapsed : Visibility::Visible);
        PdfPreviewLabels().Visibility(m_pdfTextView ? Visibility::Collapsed : Visibility::Visible);
        updatePdfTaskUi();
    }

    void MainWindow::PdfScanTabClicked(IInspectable const& sender, RoutedEventArgs const&)
    {
        m_pdfScanTranslation = unbox_value<hstring>(sender.as<Button>().Tag()) == L"translation";
        PdfPageSource().Visibility(m_pdfScanTranslation ? Visibility::Collapsed : Visibility::Visible);
        PdfPageTranslation().Visibility(m_pdfScanTranslation ? Visibility::Visible : Visibility::Collapsed);
        const auto styles = Application::Current().Resources();
        const auto active = styles.Lookup(box_value(L"YiyiSoftButton")).as<Style>();
        const auto other = styles.Lookup(box_value(L"YiyiGhostButton")).as<Style>();
        PdfScanSourceTab().Style(m_pdfScanTranslation ? other : active);
        PdfScanTranslationTab().Style(m_pdfScanTranslation ? active : other);
    }

    void MainWindow::TogglePdfReading(IInspectable const&, RoutedEventArgs const&)
    {
        setPdfReading(!m_pdfReading);
    }

    void MainWindow::setPdfReading(bool reading)
    {
        m_pdfReading = reading;
        Navigation().IsPaneVisible(!reading);
        PdfReadingMode().Text(reading ? L"显示侧栏" : L"展开阅读");
    }

    void MainWindow::ImportDragOver(IInspectable const&, DragEventArgs const& args)
    {
        if (args.DataView().Contains(StandardDataFormats::StorageItems())) {
            args.AcceptedOperation(DataPackageOperation::Copy);
            args.Handled(true);
        }
    }

    fire_and_forget MainWindow::ImportDrop(IInspectable const&, DragEventArgs const& args)
    {
        if (!args.DataView().Contains(StandardDataFormats::StorageItems())) co_return;
        const auto lifetime = get_strong();
        const auto deferral = args.GetDeferral();
        args.Handled(true);
        StorageFile file{nullptr};
        try {
            const auto items = co_await args.DataView().GetStorageItemsAsync();
            if (items.Size() == 1) file = items.GetAt(0).try_as<StorageFile>();
        } catch (...) {
            if (!m_closed) notice(L"无法读取拖入的文件。", InfoBarSeverity::Warning);
        }
        deferral.Complete();
        if (m_closed) co_return;
        if (file) {
            auto path = std::filesystem::path(file.Path().c_str());
            const auto extension = file.FileType();
            const auto matches = [&](wchar_t const* value) {
                return CompareStringOrdinal(extension.c_str(), -1, value, -1, TRUE) == CSTR_EQUAL;
            };
            if (matches(L".pdf")) {
                co_await OpenPdfAsync(std::move(path));
                co_return;
            }
            if (matches(L".txt") || matches(L".docx") || matches(L".pptx")) {
                co_await OpenDocumentAsync(std::move(path));
                co_return;
            }
        }
        notice(L"请拖入一个 TXT、DOCX、PPTX 或 PDF 文件。", InfoBarSeverity::Warning);
    }
}
