#include "pch.h"
#include "MainWindow.xaml.h"

#include <winrt/Microsoft.UI.Xaml.Input.h>

namespace winrt::Yiyi::WinUI::implementation
{
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;

    namespace
    {
        bool active(TextBox const& field)
        {
            return field.FocusState() != FocusState::Unfocused;
        }

        std::wstring shortcutFromKey(UINT key)
        {
            std::wstring shortcut;
            if (GetKeyState(VK_CONTROL) & 0x8000) shortcut += L"Ctrl+";
            if (GetKeyState(VK_MENU) & 0x8000) shortcut += L"Alt+";
            if (GetKeyState(VK_SHIFT) & 0x8000) shortcut += L"Shift+";
            if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) shortcut += L"Meta+";
            if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) shortcut += static_cast<wchar_t>(key);
            else if (key >= VK_F1 && key <= VK_F24) shortcut += L"F" + std::to_wstring(key - VK_F1 + 1);
            return ::yiyi::DesktopIntegration::normalizedShortcut(shortcut);
        }

        void showError(TextBlock const& label, std::wstring const& text)
        {
            label.Text(hstring(text));
            label.Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
        }

        void updateSelectionBubbleTheme(::yiyi::DesktopIntegration& desktop, ElementTheme theme)
        {
            const auto key = theme == ElementTheme::Dark ? L"Default" : L"Light";
            const auto palette = Application::Current().Resources().ThemeDictionaries().Lookup(box_value(key)).as<ResourceDictionary>();
            const auto color = [&](hstring const& name) {
                const auto value = unbox_value<Windows::UI::Color>(palette.Lookup(box_value(name)));
                return RGB(value.R, value.G, value.B);
            };
            desktop.setSelectionBubbleColors(color(L"YiyiAccentColor"), color(L"YiyiAccentStrongColor"),
                                             color(L"YiyiAccentTextColor"));
        }
    }

    void MainWindow::StartDesktop(std::wstring channel)
    {
        if (channel.empty()) {
            DesktopOptions().Visibility(Visibility::Collapsed);
            return;
        }
        const HWND window = NativeWindowHandle();
        DesktopOptions().Visibility(Visibility::Visible);
        const auto weak = get_weak();
        ::yiyi::DesktopCallbacks callbacks;
        callbacks.workspace = [weak] { if (const auto self = weak.get()) self->ShowDesktopWorkspace(); };
        callbacks.settings = [weak] { if (const auto self = weak.get()) self->ShowDesktopSettings(); };
        callbacks.screenshot = [weak] { if (const auto self = weak.get()) self->StartScreenshot(); };
        callbacks.lookup = [weak](const std::wstring& text) {
            if (const auto self = weak.get()) self->lookupDesktopSelection(text);
        };
        callbacks.error = [weak](const std::wstring& text) {
            if (const auto self = weak.get()) self->notice(hstring(text), InfoBarSeverity::Warning);
        };
        callbacks.preferencesChanged = [weak] { if (const auto self = weak.get()) self->refreshDesktopSettings(); };
        callbacks.shortcutRecorded = [weak](const std::wstring& text) {
            if (const auto self = weak.get()) self->recordDesktopShortcut(text);
        };
        m_desktop = std::make_unique<::yiyi::DesktopIntegration>(window, m_dataDirectory, std::move(channel), std::move(callbacks));
        updateSelectionBubbleTheme(*m_desktop, Workspace().ActualTheme());
        Workspace().ActualThemeChanged([weak](FrameworkElement const& sender, IInspectable const&) {
            if (const auto self = weak.get(); self && self->m_desktop)
                updateSelectionBubbleTheme(*self->m_desktop, sender.ActualTheme());
        });
        if (!m_desktop->start()) notice(L"无法初始化桌面集成，仍可在工作台翻译。", InfoBarSeverity::Warning);
        refreshDesktopSettings();
        Closed([weak](auto const&, auto const&) { if (const auto self = weak.get()) self->m_desktop.reset(); });
    }

    void MainWindow::ShowDesktopWorkspace()
    {
        Navigation().SelectedItem(TranslateItem());
    }

    void MainWindow::ShowDesktopSettings()
    {
        Navigation().SelectedItem(SettingsItem());
    }

    void MainWindow::refreshDesktopSettings()
    {
        if (!m_desktop) return;
        m_updatingDesktop = true;
        const auto& options = m_desktop->preferences();
        DesktopCloseToTray().IsOn(options.closeToTray);
        DesktopSelectionEnabled().IsOn(options.selectionEnabled);
        DesktopShortcutsEnabled().IsOn(options.shortcutsEnabled);
        ShowShortcutField().Text(hstring(options.showShortcut));
        SelectionShortcutField().Text(hstring(options.selectionShortcut));
        ShowShortcutField().IsEnabled(options.shortcutsEnabled);
        SelectionShortcutField().IsEnabled(options.shortcutsEnabled);
        DesktopTrayStatus().Text(m_desktop->trayAvailable() ? L"托盘已就绪" : L"托盘未启用");
        showError(DesktopShortcutError(), m_desktop->shortcutError());
        showError(DesktopSelectionError(), m_desktop->selectionError());
        m_updatingDesktop = false;
    }

    void MainWindow::DesktopPreferencesChanged(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (!m_desktop || m_updatingDesktop) return;
        if (sender == DesktopCloseToTray()) m_desktop->setCloseToTray(DesktopCloseToTray().IsOn());
        else if (sender == DesktopSelectionEnabled()) m_desktop->setSelectionEnabled(DesktopSelectionEnabled().IsOn());
        else if (sender == DesktopShortcutsEnabled()) {
            ShowShortcutField().IsEnabled(DesktopShortcutsEnabled().IsOn());
            SelectionShortcutField().IsEnabled(DesktopShortcutsEnabled().IsOn());
        }
    }

    void MainWindow::ApplyDesktopShortcuts(IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_desktop) return;
        if (m_desktop->applyShortcuts(DesktopShortcutsEnabled().IsOn(), ShowShortcutField().Text(), SelectionShortcutField().Text())) {
            notice(L"快捷键已保存。", InfoBarSeverity::Success);
        } else showError(DesktopShortcutError(), m_desktop->shortcutError());
    }

    void MainWindow::ResetDesktopShortcuts(IInspectable const&, RoutedEventArgs const&)
    {
        const ::yiyi::DesktopPreferences defaults;
        DesktopShortcutsEnabled().IsOn(defaults.shortcutsEnabled);
        ShowShortcutField().Text(hstring(defaults.showShortcut));
        SelectionShortcutField().Text(hstring(defaults.selectionShortcut));
    }

    void MainWindow::recordDesktopShortcut(std::wstring const& shortcut)
    {
        if (active(ShowShortcutField())) ShowShortcutField().Text(hstring(shortcut));
        else if (active(SelectionShortcutField())) SelectionShortcutField().Text(hstring(shortcut));
        else return;
        DesktopShortcutsEnabled().Focus(FocusState::Programmatic);
    }

    void MainWindow::DesktopShortcutKeyDown(IInspectable const&, Input::KeyRoutedEventArgs const& args)
    {
        if (const auto shortcut = shortcutFromKey(static_cast<UINT>(args.Key())); !shortcut.empty()) {
            recordDesktopShortcut(shortcut);
            args.Handled(true);
        }
    }

    void MainWindow::DesktopShortcutFocusChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_desktop) m_desktop->setRecordingShortcut(active(ShowShortcutField()) || active(SelectionShortcutField()));
    }

    void MainWindow::lookupDesktopSelection(std::wstring const& text)
    {
        if (m_busy) {
            notice(L"正在生成，请先停止当前查询，再查询选中文字。", InfoBarSeverity::Warning);
            return;
        }
        ShowDesktopWorkspace();
        SourceText().Text(hstring(text));
        Translate(nullptr, nullptr);
    }
}
