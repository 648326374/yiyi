#include "DocumentWorkspace.h"
#include "DocumentFiles.h"
#include "NativeImage.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <utility>

using namespace winrt;
using namespace Microsoft::UI::Dispatching;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Automation;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;

namespace yiyi {
namespace {

constexpr std::array<std::wstring_view, 13> languages = {
    L"自动检测", L"简体中文", L"繁體中文", L"英语", L"日语", L"韩语", L"法语", L"德语",
    L"西班牙语", L"俄语", L"葡萄牙语", L"意大利语", L"阿拉伯语"
};
constexpr std::array<std::wstring_view, 4> tones = {L"自然", L"正式", L"口语", L"学术"};
constexpr std::array<DocumentEncoding, 5> encodings = {
    DocumentEncoding::Auto, DocumentEncoding::Utf8, DocumentEncoding::Gb18030, DocumentEncoding::Utf16Le, DocumentEncoding::Utf16Be
};

class BusyFlag {
public:
    explicit BusyFlag(bool& flag) : m_flag(flag), m_initial(std::exchange(flag, true)) {}
    ~BusyFlag() { m_flag = m_initial; }
private:
    bool& m_flag;
    bool m_initial;
};

void row(Grid const& grid, GridUnitType type = GridUnitType::Auto, double value = 1)
{
    RowDefinition definition;
    definition.Height({value, type});
    grid.RowDefinitions().Append(definition);
}

void column(Grid const& grid, GridUnitType type = GridUnitType::Star, double value = 1)
{
    ColumnDefinition definition;
    definition.Width({value, type});
    grid.ColumnDefinitions().Append(definition);
}

void place(Grid const& grid, FrameworkElement const& element, int rowIndex, int columnIndex = 0)
{
    Grid::SetRow(element, rowIndex);
    Grid::SetColumn(element, columnIndex);
    grid.Children().Append(element);
}

StackPanel stack()
{
    StackPanel panel;
    panel.Spacing(8);
    panel.Orientation(Orientation::Horizontal);
    return panel;
}

Style style(hstring const& name)
{
    return Application::Current().Resources().Lookup(box_value(name)).as<Style>();
}

TextBlock label(hstring const& text, double font = 13)
{
    TextBlock block;
    block.Style(style(font == 10 ? L"YiyiCaption" : font >= 20 ? L"YiyiTitle" : L"YiyiBodyLabel"));
    block.Text(text);
    block.FontSize(font);
    block.TextTrimming(TextTrimming::CharacterEllipsis);
    block.VerticalAlignment(VerticalAlignment::Center);
    return block;
}

TextBox textBox(hstring const& header, hstring const& placeholder, bool readOnly)
{
    TextBox editor;
    editor.Style(style(L"YiyiEditor"));
    editor.FontSize(15);
    editor.PlaceholderText(placeholder);
    editor.AcceptsReturn(true);
    editor.IsReadOnly(readOnly);
    editor.TextWrapping(TextWrapping::Wrap);
    editor.VerticalAlignment(VerticalAlignment::Stretch);
    ScrollViewer::SetVerticalScrollBarVisibility(editor, ScrollBarVisibility::Auto);
    AutomationProperties::SetName(editor, header);
    return editor;
}

Border card()
{
    Border border;
    border.Style(style(L"YiyiCard"));
    return border;
}

Border reader(TextBlock const& title, TextBox const& editor, bool divider)
{
    auto border = card();
    Grid content;
    row(content);
    row(content);
    row(content, GridUnitType::Star);
    title.Margin({14, divider ? 11.0 : 9.0, 14, divider ? 11.0 : 4.0});
    place(content, title, 0);
    if (divider) {
        Border rule;
        rule.Style(style(L"YiyiDictionaryDivider"));
        place(content, rule, 1);
    }
    editor.Margin({14, divider ? 10.0 : 4.0, 14, 9});
    place(content, editor, 2);
    border.Child(content);
    return border;
}

void buttonText(Button const& control, hstring const& text, std::wstring_view glyph = {})
{
    if (AutomationProperties::GetName(control) == text && control.Content()) return;
    AutomationProperties::SetName(control, text);
    if (glyph.empty()) {
        control.Content(box_value(text));
        return;
    }
    auto content = stack();
    content.Spacing(6);
    FontIcon icon;
    icon.Glyph(hstring(glyph));
    icon.FontSize(13);
    AutomationProperties::SetAccessibilityView(icon, Peers::AccessibilityView::Raw);
    content.Children().Append(icon);
    TextBlock caption;
    caption.Text(text);
    content.Children().Append(caption);
    control.Content(content);
}

template <typename Handler>
Button button(StackPanel const& panel, hstring const& text, Handler&& action,
              hstring const& variant = L"YiyiButton", std::wstring_view glyph = {})
{
    Button control;
    control.Style(style(variant));
    control.MinHeight(34);
    control.Padding({10, 4, 10, 4});
    buttonText(control, text, glyph);
    control.Click([handler = std::forward<Handler>(action)](auto const&, auto const&) { handler(); });
    panel.Children().Append(control);
    return control;
}

template <typename Member>
auto call(std::weak_ptr<DocumentWorkspace> weak, Member method)
{
    return [weak, method] {
        if (const auto self = weak.lock()) (self.get()->*method)();
    };
}

ComboBox combo(StackPanel const& panel, hstring const& name, double width)
{
    ComboBox picker;
    picker.Style(style(L"YiyiSelect"));
    picker.Width(width);
    AutomationProperties::SetName(picker, name);
    panel.Children().Append(picker);
    return picker;
}

template <size_t Count>
void fill(ComboBox const& picker, std::array<std::wstring_view, Count> const& options, size_t first = 0)
{
    for (size_t i = first; i < options.size(); ++i) picker.Items().Append(box_value(hstring(options[i])));
}

template <size_t Count>
int selection(std::array<std::wstring_view, Count> const& options, hstring const& text, size_t first = 0)
{
    const auto start = options.begin() + static_cast<ptrdiff_t>(first);
    const auto match = std::find(start, options.end(), std::wstring_view(text));
    return match == options.end() ? 0 : static_cast<int>(match - start);
}

template <size_t Count>
hstring value(ComboBox const& picker, std::array<std::wstring_view, Count> const& options, size_t first = 0)
{
    const int index = picker.SelectedIndex();
    const auto chosen = first + static_cast<size_t>(std::max(0, index));
    return hstring(options[std::min(chosen, options.size() - 1)]);
}

void progress(ProgressBar const& bar, size_t completed, size_t total)
{
    bar.Maximum(static_cast<double>(std::max<size_t>(1, total)));
    bar.Value(static_cast<double>(completed));
}

ContentDialog dialog(XamlRoot const& root, hstring const& title)
{
    ContentDialog result;
    result.XamlRoot(root);
    result.Title(box_value(title));
    result.CloseButtonText(L"取消");
    result.DefaultButton(ContentDialogButton::Close);
    return result;
}

IAsyncOperation<bool> confirm(XamlRoot root, hstring title, hstring detail, hstring accept)
{
    auto window = dialog(root, title);
    TextBlock content;
    content.Text(detail);
    content.TextWrapping(TextWrapping::Wrap);
    window.Content(content);
    window.PrimaryButtonText(accept);
    co_return co_await window.ShowAsync() == ContentDialogResult::Primary;
}

std::wstring shorten(hstring const& value, size_t limit)
{
    auto text = std::wstring_view(value);
    if (text.size() <= limit) return std::wstring(text);
    if (text[limit - 1] >= 0xd800 && text[limit - 1] <= 0xdbff) --limit;
    return std::wstring(text.substr(0, limit)) + L"…";
}

std::wstring count(size_t completed, size_t total)
{
    return std::to_wstring(completed) + L" / " + std::to_wstring(total) + L" 段";
}

bool sameTranslationSettings(DocumentSettings const& left, DocumentSettings const& right)
{
    return left.provider.id == right.provider.id && left.provider.protocol == right.provider.protocol
        && left.provider.baseUrl == right.provider.baseUrl && left.provider.model == right.provider.model
        && left.provider.reasoningEffort == right.provider.reasoningEffort && left.sourceLanguage == right.sourceLanguage
        && left.targetLanguage == right.targetLanguage && left.tone == right.tone;
}

std::wstring settingsDescription(DocumentSettings const& settings)
{
    return (settings.provider.name.empty() ? std::wstring(L"未选择 AI 服务") : std::wstring(settings.provider.name)) + L" · "
        + std::wstring(settings.sourceLanguage) + L" → " + std::wstring(settings.targetLanguage) + L" · " + std::wstring(settings.tone);
}

std::filesystem::path rendererSettings(std::filesystem::path const& profile)
{
    return profile / L"office-renderer.txt";
}

std::filesystem::path configuredRenderer(std::filesystem::path const& profile)
{
    const auto preferences = rendererSettings(profile);
    try {
        if (std::filesystem::exists(preferences))
            return std::filesystem::path(to_hstring(documentFiles::read(preferences, 32768)).c_str());
    } catch (...) {
    }
    for (const auto& folder : {FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86}) {
        PWSTR raw{};
        if (FAILED(SHGetKnownFolderPath(folder, 0, nullptr, &raw))) continue;
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> base(raw, CoTaskMemFree);
        const auto executable = std::filesystem::path(base.get()) / L"LibreOffice" / L"program" / L"soffice.exe";
        std::error_code error;
        if (std::filesystem::is_regular_file(executable, error)) return executable;
    }
    return {};
}

}

DocumentWorkspace::DocumentWorkspace(DocumentWorkspaceHost host)
    : m_host(std::move(host)), m_text(std::make_shared<DocumentTask>()), m_office(std::make_shared<OfficeTask>(m_host.profile))
{
}

std::shared_ptr<DocumentWorkspace> DocumentWorkspace::create(DocumentWorkspaceHost host)
{
    auto workspace = std::shared_ptr<DocumentWorkspace>(new DocumentWorkspace(std::move(host)));
    workspace->build();
    workspace->refresh();
    return workspace;
}

UIElement DocumentWorkspace::root() const { return m_root; }

void DocumentWorkspace::build()
{
    m_renderer = configuredRenderer(m_host.profile);
    m_queue = DispatcherQueue::GetForCurrentThread();
    m_root = Markup::XamlReader::Load(LR"(
        <Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
              xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
            <Grid.Resources>
                <SolidColorBrush x:Key="TextControlBackground" Color="Transparent" />
                <SolidColorBrush x:Key="TextControlBackgroundPointerOver" Color="Transparent" />
                <SolidColorBrush x:Key="TextControlBackgroundFocused" Color="Transparent" />
                <StaticResource x:Key="TextControlForegroundFocused" ResourceKey="YiyiTextBrush" />
                <StaticResource x:Key="TextControlPlaceholderForeground" ResourceKey="YiyiMutedBrush" />
                <StaticResource x:Key="TextControlPlaceholderForegroundFocused" ResourceKey="YiyiMutedBrush" />
                <StaticResource x:Key="ProgressBarForeground" ResourceKey="YiyiAccentBrush" />
                <StaticResource x:Key="ProgressBarBackground" ResourceKey="YiyiBorderBrush" />
                <StaticResource x:Key="AccentButtonBackgroundPointerOver" ResourceKey="YiyiAccentStrongBrush" />
                <StaticResource x:Key="AccentButtonBackgroundPressed" ResourceKey="YiyiAccentBrush" />
                <StaticResource x:Key="AccentButtonForegroundPointerOver" ResourceKey="YiyiAccentTextBrush" />
                <StaticResource x:Key="AccentButtonForegroundPressed" ResourceKey="YiyiAccentTextBrush" />
            </Grid.Resources>
        </Grid>)").as<Grid>();
    m_root.RowSpacing(10);
    row(m_root);
    row(m_root, GridUnitType::Star);
    Grid formats;
    column(formats);
    column(formats, GridUnitType::Auto);
    m_tabs = ListView{};
    m_tabs.HorizontalAlignment(HorizontalAlignment::Left);
    m_tabs.SelectionMode(ListViewSelectionMode::Single);
    m_tabs.SingleSelectionFollowsFocus(true);
    m_tabs.ItemsPanel(Markup::XamlReader::Load(LR"(
        <ItemsPanelTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
            <StackPanel Orientation="Horizontal" />
        </ItemsPanelTemplate>)").as<ItemsPanelTemplate>());
    ScrollViewer::SetHorizontalScrollMode(m_tabs, ScrollMode::Disabled);
    ScrollViewer::SetVerticalScrollMode(m_tabs, ScrollMode::Disabled);
    AutomationProperties::SetName(m_tabs, L"文档格式");
    for (auto name : {L"TXT", L"Word / PPT"}) {
        ListViewItem item;
        item.Style(style(L"YiyiModeSegment"));
        item.Content(box_value(hstring(name)));
        m_tabs.Items().Append(item);
    }
    place(formats, m_tabs, 0);
    place(formats, label(L"打开或拖入文件", 10), 0, 1);
    place(m_root, formats, 0);
    buildText();
    buildOffice();
    place(m_root, m_textPage, 1);
    place(m_root, m_officePage, 1);
    m_officePage.Visibility(Visibility::Collapsed);
    m_tabs.SelectionChanged([weak = weak_from_this()](auto const&, auto const&) {
        if (const auto self = weak.lock()) {
            const auto office = self->m_tabs.SelectedIndex() == 1;
            self->m_textPage.Visibility(office ? Visibility::Collapsed : Visibility::Visible);
            self->m_officePage.Visibility(office ? Visibility::Visible : Visibility::Collapsed);
        }
    });
    m_tabs.SelectedIndex(0);
    m_draftTimer = m_queue.CreateTimer();
    m_draftTimer.Interval(std::chrono::milliseconds(600));
    m_draftTimer.IsRepeating(false);
    m_draftTimer.Tick([weak = weak_from_this()](auto const&, auto const&) {
        if (const auto self = weak.lock(); self && !self->m_closed) {
            try { self->m_office->flush(); }
            catch (...) { self->report(true, to_message(), true); }
        }
    });
}

Grid DocumentWorkspace::preferenceBar(Preferences& preferences, StackPanel const& providerPanel)
{
    Grid rowContent;
    column(rowContent);
    column(rowContent, GridUnitType::Auto);
    const bool isOffice = &preferences == &m_officeSettings;
    auto languagesPanel = stack();
    languagesPanel.Spacing(6);
    preferences.source = combo(languagesPanel, L"原文语言", isOffice ? 112 : 120);
    languagesPanel.Children().Append(label(L"→", 12));
    preferences.target = combo(languagesPanel, L"目标语言", isOffice ? 105 : 112);
    preferences.tone = combo(languagesPanel, L"翻译风格", 82);
    preferences.provider = combo(providerPanel, L"AI 服务", isOffice ? 146 : 166);
    preferences.provider.PlaceholderText(L"选择 AI 服务");
    fill(preferences.source, languages);
    fill(preferences.target, languages, 1);
    fill(preferences.tone, tones);
    for (auto const& picker : {preferences.provider, preferences.source, preferences.target, preferences.tone}) {
        picker.SelectionChanged([weak = weak_from_this(), isOffice](auto const&, auto const&) {
            if (const auto self = weak.lock(); self && !self->m_updatingPreferences && !self->m_closed)
                self->preferencesChanged(isOffice);
        });
    }
    if (!isOffice && m_host.openSettings) {
        auto settings = button(providerPanel, L"管理 AI 服务", [weak = weak_from_this()] {
            if (const auto self = weak.lock(); self && !self->m_closed) self->m_host.openSettings();
        }, L"YiyiGhostButton");
        FontIcon icon;
        icon.Glyph(L"\uE713");
        icon.FontSize(15);
        settings.Content(icon);
        ToolTipService::SetToolTip(settings, box_value(hstring(L"管理 AI 服务")));
    }
    place(rowContent, languagesPanel, 0);
    if (isOffice) place(rowContent, providerPanel, 0, 1);
    return rowContent;
}

void DocumentWorkspace::buildText()
{
    m_textPage = Grid{};
    m_textPage.RowSpacing(10);
    for (const auto size : {GridUnitType::Auto, GridUnitType::Auto, GridUnitType::Auto, GridUnitType::Auto,
                            GridUnitType::Star, GridUnitType::Auto, GridUnitType::Auto})
        row(m_textPage, size);

    Grid header;
    column(header);
    column(header, GridUnitType::Auto);
    place(header, label(L"文档翻译", 22), 0);
    auto import = stack();
    m_textOpen = button(import, L"导入 TXT", [weak = weak_from_this()] { if (const auto self = weak.lock()) self->pick(false); },
                        L"YiyiSoftButton", L"\uE8A5");
    place(header, import, 0, 1);
    place(m_textPage, header, 0);

    Grid connection;
    column(connection);
    column(connection, GridUnitType::Auto);
    auto encodingPanel = stack();
    encodingPanel.Children().Append(label(L"编码", 11));
    m_encoding = combo(encodingPanel, L"TXT 编码", 148);
    for (const auto* encoding : {L"自动 / UTF-8", L"UTF-8", L"GB18030 / GBK", L"UTF-16 LE", L"UTF-16 BE"})
        m_encoding.Items().Append(box_value(hstring(encoding)));
    m_encoding.SelectedIndex(0);
    place(connection, encodingPanel, 0);
    auto provider = stack();
    auto languageBar = preferenceBar(m_textSettings, provider);
    place(connection, provider, 0, 1);
    place(m_textPage, connection, 1);
    m_txtInfo = InfoBar{};
    m_txtInfo.IsClosable(true);
    place(m_textPage, m_txtInfo, 2);
    m_txtCount = label(L"", 10);
    place(languageBar, m_txtCount, 0, 1);
    place(m_textPage, languageBar, 3);

    Grid readers;
    readers.ColumnSpacing(14);
    column(readers);
    column(readers);
    m_txtSource = textBox(L"原文", L"打开 TXT 后显示原文", true);
    m_txtResult = textBox(L"译文", L"按段显示译文，可暂停后继续", true);
    m_txtName = label(L"原文预览", 11);
    place(readers, reader(m_txtName, m_txtSource, true), 0);
    place(readers, reader(label(L"译文", 11), m_txtResult, true), 0, 1);
    place(m_textPage, readers, 4);
    m_txtProgress = ProgressBar{};
    AutomationProperties::SetName(m_txtProgress, L"TXT 翻译进度");
    place(m_textPage, m_txtProgress, 5);

    Grid footer;
    footer.ColumnSpacing(10);
    column(footer);
    column(footer, GridUnitType::Auto);
    StackPanel status;
    status.Spacing(3);
    status.VerticalAlignment(VerticalAlignment::Center);
    m_txtStatus = label(L"", 11);
    m_txtTask = label(L"", 10);
    status.Children().Append(m_txtStatus);
    status.Children().Append(m_txtTask);
    place(footer, status, 0);
    auto actions = stack();
    actions.Spacing(5);
    m_textReset = button(actions, L"重新翻译", call(weak_from_this(), &DocumentWorkspace::resetText), L"YiyiGhostButton");
    m_textCopy = button(actions, L"复制译文", call(weak_from_this(), &DocumentWorkspace::copyText), L"YiyiGhostButton");
    m_textSave = button(actions, L"导出 TXT", [weak = weak_from_this()] { if (const auto self = weak.lock()) self->save(false); },
                        L"YiyiButton", L"\uE896");
    m_textTranslate = button(actions, L"开始翻译", call(weak_from_this(), &DocumentWorkspace::translateText),
                             L"YiyiPrimaryButton", L"\uE72A");
    place(footer, actions, 0, 1);
    place(m_textPage, footer, 6);
}

void DocumentWorkspace::buildOffice()
{
    m_officePage = Grid{};
    m_officePage.RowSpacing(8);
    for (const auto size : {GridUnitType::Auto, GridUnitType::Auto, GridUnitType::Auto, GridUnitType::Star,
                            GridUnitType::Auto, GridUnitType::Auto})
        row(m_officePage, size);
    Grid header;
    header.ColumnSpacing(12);
    column(header);
    column(header, GridUnitType::Auto);
    StackPanel heading;
    heading.Spacing(2);
    heading.Children().Append(label(L"Word / PPT 翻译", 22));
    m_docName = label(L"", 12);
    heading.Children().Append(m_docName);
    place(header, heading, 0);
    auto top = stack();
    top.VerticalAlignment(VerticalAlignment::Center);
    m_officeOpen = button(top, L"打开文档", [weak = weak_from_this()] { if (const auto self = weak.lock()) self->pick(true); },
                          L"YiyiSoftButton", L"\uE8A5");
    m_officeImages = button(top, L"图片取字", call(weak_from_this(), &DocumentWorkspace::showImages), L"YiyiButton", L"\uE722");
    m_officeOptions = Button{};
    m_officeOptions.Style(style(L"YiyiGhostButton"));
    m_officeOptions.MinHeight(34);
    m_officeOptions.MinWidth(34);
    FontIcon optionsIcon;
    optionsIcon.Glyph(L"\uE713");
    optionsIcon.FontSize(15);
    m_officeOptions.Content(optionsIcon);
    AutomationProperties::SetName(m_officeOptions, L"文档与版式选项");
    ToolTipService::SetToolTip(m_officeOptions, box_value(hstring(L"文档与版式选项")));
    MenuFlyout menu;
    const auto action = [&](hstring const& name, auto method) {
        MenuFlyoutItem item;
        item.Text(name);
        item.Click([invoke = call(weak_from_this(), method)](auto const&, auto const&) { invoke(); });
        menu.Items().Append(item);
        return item;
    };
    m_fitOptions = action(L"PPT 字号设置", &DocumentWorkspace::configureOffice);
    action(L"选择 LibreOffice", &DocumentWorkspace::selectRenderer);
    action(L"生成 PDF 版式预览", &DocumentWorkspace::generateLayoutPreview);
    m_previewOpen = action(L"打开版式预览", &DocumentWorkspace::openLayoutPreview);
    menu.Items().Append(MenuFlyoutSeparator{});
    m_officeReset = action(L"清除译文", &DocumentWorkspace::resetOffice);
    if (m_host.openSettings) {
        MenuFlyoutItem settings;
        settings.Text(L"管理 AI 服务");
        settings.Click([weak = weak_from_this()](auto const&, auto const&) {
            if (const auto self = weak.lock(); self && !self->m_closed) self->m_host.openSettings();
        });
        menu.Items().Append(settings);
    }
    m_officeOptions.Flyout(menu);
    top.Children().Append(m_officeOptions);
    place(header, top, 0, 1);
    place(m_officePage, header, 0);
    place(m_officePage, preferenceBar(m_officeSettings, stack()), 1);
    m_docInfo = InfoBar{};
    m_docInfo.IsClosable(true);
    place(m_officePage, m_docInfo, 2);

    Grid readers;
    readers.ColumnSpacing(10);
    column(readers, GridUnitType::Pixel, 190);
    column(readers);
    auto listCard = card();
    Grid list;
    list.Margin({10, 9, 8, 6});
    list.RowSpacing(5);
    row(list);
    row(list, GridUnitType::Star);
    m_docCount = label(L"文档段落", 11);
    place(list, m_docCount, 0);
    m_segmentList = ListView{};
    m_segmentList.ItemTemplate(Markup::XamlReader::Load(LR"(
        <DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
            <StackPanel Padding="0,4" Spacing="4">
                <TextBlock Text="{Binding [Status]}" Style="{StaticResource YiyiCaption}" TextTrimming="CharacterEllipsis" />
                <TextBlock Text="{Binding [Source]}" Style="{StaticResource YiyiBodyLabel}"
                           Foreground="{ThemeResource YiyiTextBrush}" TextTrimming="CharacterEllipsis" />
            </StackPanel>
        </DataTemplate>)").as<DataTemplate>());
    AutomationProperties::SetName(m_segmentList, L"文档段落");
    m_segmentList.SelectionChanged([weak = weak_from_this()](auto const&, auto const&) {
        if (const auto self = weak.lock(); self && !self->m_closed && !self->m_updating)
            self->chooseSegment(self->m_segmentList.SelectedIndex());
    });
    place(list, m_segmentList, 1);
    listCard.Child(list);
    place(readers, listCard, 0);
    Grid editors;
    editors.RowSpacing(8);
    row(editors);
    row(editors, GridUnitType::Star);
    row(editors, GridUnitType::Star);
    row(editors);
    m_docLocation = label(L"选择段落查看原文与译文", 11);
    place(editors, m_docLocation, 0);
    m_docSource = textBox(L"原文", L"打开或拖入 DOCX / PPTX 文档", true);
    m_docResult = textBox(L"译文校对", L"开始翻译后显示译文，也可自行填写", false);
    m_docResult.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
        if (const auto self = weak.lock(); self && !self->m_updating && !self->m_closed) self->editDraft();
    });
    place(editors, reader(label(L"原文", 11), m_docSource, false), 1);
    place(editors, reader(label(L"译文 · 可校对编辑", 11), m_docResult, false), 2);
    Grid checkRow;
    column(checkRow);
    column(checkRow, GridUnitType::Auto);
    auto checks = stack();
    checks.Spacing(5);
    m_officeEdit = button(checks, L"保存校对", call(weak_from_this(), &DocumentWorkspace::commitDraft), L"YiyiSoftButton");
    m_officeKeep = button(checks, L"保留原文", call(weak_from_this(), &DocumentWorkspace::keepSegment));
    m_officeRetry = button(checks, L"重译此段", [weak = weak_from_this()] {
        if (const auto self = weak.lock()) {
            const auto state = self->m_office->snapshot();
            if (self->m_segment >= 0 && state.document)
                self->translateOffice(state.document->segments()[self->m_segment].id);
        }
    });
    place(checkRow, checks, 0);
    auto issues = stack();
    m_officeIssues = button(issues, L"校对清单", call(weak_from_this(), &DocumentWorkspace::showIssues), L"YiyiGhostButton");
    place(checkRow, issues, 0, 1);
    place(editors, checkRow, 3);
    place(readers, editors, 0, 1);
    place(m_officePage, readers, 3);
    m_docProgress = ProgressBar{};
    AutomationProperties::SetName(m_docProgress, L"Word / PPT 翻译进度");
    place(m_officePage, m_docProgress, 4);

    Grid footer;
    footer.ColumnSpacing(10);
    column(footer);
    column(footer, GridUnitType::Auto);
    StackPanel status;
    status.Spacing(3);
    status.VerticalAlignment(VerticalAlignment::Center);
    m_docStatus = label(L"", 11);
    m_docTask = label(L"", 10);
    status.Children().Append(m_docStatus);
    status.Children().Append(m_docTask);
    place(footer, status, 0);
    auto actions = stack();
    m_officeSave = button(actions, L"另存译文", [weak = weak_from_this()] { if (const auto self = weak.lock()) self->save(true); },
                          L"YiyiButton", L"\uE896");
    m_officeTranslate = button(actions, L"开始翻译", [weak = weak_from_this()] { if (const auto self = weak.lock()) self->translateOffice(); },
                               L"YiyiPrimaryButton", L"\uE72A");
    place(footer, actions, 0, 1);
    place(m_officePage, footer, 5);
}

void DocumentWorkspace::updatePreferences(Preferences& preferences, DocumentSettings const& settings)
{
    const BusyFlag updating(m_updatingPreferences);
    auto providers = m_host.providers ? m_host.providers() : std::vector<Provider>();
    if (!std::ranges::equal(preferences.providers, providers, [](Provider const& left, Provider const& right) {
        return left.id == right.id && left.name == right.name;
    })) {
        preferences.provider.Items().Clear();
        for (auto const& provider : providers) preferences.provider.Items().Append(box_value(provider.name));
    }
    preferences.providers = std::move(providers);
    int index = -1;
    for (size_t i = 0; i < preferences.providers.size(); ++i) {
        if (preferences.providers[i].id == settings.provider.id) index = static_cast<int>(i);
    }
    preferences.provider.SelectedIndex(index);
    preferences.source.SelectedIndex(selection(languages, settings.sourceLanguage));
    preferences.target.SelectedIndex(selection(languages, settings.targetLanguage, 1));
    preferences.tone.SelectedIndex(selection(tones, settings.tone));
}

fire_and_forget DocumentWorkspace::preferencesChanged(bool office)
{
    if (m_closed || !m_host.settingsChanged) co_return;
    const auto lifetime = shared_from_this();
    auto& selected = office ? m_officeSettings : m_textSettings;
    try {
        const auto original = m_host.currentSettings ? m_host.currentSettings() : DocumentSettings();
        const auto state = office ? m_office->snapshot() : OfficeTaskSnapshot{};
        const auto previous = state.settings.value_or(original);
        if (office && (busy() || state.translating || state.processing)) {
            updatePreferences(selected, previous);
            co_return;
        }
        auto settings = original;
        const auto index = selected.provider.SelectedIndex();
        if (index >= 0 && static_cast<size_t>(index) < selected.providers.size())
            settings.provider = selected.providers[index];
        settings.sourceLanguage = value(selected.source, languages);
        settings.targetLanguage = value(selected.target, languages, 1);
        settings.tone = value(selected.tone, tones);
        if (sameTranslationSettings(previous, settings)) co_return;

        const bool clearOffice = office && state.document && (state.settings || !state.progress.empty() || state.draftDirty);
        if (clearOffice) {
            updatePreferences(selected, previous);
            const BusyFlag modal(m_modal);
            const auto message = L"当前任务 · " + settingsDescription(previous) + L"\n切换为 · " + settingsDescription(settings)
                + L"\n\n切换设置会清除当前文档的译文和校对稿。需要保留译文时，请先取消并另存。";
            if (!co_await confirm(m_tabs.XamlRoot(), L"更改翻译设置？", hstring(message), L"清除译文并切换") || m_closed) co_return;
        }

        const BusyFlag updating(m_updatingPreferences);
        m_host.settingsChanged(settings);
        if (clearOffice) {
            try {
                m_office->reset();
            } catch (...) {
                const auto error = to_message();
                try { m_host.settingsChanged(original); }
                catch (...) {
                    throw hresult_error(E_FAIL, hstring(std::wstring(error) + L" 恢复原翻译设置失败，请检查偏好设置。"));
                }
                throw;
            }
            m_preview.reset();
            m_draftTimer.Stop();
            report(true, {});
        }
        updatePreferences(m_textSettings, settings);
        if (office || !m_office->snapshot().settings) updatePreferences(m_officeSettings, settings);
        if (clearOffice) updateOffice();
    } catch (...) {
        const auto error = to_message();
        if (!m_closed) {
            refresh();
            report(office, error, true);
        }
    }
}

DocumentSettings DocumentWorkspace::selectedSettings(Preferences const& preferences) const
{
    const auto index = preferences.provider.SelectedIndex();
    if (index < 0 || static_cast<size_t>(index) >= preferences.providers.size())
        throw hresult_invalid_argument(L"请先选择或配置用于文档翻译的 AI 服务。");
    return {
        preferences.providers[index], value(preferences.source, languages),
        value(preferences.target, languages, 1), value(preferences.tone, tones),
    };
}

bool DocumentWorkspace::busy() const
{
    return m_fileOperation || m_modal || m_modelTask;
}

void DocumentWorkspace::refresh()
{
    if (m_closed || m_updatingPreferences) return;
    const auto settings = m_host.currentSettings ? m_host.currentSettings() : DocumentSettings();
    updatePreferences(m_textSettings, settings);
    updateText();
    updateOffice(true);
}

void DocumentWorkspace::close()
{
    if (m_closed) return;
    m_closed = true;
    m_draftTimer.Stop();
    m_text->cancel();
    try { m_office->cancel(); m_office->flush(); } catch (...) {}
}

void DocumentWorkspace::report(bool office, hstring const& message, bool isError)
{
    auto info = office ? m_docInfo : m_txtInfo;
    info.Severity(isError ? InfoBarSeverity::Error : InfoBarSeverity::Informational);
    info.Message(message);
    info.IsOpen(!message.empty());
}

bool DocumentWorkspace::available(bool office)
{
    if (m_host.modelAvailable && !m_host.modelAvailable()) {
        report(office, L"请先停止当前翻译、识别或连接测试。");
        return false;
    }
    return true;
}

void DocumentWorkspace::updateText()
{
    const auto state = m_text->snapshot();
    const bool translating = state.state == DocumentState::Translating;
    const bool paused = state.state == DocumentState::Paused || state.state == DocumentState::Failed;
    const bool done = state.state == DocumentState::Completed;
    const bool locked = translating || m_fileOperation || m_modal;
    if (m_txtSource.Text() != state.source) m_txtSource.Text(state.source);
    if (m_txtResult.Text() != state.result) m_txtResult.Text(state.result);
    m_txtName.Text(state.path.empty() ? L"原文预览" : hstring(state.path.filename().native() + L" · " + std::wstring(state.encoding)));
    m_txtCount.Text(state.path.empty() ? L"TXT · 最多 20 万字符"
        : hstring(std::to_wstring(state.source.size()) + L" 字符 · " + std::to_wstring(state.total) + L" 段"));
    progress(m_txtProgress, state.completed, state.total);
    std::wstring status;
    if (translating) status = L"正在翻译 " + count(std::min(state.completed + 1, state.total), state.total);
    else if (done) status = L"已完成 " + count(state.completed, state.total);
    else if (paused) status = L"已保留 " + count(state.completed, state.total);
    else if (state.total) status = L"已导入 " + std::to_wstring(state.total) + L" 段";
    else status = L"先导入文件，再开始翻译";
    m_txtStatus.Text(hstring(status));
    m_txtTask.Text(state.taskDescription);
    m_txtTask.Visibility(state.taskDescription.empty() ? Visibility::Collapsed : Visibility::Visible);
    buttonText(m_textOpen, state.path.empty() ? L"导入 TXT" : L"更换 TXT", L"\uE8A5");
    m_textOpen.IsEnabled(!locked && !m_modelTask);
    m_encoding.IsEnabled(!locked);
    m_textReset.IsEnabled(!locked && state.hasSnapshot);
    m_textReset.Visibility(state.hasSnapshot ? Visibility::Visible : Visibility::Collapsed);
    m_textSave.IsEnabled(!locked && state.completed > 0);
    m_textCopy.IsEnabled(!locked && !state.result.empty());
    m_textCopy.Visibility(state.result.empty() ? Visibility::Collapsed : Visibility::Visible);
    buttonText(m_textSave, done || state.completed == 0 ? L"导出 TXT" : L"导出已完成部分", L"\uE896");
    buttonText(m_textTranslate, translating ? L"暂停" : paused ? L"继续翻译" : L"开始翻译", translating ? L"\uE769" : L"\uE72A");
    m_textTranslate.IsEnabled(translating || (!done && state.total > 0 && !locked && !m_modelTask));
    for (auto const& picker : {m_textSettings.provider, m_textSettings.source, m_textSettings.target, m_textSettings.tone})
        picker.IsEnabled(!locked);
    m_textSettings.provider.IsEnabled(!locked && !m_textSettings.providers.empty());
    if (!state.error.empty()) report(false, state.error, true);
}

void DocumentWorkspace::updateOffice(bool preferences)
{
    const auto state = m_office->snapshot();
    const bool locked = state.translating || state.processing || m_fileOperation || m_modal;
    const auto total = state.document ? state.document->segments().size() : 0;
    if (preferences) updatePreferences(m_officeSettings, state.settings.value_or(m_host.currentSettings ? m_host.currentSettings() : DocumentSettings()));
    m_docName.Text(state.document ? hstring(state.document->path().filename().native()) : hstring(L"保留文档结构，另存为可编辑的译文"));
    const auto segmentCount = std::to_wstring(total) + L" 段";
    m_docCount.Text(state.document && state.document->kind() == L"pptx"
        ? hstring(std::to_wstring(state.document->pageCount()) + L" 页 · " + segmentCount)
        : hstring(L"文档段落 · " + segmentCount));
    progress(m_docProgress, state.completed, total);
    std::wstring status;
    if (state.translating && state.activeSegment) status = L"正在翻译 " + count(*state.activeSegment + 1, total);
    else if (state.document) status = L"已处理 " + count(state.completed, total);
    else status = L"先打开文档，再开始翻译";
    m_docStatus.Text(hstring(status));
    m_docTask.Text(state.settings ? hstring(std::wstring(state.settings->provider.name) + L" · " +
        std::wstring(state.settings->sourceLanguage) + L" → " + std::wstring(state.settings->targetLanguage) + L" · " +
        std::wstring(state.settings->tone)) : hstring());
    m_docTask.Visibility(state.settings ? Visibility::Visible : Visibility::Collapsed);
    buttonText(m_officeOpen, state.document ? L"更换文档" : L"打开文档", L"\uE8A5");
    m_officeOpen.IsEnabled(!locked && !m_modelTask);
    m_officeSave.IsEnabled(!locked && state.document != nullptr);
    m_officeIssues.IsEnabled(!locked && state.document != nullptr);
    const bool hasImages = state.document && !state.document->images().empty();
    m_officeImages.IsEnabled(!locked && hasImages);
    m_officeImages.Visibility(hasImages ? Visibility::Visible : Visibility::Collapsed);
    m_officeOptions.IsEnabled(!locked);
    m_officeReset.IsEnabled(!locked && state.document != nullptr);
    m_fitOptions.IsEnabled(state.document && state.document->kind() == L"pptx");
    m_previewOpen.IsEnabled(m_preview != nullptr);
    buttonText(m_officeTranslate, state.translating ? L"暂停" : total && state.completed == total ? L"重新翻译"
        : state.completed ? L"继续翻译" : L"开始翻译", state.translating ? L"\uE769" : L"\uE72A");
    m_officeTranslate.IsEnabled(state.translating || (total && !locked && !m_modelTask));
    for (auto const& picker : {m_officeSettings.provider, m_officeSettings.source, m_officeSettings.target, m_officeSettings.tone})
        picker.IsEnabled(!locked);
    m_officeSettings.provider.IsEnabled(!locked && !m_officeSettings.providers.empty());
    if (!state.error.empty()) report(true, state.error, true);
    if (state.activeSegment) m_segment = static_cast<int>(*state.activeSegment);
    if (m_segment < 0 && total) m_segment = 0;
    if (m_segment >= 0 && static_cast<size_t>(m_segment) >= total) m_segment = -1;
    m_updating = true;
    m_segmentList.Items().Clear();
    if (state.document) {
        for (const auto& item : state.document->segments()) {
            std::wstring_view statusPrefix = L"待译 · ";
            if (const auto record = state.progress.find(item.id); record != state.progress.end()) {
                if (!record->second.failure.empty()) statusPrefix = L"待校对 · ";
                else if (record->second.kept) statusPrefix = L"原文 · ";
                else if (record->second.done) statusPrefix = L"已译 · ";
            }
            auto source = shorten(item.source, 90);
            std::replace_if(source.begin(), source.end(), [](wchar_t c) { return c == L'\r' || c == L'\n' || c == L'\t'; }, L' ');
            Windows::Foundation::Collections::PropertySet rowData;
            rowData.Insert(L"Status", box_value(hstring(std::wstring(statusPrefix) + std::wstring(item.location))));
            rowData.Insert(L"Source", box_value(hstring(source)));
            m_segmentList.Items().Append(rowData);
        }
    }
    m_segmentList.SelectedIndex(m_segment);
    m_updating = false;
    updateOfficeSelection();
}

void DocumentWorkspace::updateOfficeSelection()
{
    const auto state = m_office->snapshot();
    const bool selected = state.document && m_segment >= 0 && static_cast<size_t>(m_segment) < state.document->segments().size();
    const bool editable = selected && !state.translating && !state.processing && !m_fileOperation && !m_modal;
    m_officeEdit.IsEnabled(editable);
    m_officeKeep.IsEnabled(editable);
    m_officeRetry.IsEnabled(editable && !m_modelTask);
    m_docResult.IsReadOnly(!editable);
    hstring source;
    hstring text;
    hstring location = L"选择段落查看原文与译文";
    if (selected) {
        const auto& item = state.document->segments()[m_segment];
        source = item.source;
        location = item.location;
        if (state.draftId == item.id) text = state.draftText;
        else if (const auto record = state.progress.find(item.id); record != state.progress.end() && record->second.done)
            text = record->second.translation;
    }
    m_updating = true;
    m_docLocation.Text(location);
    if (m_docSource.Text() != source) m_docSource.Text(source);
    if (m_docResult.Text() != text) m_docResult.Text(text);
    m_updating = false;
}

DocumentCallbacks DocumentWorkspace::callbacks(bool office)
{
    const auto weak = weak_from_this();
    const auto queue = m_queue;
    return {
        [weak, office] {
            if (const auto self = weak.lock(); self && !self->m_closed) {
                if (office) self->updateOffice();
                else self->updateText();
            }
        },
        [weak, queue, office](uint64_t generation, size_t segment, hstring const& current) {
            queue.TryEnqueue([weak, generation, segment, current, office] {
                if (const auto self = weak.lock(); self && !self->m_closed) {
                    if (office) {
                        const auto state = self->m_office->snapshot();
                        if (state.translating && state.generation == generation && state.activeSegment == segment - 1
                            && self->m_segment == static_cast<int>(segment - 1)) {
                            self->m_updating = true;
                            self->m_docResult.Text(current);
                            self->m_updating = false;
                        }
                    } else {
                        const auto state = self->m_text->snapshot();
                        if (state.state == DocumentState::Translating && state.generation == generation && state.completed + 1 == segment)
                            self->m_txtResult.Text(hstring(std::wstring(state.result) + std::wstring(current)));
                    }
                }
            });
        },
    };
}

IAsyncAction DocumentWorkspace::open(std::filesystem::path path)
{
    const auto lifetime = shared_from_this();
    const bool office = documentFiles::hasExtension(path, L".docx") || documentFiles::hasExtension(path, L".pptx");
    if (m_closed) co_return;
    if (busy()) {
        report(office, L"请先停止文档翻译或等待当前操作完成。");
        co_return;
    }
    m_tabs.SelectedIndex(office ? 1 : 0);
    try {
        const bool hasResult = office ? m_office->snapshot().hasUnexportedResult : m_text->snapshot().hasUnexportedResult;
        if (hasResult) {
            m_modal = true;
            const bool proceed = co_await confirm(m_tabs.XamlRoot(), L"更换文档", L"当前文档含未导出的译文或校对稿。", L"更换");
            m_modal = false;
            if (!proceed || m_closed) co_return;
        }
        const auto encoding = encodings[static_cast<size_t>(std::clamp(m_encoding.SelectedIndex(), 0, static_cast<int>(encodings.size() - 1)))];
        m_fileOperation = true;
        report(office, L"正在读取文档…");
        if (office) updateOffice(); else updateText();
        apartment_context ui;
        co_await resume_background();
        hstring error;
        try {
            if (office) m_office->load(path);
            else m_text->load(path, encoding);
        } catch (...) {
            error = to_message();
        }
        co_await ui;
        m_fileOperation = false;
        if (m_closed) co_return;
        report(office, error, !error.empty());
        if (office) {
            if (error.empty()) {
                m_segment = -1;
                m_preview.reset();
            }
            updateOffice(error.empty());
        } else {
            updateText();
        }
    } catch (...) {
        m_modal = m_fileOperation = false;
        if (!m_closed) {
            report(office, to_message(), true);
            if (office) updateOffice(); else updateText();
        }
    }
}

fire_and_forget DocumentWorkspace::pick(bool office)
{
    if (m_closed || busy()) co_return;
    const auto lifetime = shared_from_this();
    try {
        m_modal = true;
        Pickers::FileOpenPicker picker;
        if (office) {
            picker.FileTypeFilter().Append(L".docx");
            picker.FileTypeFilter().Append(L".pptx");
        } else {
            picker.FileTypeFilter().Append(L".txt");
        }
        check_hresult(picker.as<::IInitializeWithWindow>()->Initialize(static_cast<HWND>(m_host.owner)));
        const auto file = co_await picker.PickSingleFileAsync();
        m_modal = false;
        if (file && !m_closed) co_await open(std::filesystem::path(file.Path().c_str()));
    } catch (...) {
        m_modal = false;
        if (!m_closed) report(office, to_message(), true);
    }
}

fire_and_forget DocumentWorkspace::save(bool office)
{
    if (m_closed || busy()) co_return;
    const auto lifetime = shared_from_this();
    try {
        m_modal = true;
        const auto suggested = office ? m_office->suggestedOutput() : m_text->suggestedOutput();
        Pickers::FileSavePicker picker;
        const auto extension = hstring(suggested.extension().native());
        picker.FileTypeChoices().Insert(office ? L"Office 文档" : L"TXT 文本", single_threaded_vector<hstring>({extension}));
        picker.SuggestedFileName(hstring(suggested.stem().native()));
        check_hresult(picker.as<::IInitializeWithWindow>()->Initialize(static_cast<HWND>(m_host.owner)));
        const auto file = co_await picker.PickSaveFileAsync();
        m_modal = false;
        if (!file || m_closed) co_return;
        const auto path = std::filesystem::path(file.Path().c_str());
        m_fileOperation = true;
        report(office, L"正在保存译文…");
        if (office) updateOffice(); else updateText();
        apartment_context ui;
        co_await resume_background();
        hstring error;
        try {
            if (office) m_office->exportTo(path);
            else m_text->exportTo(path);
        } catch (...) { error = to_message(); }
        co_await ui;
        m_fileOperation = false;
        if (!m_closed) {
            if (office) updateOffice(); else updateText();
            report(office, error.empty() ? (office ? L"已另存译文及校对清单。" : L"已导出 UTF-8 TXT。") : error, !error.empty());
        }
    } catch (...) {
        m_modal = m_fileOperation = false;
        if (!m_closed) {
            report(office, to_message(), true);
            if (office) updateOffice(); else updateText();
        }
    }
}

fire_and_forget DocumentWorkspace::translateText()
{
    if (m_closed) co_return;
    const auto lifetime = shared_from_this();
    const auto state = m_text->snapshot();
    try {
        if (state.state == DocumentState::Translating) {
            m_text->cancel();
            updateText();
            co_return;
        }
        if (busy() || !available(false)) co_return;
        report(false, {});
        m_modelTask = true;
        if (state.hasSnapshot) co_await m_text->resume(callbacks(false));
        else co_await m_text->start(selectedSettings(m_textSettings), callbacks(false));
    } catch (...) {
        if (!m_closed) report(false, to_message(), true);
    }
    m_modelTask = false;
    if (!m_closed) {
        updateText();
        updateOffice();
    }
}

fire_and_forget DocumentWorkspace::translateOffice(std::optional<hstring> id)
{
    if (m_closed) co_return;
    const auto lifetime = shared_from_this();
    try {
        auto state = m_office->snapshot();
        if (state.translating) {
            if (!id) m_office->cancel();
            updateOffice();
            co_return;
        }
        if (busy() || !state.document || !available(true)) co_return;
        const auto settings = selectedSettings(m_officeSettings);
        const bool changed = state.settings && !sameTranslationSettings(*state.settings, settings);
        const bool complete = !id && state.completed > 0 && state.completed == state.document->segments().size();
        if (changed || complete) {
            m_modal = true;
            const auto reset = co_await confirm(m_tabs.XamlRoot(), L"重新翻译文档",
                L"确认使用当前服务与语言设置，清除已有译文及校对稿后重新翻译。", L"重新翻译");
            m_modal = false;
            if (!reset || m_closed) co_return;
            m_office->reset();
        } else if (state.draftDirty && !co_await resolveDraft(id ? L"重译此段" : L"开始翻译")) {
            co_return;
        }
        if (m_closed) co_return;
        report(true, {});
        m_preview.reset();
        m_modelTask = true;
        if (id) co_await m_office->retry(*id, settings, callbacks(true));
        else co_await m_office->start(settings, callbacks(true));
    } catch (...) {
        m_modal = false;
        if (!m_closed) report(true, to_message(), true);
    }
    m_modelTask = false;
    if (!m_closed) {
        updateText();
        updateOffice();
    }
}

fire_and_forget DocumentWorkspace::resetText()
{
    if (m_closed || busy()) co_return;
    const auto lifetime = shared_from_this();
    try {
        if (m_text->snapshot().hasUnexportedResult) {
            m_modal = true;
            const bool clear = co_await confirm(m_tabs.XamlRoot(), L"重新翻译 TXT", L"已有译文尚未全部导出。", L"清除译文");
            m_modal = false;
            if (!clear || m_closed) co_return;
        }
        m_text->reset();
        report(false, {});
        updateText();
    } catch (...) {
        m_modal = false;
        if (!m_closed) report(false, to_message(), true);
    }
}

fire_and_forget DocumentWorkspace::resetOffice()
{
    if (m_closed || busy() || !m_office->snapshot().document) co_return;
    const auto lifetime = shared_from_this();
    try {
        m_modal = true;
        const bool clear = co_await confirm(m_tabs.XamlRoot(), L"清除当前文档译文",
            L"译文与校对稿将从本机任务中移除，原文件继续保留。", L"清除译文");
        m_modal = false;
        if (!clear || m_closed) co_return;
        m_office->reset();
        m_preview.reset();
        report(true, {});
        updateOffice();
    } catch (...) {
        m_modal = false;
        if (!m_closed) report(true, to_message(), true);
    }
}

IAsyncOperation<bool> DocumentWorkspace::resolveDraft(hstring title)
{
    const auto state = m_office->snapshot();
    if (!state.draftDirty) co_return true;
    auto decision = dialog(m_tabs.XamlRoot(), title);
    decision.Content(box_value(hstring(L"当前校对稿尚未保存。")));
    decision.PrimaryButtonText(L"保存校对");
    decision.SecondaryButtonText(L"放弃校对");
    m_modal = true;
    const auto result = co_await decision.ShowAsync();
    m_modal = false;
    if (m_closed || result == ContentDialogResult::None) co_return false;
    if (result == ContentDialogResult::Primary) m_office->edit(state.draftId, state.draftText);
    else m_office->discardDraft();
    co_return true;
}

fire_and_forget DocumentWorkspace::chooseSegment(int selected)
{
    if (m_closed || m_updating || m_modal || selected == m_segment) co_return;
    const auto lifetime = shared_from_this();
    try {
        if (m_office->snapshot().draftDirty) {
            m_updating = true;
            m_segmentList.SelectedIndex(m_segment);
            m_updating = false;
            if (!co_await resolveDraft(L"切换文档段落")) co_return;
        }
        if (!m_closed) {
            m_updating = true;
            m_segment = selected;
            m_segmentList.SelectedIndex(selected);
            m_updating = false;
            updateOffice();
        }
    } catch (...) {
        m_modal = false;
        if (!m_closed) report(true, to_message(), true);
    }
}

void DocumentWorkspace::copyText()
{
    try {
        DataPackage package;
        package.SetText(m_text->snapshot().result);
        Clipboard::SetContent(package);
        Clipboard::Flush();
        report(false, L"已复制译文。");
    } catch (...) { report(false, to_message(), true); }
}

void DocumentWorkspace::editDraft()
{
    const auto state = m_office->snapshot();
    if (m_segment < 0 || !state.document || static_cast<size_t>(m_segment) >= state.document->segments().size()
        || state.processing || state.translating || m_fileOperation)
        return;
    try {
        m_office->stageDraft(state.document->segments()[m_segment].id, m_docResult.Text());
        m_preview.reset();
        m_draftTimer.Stop();
        m_draftTimer.Start();
    } catch (...) { report(true, to_message(), true); }
}

void DocumentWorkspace::commitDraft()
{
    const auto state = m_office->snapshot();
    if (m_segment < 0 || !state.document || static_cast<size_t>(m_segment) >= state.document->segments().size()) return;
    try {
        m_draftTimer.Stop();
        m_office->edit(state.document->segments()[m_segment].id, m_docResult.Text());
        m_preview.reset();
        updateOffice();
        report(true, L"已保存校对。");
    } catch (...) { report(true, to_message(), true); }
}

void DocumentWorkspace::keepSegment()
{
    const auto state = m_office->snapshot();
    if (m_segment < 0 || !state.document || static_cast<size_t>(m_segment) >= state.document->segments().size()) return;
    try {
        m_draftTimer.Stop();
        m_office->keepOriginal(state.document->segments()[m_segment].id);
        m_preview.reset();
        updateOffice();
        report(true, L"已保留该段原文。");
    } catch (...) { report(true, to_message(), true); }
}

fire_and_forget DocumentWorkspace::showIssues()
{
    if (m_closed || busy()) co_return;
    const auto lifetime = shared_from_this();
    try {
        m_modal = true;
        apartment_context ui;
        co_await resume_background();
        std::vector<OfficeNotice> issues;
        hstring error;
        try { issues = m_office->issues(); }
        catch (...) { error = to_message(); }
        co_await ui;
        if (!m_closed) {
            if (!error.empty()) {
                report(true, error, true);
            } else {
                auto window = dialog(m_tabs.XamlRoot(), L"校对清单");
                window.CloseButtonText(L"关闭");
                if (issues.empty()) {
                    window.Content(label(L"暂无校对提示。"));
                    co_await window.ShowAsync();
                } else {
                    ListView list;
                    list.MaxHeight(450);
                    AutomationProperties::SetName(list, L"校对提示");
                    auto locators = std::make_shared<std::vector<int>>();
                    const auto segments = m_office->snapshot().document;
                    for (const auto& issue : issues) {
                        TextBlock details;
                        details.Text(hstring(std::wstring(issue.location) + L"\n" + std::wstring(issue.message)));
                        details.TextWrapping(TextWrapping::Wrap);
                        list.Items().Append(details);
                        const auto& items = segments->segments();
                        const auto found = std::find_if(items.begin(), items.end(), [&](OfficeSegment const& item) { return item.id == issue.id; });
                        locators->push_back(found == items.end() ? -1 : static_cast<int>(found - items.begin()));
                    }
                    window.Content(list);
                    window.PrimaryButtonText(L"定位段落");
                    window.IsPrimaryButtonEnabled(false);
                    list.SelectionChanged([weak = make_weak(window), locators](auto const& sender, auto const&) {
                        if (const auto active = weak.get()) {
                            const auto index = sender.template as<ListView>().SelectedIndex();
                            active.IsPrimaryButtonEnabled(index >= 0 && static_cast<size_t>(index) < locators->size() && (*locators)[index] >= 0);
                        }
                    });
                    if (co_await window.ShowAsync() == ContentDialogResult::Primary && !m_closed) {
                        const auto index = list.SelectedIndex();
                        m_modal = false;
                        if (index >= 0 && static_cast<size_t>(index) < locators->size()) {
                            chooseSegment((*locators)[index]);
                            co_return;
                        }
                    }
                }
            }
        }
    } catch (...) {
        if (!m_closed) report(true, to_message(), true);
    }
    m_modal = false;
}

fire_and_forget DocumentWorkspace::showImages()
{
    if (m_closed || busy() || !m_host.reviewImage) co_return;
    const auto lifetime = shared_from_this();
    const auto state = m_office->snapshot();
    if (!state.document || state.document->images().empty()) co_return;
    try {
        m_modal = true;
        auto window = dialog(m_tabs.XamlRoot(), L"图片取字");
        window.PrimaryButtonText(L"打开图片");
        ComboBox picker;
        picker.MinWidth(280);
        AutomationProperties::SetName(picker, L"文档图片");
        const auto& images = state.document->images();
        for (const auto& image : images) picker.Items().Append(box_value(image.name));
        picker.SelectedIndex(0);
        window.Content(picker);
        if (co_await window.ShowAsync() == ContentDialogResult::Primary && !m_closed) {
            const auto index = picker.SelectedIndex();
            if (index >= 0 && static_cast<size_t>(index) < images.size()) {
                apartment_context ui;
                co_await resume_background();
                Buffer buffer{nullptr};
                hstring error;
                try {
                    const auto bytes = state.document->imageBytes(images[index].id);
                    if (bytes.empty()) throw hresult_invalid_argument(L"图片内容为空。");
                    const auto input = std::span(reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size());
                    const auto png = encodePng(decodeImage(input));
                    const auto length = static_cast<uint32_t>(png.size());
                    buffer = Buffer(length);
                    std::memcpy(buffer.data(), png.data(), png.size());
                    buffer.Length(length);
                } catch (...) { error = to_message(); }
                co_await ui;
                m_modal = false;
                if (!m_closed) {
                    if (buffer) m_host.reviewImage(buffer);
                    else report(true, error, true);
                }
            }
        }
    } catch (...) {
        if (!m_closed) report(true, to_message(), true);
    }
    m_modal = false;
}

fire_and_forget DocumentWorkspace::configureOffice()
{
    if (m_closed || busy()) co_return;
    const auto lifetime = shared_from_this();
    try {
        m_modal = true;
        const auto options = m_office->snapshot().exportOptions;
        auto window = dialog(m_tabs.XamlRoot(), L"PPT 字号设置");
        window.PrimaryButtonText(L"保存");
        StackPanel panel;
        panel.Spacing(12);
        CheckBox fit;
        fit.Content(box_value(hstring(L"允许在字号下限内缩小译文文字")));
        fit.IsChecked(box_value(options.fitText).as<Windows::Foundation::IReference<bool>>());
        panel.Children().Append(fit);
        NumberBox minimum;
        minimum.Header(box_value(hstring(L"最小字号 (pt)")));
        minimum.Minimum(6);
        minimum.Maximum(36);
        minimum.Value(options.minimumFont);
        minimum.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Inline);
        panel.Children().Append(minimum);
        window.Content(panel);
        if (co_await window.ShowAsync() == ContentDialogResult::Primary && !m_closed) {
            m_office->setExportOptions({fit.IsChecked() && fit.IsChecked().Value(), minimum.Value()});
            m_preview.reset();
            m_previewOpen.IsEnabled(false);
        }
    } catch (...) {
        if (!m_closed) report(true, to_message(), true);
    }
    m_modal = false;
}

fire_and_forget DocumentWorkspace::selectRenderer()
{
    if (m_closed || busy()) co_return;
    const auto lifetime = shared_from_this();
    try {
        m_modal = true;
        Pickers::FileOpenPicker picker;
        picker.FileTypeFilter().Append(L".exe");
        check_hresult(picker.as<::IInitializeWithWindow>()->Initialize(static_cast<HWND>(m_host.owner)));
        const auto file = co_await picker.PickSingleFileAsync();
        if (file && !m_closed) {
            const auto path = std::filesystem::path(file.Path().c_str());
            if (CompareStringOrdinal(path.filename().c_str(), -1, L"soffice.exe", -1, TRUE) != CSTR_EQUAL)
                throw hresult_invalid_argument(L"请选择已安装 LibreOffice 的 soffice.exe。");
            std::filesystem::create_directories(m_host.profile);
            documentFiles::writeAtomically(rendererSettings(m_host.profile), to_string(path.native()));
            m_renderer = path;
            report(true, L"已设置 LibreOffice 版式预览。");
        }
    } catch (...) {
        if (!m_closed) report(true, to_message(), true);
    }
    m_modal = false;
}

fire_and_forget DocumentWorkspace::generateLayoutPreview()
{
    if (m_closed || busy() || !m_office->snapshot().document) co_return;
    const auto lifetime = shared_from_this();
    if (m_renderer.empty()) {
        report(true, L"版式预览需要已安装的 LibreOffice。请在文档选项中选择 soffice.exe。");
        co_return;
    }
    try {
        m_fileOperation = true;
        m_preview.reset();
        report(true, L"正在生成 PDF 版式预览…");
        updateOffice();
        const auto renderer = m_renderer;
        apartment_context ui;
        co_await resume_background();
        std::shared_ptr<OfficeLayoutPreview const> preview;
        hstring error;
        try { preview = m_office->renderPreview(renderer); }
        catch (...) { error = to_message(); }
        co_await ui;
        m_fileOperation = false;
        if (m_closed) co_return;
        m_preview = std::move(preview);
        updateOffice();
        report(true, error.empty() ? L"已生成 PDF 版式预览。" : error, !error.empty());
    } catch (...) {
        m_fileOperation = false;
        if (!m_closed) {
            updateOffice();
            report(true, to_message(), true);
        }
    }
}

fire_and_forget DocumentWorkspace::openLayoutPreview()
{
    if (m_closed || busy() || !m_preview) co_return;
    const auto lifetime = shared_from_this();
    const auto preview = m_preview;
    try {
        const auto file = co_await StorageFile::GetFileFromPathAsync(hstring(preview->path().native()));
        if (!m_closed && !co_await Windows::System::Launcher::LaunchFileAsync(file))
            report(true, L"无法打开 PDF 预览，请确认本机安装有 PDF 阅读器。", true);
    } catch (...) {
        if (!m_closed) report(true, to_message(), true);
    }
}

}
