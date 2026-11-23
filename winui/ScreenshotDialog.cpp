#include "ScreenshotDialog.h"

#include "OcrClient.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace yiyi {
namespace {

using namespace winrt;
using namespace Microsoft::UI::Dispatching;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Automation;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Media::Imaging;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace Windows::Foundation;
using namespace Windows::Storage::Streams;

constexpr std::array<std::wstring_view, 12> languages{
    L"简体中文", L"繁體中文", L"英语", L"日语", L"韩语", L"法语", L"德语", L"西班牙语", L"俄语", L"葡萄牙语", L"意大利语", L"阿拉伯语"
};

bool hasText(hstring const& text)
{
    return std::wstring_view{text}.find_first_not_of(L" \t\r\n\v\f\u0085\u00a0\u3000") != std::wstring_view::npos;
}

SolidColorBrush brush(std::uint32_t hex)
{
    return SolidColorBrush{Windows::UI::Color{
        255, static_cast<std::uint8_t>(hex >> 16), static_cast<std::uint8_t>(hex >> 8), static_cast<std::uint8_t>(hex)
    }};
}

void column(Grid const& grid, GridLength size)
{
    ColumnDefinition definition;
    definition.Width(size);
    grid.ColumnDefinitions().Append(definition);
}

void place(Grid const& grid, FrameworkElement const& control, int index)
{
    Grid::SetColumn(control, index);
    grid.Children().Append(control);
}

enum class Outcome { Close, Recapture, Insert, Translate };

struct Draft {
    hstring text;
    hstring note;
    hstring error;
};

struct Review : std::enable_shared_from_this<Review> {
    ScreenshotDialogOptions options;
    IBuffer image;
    ContentDialog dialog;
    ComboBox mode;
    ComboBox provider;
    ComboBox target;
    TextBox editor;
    TextBlock note;
    TextBlock error;
    TextBlock status;
    TextBlock localHint;
    TextBlock privacy;
    Button recapture;
    Button start;
    Button stop;
    Button copy;
    Button insert;
    Button translate;
    DispatcherQueue dispatcher{DispatcherQueue::GetForCurrentThread()};
    IAsyncInfo recognition{nullptr};
    std::array<Draft, 2> drafts;
    Outcome outcome{Outcome::Close};
    std::uint64_t generation{};
    bool busy{};
    bool updating{};
    bool closed{};

    Review(ScreenshotDialogOptions state, IBuffer png) : options(std::move(state)), image(std::move(png)) {}

    Draft& draft() { return drafts[options.visionMode ? 1 : 0]; }

    bool actionsAvailable() const { return !options.aiAvailable || options.aiAvailable(); }

    bool validProvider() const
    {
        return provider.SelectedIndex() >= 0 && static_cast<std::size_t>(provider.SelectedIndex()) < options.providers.size();
    }

    void syncEditor(hstring const& text)
    {
        if (editor.Text() == text) return;
        updating = true;
        editor.Text(text);
        updating = false;
    }

    void refresh()
    {
        auto const vision = options.visionMode;
        provider.Visibility(vision ? Visibility::Visible : Visibility::Collapsed);
        localHint.Visibility(vision ? Visibility::Collapsed : Visibility::Visible);
        privacy.Text(vision
            ? L"点击识别后，截图发送给所选视觉模型。点击翻译时，仅发送校对后的文字。llama.cpp 需加载匹配的 mmproj。"
            : L"OCR 在本机完成。点击翻译后，校对过的文字才会发送至当前 AI 服务。");
        mode.IsEnabled(!busy);
        provider.IsEnabled(!busy);
        target.IsEnabled(!busy);
        recapture.IsEnabled(!busy);
        editor.IsReadOnly(busy);
        editor.PlaceholderText(busy ? L"正在识别原文…" : L"识别后的原文显示在这里，可校对、复制或翻译。");
        start.Content(box_value(draft().text.empty() ? L"开始识别" : L"重新识别"));
        start.IsEnabled(!busy && (!vision || (validProvider() && actionsAvailable())));
        stop.Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
        auto const usable = !busy && hasText(editor.Text());
        copy.IsEnabled(usable);
        insert.IsEnabled(usable && options.useText && actionsAvailable());
        translate.IsEnabled(usable && options.useText && actionsAvailable());
        error.Text(draft().error);
        error.Visibility(draft().error.empty() ? Visibility::Collapsed : Visibility::Visible);
        note.Text(draft().note);
        note.Visibility(draft().note.empty() ? Visibility::Collapsed : Visibility::Visible);
        status.Text(busy ? (vision ? L"等待模型识图…" : L"正在本机识别…")
                         : hstring{std::to_wstring(editor.Text().size()) + L" 字符 · 可编辑"});
    }

    void close()
    {
        closed = true;
        if (recognition) recognition.Cancel();
    }

    void rememberVision()
    {
        if (options.visionChanged) options.visionChanged(options.visionMode, options.visionProviderId);
    }

    fire_and_forget recognize()
    {
        auto lifetime = shared_from_this();
        if (closed || busy || (options.visionMode && (!validProvider() || !actionsAvailable()))) co_return;
        auto const job = ++generation;
        auto& retained = draft();
        retained.error.clear();
        retained.note.clear();
        busy = true;
        refresh();

        hstring text;
        hstring summary;
        hstring failure;
        try {
            if (options.visionMode) {
                auto const& selected = options.providers[provider.SelectedIndex()];
                auto operation = transcribeImageAsync(selected, image, [weak = weak_from_this(), queue = dispatcher, job](hstring const& preview) {
                    queue.TryEnqueue([weak, job, preview] {
                        if (auto review = weak.lock(); review && !review->closed && review->busy && review->generation == job) {
                            review->syncEditor(preview);
                            review->status.Text(L"正在识别原文…");
                        }
                    });
                });
                recognition = operation;
                text = co_await operation;
                summary = selected.name + L" · " + selected.model;
            } else {
                auto operation = recognizeImageAsync(image);
                recognition = operation;
                auto const result = co_await operation;
                text = result.GetNamedString(L"text");
                summary = result.GetNamedNumber(L"confidence", 1.0) < 0.85
                    ? L"部分文字置信度较低，请对照原图校对。" : L"识别完成，请校对后使用。";
            }
        } catch (hresult_canceled const&) {
            failure = L"识别已取消。";
        } catch (hresult_error const& detail) {
            failure = detail.message();
        } catch (...) {
            failure = L"图片识别未能完成，请重试。";
        }
        recognition = nullptr;
        busy = false;
        if (closed) co_return;
        if (failure.empty()) {
            retained.text = std::move(text);
            retained.note = std::move(summary);
        } else {
            retained.error = hasText(retained.text) ? failure + L" 已保留上次原文及修改。" : failure;
        }
        syncEditor(retained.text);
        refresh();
    }

    void makeContent(BitmapImage const& bitmap);
    void connectActions();
};

void Review::makeContent(BitmapImage const& bitmap)
{
    auto const dark = options.theme == ElementTheme::Dark;
    auto const secondary = brush(dark ? 0xacbcb1 : 0x758379);
    auto const accent = brush(dark ? 0x86c5a2 : 0x347457);
    auto const subtle = brush(dark ? 0x233129 : 0xf5f8f5);
    auto const borderColor = brush(dark ? 0x324037 : 0xe4ebe5);

    auto const size = options.root.Size();
    auto const width = std::clamp(static_cast<double>(size.Width) - 40.0, 320.0, 820.0);
    dialog.Title(box_value(L"图片取字"));
    dialog.CloseButtonText(L"关闭");
    dialog.XamlRoot(options.root);
    dialog.RequestedTheme(options.theme);
    dialog.Resources().Insert(box_value(L"ContentDialogMaxWidth"), box_value(width));

    StackPanel content;
    content.Spacing(10);
    content.Width(width - 60.0);
    ScrollViewer scroll;
    scroll.MaxHeight(std::max(220.0, static_cast<double>(size.Height) - 155.0));
    scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    scroll.Content(content);
    dialog.Content(scroll);

    Grid modes;
    modes.ColumnSpacing(8);
    column(modes, {0, GridUnitType::Auto});
    column(modes, {1, GridUnitType::Star});
    column(modes, {0, GridUnitType::Auto});
    mode.Items().Append(box_value(L"本地 OCR"));
    mode.Items().Append(box_value(L"AI 识图取字"));
    mode.SelectedIndex(options.visionMode ? 1 : 0);
    mode.MinWidth(138);
    AutomationProperties::SetName(mode, L"取字方式");
    place(modes, mode, 0);
    TextBlock heading;
    heading.Text(L"识别原文，校对或复制后再翻译");
    heading.VerticalAlignment(VerticalAlignment::Center);
    heading.Foreground(secondary);
    heading.TextTrimming(TextTrimming::CharacterEllipsis);
    place(modes, heading, 1);
    recapture.Content(box_value(L"重新截图"));
    place(modes, recapture, 2);
    content.Children().Append(modes);

    Grid models;
    models.ColumnSpacing(8);
    column(models, {1, GridUnitType::Star});
    column(models, {0, GridUnitType::Auto});
    localHint.Text(L"RapidOCR · 中英文 · 支持深色背景与小字预处理");
    localHint.Foreground(secondary);
    localHint.VerticalAlignment(VerticalAlignment::Center);
    localHint.TextTrimming(TextTrimming::CharacterEllipsis);
    place(models, localHint, 0);
    provider.PlaceholderText(L"请先在偏好设置添加视觉模型");
    AutomationProperties::SetName(provider, L"识图模型");
    int selected = -1;
    int active = -1;
    for (std::size_t i = 0; i < options.providers.size(); ++i) {
        auto const& service = options.providers[i];
        provider.Items().Append(box_value(service.name + L" · " + service.model));
        if (service.id == options.translationProviderId) active = static_cast<int>(i);
        if (service.id == options.visionProviderId) selected = static_cast<int>(i);
    }
    provider.SelectedIndex(selected >= 0 ? selected : active);
    if (validProvider()) options.visionProviderId = options.providers[provider.SelectedIndex()].id;
    place(models, provider, 0);
    place(models, start, 1);
    content.Children().Append(models);

    error.Foreground(brush(dark ? 0xefab9f : 0xaf5349));
    error.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(error);

    Grid workspace;
    workspace.ColumnSpacing(12);
    workspace.Height(std::clamp(static_cast<double>(size.Height) - 365.0, 160.0, 270.0));
    column(workspace, {std::min(210.0, width * .30), GridUnitType::Pixel});
    column(workspace, {1, GridUnitType::Star});
    Border preview;
    preview.CornerRadius({10, 10, 10, 10});
    preview.BorderBrush(borderColor);
    preview.BorderThickness({1, 1, 1, 1});
    preview.Padding({10, 10, 10, 10});
    preview.Background(subtle);
    Image screenshot;
    screenshot.Source(bitmap);
    screenshot.Stretch(Stretch::Uniform);
    AutomationProperties::SetName(screenshot, L"截图预览");
    preview.Child(screenshot);
    place(workspace, preview, 0);
    editor.AcceptsReturn(true);
    editor.TextWrapping(TextWrapping::Wrap);
    editor.FontSize(15);
    editor.CornerRadius({10, 10, 10, 10});
    editor.Background(subtle);
    editor.BorderBrush(borderColor);
    AutomationProperties::SetName(editor, L"识别原文");
    ScrollViewer::SetVerticalScrollBarVisibility(editor, ScrollBarVisibility::Auto);
    place(workspace, editor, 1);
    content.Children().Append(workspace);

    note.Foreground(secondary);
    note.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(note);

    Grid translation;
    translation.ColumnSpacing(8);
    column(translation, {1, GridUnitType::Star});
    column(translation, {0, GridUnitType::Auto});
    TextBlock selectedService;
    selectedService.Text(active >= 0 ? L"翻译使用  " + options.providers[active].name + L" · " + options.providers[active].model
                                     : hstring{L"未连接翻译服务"});
    selectedService.Foreground(secondary);
    selectedService.VerticalAlignment(VerticalAlignment::Center);
    selectedService.TextTrimming(TextTrimming::CharacterEllipsis);
    place(translation, selectedService, 0);
    for (auto const language : languages) target.Items().Append(box_value(hstring{language}));
    auto const language = std::find(languages.begin(), languages.end(), std::wstring_view{options.targetLanguage});
    target.SelectedIndex(language == languages.end() ? 0 : static_cast<int>(language - languages.begin()));
    options.targetLanguage = hstring{languages[target.SelectedIndex()]};
    AutomationProperties::SetName(target, L"译为");
    place(translation, target, 1);
    content.Children().Append(translation);

    status.Foreground(secondary);
    content.Children().Append(status);
    StackPanel buttons;
    buttons.Orientation(Orientation::Horizontal);
    buttons.HorizontalAlignment(HorizontalAlignment::Right);
    buttons.Spacing(8);
    stop.Content(box_value(L"取消识别"));
    copy.Content(box_value(L"复制原文"));
    insert.Content(box_value(L"放入原文"));
    translate.Content(box_value(L"翻译文字"));
    translate.Background(accent);
    translate.Foreground(brush(dark ? 0x14271c : 0xffffff));
    buttons.Children().Append(stop);
    buttons.Children().Append(copy);
    buttons.Children().Append(insert);
    buttons.Children().Append(translate);
    content.Children().Append(buttons);
    privacy.Foreground(secondary);
    privacy.FontSize(12);
    privacy.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(privacy);

    refresh();
}

void Review::connectActions()
{
    auto weak = weak_from_this();
    dialog.Opened([weak](auto const&, auto const&) {
        if (auto review = weak.lock(); review && !review->options.visionMode) review->recognize();
    });
    dialog.Closed([weak](auto const&, auto const&) { if (auto review = weak.lock()) review->close(); });
    mode.SelectionChanged([weak](auto const&, auto const&) {
        if (auto review = weak.lock()) {
            review->options.visionMode = review->mode.SelectedIndex() == 1;
            review->syncEditor(review->draft().text);
            review->rememberVision();
            review->refresh();
        }
    });
    provider.SelectionChanged([weak](auto const&, auto const&) {
        if (auto review = weak.lock(); review && review->validProvider()) {
            review->options.visionProviderId = review->options.providers[review->provider.SelectedIndex()].id;
            review->rememberVision();
            review->refresh();
        }
    });
    target.SelectionChanged([weak](auto const&, auto const&) {
        if (auto review = weak.lock(); review && review->target.SelectedIndex() >= 0) {
            review->options.targetLanguage = hstring{languages[review->target.SelectedIndex()]};
            if (review->options.targetChanged) review->options.targetChanged(review->options.targetLanguage);
        }
    });
    editor.TextChanged([weak](auto const&, auto const&) {
        if (auto review = weak.lock(); review && !review->updating && !review->busy) {
            review->draft().text = review->editor.Text();
            review->refresh();
        }
    });
    start.Click([weak](auto const&, auto const&) { if (auto review = weak.lock()) review->recognize(); });
    stop.Click([weak](auto const&, auto const&) {
        if (auto review = weak.lock(); review && review->recognition) review->recognition.Cancel();
    });
    copy.Click([weak](auto const&, auto const&) {
        if (auto review = weak.lock()) {
            try {
                DataPackage package;
                package.SetText(review->editor.Text());
                Clipboard::SetContent(package);
                review->status.Text(L"已复制原文");
            } catch (...) {
                review->draft().error = L"无法复制原文，请重试。";
                review->refresh();
            }
        }
    });
    auto const finish = [weak](Outcome next) {
        if (auto review = weak.lock()) {
            review->outcome = next;
            review->dialog.Hide();
        }
    };
    recapture.Click([finish](auto const&, auto const&) { finish(Outcome::Recapture); });
    insert.Click([finish](auto const&, auto const&) { finish(Outcome::Insert); });
    translate.Click([finish](auto const&, auto const&) { finish(Outcome::Translate); });
}

}

IAsyncAction showScreenshotDialogAsync(ScreenshotDialogOptions options, IBuffer image)
{
    if (!options.root) throw hresult_invalid_argument(L"截图对话框未连接到主窗口。");
    auto cancellation = co_await get_cancellation_token();
    cancellation.enable_propagation();
    if (options.theme == ElementTheme::Default && Application::Current()) {
        options.theme = Application::Current().RequestedTheme() == ApplicationTheme::Dark ? ElementTheme::Dark : ElementTheme::Light;
    }
    while (true) {
        if (!image) image = co_await captureScreenRegionAsync(options.owner);
        if (!image) co_return;

        BitmapImage bitmap;
        auto const stream = co_await imageStreamAsync(image);
        co_await bitmap.SetSourceAsync(stream);
        auto review = std::make_shared<Review>(options, std::move(image));
        review->makeContent(bitmap);
        review->connectActions();
        co_await review->dialog.ShowAsync();
        review->close();
        if (review->outcome == Outcome::Recapture) {
            options = review->options;
            continue;
        }
        if ((review->outcome == Outcome::Insert || review->outcome == Outcome::Translate) && options.useText) {
            options.useText(review->draft().text, review->options.targetLanguage, review->outcome == Outcome::Translate);
        }
        co_return;
    }
}

}
