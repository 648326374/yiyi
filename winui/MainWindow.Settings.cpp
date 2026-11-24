#include "pch.h"
#include "MainWindow.xaml.h"
#include "NativeImage.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <string_view>

namespace winrt::Yiyi::WinUI::implementation
{
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Automation;
    using namespace Microsoft::UI::Xaml::Controls;
    using namespace Microsoft::UI::Xaml::Controls::Primitives;
    using namespace Windows::Foundation;
    using namespace Windows::Storage::Streams;

    namespace
    {
        struct Preset {
            wchar_t const* name;
            int protocol;
            wchar_t const* address;
            wchar_t const* model;
        };

        constexpr Preset presets[] = {
            {L"Ollama", 2, L"http://localhost:11434", L"qwen3:8b"},
            {L"LM Studio", 0, L"http://localhost:1234/v1", L""},
            {L"OpenAI", 0, L"https://api.openai.com/v1", L""},
            {L"DeepSeek", 0, L"https://api.deepseek.com/v1", L"deepseek-chat"},
            {L"通义千问", 0, L"https://dashscope.aliyuncs.com/compatible-mode/v1", L"qwen-plus"},
            {L"Claude", 3, L"https://api.anthropic.com", L""},
            {L"Gemini", 4, L"https://generativelanguage.googleapis.com/v1beta", L""},
            {L"自定义服务", 0, L"", L""},
            {L"OpenAI Responses", 1, L"https://api.openai.com/v1", L""},
        };
        constexpr std::wstring_view protocols[] = {L"openai", L"responses", L"ollama", L"anthropic", L"gemini"};
        constexpr wchar_t const* protocolLabels[] = {
            L"OpenAI Chat Completions", L"OpenAI Responses", L"Ollama 原生", L"Anthropic", L"Gemini"
        };
        constexpr wchar_t const* endpointHints[] = {
            L"此协议请求 /chat/completions。仅支持 /responses 的服务请选择 OpenAI Responses。",
            L"可填写以 /v1 结尾的服务地址，也可填写完整的 /v1/responses 地址。",
            L"填写 Ollama 主机地址和端口。程序会请求 /api/chat。",
            L"填写 Anthropic API 基础地址。程序会请求 /v1/messages。",
            L"填写 Gemini API 基础地址，例如 https://generativelanguage.googleapis.com/v1beta。",
        };
        constexpr std::wstring_view reasoningValues[] = {L"", L"none", L"low", L"medium", L"high", L"xhigh", L"max"};
        constexpr wchar_t const* reasoningLabels[] = {
            L"跟随服务（默认）", L"关闭思考 · none", L"低 · low", L"中 · medium", L"高 · high", L"很高 · xhigh", L"最高 · max"
        };

        template <size_t N>
        int indexOf(std::wstring_view const (&values)[N], hstring const& value)
        {
            const auto found = std::ranges::find(values, std::wstring_view(value));
            return found == std::end(values) ? 0 : static_cast<int>(found - std::begin(values));
        }

        hstring trim(hstring const& text)
        {
            std::wstring_view value(text);
            while (!value.empty() && iswspace(value.front())) value.remove_prefix(1);
            while (!value.empty() && iswspace(value.back())) value.remove_suffix(1);
            return hstring(value);
        }

        Style style(wchar_t const* name)
        {
            return Application::Current().Resources().Lookup(box_value(name)).as<Style>();
        }

        TextBlock label(hstring const& text, wchar_t const* styleName = L"YiyiBodyLabel")
        {
            TextBlock block;
            block.Style(style(styleName));
            block.Text(text);
            block.TextWrapping(TextWrapping::Wrap);
            return block;
        }

        Button button(hstring const& text)
        {
            Button result;
            result.Style(style(L"YiyiButton"));
            result.Content(box_value(text));
            return result;
        }

        void column(Grid const& grid, GridUnitType type)
        {
            ColumnDefinition definition;
            definition.Width({1, type});
            grid.ColumnDefinitions().Append(definition);
        }

        void place(Grid const& grid, FrameworkElement const& element, int index)
        {
            Grid::SetColumn(element, index);
            grid.Children().Append(element);
        }

        hstring probeCode()
        {
            GUID random{};
            check_hresult(CoCreateGuid(&random));
            constexpr std::wstring_view alphabet = L"ABCDEFGHJKMNPQRSTUVWXYZ23456789";
            auto value = random.Data1;
            std::wstring code;
            for (int i = 0; i < 5; ++i) {
                code += alphabet[value % alphabet.size()];
                value /= static_cast<uint32_t>(alphabet.size());
            }
            return hstring(code);
        }

        IBuffer probeImage(hstring const& code)
        {
            constexpr int width = 440, height = 140;
            using Device = std::unique_ptr<std::remove_pointer_t<HDC>, decltype(&DeleteDC)>;
            using DrawingObject = std::unique_ptr<void, decltype(&DeleteObject)>;
            Device dc(CreateCompatibleDC(nullptr), DeleteDC);
            if (!dc) throw hresult_error(E_FAIL, L"无法创建识图测试图片。");
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* pixels{};
            DrawingObject bitmap(CreateDIBSection(dc.get(), &info, DIB_RGB_COLORS, &pixels, nullptr, 0), DeleteObject);
            DrawingObject font(CreateFontW(-56, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas"), DeleteObject);
            if (!bitmap || !font || !pixels) throw hresult_error(E_FAIL, L"无法创建识图测试图片。");
            const auto oldBitmap = SelectObject(dc.get(), bitmap.get());
            const auto oldFont = SelectObject(dc.get(), font.get());
            const auto painted = PatBlt(dc.get(), 0, 0, width, height, WHITENESS);
            SetTextColor(dc.get(), RGB(0, 0, 0));
            SetBkMode(dc.get(), TRANSPARENT);
            RECT area{0, 0, width, height};
            const auto draw = DrawTextW(dc.get(), code.c_str(), static_cast<int>(code.size()), &area,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            GdiFlush();
            SelectObject(dc.get(), oldFont);
            SelectObject(dc.get(), oldBitmap);
            if (!painted || !draw) throw hresult_error(E_FAIL, L"无法绘制识图测试图片。");
            const auto bytes = static_cast<uint8_t const*>(pixels);
            ::yiyi::NativeImage image{width, height, {bytes, bytes + width * height * 4}};
            for (size_t i = 3; i < image.pixels.size(); i += 4) image.pixels[i] = 255;
            DataWriter writer;
            const auto png = ::yiyi::encodePng(image);
            writer.WriteBytes(png);
            return writer.DetachBuffer();
        }
    }

    struct ProviderEditor {
        ContentDialog dialog;
        ComboBox preset;
        ComboBox protocol;
        TextBox name;
        TextBox address;
        TextBox model;
        PasswordBox key;
        CheckBox clearKey;
        ToggleButton revealKey;
        ComboBox reasoning;
        ComboBox dictionary;
        StackPanel reasoningOptions;
        TextBlock endpointHint;
        TextBlock reasoningHint;
        TextBlock dictionaryHint;
        Button textTest{button(L"测试连接")};
        Button visionTest{button(L"测试识图")};
        InfoBar status;
        ::yiyi::Provider original;
        bool hasStoredKey{};
        bool testing{};
        bool closed{};

        ProviderEditor(XamlRoot const& root, ElementTheme theme, ::yiyi::Provider existing, bool unreadable)
            : original(std::move(existing)), hasStoredKey(!original.apiKey.empty() || unreadable)
        {
            dialog.XamlRoot(root);
            dialog.RequestedTheme(theme);
            dialog.Title(box_value(original.id.empty() ? L"连接一个 AI 服务" : L"编辑 AI 服务"));
            dialog.PrimaryButtonText(L"保存服务");
            dialog.CloseButtonText(L"取消");
            dialog.DefaultButton(ContentDialogButton::Primary);
            AutomationProperties::SetAutomationId(preset, L"ServicePreset");
            AutomationProperties::SetAutomationId(protocol, L"ServiceProtocol");
            AutomationProperties::SetAutomationId(name, L"ServiceName");
            AutomationProperties::SetAutomationId(address, L"ServiceAddress");
            AutomationProperties::SetAutomationId(model, L"ServiceModel");
            AutomationProperties::SetAutomationId(key, L"ServiceKey");

            preset.Header(box_value(L"快速选择"));
            preset.PlaceholderText(L"选择预设会替换以下服务信息");
            for (const auto& item : presets) preset.Items().Append(box_value(item.name));
            protocol.Header(box_value(L"接口协议"));
            for (const auto item : protocolLabels) protocol.Items().Append(box_value(item));
            protocol.SelectedIndex(indexOf(protocols, original.protocol));
            name.Header(box_value(L"服务名称"));
            name.MaxLength(60);
            name.PlaceholderText(L"例如：我的本地模型");
            name.Text(original.name);
            address.Header(box_value(L"服务地址"));
            address.MaxLength(2048);
            address.PlaceholderText(L"https://api.example.com/v1");
            address.Text(original.baseUrl);
            model.Header(box_value(L"模型名称"));
            model.MaxLength(200);
            model.PlaceholderText(L"输入服务端提供的模型 ID");
            model.Text(original.model);
            key.Header(box_value(L"API Key"));
            key.MaxLength(4096);
            key.PasswordRevealMode(PasswordRevealMode::Hidden);
            key.PlaceholderText(unreadable ? L"无法解密，请输入新的 API Key" :
                hasStoredKey ? L"留空保留已加密保存的 API Key" : L"本地服务通常可留空");
            revealKey.Content(box_value(L"显示"));
            revealKey.VerticalAlignment(VerticalAlignment::Bottom);
            AutomationProperties::SetName(revealKey, L"显示或隐藏 API Key");
            clearKey.Content(box_value(L"清除已保存的 API Key"));
            clearKey.Visibility(hasStoredKey ? Visibility::Visible : Visibility::Collapsed);
            reasoning.Header(box_value(L"思考等级（可选）"));
            for (const auto item : reasoningLabels) reasoning.Items().Append(box_value(item));
            reasoning.SelectedIndex(indexOf(reasoningValues, original.reasoningEffort));
            dictionary.Header(box_value(L"词典内容"));
            dictionary.Items().Append(box_value(L"简明词条（默认）"));
            dictionary.Items().Append(box_value(L"完整词条"));
            dictionary.SelectedIndex(original.dictionaryMode == L"detailed" ? 1 : 0);
            for (auto const& combo : {preset, protocol, reasoning, dictionary}) {
                combo.Style(style(L"YiyiSelect"));
                combo.HorizontalAlignment(HorizontalAlignment::Stretch);
            }
            for (auto const& hint : {endpointHint, reasoningHint, dictionaryHint}) {
                hint.Style(style(L"YiyiCaption"));
                hint.TextWrapping(TextWrapping::Wrap);
            }

            Grid nameAndProtocol;
            nameAndProtocol.ColumnSpacing(10);
            column(nameAndProtocol, GridUnitType::Star);
            column(nameAndProtocol, GridUnitType::Star);
            place(nameAndProtocol, name, 0);
            place(nameAndProtocol, protocol, 1);
            Grid credential;
            credential.ColumnSpacing(8);
            column(credential, GridUnitType::Star);
            column(credential, GridUnitType::Auto);
            place(credential, key, 0);
            place(credential, revealKey, 1);
            reasoningOptions.Spacing(5);
            reasoningOptions.Children().Append(reasoning);
            reasoningOptions.Children().Append(reasoningHint);

            StackPanel fields;
            fields.Spacing(8);
            fields.Padding({0, 0, 8, 0});
            fields.Children().Append(preset);
            fields.Children().Append(nameAndProtocol);
            fields.Children().Append(address);
            fields.Children().Append(endpointHint);
            fields.Children().Append(credential);
            fields.Children().Append(clearKey);
            fields.Children().Append(model);
            fields.Children().Append(reasoningOptions);
            fields.Children().Append(dictionary);
            fields.Children().Append(dictionaryHint);
            fields.Children().Append(label(L"连接测试会发送简短文本。识图测试会发送随机文字图片。", L"YiyiCaption"));

            ScrollViewer scroller;
            scroller.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            scroller.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroller.MaxHeight(std::clamp(root.Size().Height - 245.0, 180.0, 460.0));
            scroller.Content(fields);
            StackPanel testBar;
            testBar.Orientation(Orientation::Horizontal);
            testBar.Spacing(8);
            testBar.Children().Append(textTest);
            testBar.Children().Append(visionTest);
            status.IsClosable(false);
            StackPanel content;
            content.Spacing(8);
            content.Children().Append(scroller);
            content.Children().Append(testBar);
            content.Children().Append(status);
            dialog.Content(content);
            updateHints();
        }

        void updateHints()
        {
            const int type = std::clamp(protocol.SelectedIndex(), 0, static_cast<int>(std::size(protocols)) - 1);
            endpointHint.Text(endpointHints[type]);
            reasoningOptions.Visibility(type <= 1 ? Visibility::Visible : Visibility::Collapsed);
            if (type > 1) reasoning.SelectedIndex(0);
            if (reasoning.SelectedIndex() == 0) reasoningHint.Text(L"跟随服务时不发送思考等级参数。");
            else if (reasoning.SelectedIndex() == 1) reasoningHint.Text(L"关闭思考也需要模型支持。");
            else reasoningHint.Text(L"支持的思考等级因模型而异，可通过测试连接检查。");
            dictionaryHint.Text(dictionary.SelectedIndex() == 1
                ? L"生成释义、音标、例句和搭配。"
                : L"生成词性和常见译义，适合本地小模型。");
        }

        void applyPreset(int index)
        {
            if (index < 0 || index >= static_cast<int>(std::size(presets))) return;
            const auto& choice = presets[index];
            name.Text(choice.name);
            protocol.SelectedIndex(choice.protocol);
            address.Text(choice.address);
            model.Text(choice.model);
            key.Password(L"");
            clearKey.IsChecked(box_value(hasStoredKey).as<IReference<bool>>());
            reasoning.SelectedIndex(0);
            dictionary.SelectedIndex(0);
            status.IsOpen(false);
            updateHints();
        }

        bool clearingKey() const
        {
            const auto clear = clearKey.IsChecked();
            return clear && clear.Value();
        }

        ::yiyi::Provider draft() const
        {
            auto provider = original;
            provider.name = trim(name.Text());
            provider.protocol = hstring(protocols[std::clamp(protocol.SelectedIndex(), 0, 4)]);
            provider.baseUrl = trim(address.Text());
            provider.model = trim(model.Text());
            const auto replacement = trim(key.Password());
            if (!replacement.empty()) provider.apiKey = replacement;
            else if (clearingKey()) provider.apiKey.clear();
            else if (hasStoredKey && original.apiKey.empty())
                throw hresult_invalid_argument(L"已保存的 API Key 无法解密，请替换或清除。");
            provider.reasoningEffort = protocol.SelectedIndex() <= 1
                ? hstring(reasoningValues[std::clamp(reasoning.SelectedIndex(), 0, 6)]) : hstring{};
            provider.dictionaryMode = dictionary.SelectedIndex() == 1 ? L"detailed" : L"brief";
            if (provider.name.empty()) throw hresult_invalid_argument(L"请填写服务名称。");
            ::yiyi::validateProvider(provider);
            return provider;
        }

        void display(hstring const& text, InfoBarSeverity severity)
        {
            status.Message(text);
            status.Severity(severity);
            status.IsOpen(!text.empty());
        }

        void busy(bool running)
        {
            testing = running;
            for (auto const& field : {preset.as<Control>(), protocol.as<Control>(), name.as<Control>(),
                 address.as<Control>(), model.as<Control>(), key.as<Control>(), clearKey.as<Control>(),
                 revealKey.as<Control>(), reasoning.as<Control>(), dictionary.as<Control>()})
                field.IsEnabled(!running);
            dialog.IsPrimaryButtonEnabled(!running);
            textTest.Content(box_value(running ? L"取消测试" : L"测试连接"));
            visionTest.IsEnabled(!running);
        }
    };

    void MainWindow::refreshProviders()
    {
        const auto wasUpdating = m_updating;
        m_updating = true;
        auto combo = ActiveProvider();
        const auto services = combo.Items();
        const auto& providers = m_settings.providers;
        const bool rebuildCombo = services.Size() != providers.size() ||
            !std::ranges::equal(services, providers, {}, [](auto const& item) { return unbox_value<hstring>(item); },
                &::yiyi::Provider::name);
        if (rebuildCombo) services.Clear();
        int active = -1;
        for (size_t index = 0; index < providers.size(); ++index) {
            const auto& item = providers[index];
            if (rebuildCombo) services.Append(box_value(item.name));
            if (item.id == m_settings.selectedProviderId) active = static_cast<int>(index);
        }
        combo.SelectedIndex(active);
        m_updating = wasUpdating;
        refreshProviderRows();
        updateWorkbenchChrome();
    }

    void MainWindow::refreshProviderRows()
    {
        if (SettingsPage().Visibility() != Visibility::Visible) return;
        auto rows = SettingsProviderRows().Children();
        rows.Clear();
        const auto weak = get_weak();
        for (const auto& item : m_settings.providers) {
            const bool selected = item.id == m_settings.selectedProviderId;
            Grid grid;
            grid.ColumnSpacing(8);
            column(grid, GridUnitType::Auto);
            column(grid, GridUnitType::Star);
            column(grid, GridUnitType::Auto);
            column(grid, GridUnitType::Auto);
            column(grid, GridUnitType::Auto);
            FontIcon icon;
            const std::wstring_view address(item.baseUrl);
            const bool local = item.protocol == L"ollama" || address.find(L"localhost") != address.npos ||
                address.find(L"127.0.0.1") != address.npos || address.find(L"[::1]") != address.npos;
            icon.Glyph(local ? L"\uE7F4" : L"\uE753");
            icon.FontSize(19);
            icon.Width(30);
            place(grid, icon, 0);
            StackPanel description;
            description.Spacing(3);
            description.VerticalAlignment(VerticalAlignment::Center);
            auto name = label(item.name);
            name.FontSize(14);
            name.FontWeight(Windows::UI::Text::FontWeight{600});
            name.TextWrapping(TextWrapping::NoWrap);
            name.TextTrimming(TextTrimming::CharacterEllipsis);
            description.Children().Append(name);
            auto detail = label(item.model + L" · " + item.baseUrl, L"YiyiCaption");
            detail.TextWrapping(TextWrapping::NoWrap);
            detail.TextTrimming(TextTrimming::CharacterEllipsis);
            description.Children().Append(detail);
            place(grid, description, 1);

            if (selected) {
                auto current = label(L"当前使用", L"YiyiDictionaryAccent");
                current.VerticalAlignment(VerticalAlignment::Center);
                place(grid, current, 2);
            } else {
                auto use = button(L"使用");
                AutomationProperties::SetName(use, L"使用 " + item.name);
                use.Click([weak, id = item.id](auto const&, auto const&) {
                    if (const auto self = weak.get()) self->selectProvider(id);
                });
                place(grid, use, 2);
            }
            auto edit = button(L"编辑");
            edit.Style(style(L"YiyiGhostButton"));
            AutomationProperties::SetName(edit, L"编辑 " + item.name);
            edit.Click([weak, id = item.id](auto const&, auto const&) {
                if (const auto self = weak.get()) self->showProviderEditor(id, 0);
            });
            place(grid, edit, 3);
            auto remove = button(L"移除");
            remove.Style(style(L"YiyiGhostButton"));
            AutomationProperties::SetName(remove, L"移除 " + item.name);
            remove.Click([weak, id = item.id](auto const&, auto const&) {
                if (const auto self = weak.get()) self->removeProvider(id);
            });
            place(grid, remove, 4);
            Border card;
            card.Style(style(L"YiyiCard"));
            card.Padding({12, 8, 9, 8});
            card.Child(grid);
            rows.Append(card);
        }
        SettingsProviderCount().Text(hstring(std::to_wstring(m_settings.providers.size()) + L" 个配置"));
        SettingsProviderEmpty().Visibility(m_settings.providers.empty() ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::selectProvider(hstring const& id)
    {
        if (m_busy || id == m_settings.selectedProviderId) return;
        try {
            auto next = m_settings;
            next.selectedProviderId = id;
            applySettings(std::move(next));
            refreshProviders();
        } catch (...) {
            refreshProviders();
            reportError(L"无法保存 AI 服务选择。");
        }
    }

    void MainWindow::ActiveProviderChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        const int index = ActiveProvider().SelectedIndex();
        if (!m_updating && index >= 0 && static_cast<size_t>(index) < m_settings.providers.size())
            selectProvider(m_settings.providers[index].id);
    }

    void MainWindow::ApplySidebarTheme(IInspectable const& sender, RoutedEventArgs const&)
    {
        const auto control = sender.try_as<Button>();
        if (!control || !control.Tag()) return;
        const auto theme = unbox_value<hstring>(control.Tag());
        if (theme == m_settings.theme) return;
        try {
            auto next = m_settings;
            next.theme = theme;
            applySettings(std::move(next));
            updateTheme();
        } catch (...) {
            reportError(L"无法保存偏好设置。");
        }
    }

    void MainWindow::NewProvider(IInspectable const& sender, RoutedEventArgs const&)
    {
        const auto control = sender.try_as<Button>();
        int index = 0;
        if (control && control.Tag()) index = static_cast<int>(unbox_value<hstring>(control.Tag())[0] - L'0');
        showProviderEditor({}, index);
    }

    fire_and_forget MainWindow::showProviderEditor(hstring id, int preset)
    {
        if (m_closed || m_busy) co_return;
        const auto lifetime = get_strong();
        try {
            ::yiyi::Provider original;
            if (!id.empty()) {
                const auto found = std::ranges::find(m_settings.providers, id, &::yiyi::Provider::id);
                if (found == m_settings.providers.end()) co_return;
                original = *found;
            }
            const auto editor = std::make_shared<ProviderEditor>(Workspace().XamlRoot(), Workspace().RequestedTheme(),
                std::move(original), m_settings.unreadableCredentials.contains(id));
            const std::weak_ptr<ProviderEditor> form = editor;
            const auto weak = get_weak();
            if (id.empty()) {
                editor->preset.SelectedIndex(preset);
                editor->applyPreset(preset);
            }
            editor->preset.SelectionChanged([form](auto const&, auto const&) {
                if (const auto current = form.lock()) current->applyPreset(current->preset.SelectedIndex());
            });
            const auto changed = [form](auto const&, auto const&) {
                if (const auto current = form.lock(); current && !current->testing) current->status.IsOpen(false);
            };
            for (auto const& field : {editor->name, editor->address, editor->model}) field.TextChanged(changed);
            editor->key.PasswordChanged(changed);
            editor->clearKey.Checked(changed);
            editor->clearKey.Unchecked(changed);
            editor->revealKey.Click([form](auto const&, auto const&) {
                if (const auto current = form.lock()) {
                    const auto checked = current->revealKey.IsChecked();
                    const bool reveal = checked && checked.Value();
                    current->key.PasswordRevealMode(reveal ? PasswordRevealMode::Visible : PasswordRevealMode::Hidden);
                    current->revealKey.Content(box_value(reveal ? L"隐藏" : L"显示"));
                }
            });
            for (auto const& choice : {editor->protocol, editor->reasoning, editor->dictionary}) {
                choice.SelectionChanged([form, changed](auto const& sender, auto const& args) {
                    if (const auto current = form.lock()) current->updateHints();
                    changed(sender, args);
                });
            }
            editor->textTest.Click([weak, form](auto const&, auto const&) {
                const auto self = weak.get();
                const auto current = form.lock();
                if (!self || !current) return;
                if (current->testing) self->Cancel(nullptr, nullptr);
                else self->probeProvider(current, false);
            });
            editor->visionTest.Click([weak, form](auto const&, auto const&) {
                const auto self = weak.get();
                if (const auto current = form.lock(); self && current) self->probeProvider(current, true);
            });
            editor->dialog.PrimaryButtonClick([weak, form](auto const&, ContentDialogButtonClickEventArgs const& args) {
                const auto self = weak.get();
                const auto current = form.lock();
                if (!self || !current || self->m_closed || current->testing) {
                    args.Cancel(true);
                    return;
                }
                try {
                    auto provider = current->draft();
                    auto next = self->m_settings;
                    const auto found = std::ranges::find(next.providers, provider.id, &::yiyi::Provider::id);
                    if (provider.id.empty()) {
                        GUID newId{};
                        check_hresult(CoCreateGuid(&newId));
                        provider.id = to_hstring(guid(newId));
                        next.providers.push_back(provider);
                    } else {
                        if (found == next.providers.end()) throw hresult_invalid_argument(L"该服务已被移除。");
                        *found = provider;
                    }
                    if (!trim(current->key.Password()).empty() || current->clearingKey())
                        next.unreadableCredentials.erase(provider.id);
                    if (next.selectedProviderId.empty()) next.selectedProviderId = provider.id;
                    self->applySettings(std::move(next));
                    self->refreshProviders();
                    self->notice(L"AI 服务已保存。", InfoBarSeverity::Success);
                } catch (...) {
                    args.Cancel(true);
                    current->display(to_message(), InfoBarSeverity::Error);
                }
            });
            co_await editor->dialog.ShowAsync();
            editor->closed = true;
            if (editor->testing) Cancel(nullptr, nullptr);
            editor->key.Password(L"");
        } catch (...) {
            if (!m_closed) reportError(L"无法打开 AI 服务配置。");
        }
    }

    fire_and_forget MainWindow::removeProvider(hstring id)
    {
        if (m_closed || m_busy) co_return;
        const auto lifetime = get_strong();
        try {
            const auto found = std::ranges::find(m_settings.providers, id, &::yiyi::Provider::id);
            if (found == m_settings.providers.end()) co_return;
            ContentDialog dialog;
            dialog.XamlRoot(Workspace().XamlRoot());
            dialog.RequestedTheme(Workspace().RequestedTheme());
            dialog.Title(box_value(L"移除「" + found->name + L"」？"));
            dialog.Content(label(L"将删除此服务的配置和保存的凭据。已有历史记录会保留。"));
            dialog.PrimaryButtonText(L"移除服务");
            dialog.CloseButtonText(L"取消");
            dialog.DefaultButton(ContentDialogButton::Close);
            if (co_await dialog.ShowAsync() != ContentDialogResult::Primary || m_closed || m_busy) co_return;
            auto next = m_settings;
            std::erase_if(next.providers, [&](auto const& provider) { return provider.id == id; });
            next.unreadableCredentials.erase(id);
            if (next.selectedProviderId == id)
                next.selectedProviderId = next.providers.empty() ? hstring{} : next.providers.front().id;
            if (next.visionProviderId == id) next.visionProviderId.clear();
            applySettings(std::move(next));
            refreshProviders();
            notice(L"AI 服务已移除。");
        } catch (...) {
            if (!m_closed) reportError(L"无法移除 AI 服务。");
        }
    }

    fire_and_forget MainWindow::probeProvider(std::shared_ptr<ProviderEditor> editor, bool vision)
    {
        if (m_closed || m_busy || editor->closed || editor->testing) co_return;
        if (documentsBusy() || pdfRequestBusy() || m_screenshot) {
            editor->display(L"请等待当前文档、截图或翻译操作完成。", InfoBarSeverity::Warning);
            co_return;
        }
        const auto lifetime = get_strong();
        ::yiyi::Provider provider;
        try {
            provider = editor->draft();
        } catch (...) {
            editor->display(to_message(), InfoBarSeverity::Error);
            co_return;
        }
        editor->busy(true);
        setBusy(true, true);
        editor->display(vision ? L"正在发送随机文字测试图片…" : L"正在发送连接测试请求…", InfoBarSeverity::Informational);
        const auto serial = ++m_requestSerial;
        try {
            hstring expected;
            if (vision) {
                expected = probeCode();
                m_request = ::yiyi::transcribeImageAsync(std::move(provider), probeImage(expected), {}, std::chrono::minutes(1));
            } else {
                m_request = ::yiyi::translateAsync(std::move(provider), L"Reply with exactly OK. Do not explain.",
                    L"Connection test.", {}, true, std::chrono::minutes(1));
            }
            const auto result = co_await m_request;
            if (serial != m_requestSerial) throw hresult_canceled();
            if (!m_closed && !editor->closed) {
                const bool matches = !vision || trim(result) == expected;
                editor->display(matches ? (vision ? L"识图测试成功，模型正确读取了图片文字。" : L"连接成功，模型已返回响应。")
                    : L"模型已响应，但未正确读取测试图片中的文字。",
                    matches ? InfoBarSeverity::Success : InfoBarSeverity::Error);
            }
        } catch (hresult_canceled const&) {
            if (!m_closed && !editor->closed) editor->display(L"已取消测试。", InfoBarSeverity::Informational);
        } catch (...) {
            if (!m_closed && !editor->closed) editor->display(to_message(), InfoBarSeverity::Error);
        }
        m_request = nullptr;
        if (!m_closed) setBusy(false);
        if (!m_closed && !editor->closed) editor->busy(false);
    }
}

