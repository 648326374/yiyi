#include "desktopintegration.h"
#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QIcon>
#include <QKeySequence>
#include <QMenu>
#include <QScreen>
#include <QSessionManager>
#include <QSignalBlocker>
#include <QSystemTrayIcon>
#include <QWindow>
#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace {
DesktopIntegration *hookOwner = nullptr;
bool ownWindow(HWND window) {
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId();
}
LRESULT CALLBACK mouseHook(int code, WPARAM event, LPARAM data) {
    if (code == HC_ACTION && hookOwner
        && (event == WM_LBUTTONDOWN || event == WM_LBUTTONUP || event == WM_RBUTTONDOWN
            || event == WM_MOUSEWHEEL || event == WM_MOUSEHWHEEL)) {
        const auto *mouse = reinterpret_cast<MSLLHOOKSTRUCT *>(data);
        const QPoint point(mouse->pt.x, mouse->pt.y);
        QMetaObject::invokeMethod(hookOwner, [owner = hookOwner, event, point] {
            owner->nativeMouseEvent(int(event), point);
        }, Qt::QueuedConnection);
    }
    return CallNextHookEx(nullptr, code, event, data);
}
UINT nativeModifiers(Qt::KeyboardModifiers modifiers) {
    return MOD_NOREPEAT | (modifiers.testFlag(Qt::ControlModifier) ? MOD_CONTROL : 0)
        | (modifiers.testFlag(Qt::AltModifier) ? MOD_ALT : 0)
        | (modifiers.testFlag(Qt::ShiftModifier) ? MOD_SHIFT : 0)
        | (modifiers.testFlag(Qt::MetaModifier) ? MOD_WIN : 0);
}
UINT nativeKey(Qt::Key key) {
    return key >= Qt::Key_F1 && key <= Qt::Key_F24 ? UINT(VK_F1 + key - Qt::Key_F1) : UINT(key);
}
}
#endif

DesktopIntegration::DesktopIntegration(const QString &directory, bool nativeEnabled, QObject *parent)
    : QObject(parent), m_settings(QDir(directory).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat),
      m_capture(this), m_nativeEnabled(nativeEnabled) {
    m_closeToTray = m_settings.value(QStringLiteral("desktop/closeToTray"), true).toBool();
    m_shortcutsEnabled = m_settings.value(QStringLiteral("desktop/shortcutsEnabled"), true).toBool();
    m_selectionEnabled = m_settings.value(QStringLiteral("desktop/selectionEnabled"), true).toBool();
    m_showShortcut = m_settings.value(QStringLiteral("desktop/showShortcut"), QStringLiteral("Ctrl+Alt+Y")).toString();
    m_selectionShortcut = m_settings.value(QStringLiteral("desktop/selectionShortcut"), QStringLiteral("Ctrl+Alt+D")).toString();
    m_selectionDelay.setSingleShot(true); m_selectionDelay.setInterval(180);
    m_bubbleTimer.setSingleShot(true); m_bubbleTimer.setInterval(8000);
    m_foregroundTimer.setInterval(150);
    connect(&m_selectionDelay, &QTimer::timeout, this, [this] {
        if (m_selectionEnabled) m_capture.request(m_source, m_releasePoint, true);
    });
    connect(&m_bubbleTimer, &QTimer::timeout, this, &DesktopIntegration::dismissSelection);
    connect(&m_foregroundTimer, &QTimer::timeout, this, [this] {
#ifdef Q_OS_WIN
        if (reinterpret_cast<quintptr>(GetForegroundWindow()) != m_bubbleSource || (GetAsyncKeyState(VK_ESCAPE) & 0x8000))
            dismissSelection();
#endif
    });
    connect(&m_capture, &SelectionCapture::captured, this, &DesktopIntegration::deliverSelection);
    connect(&m_capture, &SelectionCapture::failed, this, [this](const QString &message, bool automatic) {
        if (automatic) return;
        showWindow(); emit toast(message);
    });
}

DesktopIntegration::~DesktopIntegration() {
    m_exiting = true;
    m_capture.cancel();
#ifdef Q_OS_WIN
    if (m_hook) UnhookWindowsHookEx(reinterpret_cast<HHOOK>(m_hook));
    if (hookOwner == this) hookOwner = nullptr;
    for (auto it = m_hotkeys.cbegin(); it != m_hotkeys.cend(); ++it) UnregisterHotKey(reinterpret_cast<HWND>(m_hotkeyWindow->winId()), it.key());
#endif
    if (qApp) qApp->removeNativeEventFilter(this);
    if (m_tray) m_tray->hide();
}

bool DesktopIntegration::available() const {
#ifdef Q_OS_WIN
    return m_nativeEnabled && QGuiApplication::platformName() == QLatin1String("windows");
#else
    return false;
#endif
}

bool DesktopIntegration::trayAvailable() const {
    return m_tray && m_tray->isVisible() && QSystemTrayIcon::isSystemTrayAvailable();
}

void DesktopIntegration::setWindow(QWindow *window) {
    m_window = window;
    if (!window) return;
    connect(window, &QWindow::visibilityChanged, this, [this](QWindow::Visibility state) {
        if (state == QWindow::Maximized) m_wasMaximized = true;
        else if (state == QWindow::Windowed) m_wasMaximized = false;
    });
}

void DesktopIntegration::start() {
    if (m_started) return;
    m_started = true;
    if (!available()) return;
    m_hotkeyWindow = std::make_unique<QWindow>();
    m_hotkeyWindow->setFlags(Qt::Tool | Qt::FramelessWindowHint);
    m_hotkeyWindow->create();
    qApp->installNativeEventFilter(this);
    connect(qApp, &QGuiApplication::commitDataRequest, this, [this](QSessionManager &) { m_exiting = true; });
    m_menu = std::make_unique<QMenu>();
    m_menu->addAction(QStringLiteral("打开译意"), this, &DesktopIntegration::showWindow);
    m_menu->addAction(QStringLiteral("截图取字"), this, [this] { showWindow(); emit screenshotRequested(); });
    m_menu->addAction(QStringLiteral("偏好设置"), this, [this] { showWindow(); emit settingsRequested(); });
    m_menu->addSeparator();
    m_selectionAction = m_menu->addAction(QStringLiteral("划词浮标"));
    m_selectionAction->setCheckable(true); m_selectionAction->setChecked(m_selectionEnabled);
    connect(m_selectionAction, &QAction::toggled, this, &DesktopIntegration::setSelectionEnabled);
    m_menu->addSeparator();
    m_menu->addAction(QStringLiteral("退出译意"), this, &DesktopIntegration::quit);
    m_tray = std::make_unique<QSystemTrayIcon>(QIcon(QStringLiteral(":/assets/logo.svg")));
    m_tray->setToolTip(QStringLiteral("译意 · AI 翻译词典"));
    m_tray->setContextMenu(m_menu.get());
    connect(m_tray.get(), &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) showWindow();
    });
    m_tray->show();
    registerShortcuts(m_shortcutsEnabled, m_showShortcut, m_selectionShortcut);
    updateSelectionHook();
    emit statusChanged();
}

void DesktopIntegration::save(const QString &key, const QVariant &value) {
    m_settings.setValue(QStringLiteral("desktop/") + key, value); m_settings.sync();
    if (m_settings.status() != QSettings::NoError) emit toast(QStringLiteral("偏好设置未能写入磁盘，请检查数据目录权限。"));
}

void DesktopIntegration::setCloseToTray(bool enabled) {
    if (m_closeToTray == enabled) return;
    m_closeToTray = enabled; save(QStringLiteral("closeToTray"), enabled); emit preferencesChanged();
}

void DesktopIntegration::setSelectionEnabled(bool enabled) {
    if (m_selectionEnabled == enabled) return;
    m_selectionEnabled = enabled;
    save(QStringLiteral("selectionEnabled"), enabled);
    if (m_selectionAction) { QSignalBlocker block(m_selectionAction); m_selectionAction->setChecked(enabled); }
    updateSelectionHook(); emit preferencesChanged();
}

void DesktopIntegration::updateSelectionHook() {
    m_selectionError.clear();
    m_selectionDelay.stop(); m_capture.cancel(); dismissSelection(); m_pressed = false;
#ifdef Q_OS_WIN
    if (m_hook) { UnhookWindowsHookEx(reinterpret_cast<HHOOK>(m_hook)); m_hook = 0; }
    if (hookOwner == this) hookOwner = nullptr;
    if (m_started && available() && m_selectionEnabled) {
        hookOwner = this;
        m_hook = reinterpret_cast<quintptr>(SetWindowsHookExW(WH_MOUSE_LL, mouseHook, GetModuleHandleW(nullptr), 0));
        if (!m_hook) { hookOwner = nullptr; m_selectionError = QStringLiteral("划词监听未能启动，可使用全局取词快捷键。关闭再打开此开关可重试。"); }
    }
#endif
    emit statusChanged();
}

QString DesktopIntegration::normalizedShortcut(const QString &text) {
    const auto sequence = QKeySequence::fromString(text.trimmed(), QKeySequence::PortableText);
    if (sequence.count() != 1) return {};
    const auto combination = sequence[0];
    const auto modifiers = combination.keyboardModifiers(); const auto key = combination.key();
    if (!(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) return {};
    if (modifiers & ~(Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier)) return {};
    if (!((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9)
        || (key >= Qt::Key_F1 && key <= Qt::Key_F24 && key != Qt::Key_F12))) return {};
    return sequence.toString(QKeySequence::PortableText);
}

QString DesktopIntegration::shortcutFromKey(int key, int modifiers) const {
    return normalizedShortcut(QKeySequence(QKeyCombination(Qt::KeyboardModifiers(modifiers), Qt::Key(key))).toString(QKeySequence::PortableText));
}

bool DesktopIntegration::registerShortcuts(bool enabled, const QString &show, const QString &selection) {
    m_shortcutError.clear();
    const auto first = normalizedShortcut(show), second = normalizedShortcut(selection);
    if (enabled && (first.isEmpty() || second.isEmpty() || first == second)) {
        m_shortcutError = QStringLiteral("请设置两个不同的组合键：Ctrl、Alt 或 Win 配合字母、数字或 F1–F24（F12 除外）。");
        emit statusChanged(); return false;
    }
#ifdef Q_OS_WIN
    if (available() && m_started) {
        QMap<int, QString> desired;
        const auto hotkeyWindow = reinterpret_cast<HWND>(m_hotkeyWindow->winId());
        QList<int> added;
        if (enabled) for (const auto &sequence : {first, second}) {
            int id = m_hotkeys.key(sequence, 0);
            if (!id) {
                id = 0x5900;
                while (m_hotkeys.contains(id) || desired.contains(id)) ++id;
                const auto key = QKeySequence::fromString(sequence, QKeySequence::PortableText)[0];
                if (!RegisterHotKey(hotkeyWindow, id, nativeModifiers(key.keyboardModifiers()), nativeKey(key.key()))) {
                    for (int newId : added) UnregisterHotKey(hotkeyWindow, newId);
                    m_shortcutError = QStringLiteral("%1 已被占用或被系统保留，请换一个组合键。").arg(sequence)
                        + (m_hotkeys.isEmpty() ? QStringLiteral("当前未启用全局快捷键。") : QStringLiteral("原有快捷键保持有效。"));
                    emit statusChanged(); return false;
                }
                added.append(id);
            }
            desired.insert(id, sequence);
        }
        for (auto it = m_hotkeys.cbegin(); it != m_hotkeys.cend(); ++it)
            if (!desired.contains(it.key())) UnregisterHotKey(hotkeyWindow, it.key());
        m_hotkeys = desired;
        m_showId = desired.key(first, 0); m_selectionId = desired.key(second, 0);
    }
#endif
    emit statusChanged(); return true;
}

bool DesktopIntegration::applyShortcuts(bool enabled, const QString &show, const QString &selection) {
    if (!registerShortcuts(enabled, show, selection)) return false;
    m_shortcutsEnabled = enabled;
    if (!normalizedShortcut(show).isEmpty()) m_showShortcut = normalizedShortcut(show);
    if (!normalizedShortcut(selection).isEmpty()) m_selectionShortcut = normalizedShortcut(selection);
    save(QStringLiteral("shortcutsEnabled"), enabled);
    save(QStringLiteral("showShortcut"), m_showShortcut); save(QStringLiteral("selectionShortcut"), m_selectionShortcut);
    emit preferencesChanged(); return true;
}

bool DesktopIntegration::nativeEventFilter(const QByteArray &, void *message, qintptr *result) {
#ifdef Q_OS_WIN
    const auto *msg = static_cast<MSG *>(message);
    if (msg->message == WM_HOTKEY && m_hotkeyWindow
        && msg->hwnd == reinterpret_cast<HWND>(m_hotkeyWindow->winId()) && m_hotkeys.contains(int(msg->wParam))) {
        if (m_recordingShortcut) emit shortcutRecorded(m_hotkeys.value(int(msg->wParam)));
        else {
            if (int(msg->wParam) == m_showId) { showWindow(); emit workspaceRequested(); }
            else if (int(msg->wParam) == m_selectionId) captureSelection();
        }
        if (result) *result = 0;
        return true;
    }
#endif
    return false;
}

bool DesktopIntegration::handleClose() {
    if (m_exiting || !m_closeToTray || !trayAvailable() || !m_window) return false;
    dismissSelection(); m_window->hide();
    if (!m_closeNoticeShown) {
        m_closeNoticeShown = true;
        m_tray->showMessage(QStringLiteral("译意仍在运行"), QStringLiteral("点击托盘图标可打开窗口；右键托盘菜单可退出。"), QSystemTrayIcon::Information, 2500);
    }
    return true;
}

void DesktopIntegration::showWindow() {
    dismissSelection();
    if (!m_window) return;
    if (!m_window->isVisible() || m_window->windowState() == Qt::WindowMinimized) {
        if (m_wasMaximized) m_window->showMaximized(); else m_window->showNormal();
    }
    m_window->raise(); m_window->requestActivate();
#ifdef Q_OS_WIN
    if (available()) SetForegroundWindow(reinterpret_cast<HWND>(m_window->winId()));
#endif
}

void DesktopIntegration::quit() {
    m_exiting = true; m_selectionDelay.stop(); m_capture.cancel(); dismissSelection();
    if (m_tray) m_tray->hide();
    QCoreApplication::quit();
}

void DesktopIntegration::captureSelection() {
    if (m_capture.busy()) return;
    m_selectionDelay.stop(); dismissSelection();
#ifdef Q_OS_WIN
    if (!available()) return;
    const auto source = GetForegroundWindow();
    if (!source || ownWindow(source)) { emit toast(QStringLiteral("请先在其他应用中选中文字，再按取词快捷键。")); return; }
    POINT point{}; GetCursorPos(&point);
    m_capture.request(reinterpret_cast<quintptr>(source), QPoint(point.x, point.y), false);
#endif
}

void DesktopIntegration::nativeMouseEvent(int event, const QPoint &point) {
#ifdef Q_OS_WIN
    if (!m_selectionEnabled || !m_hook) return;
    const HWND underPointer = WindowFromPoint(POINT{point.x(), point.y()});
    if (ownWindow(underPointer)) return; // Let the floating query button receive its click.
    if (event != WM_LBUTTONUP) {
        dismissSelection(); m_selectionDelay.stop(); m_capture.cancel();
    }
    if (event == WM_LBUTTONDOWN) {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        m_doubleClick = now - m_lastPressTime <= GetDoubleClickTime()
            && (point - m_lastPressPoint).manhattanLength() <= 8;
        m_lastPressTime = now; m_lastPressPoint = point;
        m_pressed = true; m_pressPoint = point;
        m_source = reinterpret_cast<quintptr>(GetAncestor(underPointer, GA_ROOT));
    } else if (event == WM_LBUTTONUP && m_pressed) {
        m_pressed = false;
        if ((point - m_pressPoint).manhattanLength() < 6 && !m_doubleClick) return;
        if (m_source != reinterpret_cast<quintptr>(GetForegroundWindow())) return;
        m_releasePoint = point; m_bubblePosition = QCursor::pos() + QPoint(12, 18);
        m_selectionDelay.start();
    } else if (event == WM_RBUTTONDOWN || event == WM_MOUSEWHEEL || event == WM_MOUSEHWHEEL) {
        m_pressed = false;
    }
#endif
}

void DesktopIntegration::deliverSelection(const QString &text, bool automatic, quintptr source) {
    if (!automatic) { showWindow(); emit lookupRequested(text); return; }
    if (!m_selectionEnabled) return;
    m_selection = text; m_bubbleSource = source;
    auto *screen = QGuiApplication::screenAt(m_bubblePosition);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const auto area = screen->availableGeometry();
        m_bubblePosition.setX(qBound(area.left() + 4, m_bubblePosition.x(), area.right() - 116));
        m_bubblePosition.setY(qBound(area.top() + 4, m_bubblePosition.y(), area.bottom() - 48));
    }
    emit selectionChanged(); m_bubbleTimer.start(); m_foregroundTimer.start();
}

void DesktopIntegration::acceptSelection() {
    const auto text = m_selection;
    dismissSelection();
    if (text.isEmpty()) return;
    showWindow(); emit lookupRequested(text);
}

void DesktopIntegration::dismissSelection() {
    m_bubbleTimer.stop(); m_foregroundTimer.stop();
    if (m_selection.isEmpty()) return;
    m_selection.clear(); m_bubbleSource = 0; emit selectionChanged();
}
