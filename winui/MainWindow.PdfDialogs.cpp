#include "pch.h"
#include "MainWindow.xaml.h"
#include "PdfStructure.h"

#include <shobjidl_core.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <array>
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
        ContentDialog pdfDialog(XamlRoot const& root, hstring const& title, hstring const& primary = {}, hstring const& close = L"关闭")
        {
            ContentDialog dialog;
            dialog.XamlRoot(root);
            dialog.Title(box_value(title));
            dialog.PrimaryButtonText(primary);
            dialog.CloseButtonText(close);
            return dialog;
        }

        TextBlock description(hstring const& text)
        {
            TextBlock label;
            label.Text(text);
            label.TextWrapping(TextWrapping::Wrap);
            return label;
        }

        TextBox textEditor(hstring const& label, hstring const& value, double height, bool readOnly = false)
        {
            TextBox editor;
            editor.Header(box_value(label));
            editor.Text(value);
            editor.AcceptsReturn(true);
            editor.IsReadOnly(readOnly);
            editor.TextWrapping(TextWrapping::Wrap);
            editor.MinHeight(height);
            editor.MaxHeight(height + 72);
            editor.MaxLength(16000);
            ScrollViewer::SetVerticalScrollBarVisibility(editor, ScrollBarVisibility::Auto);
            return editor;
        }

        NumberBox number(hstring const& label, double value, double min, double max)
        {
            NumberBox field;
            field.Header(box_value(label));
            field.Minimum(min);
            field.Maximum(max);
            field.Value(value);
            field.HorizontalAlignment(HorizontalAlignment::Stretch);
            return field;
        }

        CheckBox check(hstring const& label, bool value)
        {
            CheckBox field;
            field.Content(box_value(label));
            field.IsChecked(box_value(value).as<IReference<bool>>());
            return field;
        }

        bool checked(CheckBox const& field)
        {
            return field.IsChecked() && field.IsChecked().Value();
        }

        ComboBox choices(hstring const& label, std::initializer_list<wchar_t const*> items, int index)
        {
            ComboBox field;
            field.Header(box_value(label));
            field.HorizontalAlignment(HorizontalAlignment::Stretch);
            for (const auto item : items) field.Items().Append(box_value(item));
            field.SelectedIndex(index);
            return field;
        }

        JsonArray blocksOnPage(JsonObject const& page)
        {
            return page ? page.GetNamedObject(L"scan", page).GetNamedArray(L"blocks", JsonArray{}) : JsonArray{};
        }

        void setText(JsonObject const& object, wchar_t const* key, hstring const& value)
        {
            object.Insert(key, JsonValue::CreateStringValue(value));
        }

        void setFlag(JsonObject const& object, wchar_t const* key, bool value)
        {
            object.Insert(key, JsonValue::CreateBooleanValue(value));
        }

        hstring blockLabel(JsonObject const& block)
        {
            const auto title = block.GetNamedString(L"source", L"");
            const auto source = std::wstring_view(title);
            auto details = std::wstring(source.substr(0, std::min(source.size(), size_t(72))));
            std::ranges::replace(details, L'\n', L' ');
            std::ranges::replace(details, L'\r', L' ');
            if (title.size() > details.size()) details += L"…";
            const auto issues = ::yiyi::pdf::PdfTaskStore::blockIssues(block);
            if (issues.Size()) details += L" · " + std::wstring(issues.GetObjectAt(0).GetNamedString(L"reason"));
            return hstring(details);
        }

        void copyPdfText(hstring const& text)
        {
            DataPackage package;
            package.SetText(text);
            Clipboard::SetContent(package);
        }
    }

    fire_and_forget MainWindow::OpenPdfSettings(IInspectable const&, RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        try {
            const auto state = m_pdfTask ? m_pdfTask->snapshot() : ::yiyi::PdfTaskSnapshot{};
            if (state.busy || m_pdfExport) co_return;
            const auto dialog = pdfDialog(PdfPage().XamlRoot(), L"翻译设置", L"完成", L"取消");
            dialog.SecondaryButtonText(L"管理 AI 服务");
            StackPanel fields;
            fields.Spacing(12);
            auto model = choices(L"翻译模型", {}, -1);
            auto vision = choices(L"扫描页识图模型", {L"选择支持图片的模型"}, 0);
            const auto modelId = state.providerId.empty() ? m_settings.selectedProviderId : state.providerId;
            int i = 0;
            for (const auto& provider : m_settings.providers) {
                model.Items().Append(box_value(provider.name));
                vision.Items().Append(box_value(provider.name));
                if (provider.id == modelId) model.SelectedIndex(i);
                if (provider.id == m_settings.visionProviderId) vision.SelectedIndex(i + 1);
                ++i;
            }
            model.PlaceholderText(L"选择翻译模型");
            model.IsEnabled(state.providerId.empty());
            vision.IsEnabled(!state.taskPending);
            fields.Children().Append(model);
            fields.Children().Append(description(state.taskDescription.empty() ?
                hstring(L"模型：" + std::wstring(pdfProvider(modelId).model)) :
                hstring(L"本任务使用 " + std::wstring(state.taskDescription) + L"。清空全书译文后可更换。")));
            fields.Children().Append(vision);
            const auto quality = choices(L"识别清晰度", {L"标准", L"高清"}, m_settings.pdfRecognitionPixels == 2400 ? 1 : 0);
            quality.IsEnabled(!state.taskPending);
            fields.Children().Append(quality);
            fields.Children().Append(description(L"扫描页会先识别，再自动翻译。高清适合小字，处理时间更长。"));
            constexpr std::array timeouts{5, 10, 20, 30, 60};
            const auto position = std::ranges::find(timeouts, m_settings.pdfRequestTimeout);
            const auto timeout = choices(L"无响应等待", {L"5 分钟", L"10 分钟", L"20 分钟", L"30 分钟", L"60 分钟"},
                position == timeouts.end() ? 1 : static_cast<int>(position - timeouts.begin()));
            fields.Children().Append(timeout);
            fields.Children().Append(description(L"每段请求连续无响应时的等待上限，收到数据后重新计时。"));
            dialog.Content(fields);
            const auto choice = co_await dialog.ShowAsync();
            if (m_closed) co_return;
            if (choice == ContentDialogResult::Secondary) {
                Navigation().SelectedItem(SettingsItem());
            } else if (choice == ContentDialogResult::Primary) {
                auto next = m_settings;
                if (model.IsEnabled() && model.SelectedIndex() >= 0) next.selectedProviderId = next.providers[model.SelectedIndex()].id;
                if (vision.IsEnabled()) next.visionProviderId = vision.SelectedIndex() > 0 ? next.providers[vision.SelectedIndex() - 1].id : hstring{};
                next.pdfRecognitionPixels = quality.SelectedIndex() == 1 ? 2400 : 1800;
                next.pdfRequestTimeout = timeouts[std::max(0, timeout.SelectedIndex())];
                applySettings(std::move(next));
                refreshProviders();
            }
        } catch (...) {
            if (!m_closed) showPdfError(to_message());
        }
    }

    fire_and_forget MainWindow::ResetPdfTranslations(IInspectable const&, RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        const auto task = m_pdfTask;
        if (!task) co_return;
        try {
            const auto dialog = pdfDialog(PdfPage().XamlRoot(), L"清空全书译文", L"清空全书译文", L"取消");
            dialog.Content(description(L"清除这份文档已保存的译文和译文修改，下次翻译使用当前服务与语言。已识别、校对的原文会保留。"));
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary || m_closed || task != m_pdfTask) co_return;
            m_pdfStructureUndo.clear();
            showPdfError({});
            runPdfOperation(task->resetTranslationsAsync(pdfCallbacks()), true);
        } catch (...) {
            if (!m_closed) showPdfError(to_message());
        }
    }

    fire_and_forget MainWindow::ExportPdf(IInspectable const&, RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        const auto task = m_pdfTask;
        if (!task || m_pdfExport || task->snapshot().busy) co_return;
        try {
            const auto state = task->snapshot();
            if (state.analyzedPages < state.pageCount || state.completedBlocks < state.totalBlocks ||
                state.scanPages > state.recognizedPages) {
                auto dialog = pdfDialog(PdfPage().XamlRoot(), L"导出当前进度", L"继续导出", L"取消");
                dialog.Content(description(hstring(L"已完成 " + std::to_wstring(state.completedBlocks) + L" / " +
                    std::to_wstring(state.totalBlocks) + L" 个翻译项。尚未翻译及未通过排版检查的区域保留原文，并附检查清单。")));
                if (co_await dialog.ShowAsync() != ContentDialogResult::Primary || m_closed || task != m_pdfTask) co_return;
            }
            Pickers::FileSavePicker picker;
            picker.FileTypeChoices().Insert(L"PDF 文档", single_threaded_vector<hstring>({L".pdf"}));
            picker.SuggestedFileName(hstring(m_pdfSource.stem().native() + L"-译文"));
            check_hresult(picker.as<::IInitializeWithWindow>()->Initialize(NativeWindowHandle()));
            const auto file = co_await picker.PickSaveFileAsync();
            if (!file || m_closed || task != m_pdfTask || task->snapshot().busy || m_pdfExport) co_return;

            PdfActivityStatus().Text(L"正在导出译文 PDF…");
            PdfProgress().Visibility(Visibility::Visible);
            PdfProgress().IsIndeterminate(true);
            showPdfError({});
            const auto queue = DispatcherQueue();
            const auto progress = [weak = get_weak(), queue, task](JsonObject const& value) {
                const auto total = value.GetNamedNumber(L"total", 0);
                const auto completed = value.GetNamedNumber(L"completed", 0);
                queue.TryEnqueue([weak, task, total, completed] {
                    if (const auto self = weak.get(); self && !self->m_closed && self->m_pdfExport && task == self->m_pdfTask) {
                        self->PdfProgress().IsIndeterminate(total <= 0);
                        if (total > 0) self->PdfProgress().Value(std::clamp(completed / total, 0.0, 1.0));
                    }
                });
            };
            m_pdfExport = ::yiyi::exportPdfAsync(m_pdfSource, task->databasePath(), file.Path().c_str(), true, progress);
            updatePdfTaskUi();
            const auto result = co_await m_pdfExport;
            if (const auto statuses = result.GetNamedArray(L"pageStatuses", nullptr); statuses && statuses.Size()) {
                const apartment_context ui;
                co_await resume_background();
                hstring statusError;
                try {
                    task->recordExportStatuses(statuses);
                } catch (...) {
                    statusError = to_message();
                }
                co_await ui;
                if (!m_closed && !statusError.empty()) showPdfError(statusError);
            }
            m_pdfExport = nullptr;
            if (m_closed) co_return;
            m_pdfDisplayedPage = task->page(m_pdfPage);
            updatePdfBlockOverlay();
            updatePdfTaskUi();
            const auto error = result.GetNamedString(L"error", L"");
            if (!result.GetNamedBoolean(L"saved", false)) {
                showPdfError(error.empty() ? hstring(L"PDF 导出未完成。") : error);
                co_return;
            }
            const auto preserved = result.GetNamedArray(L"preserved", JsonArray{}).Size();
            if (!preserved && !result.HasKey(L"reportError")) {
                notice(L"PDF 已导出。");
                co_return;
            }
            const auto dialog = pdfDialog(PdfPage().XamlRoot(), L"PDF 已导出", L"查看待校对", L"完成");
            auto report = result.GetNamedString(L"report", L"");
            if (!report.empty()) dialog.SecondaryButtonText(L"打开清单");
            dialog.Content(description(hstring(std::to_wstring(static_cast<int>(result.GetNamedNumber(L"preservedPages", 0))) +
                L" 页中的 " + std::to_wstring(preserved) + L" 个区域保留原文。\r\n" +
                std::wstring(result.GetNamedString(L"reportError", L"同目录已保存 .issues.txt 检查清单。")))));
            const auto choice = co_await dialog.ShowAsync();
            if (m_closed) co_return;
            if (choice == ContentDialogResult::Primary) OpenPdfIssues(nullptr, nullptr);
            else if (choice == ContentDialogResult::Secondary) {
                const auto reportFile = co_await StorageFile::GetFileFromPathAsync(report);
                co_await Windows::System::Launcher::LaunchFileAsync(reportFile);
            }
        } catch (hresult_canceled const&) {
            m_pdfExport = nullptr;
            if (!m_closed) updatePdfTaskUi();
        } catch (...) {
            m_pdfExport = nullptr;
            if (!m_closed) {
                updatePdfTaskUi();
                showPdfError(to_message());
            }
        }
    }

    fire_and_forget MainWindow::OpenPdfIssues(IInspectable const&, RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        const auto task = m_pdfTask;
        if (!task) co_return;
        try {
            auto dialog = pdfDialog(PdfPage().XamlRoot(), L"待校对", L"定位");
            StackPanel panel;
            panel.Spacing(8);
            ListBox items;
            items.MaxHeight(370);
            struct Issue { int page; JsonObject block{nullptr}; };
            std::vector<Issue> issues;
            const auto summaries = task->issueSummaries();
            const int current = m_pdfPage;
            const auto blocks = blocksOnPage(task->page(current));
            for (const auto& item : blocks) {
                const auto block = item.GetObject();
                if (!::yiyi::pdf::PdfTaskStore::blockIssues(block).Size()) continue;
                items.Items().Append(box_value(blockLabel(block)));
                issues.push_back({current, block});
            }
            for (const auto& value : summaries) {
                const auto summary = value.GetObject();
                const int page = static_cast<int>(summary.GetNamedNumber(L"number"));
                auto reason = summary.GetNamedString(L"warning", L"");
                if (reason.empty()) {
                    const auto count = static_cast<int>(summary.GetNamedNumber(L"review", 0));
                    const auto pending = static_cast<int>(summary.GetNamedNumber(L"total", 0) - summary.GetNamedNumber(L"completed", 0));
                    reason = count > 0 ? hstring(std::to_wstring(count) + L" 段待校对")
                        : pending > 0 ? hstring(std::to_wstring(pending) + L" 段待翻译") : hstring(L"扫描页待识别");
                }
                items.Items().Append(box_value(hstring(L"第 " + std::to_wstring(page + 1) + L" 页 · " + std::wstring(reason))));
                issues.push_back({page, nullptr});
            }
            if (issues.empty()) {
                dialog.PrimaryButtonText(L"");
                panel.Children().Append(description(L"当前没有需要校对的区域。"));
            } else {
                items.SelectedIndex(0);
                panel.Children().Append(items);
            }
            dialog.Content(panel);
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary || m_closed || task != m_pdfTask) co_return;
            const auto selected = items.SelectedIndex();
            if (selected < 0) co_return;
            const auto& issue = issues[selected];
            if (issue.page != m_pdfPage) co_await renderPdfPage(issue.page);
            if (issue.block && !m_closed && task == m_pdfTask && !task->snapshot().busy) co_await editPdfBlock(issue.page, issue.block);
        } catch (...) {
            if (!m_closed) showPdfError(to_message());
        }
    }

    fire_and_forget MainWindow::ReviewPdfBlocks(IInspectable const&, RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        const auto task = m_pdfTask;
        if (!task || task->snapshot().busy) co_return;
        try {
            const int page = m_pdfPage;
            const auto blocks = blocksOnPage(task->page(page));
            std::vector<JsonObject> rows;
            ListBox items;
            items.MaxHeight(360);
            for (const auto& value : blocks) {
                const auto block = value.GetObject();
                if (block.GetNamedBoolean(L"fixed", false)) continue;
                rows.push_back(block);
                items.Items().Append(box_value(blockLabel(block)));
            }
            if (rows.empty()) co_return;
            items.SelectedIndex(0);
            const auto dialog = pdfDialog(PdfPage().XamlRoot(), L"校对段落", L"编辑");
            dialog.Content(items);
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary || m_closed || task != m_pdfTask ||
                items.SelectedIndex() < 0) co_return;
            co_await editPdfBlock(page, rows[items.SelectedIndex()]);
        } catch (...) {
            if (!m_closed) showPdfError(to_message());
        }
    }

    fire_and_forget MainWindow::OpenPdfStructure(IInspectable const&, RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        const auto task = m_pdfTask;
        if (!task || m_pdfExport || task->snapshot().busy) co_return;
        try {
            const int page = m_pdfPage;
            const auto current = task->page(page);
            if (!current) co_return;
            const auto blocks = blocksOnPage(current);
            const auto content = current.GetNamedObject(L"scan", current);
            const auto layout = content.GetNamedObject(L"layout", JsonObject{});
            const auto regions = layout.GetNamedArray(L"regions", JsonArray{});
            ListBox blockList;
            blockList.SelectionMode(SelectionMode::Multiple);
            blockList.MaxHeight(260);
            for (const auto& value : blocks) blockList.Items().Append(box_value(blockLabel(value.GetObject())));
            ComboBox regionList;
            regionList.Header(box_value(L"栏和表格结构"));
            regionList.HorizontalAlignment(HorizontalAlignment::Stretch);
            regionList.PlaceholderText(L"选择区域");
            for (const auto& value : regions) {
                const auto node = value.GetObject();
                const auto kind = node.GetNamedString(L"kind", L"");
                std::wstring label = kind == L"table" ? L"表格" : kind == L"cell" ? L"单元格" : kind == L"column" ? L"栏" : L"区域";
                label += L" · " + std::wstring(node.GetNamedString(L"id"));
                regionList.Items().Append(box_value(hstring(label)));
            }
            const auto operation = choices(L"操作", {L"调整阅读顺序", L"合并相邻区域", L"拆分区域", L"调整栏或表格", L"校对段落"}, 0);
            const auto dialog = pdfDialog(PdfPage().XamlRoot(), L"结构校正", L"继续");
            if (m_pdfStructureUndo.contains(page)) dialog.SecondaryButtonText(L"撤销上次结构修改");
            StackPanel panel;
            panel.Spacing(10);
            panel.Children().Append(operation);
            panel.Children().Append(description(L"文字区域"));
            panel.Children().Append(blockList);
            if (regions.Size()) panel.Children().Append(regionList);
            dialog.Content(panel);
            const auto action = co_await dialog.ShowAsync();
            if (action == ContentDialogResult::None || m_closed || task != m_pdfTask) co_return;
            if (action == ContentDialogResult::Secondary) {
                if (task->replacePageStructure(page, current, m_pdfStructureUndo.at(page))) {
                    m_pdfStructureUndo.erase(page);
                    co_await renderPdfPage(page);
                }
                co_return;
            }

            std::vector<uint32_t> selected;
            for (const auto& item : blockList.SelectedItems()) {
                uint32_t index{};
                if (blockList.Items().IndexOf(item, index)) selected.push_back(index);
            }
            const int command = operation.SelectedIndex();
            if ((command == 1 && selected.size() < 2) || (command != 1 && command != 3 && selected.size() != 1))
                throw hresult_invalid_argument(command == 1 ? L"请选择至少两个相邻区域。" : L"请选择一个文字区域。");
            JsonObject next{nullptr};
            if (command == 1) {
                std::vector<hstring> ids;
                for (const auto index : selected) ids.push_back(blocks.GetObjectAt(index).GetNamedString(L"id"));
                next = ::yiyi::mergePdfBlocks(current, ids);
            } else if (command == 3) {
                const auto index = regionList.SelectedIndex();
                if (index < 0) throw hresult_invalid_argument(L"请先选择栏或表格区域。");
                const auto node = regions.GetObjectAt(index);
                const auto bounds = node.GetNamedArray(L"rect");
                const auto id = node.GetNamedString(L"id");
                const bool table = node.GetNamedString(L"kind", L"") == L"table";
                const auto edit = pdfDialog(PdfPage().XamlRoot(), table ? L"调整表头" : L"调整区域边界", L"保存", L"取消");
                StackPanel fields;
                fields.Spacing(8);
                const auto x = number(L"横坐标", bounds.GetNumberAt(0), 0, 4000);
                const auto y = number(L"纵坐标", bounds.GetNumberAt(1), 0, 4000);
                const auto w = number(L"宽度", bounds.GetNumberAt(2), 3, 4000);
                const auto h = number(L"高度", bounds.GetNumberAt(3), 3, 4000);
                const auto heads = number(L"表头行数", node.GetNamedNumber(L"headerRows", 0), 0, node.GetNamedNumber(L"rows", 0));
                const auto link = node.GetNamedObject(L"continuedFrom", nullptr);
                const auto previous = number(L"续表来源页（0 表示无关联）", link ? link.GetNamedNumber(L"page", -1) + 1 : 0, 0, page);
                if (table) {
                    fields.Children().Append(heads);
                    fields.Children().Append(previous);
                } else {
                    for (const auto& input : {x, y, w, h}) fields.Children().Append(input);
                }
                edit.Content(fields);
                if (co_await edit.ShowAsync() != ContentDialogResult::Primary || m_closed || task != m_pdfTask) co_return;
                JsonObject changes, before{nullptr};
                if (table) {
                    if (heads.Value() != std::floor(heads.Value()) || previous.Value() != std::floor(previous.Value()))
                        throw hresult_invalid_argument(L"表头行数和来源页必须是整数。");
                    changes.Insert(L"headerRows", JsonValue::CreateNumberValue(heads.Value()));
                    const auto beforeIndex = static_cast<int>(previous.Value()) - 1;
                    if (beforeIndex != (link ? static_cast<int>(link.GetNamedNumber(L"page")) : -1)) {
                        changes.Insert(L"previousPage", JsonValue::CreateNumberValue(beforeIndex));
                        if (beforeIndex >= 0) before = task->page(beforeIndex);
                    }
                } else {
                    JsonArray rect;
                    for (const auto& input : {x, y, w, h}) rect.Append(JsonValue::CreateNumberValue(input.Value()));
                    changes.Insert(L"rect", rect);
                }
                next = ::yiyi::revisePdfRegion(current, id, changes, before);
            } else {
                const auto selectedBlock = selected.front();
                const auto block = blocks.GetObjectAt(selectedBlock);
                const auto id = block.GetNamedString(L"id");
                if (command == 4) {
                    co_await editPdfBlock(page, block);
                    co_return;
                }
                if (command == 0) {
                    const auto edit = pdfDialog(PdfPage().XamlRoot(), L"调整阅读顺序", L"保存", L"取消");
                    const auto position = number(L"移至第几段", selectedBlock + 1, 1, blocks.Size());
                    edit.Content(position);
                    if (co_await edit.ShowAsync() != ContentDialogResult::Primary || m_closed || task != m_pdfTask) co_return;
                    if (!std::isfinite(position.Value()) || position.Value() != std::floor(position.Value()))
                        throw hresult_invalid_argument(L"阅读顺序必须是整数。");
                    next = ::yiyi::movePdfBlock(current, id, static_cast<int>(position.Value()) - 1);
                } else {
                    const auto edit = pdfDialog(PdfPage().XamlRoot(), L"拆分区域", L"拆分", L"取消");
                    StackPanel fields;
                    fields.Spacing(8);
                    const auto bounds = block.GetNamedArray(block.GetNamedString(L"role", L"") == L"cell" ? L"containerRect" : L"region");
                    const auto axis = choices(L"分界方向", {L"上下拆分", L"左右拆分"}, 0);
                    const auto position = number(L"分界位置（点）", bounds.GetNumberAt(1) + bounds.GetNumberAt(3) * .5, 0, 4000);
                    axis.SelectionChanged([position, bounds](IInspectable const& sender, SelectionChangedEventArgs const&) {
                        const bool vertical = sender.as<ComboBox>().SelectedIndex() == 1;
                        position.Value(bounds.GetNumberAt(vertical ? 0 : 1) + bounds.GetNumberAt(vertical ? 2 : 3) * .5);
                    });
                    const auto first = textEditor(L"第一部分原文", block.GetNamedString(L"source", L""), 78);
                    const auto last = textEditor(L"第二部分原文", L"", 78);
                    fields.Children().Append(axis);
                    fields.Children().Append(position);
                    fields.Children().Append(first);
                    fields.Children().Append(last);
                    edit.Content(fields);
                    if (co_await edit.ShowAsync() != ContentDialogResult::Primary || m_closed || task != m_pdfTask) co_return;
                    next = ::yiyi::splitPdfBlock(current, id, axis.SelectedIndex() == 1, position.Value(), first.Text(), last.Text());
                }
            }
            if (!next || !task->replacePageStructure(page, current, next))
                throw hresult_error(E_FAIL, L"页面内容已变化，请重新打开结构校正。");
            m_pdfStructureUndo[page] = current;
            if (page == m_pdfPage) co_await renderPdfPage(page);
        } catch (...) {
            if (!m_closed) showPdfError(to_message());
        }
    }

    IAsyncAction MainWindow::editPdfBlock(int page, JsonObject block)
    {
        const auto lifetime = get_strong();
        const auto task = m_pdfTask;
        if (!task || m_closed || m_pdfExport || task->snapshot().busy) co_return;
        try {
            const auto id = block.GetNamedString(L"id");
            const auto pageData = task->page(page);
            const bool scan = pageData && pageData.HasKey(L"scan");
            auto dialog = pdfDialog(PdfPage().XamlRoot(), L"校对段落", L"保存并排版", L"取消");
            if (!block.GetNamedBoolean(L"fixed", false) && !m_busy && !m_screenshot && !documentsBusy())
                dialog.SecondaryButtonText(L"重新翻译此段");
            StackPanel fields;
            fields.Spacing(8);
            for (const auto& value : ::yiyi::pdf::PdfTaskStore::blockIssues(block))
                fields.Children().Append(description(value.GetObject().GetNamedString(L"reason")));

            const auto source = textEditor(L"原文", block.GetNamedString(L"source", L""), 88);
            const auto translation = textEditor(L"译文", block.GetNamedString(L"translation", L""), 88);
            translation.PlaceholderText(L"输入或修改译文");
            Grid editors;
            editors.ColumnSpacing(8);
            editors.ColumnDefinitions().Append(ColumnDefinition{});
            editors.ColumnDefinitions().Append(ColumnDefinition{});
            Grid::SetColumn(translation, 1);
            editors.Children().Append(source);
            editors.Children().Append(translation);
            fields.Children().Append(editors);
            const auto failed = block.GetNamedString(L"rejectedTranslation", L"");
            if (!failed.empty()) {
                Expander details;
                details.Header(box_value(L"查看失败返回"));
                details.Content(textEditor(L"服务返回", failed, 72, true));
                fields.Children().Append(details);
            }
            const auto keep = check(L"保留原文", block.GetNamedBoolean(L"keep", false));
            const auto serif = check(L"衬线", block.GetNamedBoolean(L"serif", false));
            const auto centered = check(L"居中", block.GetNamedBoolean(L"centered", false));
            const auto bold = check(L"加粗", block.GetNamedBoolean(L"bold", false));
            StackPanel styles;
            styles.Orientation(Orientation::Horizontal);
            styles.Spacing(6);
            for (const auto& style : {keep, serif, centered, bold}) styles.Children().Append(style);
            fields.Children().Append(styles);
            const auto rect = block.GetNamedArray(L"region");
            const auto size = number(L"字号", block.GetNamedNumber(L"size", 12), 7, 72);
            const auto x = number(L"横坐标", rect.GetNumberAt(0), 0, 4000);
            const auto y = number(L"纵坐标", rect.GetNumberAt(1), 0, 4000);
            const auto width = number(L"宽度", rect.GetNumberAt(2), 3, 4000);
            const auto height = number(L"高度", rect.GetNumberAt(3), 3, 4000);
            Grid geometry;
            geometry.ColumnSpacing(6);
            int column = 0;
            for (const auto& input : {size, x, y, width, height}) {
                geometry.ColumnDefinitions().Append(ColumnDefinition{});
                Grid::SetColumn(input, column++);
                geometry.Children().Append(input);
            }
            fields.Children().Append(geometry);
            const auto confirm = check(L"原文有修改，已核对并确认当前译文", false);
            fields.Children().Append(confirm);
            const auto numbers = check(L"已核对数字差异，确认使用当前译文", false);
            const auto role = block.GetNamedString(L"role", L"");
            if (role == L"cell" || role == L"index") fields.Children().Append(numbers);
            constexpr std::array<std::wstring_view, 4> backgroundModes{L"auto", L"repair", L"panel", L"original"};
            constexpr std::array<std::wstring_view, 3> colorModes{L"auto", L"dark", L"light"};
            const auto mode = block.GetNamedString(L"backgroundMode", L"auto");
            const auto textMode = block.GetNamedString(L"textColorMode", L"auto");
            const auto background = choices(L"背景处理", {L"自动清理纯色/渐变", L"局部背景修复", L"底色译文框", L"保留原图对照"},
                static_cast<int>(std::ranges::find(backgroundModes, std::wstring_view(mode)) - backgroundModes.begin()));
            const auto color = choices(L"文字颜色", {L"自动字色", L"深色文字", L"浅色文字"},
                static_cast<int>(std::ranges::find(colorModes, std::wstring_view(textMode)) - colorModes.begin()));
            TextBox panelColor;
            panelColor.Header(box_value(L"面板颜色"));
            panelColor.Text(block.GetNamedString(L"panelColor", L"#ffffff"));
            const auto backgroundConfirmed = check(L"已检查背景修复预览", false);
            backgroundConfirmed.IsEnabled(block.GetNamedBoolean(L"backgroundPreviewed", false));
            if (scan) {
                Grid options;
                options.ColumnSpacing(8);
                for (int i = 0; i < 3; ++i) options.ColumnDefinitions().Append(ColumnDefinition{});
                Grid::SetColumn(color, 1);
                Grid::SetColumn(panelColor, 2);
                options.Children().Append(background);
                options.Children().Append(color);
                options.Children().Append(panelColor);
                fields.Children().Append(options);
                fields.Children().Append(backgroundConfirmed);
            }
            ScrollViewer scroller;
            scroller.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroller.MaxHeight(430);
            scroller.Content(fields);
            dialog.Content(scroller);
            const auto action = co_await dialog.ShowAsync();
            if (action == ContentDialogResult::None || m_closed || task != m_pdfTask) co_return;
            JsonObject edits;
            setText(edits, L"source", source.Text());
            setText(edits, L"translation", translation.Text());
            setFlag(edits, L"keep", checked(keep));
            setFlag(edits, L"serif", checked(serif));
            setFlag(edits, L"centered", checked(centered));
            setFlag(edits, L"bold", checked(bold));
            setFlag(edits, L"confirmTranslation", checked(confirm));
            setFlag(edits, L"confirmNumbers", checked(numbers));
            edits.Insert(L"size", JsonValue::CreateNumberValue(size.Value()));
            JsonArray area;
            for (const auto& input : {x, y, width, height}) area.Append(JsonValue::CreateNumberValue(input.Value()));
            edits.Insert(L"region", area);
            if (scan) {
                setText(edits, L"backgroundMode", hstring(backgroundModes[std::max(0, background.SelectedIndex())]));
                setText(edits, L"textColorMode", hstring(colorModes[std::max(0, color.SelectedIndex())]));
                setText(edits, L"panelColor", panelColor.Text());
                setFlag(edits, L"confirmBackground", checked(backgroundConfirmed));
            }
            task->reviseBlock(page, id, edits);
            showPdfError({});
            if (action == ContentDialogResult::Secondary) {
                task->configureRecognition(pdfProvider(m_settings.visionProviderId), m_settings.pdfRecognitionPixels,
                                           std::chrono::minutes(m_settings.pdfRequestTimeout));
                runPdfOperation(task->retryBlock(page, id, pdfSettings(), pdfCallbacks()), true);
            } else if (page == m_pdfPage) {
                co_await renderPdfPage(page);
            }
        } catch (...) {
            if (!m_closed) showPdfError(to_message());
        }
    }

    fire_and_forget MainWindow::RecognizePdfPage(IInspectable const&, RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        const auto task = m_pdfTask;
        if (!task || task->snapshot().busy || m_busy || m_screenshot || documentsBusy()) co_return;
        try {
            const int page = m_pdfPage;
            const auto data = task->page(page);
            if (data && (data.HasKey(L"scan") || data.GetNamedString(L"kind", L"") != L"scan")) {
                const auto dialog = pdfDialog(PdfPage().XamlRoot(), L"AI 重新识别当前页", L"开始识别", L"取消");
                dialog.Content(description(L"重新定位文字区域，并由 AI 分段识别原文。整页识别成功后替换，原有文字和译文保留草稿。"));
                if (co_await dialog.ShowAsync() != ContentDialogResult::Primary || m_closed || task != m_pdfTask) co_return;
            }
            const auto vision = pdfProvider(m_settings.visionProviderId);
            task->configureRecognition(vision, m_settings.pdfRecognitionPixels, std::chrono::minutes(m_settings.pdfRequestTimeout));
            showPdfError({});
            runPdfOperation(task->recognizePage(page, vision, m_settings.pdfRecognitionPixels, pdfCallbacks()), true);
        } catch (...) {
            if (!m_closed) showPdfError(to_message());
        }
    }

    void MainWindow::CopyPdfScanText(IInspectable const&, RoutedEventArgs const&)
    {
        try {
            copyPdfText(m_pdfScanTranslation ? PdfPageTranslation().Text() : PdfPageSource().Text());
        } catch (...) {
            showPdfError(to_message());
        }
    }

    fire_and_forget MainWindow::OpenPdfRecognitionDraft(IInspectable const&, RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        if (!m_pdfDisplayedPage) co_return;
        try {
            const auto scan = m_pdfDisplayedPage.GetNamedObject(L"scan", nullptr);
            if (!scan) co_return;
            const auto draft = hstring(std::wstring(scan.GetNamedString(L"draftSource", L"")) + L"\r\n\r\n" +
                std::wstring(scan.GetNamedString(L"draftTranslation", L"")));
            const auto dialog = pdfDialog(PdfPage().XamlRoot(), L"上次识别与译文草稿", L"复制草稿");
            dialog.Content(textEditor(L"上次草稿", draft, 235, true));
            if (co_await dialog.ShowAsync() == ContentDialogResult::Primary && !m_closed) copyPdfText(draft);
        } catch (...) {
            if (!m_closed) showPdfError(to_message());
        }
    }

    fire_and_forget MainWindow::RecognizePdfRange(IInspectable const&, RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        const auto task = m_pdfTask;
        if (!task || task->snapshot().busy || m_busy || m_screenshot || documentsBusy()) co_return;
        try {
            const auto dialog = pdfDialog(PdfPage().XamlRoot(), L"批量识别扫描页", L"开始识别", L"取消");
            StackPanel fields;
            fields.Spacing(8);
            fields.Children().Append(description(L"按顺序识别范围内尚无原文的扫描页。已完成的页会保存，可校对原文后继续翻译。"));
            const auto first = number(L"起始页", 1, 1, pdfPageCount());
            const auto last = number(L"结束页", pdfPageCount(), 1, pdfPageCount());
            fields.Children().Append(first);
            fields.Children().Append(last);
            dialog.Content(fields);
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary || m_closed || task != m_pdfTask) co_return;
            if (!std::isfinite(first.Value()) || !std::isfinite(last.Value()) ||
                std::floor(first.Value()) != first.Value() || std::floor(last.Value()) != last.Value() || first.Value() > last.Value())
                throw hresult_invalid_argument(L"请输入有效的页码范围。");
            const auto vision = pdfProvider(m_settings.visionProviderId);
            task->configureRecognition(vision, m_settings.pdfRecognitionPixels, std::chrono::minutes(m_settings.pdfRequestTimeout));
            showPdfError({});
            runPdfOperation(task->recognizeRange(static_cast<int>(first.Value()) - 1, static_cast<int>(last.Value()) - 1,
                vision, m_settings.pdfRecognitionPixels, pdfCallbacks()), true);
        } catch (...) {
            if (!m_closed) showPdfError(to_message());
        }
    }

    fire_and_forget MainWindow::RecognizePdfCrop(IInspectable const&, RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        const auto task = m_pdfTask;
        if (!task || m_busy || m_screenshot || documentsBusy() || task->snapshot().busy) co_return;
        try {
            const auto original = PdfOriginalImage().Source().try_as<Media::Imaging::BitmapImage>();
            if (!original || !original.PixelWidth() || !original.PixelHeight()) co_return;
            const int page = m_pdfPage;
            const auto dialog = pdfDialog(PdfPage().XamlRoot(), L"框选取字", L"识别所选区域", L"取消");
            dialog.IsPrimaryButtonEnabled(false);
            StackPanel panel;
            panel.Spacing(8);
            panel.Children().Append(description(L"拖动鼠标框选文字区域。"));
            const auto scale = std::min(460.0 / original.PixelWidth(), 380.0 / original.PixelHeight());
            const auto width = original.PixelWidth() * scale;
            const auto height = original.PixelHeight() * scale;
            Canvas canvas;
            canvas.Width(width);
            canvas.Height(height);
            canvas.HorizontalAlignment(HorizontalAlignment::Center);
            Image preview;
            preview.Source(original);
            preview.Width(width);
            preview.Height(height);
            preview.IsHitTestVisible(false);
            canvas.Children().Append(preview);
            canvas.Background(Media::SolidColorBrush(Microsoft::UI::Colors::White()));
            Shapes::Rectangle selection;
            selection.Fill(Media::SolidColorBrush(Microsoft::UI::ColorHelper::FromArgb(48, 52, 116, 87)));
            selection.Stroke(Media::SolidColorBrush(Microsoft::UI::ColorHelper::FromArgb(255, 52, 116, 87)));
            selection.IsHitTestVisible(false);
            canvas.Children().Append(selection);
            struct Crop {
                Point origin{};
                std::array<double, 4> area{};
                bool dragging{};
            };
            const auto crop = std::make_shared<Crop>();
            const auto update = [crop, selection, weak = make_weak(dialog), width, height](Point position) {
                const auto x = std::clamp(static_cast<double>(position.X), 0.0, width);
                const auto y = std::clamp(static_cast<double>(position.Y), 0.0, height);
                const auto left = std::min(x, static_cast<double>(crop->origin.X));
                const auto top = std::min(y, static_cast<double>(crop->origin.Y));
                const auto w = std::abs(x - crop->origin.X), h = std::abs(y - crop->origin.Y);
                crop->area = {left / width, top / height, w / width, h / height};
                Canvas::SetLeft(selection, left);
                Canvas::SetTop(selection, top);
                selection.Width(w);
                selection.Height(h);
                if (const auto value = weak.get()) value.IsPrimaryButtonEnabled(w >= width * .005 && h >= height * .005);
            };
            canvas.PointerPressed([crop, update](IInspectable const& sender, Input::PointerRoutedEventArgs const& args) {
                const auto control = sender.as<Canvas>();
                const auto point = args.GetCurrentPoint(control);
                if (!point.Properties().IsLeftButtonPressed()) return;
                crop->origin = point.Position();
                crop->dragging = control.CapturePointer(args.Pointer());
                update(crop->origin);
                args.Handled(true);
            });
            canvas.PointerMoved([crop, update](IInspectable const& sender, Input::PointerRoutedEventArgs const& args) {
                if (crop->dragging) update(args.GetCurrentPoint(sender.as<Canvas>()).Position());
            });
            canvas.PointerReleased([crop, update](IInspectable const& sender, Input::PointerRoutedEventArgs const& args) {
                if (!crop->dragging) return;
                crop->dragging = false;
                const auto control = sender.as<Canvas>();
                update(args.GetCurrentPoint(control).Position());
                control.ReleasePointerCapture(args.Pointer());
                args.Handled(true);
            });
            canvas.PointerCaptureLost([crop](auto&&, auto&&) { crop->dragging = false; });
            panel.Children().Append(canvas);
            dialog.Content(panel);
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary || m_closed || task != m_pdfTask ||
                m_busy || m_screenshot || documentsBusy() || task->snapshot().busy) co_return;
            const auto vision = pdfProvider(m_settings.visionProviderId);
            if (vision.id.empty()) throw hresult_invalid_argument(L"请先在翻译设置中选择扫描页识图模型。");
            PdfActivityStatus().Text(L"正在识别所选区域…");
            PdfProgress().Visibility(Visibility::Visible);
            PdfProgress().IsIndeterminate(true);
            auto image = ::yiyi::renderPdfCropAsync(m_pdfSource, m_pdfDocument, page, m_settings.pdfRecognitionPixels, crop->area);
            m_pdfCrop = image;
            updatePdfTaskUi();
            const auto bytes = co_await image;
            if (m_closed) co_return;
            auto request = ::yiyi::transcribeImageAsync(vision, bytes, {}, std::chrono::minutes(m_settings.pdfRequestTimeout));
            m_pdfCrop = request;
            const auto text = co_await request;
            m_pdfCrop = nullptr;
            if (m_closed) co_return;
            updatePdfTaskUi();
            const auto result = pdfDialog(PdfPage().XamlRoot(), L"区域识别结果", L"复制原文");
            const auto output = textEditor(L"识别原文", text, 220);
            result.Content(output);
            if (co_await result.ShowAsync() == ContentDialogResult::Primary && !m_closed) copyPdfText(output.Text());
        } catch (hresult_canceled const&) {
            m_pdfCrop = nullptr;
            if (!m_closed) updatePdfTaskUi();
        } catch (...) {
            m_pdfCrop = nullptr;
            if (!m_closed) {
                updatePdfTaskUi();
                showPdfError(to_message());
            }
        }
    }
}
