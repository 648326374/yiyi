#include "DocumentWorkspace.h"
#include "DocumentFiles.h"
#include "NativeImage.h"
#include "PdfWorkerClient.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#undef GetObject
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

using namespace winrt;
using namespace Microsoft::UI::Dispatching;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Automation;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace Windows::Data::Json;
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
constexpr std::array<double, 5> officeZoom = {1.0, 1.0, 1.25, 1.5, 2.0};
constexpr double previewPaperInset = 9;

double previewReadingLine(ScrollViewer const& viewer)
{
    return viewer.ViewportHeight() * 0.15;
}

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
    block.Style(style(font == 10 ? L"YiyiCaption" : L"YiyiBodyLabel"));
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

void buttonIcon(Button const& control, hstring const& name, hstring const& glyph)
{
    if (AutomationProperties::GetName(control) == name && control.Content().try_as<FontIcon>()) return;
    FontIcon icon;
    icon.Glyph(glyph);
    icon.FontSize(15);
    AutomationProperties::SetName(control, name);
    ToolTipService::SetToolTip(control, box_value(name));
    control.Content(icon);
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
        if (std::filesystem::exists(preferences)) {
            const std::filesystem::path executable(to_hstring(documentFiles::read(preferences, 32768)).c_str());
            if (std::filesystem::is_regular_file(executable) &&
                CompareStringOrdinal(executable.filename().c_str(), -1, L"soffice.exe", -1, TRUE) == CSTR_EQUAL)
                return executable;
        }
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
    m_word = installedWord();
    m_queue = DispatcherQueue::GetForCurrentThread();
    m_root = Markup::XamlReader::Load(LR"(
        <Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
              xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
            <Grid.Resources>
                <SolidColorBrush x:Key="TextControlBackground" Color="Transparent" />
                <SolidColorBrush x:Key="TextControlBackgroundPointerOver" Color="Transparent" />
                <SolidColorBrush x:Key="TextControlBackgroundFocused" Color="Transparent" />
                <SolidColorBrush x:Key="TextControlPlaceholderForeground" Color="{ThemeResource YiyiMutedColor}" />
                <SolidColorBrush x:Key="TextControlPlaceholderForegroundFocused" Color="{ThemeResource YiyiMutedColor}" />
                <StaticResource x:Key="ProgressBarForeground" ResourceKey="YiyiAccentBrush" />
                <StaticResource x:Key="ProgressBarBackground" ResourceKey="YiyiBorderBrush" />
            </Grid.Resources>
        </Grid>)").as<Grid>();
    m_root.RowSpacing(10);
    row(m_root);
    row(m_root, GridUnitType::Star);
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
    place(m_root, m_tabs, 0);
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
    if (isOffice) {
        row(rowContent);
        row(rowContent);
        rowContent.RowSpacing(8);
        place(rowContent, providerPanel, 1);
    }
    return rowContent;
}

void DocumentWorkspace::buildText()
{
    m_textPage = Grid{};
    m_textPage.RowSpacing(10);
    for (const auto size : {GridUnitType::Auto, GridUnitType::Auto, GridUnitType::Auto,
                            GridUnitType::Star, GridUnitType::Auto, GridUnitType::Auto})
        row(m_textPage, size);

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
    m_textOpen = button(provider, L"导入 TXT", [weak = weak_from_this()] { if (const auto self = weak.lock()) self->pick(false); },
                        L"YiyiSoftButton", L"\uE8A5");
    ToolTipService::SetToolTip(m_textOpen, box_value(hstring(L"打开或拖入 TXT 文件")));
    place(connection, provider, 0, 1);
    place(m_textPage, connection, 0);
    m_txtInfo = InfoBar{};
    m_txtInfo.IsClosable(true);
    place(m_textPage, m_txtInfo, 1);
    column(languageBar);
    column(languageBar, GridUnitType::Auto);
    languageBar.ColumnSpacing(12);
    m_txtCount = label(L"", 10);
    place(languageBar, m_txtCount, 0, 1);
    place(m_textPage, languageBar, 2);

    Grid readers;
    readers.ColumnSpacing(14);
    column(readers);
    column(readers);
    m_txtSource = textBox(L"原文", L"打开 TXT 后显示原文", true);
    m_txtResult = textBox(L"译文", L"按段显示译文，可暂停后继续", true);
    m_txtName = label(L"原文预览", 11);
    place(readers, reader(m_txtName, m_txtSource, true), 0);
    place(readers, reader(label(L"译文", 11), m_txtResult, true), 0, 1);
    place(m_textPage, readers, 3);
    m_txtProgress = ProgressBar{};
    AutomationProperties::SetName(m_txtProgress, L"TXT 翻译进度");
    place(m_textPage, m_txtProgress, 4);

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
    place(m_textPage, footer, 5);
}

void DocumentWorkspace::buildOffice()
{
    const auto weak = weak_from_this();
    const auto openDocument = [weak] { if (const auto self = weak.lock()) self->pick(true); };
    m_officePage = Grid{};
    m_officePage.RowSpacing(8);
    for (const auto size : {GridUnitType::Auto, GridUnitType::Auto, GridUnitType::Star})
        row(m_officePage, size);

    m_officeEmpty = StackPanel{};
    m_officeEmpty.Spacing(8);
    m_officeEmpty.HorizontalAlignment(HorizontalAlignment::Center);
    m_officeEmpty.VerticalAlignment(VerticalAlignment::Center);
    m_officeImport = button(m_officeEmpty, L"打开文档", openDocument, L"YiyiPrimaryButton", L"\uE8A5");
    m_officeImport.MinHeight(42);
    m_officeImport.HorizontalAlignment(HorizontalAlignment::Center);
    auto dropHint = label(L"或拖入 DOCX / PPTX", 12);
    dropHint.HorizontalAlignment(HorizontalAlignment::Center);
    m_officeEmpty.Children().Append(dropHint);
    place(m_officePage, m_officeEmpty, 2);

    m_officeControls = StackPanel{};
    m_officeControls.Spacing(8);
    Grid header;
    header.ColumnSpacing(12);
    column(header, GridUnitType::Auto);
    column(header);
    column(header, GridUnitType::Auto);
    auto file = stack();
    m_officeOpen = button(file, L"更换文档", openDocument, L"YiyiSoftButton", L"\uE8A5");
    ToolTipService::SetToolTip(m_officeOpen, box_value(hstring(L"打开或拖入 Word / PPT 文件")));
    place(header, file, 0);
    m_docName = label(L"", 12);
    place(header, m_docName, 0, 1);
    auto top = stack();
    top.Spacing(4);
    m_officePreferences = Button{};
    m_officePreferences.Style(style(L"YiyiGhostButton"));
    m_officePreferences.MinHeight(34);
    buttonText(m_officePreferences, L"翻译设置");
    m_officePreferencesFlyout = Flyout{};
    m_officePreferencesFlyout.Content(preferenceBar(m_officeSettings, stack()));
    m_officePreferences.Flyout(m_officePreferencesFlyout);
    top.Children().Append(m_officePreferences);
    m_officeOptions = Button{};
    m_officeOptions.Style(style(L"YiyiGhostButton"));
    m_officeOptions.MinHeight(34);
    m_officeOptions.MinWidth(34);
    buttonIcon(m_officeOptions, L"更多文档操作", L"\uE712");
    MenuFlyout menu;
    const auto action = [&](hstring const& name, auto handler) {
        MenuFlyoutItem item;
        item.Text(name);
        item.Click([handler](auto const&, auto const&) { handler(); });
        menu.Items().Append(item);
        return item;
    };
    m_officeImages = action(L"图片取字", call(weak, &DocumentWorkspace::showImages));
    m_officeRetranslate = action(L"重新翻译", [weak] { if (const auto self = weak.lock()) self->translateOffice(); });
    m_officeReset = action(L"清除译文", call(weak, &DocumentWorkspace::resetOffice));
    menu.Items().Append(MenuFlyoutSeparator{});
    m_fitOptions = action(L"PPT 字号设置", call(weak, &DocumentWorkspace::configureOffice));
    action(L"选择 LibreOffice", call(weak, &DocumentWorkspace::selectRenderer));
    m_pptPreview = action(L"生成 PDF 版式预览", call(weak, &DocumentWorkspace::generateLayoutPreview));
    m_previewOpen = action(L"打开版式预览", call(weak, &DocumentWorkspace::openLayoutPreview));
    if (m_host.openSettings) {
        action(L"管理 AI 服务", [weak] {
            if (const auto self = weak.lock(); self && !self->m_closed) self->m_host.openSettings();
        });
    }
    m_officeOptions.Flyout(menu);
    top.Children().Append(m_officeOptions);
    place(header, top, 0, 2);
    m_officeControls.Children().Append(header);

    Grid toolbar;
    toolbar.ColumnSpacing(8);
    column(toolbar, GridUnitType::Auto);
    column(toolbar);
    column(toolbar, GridUnitType::Auto);
    auto tools = stack();
    tools.Spacing(6);
    m_officeViews = stack();
    m_officeViews.Spacing(2);
    m_wholePageView = button(m_officeViews, L"整页预览", [weak] {
        if (const auto self = weak.lock()) self->showOfficePreview(true);
    }, L"YiyiGhostButton");
    m_paragraphView = button(m_officeViews, L"逐段校对", [weak] {
        if (const auto self = weak.lock()) self->showOfficePreview(false);
    }, L"YiyiSelectedThemeButton");
    tools.Children().Append(m_officeViews);
    m_previewTools = stack();
    m_previewTools.Spacing(6);
    m_previewLink = combo(m_previewTools, L"翻页方式", 116);
    m_previewLink.Items().Append(box_value(hstring(L"内容联动")));
    m_previewLink.Items().Append(box_value(hstring(L"独立翻页")));
    m_previewLink.SelectionChanged([weak](auto const&, auto const&) {
        const auto self = weak.lock();
        if (!self || self->m_closed || self->m_updatingOfficeLinks) return;
        self->m_linkOfficePages = self->m_previewLink.SelectedIndex() == 0;
        if (self->m_linkOfficePages) self->followOfficePage(self->m_activeOfficePane);
    });
    m_previewZoom = combo(m_previewTools, L"预览缩放", 116);
    for (const auto* name : {L"适合栏宽", L"适合整页", L"125%", L"150%", L"200%"})
        m_previewZoom.Items().Append(box_value(hstring(name)));
    m_previewZoom.SelectedIndex(0);
    m_previewZoom.SelectionChanged([weak](auto const&, auto const&) {
        if (const auto self = weak.lock()) self->fitOfficePages();
    });
    m_refreshPreview = button(m_previewTools, L"刷新预览", [weak] {
        if (const auto self = weak.lock()) {
            if (self->m_previewCancel) self->cancelOfficePreview();
            else self->generateLayoutPreview();
        }
    }, L"YiyiGhostButton");
    m_refreshPreview.Width(34);
    m_refreshPreview.Padding({0, 0, 0, 0});
    tools.Children().Append(m_previewTools);
    place(toolbar, tools, 0);
    m_docStatus = label(L"", 11);
    m_docStatus.TextAlignment(TextAlignment::Right);
    place(toolbar, m_docStatus, 0, 1);
    auto actions = stack();
    actions.Spacing(6);
    m_officeSave = button(actions, L"另存译文", [weak] { if (const auto self = weak.lock()) self->save(true); },
                          L"YiyiButton", L"\uE896");
    m_officeTranslate = button(actions, L"开始翻译", [weak] { if (const auto self = weak.lock()) self->translateOffice(); },
                               L"YiyiPrimaryButton", L"\uE72A");
    place(toolbar, actions, 0, 2);
    m_officeControls.Children().Append(toolbar);
    place(m_officePage, m_officeControls, 0);
    m_docInfo = InfoBar{};
    m_docInfo.IsClosable(true);
    place(m_officePage, m_docInfo, 1);

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
    m_docSource = textBox(L"原文", L"", true);
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
    m_officeParagraphs = readers;
    place(m_officePage, readers, 2);
    buildPagePreview();
    place(m_officePage, m_officeLayout, 2);
}

void DocumentWorkspace::buildPagePreview()
{
    const auto weak = weak_from_this();
    m_officeLayout = Grid{};
    m_officeLayout.Visibility(Visibility::Collapsed);

    Grid columns;
    columns.ColumnSpacing(10);
    for (size_t side = 0; side < m_previewPanes.size(); ++side) {
        column(columns);
        auto& pane = m_previewPanes[side];
        const hstring name = side == 0 ? L"原文" : L"译文";
        Grid content;
        row(content);
        row(content, GridUnitType::Star);
        Grid heading;
        heading.Margin({10, 1, 6, 1});
        column(heading);
        column(heading, GridUnitType::Auto);
        place(heading, label(name, 12), 0);
        auto pager = stack();
        pager.Spacing(2);
        pane.previous = button(pager, name + L"上一页", [weak, side] {
            if (const auto self = weak.lock()) self->changeOfficePage(side, -1);
        }, L"YiyiGhostButton");
        pane.previous.Content(box_value(hstring(L"‹")));
        pane.counter = label(L"", 11);
        AutomationProperties::SetAutomationId(pane.counter, side == 0 ? L"OfficeOriginalPage" : L"OfficeTranslatedPage");
        pager.Children().Append(pane.counter);
        pane.next = button(pager, name + L"下一页", [weak, side] {
            if (const auto self = weak.lock()) self->changeOfficePage(side, 1);
        }, L"YiyiGhostButton");
        pane.next.Content(box_value(hstring(L"›")));
        place(heading, pager, 0, 1);
        place(content, heading, 0);

        Grid page;
        pane.viewer = ScrollViewer{};
        pane.viewer.Style(style(L"YiyiOfficePreviewViewport"));
        pane.viewer.HorizontalScrollMode(ScrollMode::Enabled);
        pane.viewer.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
        pane.viewer.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        pane.viewer.HorizontalContentAlignment(HorizontalAlignment::Center);
        AutomationProperties::SetName(pane.viewer, name + L"页面");
        pane.image = Image{};
        pane.image.Stretch(Media::Stretch::Uniform);
        AutomationProperties::SetName(pane.image, name + L"整页预览");
        pane.paper = Border{};
        pane.paper.Style(style(L"YiyiOfficePreviewPaper"));
        pane.paper.VerticalAlignment(VerticalAlignment::Top);
        pane.paper.HorizontalAlignment(HorizontalAlignment::Center);
        pane.paper.Visibility(Visibility::Collapsed);
        pane.paper.Child(pane.image);
        pane.viewer.Content(pane.paper);
        pane.viewer.SizeChanged([weak, side](auto const&, auto const&) {
            if (const auto self = weak.lock()) {
                self->fitOfficePages();
                self->positionOfficePage(side);
            }
        });
        pane.viewer.ViewChanged([weak, side](auto const&, auto const& args) {
            if (const auto self = weak.lock(); self && !args.IsIntermediate()) self->officeViewChanged(side);
        });
        place(page, pane.viewer, 0);
        pane.hint = label(side == 0 ? L"等待生成原文预览" : L"翻译后显示译文", 12);
        pane.hint.HorizontalAlignment(HorizontalAlignment::Center);
        pane.hint.TextWrapping(TextWrapping::Wrap);
        pane.hint.TextAlignment(TextAlignment::Center);
        pane.hint.Margin({8, 4, 8, 4});
        place(page, pane.hint, 0);
        place(content, page, 1);
        auto frame = card();
        frame.Child(content);
        place(columns, frame, 0, static_cast<int>(side));
    }
    place(m_officeLayout, columns, 0);
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
            m_officePreferencesFlyout.Hide();
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
            clearOfficePreview();
            m_draftTimer.Stop();
            report(true, {});
        }
        updatePreferences(m_textSettings, settings);
        if (office || !m_office->snapshot().settings) updatePreferences(m_officeSettings, settings);
        if (clearOffice) {
            updateOffice();
            if (m_wholePage) generateLayoutPreview();
        }
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
    cancelOfficePreview();
    clearOfficePreview();
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
    const bool hasDocument = state.document != nullptr;
    const auto total = state.document ? state.document->segments().size() : 0;
    const bool complete = total > 0 && state.completed == total && !state.translating;
    const bool pptx = state.document && state.document->kind() == L"pptx";
    if (preferences) updatePreferences(m_officeSettings, state.settings.value_or(m_host.currentSettings ? m_host.currentSettings() : DocumentSettings()));
    const auto name = state.document ? hstring(state.document->path().filename().native()) : hstring{};
    if (m_docName.Text() != name) {
        m_docName.Text(name);
        ToolTipService::SetToolTip(m_docName, name.empty() ? nullptr : box_value(name));
    }
    const auto segmentCount = std::to_wstring(total) + L" 段";
    m_docCount.Text(pptx
        ? hstring(std::to_wstring(state.document->pageCount()) + L" 页 · " + segmentCount)
        : hstring(L"文档段落 · " + segmentCount));
    m_officeEmpty.Visibility(hasDocument ? Visibility::Collapsed : Visibility::Visible);
    m_officeControls.Visibility(hasDocument ? Visibility::Visible : Visibility::Collapsed);
    m_officeImport.IsEnabled(!locked && !m_modelTask);
    m_officeOpen.IsEnabled(!locked && !m_modelTask);
    m_officeSave.IsEnabled(!locked && state.document != nullptr);
    m_officeSave.Visibility(state.document ? Visibility::Visible : Visibility::Collapsed);
    m_officeSave.Style(style(complete ? L"YiyiPrimaryButton" : L"YiyiButton"));
    m_officeIssues.IsEnabled(!locked && state.document != nullptr);
    const bool hasImages = state.document && !state.document->images().empty();
    m_officeImages.IsEnabled(!locked && hasImages);
    m_officeImages.Visibility(hasImages ? Visibility::Visible : Visibility::Collapsed);
    m_officePreferences.IsEnabled(!locked);
    m_officeOptions.IsEnabled(!locked);
    m_officeReset.IsEnabled(!locked && state.document != nullptr);
    m_fitOptions.Visibility(pptx ? Visibility::Visible : Visibility::Collapsed);
    m_pptPreview.Visibility(pptx ? Visibility::Visible : Visibility::Collapsed);
    m_officeRetranslate.Visibility(complete ? Visibility::Visible : Visibility::Collapsed);
    m_officeRetranslate.IsEnabled(complete && !locked && !m_modelTask);
    updateOfficePreviewControls(state);
    buttonText(m_officeTranslate, state.translating ? L"暂停" : state.completed ? L"继续翻译" : L"开始翻译",
        state.translating ? L"\uE769" : L"\uE72A");
    m_officeTranslate.IsEnabled(state.translating || (total && !locked && !m_modelTask));
    m_officeTranslate.Visibility(complete ? Visibility::Collapsed : Visibility::Visible);
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
        if (item.mixedStyle) location = location + L" · 请核对加粗、颜色或链接的范围";
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
                m_linkOfficePages = true;
                clearOfficePreview();
                const auto document = m_office->snapshot().document;
                showOfficePreview(document && document->kind() == L"docx" && (!m_renderer.empty() || !m_word.empty()));
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
        const auto state = m_office->snapshot();
        if (m_wholePage && (!m_preview || !m_preview->matches(state))) generateLayoutPreview();
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
        clearOfficePreview();
        report(true, {});
        updateOffice();
        if (m_wholePage) generateLayoutPreview();
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
            clearOfficePreview();
        }
    } catch (...) {
        if (!m_closed) report(true, to_message(), true);
    }
    m_modal = false;
    if (!m_closed) updateOffice();
}

fire_and_forget DocumentWorkspace::selectRenderer()
{
    if (m_closed || busy()) co_return;
    const auto lifetime = shared_from_this();
    bool updated = false;
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
            clearOfficePreview();
            updated = true;
            report(true, L"已设置 LibreOffice 版式预览。");
        }
    } catch (...) {
        if (!m_closed) report(true, to_message(), true);
    }
    m_modal = false;
    if (!m_closed) {
        updateOffice();
        if (updated && m_wholePage) generateLayoutPreview();
    }
}

void DocumentWorkspace::showOfficePreview(bool wholePage)
{
    if (m_closed) return;
    const bool switching = wholePage != m_wholePage;
    if (!wholePage && switching) cancelOfficePreview();
    m_wholePage = wholePage;
    m_wholePageView.Style(style(wholePage ? L"YiyiSelectedThemeButton" : L"YiyiGhostButton"));
    m_paragraphView.Style(style(wholePage ? L"YiyiGhostButton" : L"YiyiSelectedThemeButton"));
    const auto state = m_office->snapshot();
    updateOfficePreviewControls(state);
    if (!wholePage) return;
    fitOfficePages();
    if (switching) {
        for (size_t side = 0; side < m_previewPanes.size(); ++side) positionOfficePage(side);
    }
    if (!m_preview || !m_preview->matches(state)) {
        if (switching && m_previewCancel && WaitForSingleObject(m_previewCancel->get(), 0) == WAIT_OBJECT_0)
            m_restartOfficePreview = true;
        else generateLayoutPreview();
    }
}

void DocumentWorkspace::updateOfficePreviewControls(OfficeTaskSnapshot const& state)
{
    const bool docx = state.document && state.document->kind() == L"docx";
    const bool current = m_preview && m_preview->matches(state);
    const bool paired = m_previewPanes[0].document && m_previewPanes[1].document;
    const bool canLink = paired && !m_previewPanes.front().links.empty();
    m_officeLayout.Visibility(docx && m_wholePage ? Visibility::Visible : Visibility::Collapsed);
    m_officeParagraphs.Visibility(state.document && !m_wholePage ? Visibility::Visible : Visibility::Collapsed);
    m_officeViews.Visibility(docx ? Visibility::Visible : Visibility::Collapsed);
    m_previewTools.Visibility(m_wholePage ? Visibility::Visible : Visibility::Collapsed);
    m_previewOpen.IsEnabled(current && !busy());
    m_refreshPreview.IsEnabled(m_previewCancel || (docx && !busy() && !state.translating));
    buttonIcon(m_refreshPreview, m_previewCancel ? L"取消预览" : L"刷新预览", m_previewCancel ? L"\uE71A" : L"\uE72C");
    m_previewZoom.IsEnabled(m_previewPanes.front().document != nullptr);
    m_previewLink.Visibility(paired ? Visibility::Visible : Visibility::Collapsed);
    m_previewLink.IsEnabled(canLink);
    {
        const BusyFlag updating(m_updatingOfficeLinks);
        m_previewLink.SelectedIndex(canLink && m_linkOfficePages ? 0 : 1);
    }
    const auto total = state.document ? state.document->segments().size() : 0;
    std::wstring status;
    if (m_wholePage && m_previewCancel) status = L"正在生成预览…";
    else if (state.translating && state.activeSegment) status = L"正在翻译 " + count(*state.activeSegment + 1, total);
    else if (m_wholePage && m_preview && !current) status = L"预览待刷新";
    else if (total && state.completed == total) status = L"已完成 · " + std::to_wstring(total) + L" 段";
    else if (state.completed) status = L"已处理 " + count(state.completed, total);
    m_docStatus.Text(hstring(status));
}

void DocumentWorkspace::fitOfficePages()
{
    const auto index = static_cast<size_t>(std::clamp(m_previewZoom.SelectedIndex(), 0, static_cast<int>(officeZoom.size() - 1)));
    for (size_t side = 0; side < m_previewPanes.size(); ++side) {
        auto& pane = m_previewPanes[side];
        if (!pane.viewer || pane.viewer.ActualWidth() <= 2 * previewPaperInset) continue;
        const auto bitmap = pane.image.Source().try_as<Media::Imaging::BitmapImage>();
        if (!bitmap || bitmap.PixelWidth() <= 0 || bitmap.PixelHeight() <= 0) continue;
        const auto ratio = static_cast<double>(bitmap.PixelHeight()) / bitmap.PixelWidth();
        auto width = (pane.viewer.ActualWidth() - 2 * previewPaperInset) * officeZoom[index];
        if (index == 1 && pane.viewer.ActualHeight() > 2 * previewPaperInset)
            width = std::min(width, (pane.viewer.ActualHeight() - 2 * previewPaperInset) / ratio);
        const auto height = width * ratio;
        if (pane.image.Width() == width && pane.image.Height() == height) continue;
        pane.image.Width(width);
        pane.image.Height(height);
        positionOfficePage(side);
    }
}

void DocumentWorkspace::changeOfficePage(size_t side, int delta)
{
    auto& pane = m_previewPanes[side];
    if (m_closed || !pane.document) return;
    const auto page = pane.page + delta;
    if (page < 0 || page >= static_cast<int>(pane.document.GetNamedNumber(L"pageCount", 0))) return;
    m_activeOfficePane = side;
    pane.page = page;
    pane.readingTop = 0;
    renderOfficePage(side);
    followOfficePage(side);
}

void DocumentWorkspace::positionOfficePage(size_t side)
{
    auto& pane = m_previewPanes[side];
    if (pane.positionQueued || !m_wholePage || !pane.image.Source()) return;
    pane.positionQueued = m_queue.TryEnqueue([weak = weak_from_this(), side] {
        const auto self = weak.lock();
        if (!self) return;
        auto& pane = self->m_previewPanes[side];
        if (!self->m_closed && self->m_wholePage && pane.image.Source()) {
            pane.viewer.UpdateLayout();
            const auto offset = std::clamp(pane.readingTop * pane.image.ActualHeight() + previewPaperInset - previewReadingLine(pane.viewer),
                                           0.0, pane.viewer.ScrollableHeight());
            pane.lastOffset = offset;
            const bool changed = pane.resetHorizontal
                ? pane.viewer.ChangeView(0.0, offset, nullptr, true)
                : pane.viewer.ChangeView(nullptr, offset, nullptr, true);
            if (!changed) pane.lastOffset = pane.viewer.VerticalOffset();
            pane.resetHorizontal = false;
        }
        pane.positionQueued = false;
    });
}

void DocumentWorkspace::officeViewChanged(size_t side)
{
    auto& pane = m_previewPanes[side];
    if (m_closed || !m_wholePage || pane.positionQueued || !pane.image.Source()) return;
    const auto offset = pane.viewer.VerticalOffset();
    const auto height = pane.image.ActualHeight();
    if (height <= 0 || std::abs(offset - pane.lastOffset) <= 0.75) return;
    pane.lastOffset = offset;
    pane.readingTop = std::clamp((offset + previewReadingLine(pane.viewer) - previewPaperInset) / height, 0.0, 1.0);
    m_activeOfficePane = side;
    followOfficePage(side);
}

DocumentWorkspace::PreviewLink const* DocumentWorkspace::officeLinkAt(size_t side) const
{
    const auto& pane = m_previewPanes[side];
    const auto page = std::ranges::equal_range(pane.links, pane.page, {}, [](auto const& link) { return link.source.page; });
    if (page.empty()) return nullptr;
    const auto top = [](auto const& link) { return link.source.top; };
    const auto next = std::ranges::upper_bound(page, pane.readingTop, {}, top);
    if (next == page.begin()) return &page.front();
    // When cells start on the same line, prefer the first paragraph in document order.
    return &*std::ranges::lower_bound(page, top(*std::prev(next)), {}, top);
}

void DocumentWorkspace::followOfficePage(size_t side)
{
    if (!m_linkOfficePages || !m_wholePage) return;
    const auto link = officeLinkAt(side);
    if (!link) return;
    auto& other = m_previewPanes[1 - side];
    if (other.page == link->target.page && other.readingTop == link->target.top) return;
    other.readingTop = link->target.top;
    if (other.page == link->target.page) positionOfficePage(1 - side);
    else {
        other.page = link->target.page;
        renderOfficePage(1 - side);
    }
}

void DocumentWorkspace::setOfficePreviewDocuments(std::array<JsonObject, 2> const& documents)
{
    const auto previous = m_linkOfficePages ? officeLinkAt(m_activeOfficePane) : nullptr;
    const int previousId = previous ? previous->id : 0;
    std::map<int, std::array<PreviewPosition, 2>> bookmarks;
    for (size_t side = 0; side < documents.size(); ++side) {
        auto& pane = m_previewPanes[side];
        pane.document = documents[side];
        pane.links.clear();
        const int pages = pane.document ? static_cast<int>(pane.document.GetNamedNumber(L"pageCount", 0)) : 0;
        pane.page = std::min(pane.page, std::max(0, pages - 1));
        if (!pane.document) continue;
        const auto entries = pane.document.GetNamedArray(L"officeBookmarks", nullptr);
        if (!entries) continue;
        for (const auto& entry : entries) {
            const auto bookmark = entry.GetObject();
            const int id = static_cast<int>(bookmark.GetNamedNumber(L"id"));
            bookmarks[id][side] = {static_cast<int>(bookmark.GetNamedNumber(L"page")), bookmark.GetNamedNumber(L"top")};
        }
        pane.document.Remove(L"officeBookmarks");
    }
    for (const auto& [id, positions] : bookmarks) {
        if (positions[0].page < 0 || positions[1].page < 0) continue;
        for (size_t side = 0; side < documents.size(); ++side)
            m_previewPanes[side].links.push_back({id, positions[side], positions[1 - side]});
    }
    for (auto& pane : m_previewPanes) {
        std::ranges::stable_sort(pane.links, {}, [](auto const& link) { return std::pair{link.source.page, link.source.top}; });
    }
    if (m_linkOfficePages) {
        auto& active = m_previewPanes[m_activeOfficePane];
        auto& other = m_previewPanes[1 - m_activeOfficePane];
        const auto restored = std::ranges::find(active.links, previousId, &PreviewLink::id);
        if (restored != active.links.end()) {
            active.page = restored->source.page;
            active.readingTop = restored->source.top;
            other.page = restored->target.page;
            other.readingTop = restored->target.top;
        } else if (const auto link = officeLinkAt(m_activeOfficePane)) {
            other.page = link->target.page;
            other.readingTop = link->target.top;
        }
    }
    for (size_t side = 0; side < documents.size(); ++side) renderOfficePage(side);
}

void DocumentWorkspace::cancelOfficePreview()
{
    m_restartOfficePreview = false;
    if (m_previewCancel) SetEvent(m_previewCancel->get());
    if (m_previewInspection) m_previewInspection.Cancel();
}

void DocumentWorkspace::clearOfficePreview()
{
    m_preview.reset();
    m_activeOfficePane = 0;
    for (size_t side = 0; side < m_previewPanes.size(); ++side) {
        auto& pane = m_previewPanes[side];
        ++pane.generation;
        if (pane.operation) pane.operation.Cancel();
        pane.operation = nullptr;
        pane.document = nullptr;
        pane.links.clear();
        pane.page = 0;
        pane.readingTop = 0;
        pane.counter.Text(L"");
        pane.previous.IsEnabled(false);
        pane.next.IsEnabled(false);
        pane.image.Source(nullptr);
        pane.paper.Visibility(Visibility::Collapsed);
        pane.hint.Text(side == 0 ? L"等待生成原文预览" : L"翻译后显示译文");
        pane.hint.Visibility(Visibility::Visible);
    }
}

fire_and_forget DocumentWorkspace::renderOfficePage(size_t side)
{
    const auto lifetime = shared_from_this();
    const auto preview = m_preview;
    auto& pane = m_previewPanes[side];
    const auto generation = ++pane.generation;
    if (pane.operation) pane.operation.Cancel();
    pane.operation = nullptr;
    const auto pages = pane.document ? static_cast<int>(pane.document.GetNamedNumber(L"pageCount", 0)) : 0;
    pane.counter.Text(pages ? to_hstring(pane.page + 1) + L" / " + to_hstring(pages) : hstring{});
    pane.previous.IsEnabled(pages > 0 && pane.page > 0);
    pane.next.IsEnabled(pane.page + 1 < pages);
    pane.image.Source(nullptr);
    pane.paper.Visibility(Visibility::Collapsed);
    pane.resetHorizontal = true;
    pane.hint.Text(pages ? L"正在绘制页面…" : side == 0 ? L"等待生成原文预览" : L"翻译后显示译文");
    pane.hint.Visibility(Visibility::Visible);
    if (!pages || !preview) co_return;
    try {
        const auto& path = side == 0 ? preview->original() : preview->translation();
        const auto root = pane.viewer.XamlRoot();
        const auto scale = root ? root.RasterizationScale() : 1.0;
        const auto width = pane.viewer.ActualWidth() - 2 * previewPaperInset;
        const int pixels = width > 0 ? static_cast<int>(std::lround(width * officeZoom.back() * scale)) : 1500;
        const auto operation = renderPdfOriginalAsync(path, pane.document, pane.page, pixels);
        pane.operation = operation.as<IAsyncInfo>();
        const auto stream = co_await operation;
        if (m_closed || generation != pane.generation) co_return;
        Media::Imaging::BitmapImage bitmap;
        co_await bitmap.SetSourceAsync(stream);
        if (m_closed || generation != pane.generation) co_return;
        pane.image.Source(bitmap);
        pane.paper.Visibility(Visibility::Visible);
        fitOfficePages();
        positionOfficePage(side);
        pane.hint.Visibility(Visibility::Collapsed);
    } catch (...) {
        if (m_closed || generation != pane.generation) co_return;
        pane.hint.Text(L"无法显示此页");
        report(true, to_message(), true);
    }
    if (generation == pane.generation) pane.operation = nullptr;
}

fire_and_forget DocumentWorkspace::generateLayoutPreview()
{
    if (m_closed || busy()) co_return;
    const auto state = m_office->snapshot();
    if (!state.document || state.translating) co_return;
    const auto lifetime = shared_from_this();
    const bool docx = state.document->kind() == L"docx";
    const auto renderer = m_renderer.empty() && docx ? m_word : m_renderer;
    if (renderer.empty()) {
        report(true, docx ? L"整页预览需要本机 Word 或 LibreOffice。" : L"版式预览需要本机 LibreOffice。");
        co_return;
    }
    try {
        auto cancel = std::make_shared<winrt::handle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!*cancel) throw_last_error();
        m_previewCancel = cancel;
        m_fileOperation = true;
        report(true, docx ? hstring{} : hstring(L"正在生成 PDF 版式预览…"));
        updateOffice();
        apartment_context ui;
        co_await resume_background();
        std::shared_ptr<OfficeLayoutPreview const> preview;
        hstring error;
        try { preview = m_office->renderPreview(renderer, cancel->get()); }
        catch (hresult_canceled const&) {}
        catch (...) { error = to_message(); }
        co_await ui;
        std::array<JsonObject, 2> documents{nullptr, nullptr};
        if (preview && docx && !m_closed) {
            try {
                const std::array paths{preview->original(), preview->translation()};
                for (size_t side = 0; side < paths.size(); ++side) {
                    if (WaitForSingleObject(cancel->get(), 0) == WAIT_OBJECT_0 || m_closed) break;
                    if (paths[side].empty()) continue;
                    const auto inspect = inspectPdfAsync(paths[side], {}, preview->bookmarkPrefix());
                    m_previewInspection = inspect.as<IAsyncInfo>();
                    documents[side] = co_await inspect;
                    m_previewInspection = nullptr;
                }
            } catch (hresult_canceled const&) {}
            catch (...) { error = to_message(); }
        }
        m_previewInspection = nullptr;
        m_previewCancel.reset();
        m_fileOperation = false;
        const bool restart = std::exchange(m_restartOfficePreview, false);
        if (m_closed) co_return;
        const bool cancelled = WaitForSingleObject(cancel->get(), 0) == WAIT_OBJECT_0;
        if (!cancelled && error.empty() && preview && preview->matches(m_office->snapshot())) {
            m_preview = std::move(preview);
            if (docx) setOfficePreviewDocuments(documents);
        }
        updateOffice();
        if (restart && cancelled && m_wholePage) generateLayoutPreview();
        else if (!error.empty()) report(true, error, true);
        else if (cancelled) report(true, L"已取消整页预览。");
        else if (!docx) report(true, L"已生成 PDF 版式预览。");
    } catch (...) {
        m_previewInspection = nullptr;
        m_previewCancel.reset();
        m_fileOperation = false;
        if (!m_closed) {
            updateOffice();
            report(true, to_message(), true);
        }
    }
}

fire_and_forget DocumentWorkspace::openLayoutPreview()
{
    if (m_closed || busy() || !m_preview || !m_preview->matches(m_office->snapshot())) co_return;
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
