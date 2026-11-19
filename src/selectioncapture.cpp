#include "selectioncapture.h"
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QRectF>
#include <cstdio>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <uiautomation.h>

namespace {
template<class T> struct ComPtr {
    T *p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T *operator->() const { return p; }
    T **out() { return &p; }
};

DWORD processOf(HWND window) {
    DWORD pid = 0;
    if (window) GetWindowThreadProcessId(window, &pid);
    return pid;
}

bool nativePassword(HWND window) {
    GUITHREADINFO info{}; info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info)) return false;
    wchar_t name[64]{};
    GetClassNameW(info.hwndFocus, name, 64);
    return QString::fromWCharArray(name).contains(QStringLiteral("edit"), Qt::CaseInsensitive)
        && (GetWindowLongPtrW(info.hwndFocus, GWL_STYLE) & ES_PASSWORD);
}

bool rangeNearPoint(IUIAutomationTextRange *range, const QPoint &point) {
    SAFEARRAY *rectangles = nullptr;
    if (FAILED(range->GetBoundingRectangles(&rectangles)) || !rectangles) return false;
    bool pointMatches = false;
    LONG first = 0, last = -1;
    double *values = nullptr;
    if (SafeArrayGetDim(rectangles) == 1
        && SUCCEEDED(SafeArrayGetLBound(rectangles, 1, &first))
        && SUCCEEDED(SafeArrayGetUBound(rectangles, 1, &last))
        && SUCCEEDED(SafeArrayAccessData(rectangles, reinterpret_cast<void **>(&values)))) {
        for (LONG i = 0; i + 3 <= last - first; i += 4) {
            const QRectF rect(values[i], values[i + 1], values[i + 2], values[i + 3]);
            if (!rect.isEmpty() && rect.adjusted(-14, -14, 14, 14).contains(point)) { pointMatches = true; break; }
        }
        SafeArrayUnaccessData(rectangles);
    }
    SafeArrayDestroy(rectangles);
    return pointMatches;
}

QJsonObject readElement(IUIAutomation *automation, IUIAutomationElement *first,
                        DWORD pid, const QPoint &point, bool automatic) {
    ComPtr<IUIAutomationTreeWalker> walker;
    automation->get_ControlViewWalker(walker.out());
    IUIAutomationElement *element = first;
    if (element) element->AddRef();
    for (int depth = 0; element && depth < 7; ++depth) {
        ComPtr<IUIAutomationElement> current; current.p = element; element = nullptr;
        int elementPid = 0; BOOL password = FALSE;
        if (FAILED(current->get_CurrentProcessId(&elementPid)) || DWORD(elementPid) != pid) break;
        if (FAILED(current->get_CurrentIsPassword(&password)) || password)
            return {{"status", "protected"}};
        ComPtr<IUIAutomationTextPattern> pattern;
        if (SUCCEEDED(current->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
                                                  reinterpret_cast<void **>(pattern.out()))) && pattern.p) {
            ComPtr<IUIAutomationTextRangeArray> ranges;
            if (SUCCEEDED(pattern->GetSelection(ranges.out())) && ranges.p) {
                int count = 0; ranges->get_Length(&count);
                if (count > 32) return {{"status", "tooLong"}};
                QString text;
                bool pointMatches = !automatic;
                for (int i = 0; i < count; ++i) {
                    ComPtr<IUIAutomationTextRange> range;
                    if (FAILED(ranges->GetElement(i, range.out())) || !range.p) continue;
                    BSTR value = nullptr;
                    if (SUCCEEDED(range->GetText(24001, &value)) && value) {
                        const QString part = QString::fromWCharArray(value, int(SysStringLen(value)));
                        SysFreeString(value);
                        if (!part.isEmpty()) {
                            if (!text.isEmpty()) text += QLatin1Char('\n');
                            text += part;
                            pointMatches = pointMatches || rangeNearPoint(range.p, point);
                        }
                    }
                    if (text.size() > 24000) return {{"status", "tooLong"}};
                }
                if (!text.trimmed().isEmpty() && pointMatches) return {{"status", "ok"}, {"text", text.trimmed()}};
            }
        }
        if (walker.p) walker->GetParentElement(current.p, &element);
    }
    if (element) element->Release();
    return {{"status", "empty"}};
}

QJsonObject readSelection(HWND source, const QPoint &point, bool automatic) {
    if (!IsWindow(source) || GetForegroundWindow() != source) return {{"status", "changed"}};
    if (nativePassword(source)) return {{"status", "protected"}};
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(IUIAutomation), reinterpret_cast<void **>(automation.out()))))
        return {{"status", "unavailable"}};
    const auto pid = processOf(source);
    if (automatic) {
        ComPtr<IUIAutomationElement> underPointer;
        automation->ElementFromPoint(POINT{point.x(), point.y()}, underPointer.out());
        if (underPointer.p) {
            const auto result = readElement(automation.p, underPointer.p, pid, point, true);
            if (result.value("status") != QLatin1String("empty")) return result;
        }
        return {{"status", "empty"}};
    }
    // Native edit controls expose their text directly. Avoid a global focus
    // traversal when the foreground thread already identifies that control.
    GUITHREADINFO info{}; info.cbSize = sizeof(info);
    if (GetGUIThreadInfo(GetWindowThreadProcessId(source, nullptr), &info) && info.hwndFocus) {
        ComPtr<IUIAutomationElement> nativeFocus;
        automation->ElementFromHandle(info.hwndFocus, nativeFocus.out());
        if (nativeFocus.p) {
            const auto result = readElement(automation.p, nativeFocus.p, pid, point, false);
            if (result.value("status") != QLatin1String("empty")) return result;
        }
    }
    ComPtr<IUIAutomationElement> focused;
    automation->GetFocusedElement(focused.out());
    return focused.p ? readElement(automation.p, focused.p, pid, point, automatic) : QJsonObject{{"status", "empty"}};
}
}
#endif

int SelectionCapture::runProbe(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 6) return 2;
    QJsonObject result{{"status", "unavailable"}};
#ifdef Q_OS_WIN
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(initialized)) {
        result = readSelection(reinterpret_cast<HWND>(args[2].toULongLong()),
                               QPoint(args[3].toInt(), args[4].toInt()), args[5] == QLatin1String("auto"));
        CoUninitialize();
    }
#endif
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
    fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    fflush(stdout);
    return 0;
}

SelectionCapture::SelectionCapture(QObject *parent) : QObject(parent) {
    m_timeout.setSingleShot(true);
    m_poll.setInterval(25);
    connect(&m_probe, &QProcess::finished, this, [this] { if (m_stage == Probing) probeFinished(); });
    connect(&m_probe, &QProcess::errorOccurred, this, [this] {
        if (m_stage == Probing && m_probe.state() == QProcess::NotRunning) probeFinished();
    });
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        // A timed-out provider is not evidence that it is safe to copy from a
        // control (it may be a password field). Do not fall back on timeout.
        fail(QStringLiteral("取词超时。可以复制文字后在译意中粘贴查询。"));
    });
    connect(&m_poll, &QTimer::timeout, this, &SelectionCapture::pollCopy);
}

SelectionCapture::~SelectionCapture() { cancel(); }

void SelectionCapture::cancel() {
#ifdef Q_OS_WIN
    // If focus changed just after our copy completed, still restore its
    // snapshot. A newer clipboard owned by another app must be left alone.
    if (m_stage == WaitingForCopy && m_clipboard && GetClipboardSequenceNumber() != m_sequence
        && m_sourcePid && processOf(GetClipboardOwner()) == m_sourcePid)
        QGuiApplication::clipboard()->setMimeData(m_clipboard.release());
#endif
    m_stage = Idle;
    m_timeout.stop(); m_poll.stop();
    m_clipboard.reset();
    if (m_probe.state() != QProcess::NotRunning) { m_probe.kill(); m_probe.waitForFinished(1000); }
    if (m_probe.isOpen()) { m_probe.readAllStandardOutput(); m_probe.readAllStandardError(); }
}

bool SelectionCapture::sourceIsCurrent() const {
#ifdef Q_OS_WIN
    return IsWindow(reinterpret_cast<HWND>(m_source)) && GetForegroundWindow() == reinterpret_cast<HWND>(m_source)
        && processOf(reinterpret_cast<HWND>(m_source)) == m_sourcePid;
#else
    return false;
#endif
}

void SelectionCapture::request(quintptr sourceWindow, const QPoint &nativePoint, bool automatic) {
    cancel(); m_source = sourceWindow; m_automatic = automatic;
#ifdef Q_OS_WIN
    m_sourcePid = processOf(reinterpret_cast<HWND>(m_source));
#endif
    if (!sourceIsCurrent()) { fail(QStringLiteral("请先在其他应用中选中文字。")); return; }
    m_stage = Probing;
    m_probe.setProgram(QCoreApplication::applicationFilePath());
    m_probe.setArguments({QStringLiteral("--selection-probe"), QString::number(sourceWindow),
                          QString::number(nativePoint.x()), QString::number(nativePoint.y()),
                          automatic ? QStringLiteral("auto") : QStringLiteral("hotkey")});
    m_probe.start(); m_timeout.start(1800);
}

void SelectionCapture::probeFinished() {
    m_timeout.stop();
    if (!sourceIsCurrent()) { fail(QStringLiteral("取词时窗口已切换，请重新选择。")); return; }
    const auto result = QJsonDocument::fromJson(m_probe.readAllStandardOutput()).object();
    const auto status = result.value("status").toString();
    if (status == QLatin1String("ok")) { complete(result.value("text").toString()); return; }
    if (status == QLatin1String("protected")) { fail(QStringLiteral("此输入框不允许取词。")); return; }
    if (status == QLatin1String("tooLong")) { fail(QStringLiteral("选中文字超过 24,000 字符，请缩短选择范围。")); return; }
    if (m_automatic || (status != QLatin1String("empty") && status != QLatin1String("unavailable"))) {
        fail(QStringLiteral("未读取到选中文字，可尝试取词快捷键。")); return;
    }
    m_stage = WaitingForKeys; m_clock.start(); m_poll.start();
}

void SelectionCapture::pollCopy() {
#ifdef Q_OS_WIN
    if (!sourceIsCurrent()) { fail(QStringLiteral("取词时窗口已切换，请重新选择。")); return; }
    if (m_clock.elapsed() > 1200) { fail(QStringLiteral("未读取到选中文字。请确认文字可复制，或手动复制后粘贴查询。")); return; }
    auto *clipboard = QGuiApplication::clipboard();
    if (m_stage == WaitingForKeys) {
        for (int key : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN, VK_LBUTTON, int('C')})
            if (GetAsyncKeyState(key) & 0x8000) return;
        if (nativePassword(reinterpret_cast<HWND>(m_source))) { fail(QStringLiteral("此输入框不允许取词。")); return; }
        m_sequence = GetClipboardSequenceNumber();
        m_clipboard = std::make_unique<QMimeData>();
        const auto *original = clipboard->mimeData();
        qsizetype total = 0;
        if (original) for (const auto &format : original->formats()) {
            const auto bytes = original->data(format);
            total += bytes.size();
            if (total > 16 * 1024 * 1024) { fail(QStringLiteral("剪贴板内容较大，已保留原内容。请手动复制后粘贴查询。")); return; }
            m_clipboard->setData(format, bytes);
        }
        if (!sourceIsCurrent() || m_sequence != GetClipboardSequenceNumber()) {
            fail(QStringLiteral("窗口或剪贴板已变化，请重试。")); return;
        }
        INPUT keys[4]{};
        for (auto &key : keys) key.type = INPUT_KEYBOARD;
        keys[0].ki.wVk = VK_CONTROL; keys[1].ki.wVk = 'C';
        keys[2].ki.wVk = 'C'; keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
        keys[3].ki.wVk = VK_CONTROL; keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
        const auto inserted = SendInput(4, keys, sizeof(INPUT));
        if (inserted != 4) {
            if (inserted > 0) SendInput(2, keys + 2, sizeof(INPUT)); // Release our synthetic keys on partial insertion.
            fail(QStringLiteral("无法向此应用发送复制操作，请手动复制后粘贴查询。")); return;
        }
        m_stage = WaitingForCopy; m_clock.restart();
    } else if (m_stage == WaitingForCopy) {
        const auto sequence = GetClipboardSequenceNumber();
        if (sequence == m_sequence) return; // Never translate an old clipboard value.
        if (processOf(GetClipboardOwner()) != m_sourcePid) {
            fail(QStringLiteral("剪贴板被其他应用修改，已取消本次取词。")); return;
        }
        // Reading delayed-rendered OLE text can itself advance the sequence
        // number. Hold the clipboard while materializing the Unicode format,
        // then capture its final sequence to distinguish a subsequent writer.
        if (!OpenClipboard(nullptr)) return;
        QString text;
        if (const HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto *value = static_cast<const wchar_t *>(GlobalLock(data))) {
                const auto limit = qMin<qsizetype>(GlobalSize(data) / sizeof(wchar_t), 24001);
                qsizetype length = 0;
                while (length < limit && value[length]) ++length;
                text = QString::fromWCharArray(value, length);
                GlobalUnlock(data);
            }
        }
        const auto copiedSequence = GetClipboardSequenceNumber();
        CloseClipboard();
        if (copiedSequence != GetClipboardSequenceNumber() || processOf(GetClipboardOwner()) != m_sourcePid) {
            m_clipboard.reset();
            fail(QStringLiteral("剪贴板已变化，请重试。")); return;
        }
        if (!sourceIsCurrent()) { fail(QStringLiteral("取词时窗口已切换，请重新选择。")); return; }
        // Only restore the snapshot while our own copy is still current.
        clipboard->setMimeData(m_clipboard.release());
        complete(text);
    }
#endif
}

void SelectionCapture::complete(const QString &text) {
    const auto value = text.trimmed();
    if (value.isEmpty()) { fail(QStringLiteral("没有选中文字，请先选中再查询。")); return; }
    if (value.size() > 24000) { fail(QStringLiteral("选中文字超过 24,000 字符，请缩短选择范围。")); return; }
    const auto source = m_source; const bool automatic = m_automatic;
    cancel(); emit captured(value, automatic, source);
}

void SelectionCapture::fail(const QString &message) {
    const bool automatic = m_automatic;
    cancel(); emit failed(message, automatic);
}
