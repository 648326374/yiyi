#include "appcontroller.h"
#include "desktopintegration.h"
#include "documentcontroller.h"
#include "fileimportcontroller.h"
#include "officecontroller.h"
#include "pdfcontroller.h"
#include "screenshotcontroller.h"
#include "visioncontroller.h"
#include "singleinstance.h"
#include "streamserver.h"
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QWindow>
#include <cstdio>
#include <cstring>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// This fixture is a separate process, exactly like an external editor. Its
// text is synthetic; neither the user's apps nor their model are accessed.
class CopyFixture : public QWidget {
public:
    QString mode;
    explicit CopyFixture(const QString &value) : mode(value) { setFocusPolicy(Qt::StrongFocus); }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->matches(QKeySequence::Copy) && mode == QLatin1String("copy")) {
            QGuiApplication::clipboard()->setText(QStringLiteral("selection from copy"));
            fputs("copied\n", stdout); fflush(stdout);
        }
    }
};

static int runFixture(int argc, char **argv) {
    QApplication app(argc, argv);
    const auto mode = app.arguments().value(2);
    std::unique_ptr<QWidget> window;
    if (mode == QLatin1String("text") || mode == QLatin1String("long")) {
        auto *edit = new QTextEdit;
        edit->setPlainText(mode == QLatin1String("long") ? QString(24001, QLatin1Char('a')) : QStringLiteral("serendipity"));
        edit->selectAll(); window.reset(edit);
    } else if (mode == QLatin1String("password")) {
        auto *edit = new QLineEdit;
        edit->setEchoMode(QLineEdit::Password); edit->setText(QStringLiteral("test-password")); edit->selectAll(); window.reset(edit);
    } else window = std::make_unique<CopyFixture>(mode);
    window->setWindowTitle(QStringLiteral("Yiyi selection test fixture"));
    window->setGeometry(160, 140, 420, 160); window->show(); window->activateWindow(); window->setFocus();
    QTimer::singleShot(250, &app, [&] {
        QPoint local(16, 14);
        QPoint selectionStart(16, 14), selectionEnd(80, 14);
        if (auto *edit = qobject_cast<QTextEdit *>(window.get())) {
            auto cursor = edit->textCursor(); cursor.setPosition(3);
            local = edit->viewport()->mapTo(window.get(), edit->cursorRect(cursor).center());
            cursor.setPosition(0); selectionStart = edit->viewport()->mapTo(window.get(), edit->cursorRect(cursor).center());
            cursor.setPosition(edit->toPlainText().size()); selectionEnd = edit->viewport()->mapTo(window.get(), edit->cursorRect(cursor).center());
        }
        const auto toNative = [&](const QPoint &position) {
#ifdef Q_OS_WIN
            POINT native{qRound(position.x() * window->devicePixelRatioF()), qRound(position.y() * window->devicePixelRatioF())};
            ClientToScreen(reinterpret_cast<HWND>(window->winId()), &native);
            return QPoint(native.x, native.y);
#else
            return window->mapToGlobal(position);
#endif
        };
        const auto point = toNative(local), begin = toNative(selectionStart), end = toNative(selectionEnd);
        const auto json = QJsonDocument(QJsonObject{{"window", QString::number(window->winId())}, {"x", point.x()}, {"y", point.y()},
            {"startX", begin.x()}, {"startY", begin.y()}, {"endX", end.x()}, {"endY", end.y()}}).toJson(QJsonDocument::Compact);
        fwrite(json.constData(), 1, size_t(json.size()), stdout); fputs("\n", stdout); fflush(stdout);
    });
    return app.exec();
}

class DesktopTests : public QObject {
    Q_OBJECT
    std::unique_ptr<QMimeData> originalClipboard;
    QPoint originalCursor;
    QProcess fixture;
    quintptr source = 0;
    QPoint point;
    QPoint dragStart, dragEnd;
    bool fixtureReady = false;

    void startFixture(const QString &mode, bool requiresPointer = false) {
        fixtureReady = false;
        fixture.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--desktop-fixture"), mode});
        QVERIFY(fixture.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.canReadLine(), 5000);
        const auto json = QJsonDocument::fromJson(fixture.readLine()).object();
        source = json.value("window").toString().toULongLong(); QVERIFY(source);
        point = QPoint(json.value("x").toInt(), json.value("y").toInt());
        dragStart = QPoint(json.value("startX").toInt(), json.value("startY").toInt());
        dragEnd = QPoint(json.value("endX").toInt(), json.value("endY").toInt());
#ifdef Q_OS_WIN
        ShowWindow(reinterpret_cast<HWND>(source), SW_SHOWNORMAL);
        SetForegroundWindow(reinterpret_cast<HWND>(source));
        // Windows may deny programmatic activation when CTest was launched
        // in the background. Click only this fixture's caption to acquire
        // foreground permission, without sending input to the user's app.
        if (reinterpret_cast<quintptr>(GetForegroundWindow()) != source) {
            const auto handle = reinterpret_cast<HWND>(source);
            QVERIFY(IsWindowVisible(handle));
            SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            RECT rect{}; QVERIFY(GetWindowRect(handle, &rect));
            const POINT caption{(rect.left + rect.right) / 2, rect.top + 12};
            if (GetAncestor(WindowFromPoint(caption), GA_ROOT) != handle) {
                QTest::qSkip("Desktop is covered by another window; no input sent outside the fixture.", __FILE__, __LINE__); return;
            }
            nativeClick(QPoint(caption.x, caption.y));
            QTest::qWait(80);
            SetWindowPos(handle, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        QElapsedTimer activation; activation.start();
        while (reinterpret_cast<quintptr>(GetForegroundWindow()) != source && activation.elapsed() < 700) QTest::qWait(25);
        if (reinterpret_cast<quintptr>(GetForegroundWindow()) != source) {
            QTest::qSkip("Windows did not grant foreground to the fixture; no keyboard input sent.", __FILE__, __LINE__); return;
        }
        if (requiresPointer && GetAncestor(WindowFromPoint(POINT{point.x(), point.y()}), GA_ROOT) != reinterpret_cast<HWND>(source)) {
            QTest::qSkip("Pointer tests need an unobstructed interactive Windows desktop.", __FILE__, __LINE__); return;
        }
        INPUT selectAll[4]{};
        for (auto &key : selectAll) key.type = INPUT_KEYBOARD;
        selectAll[0].ki.wVk = VK_CONTROL; selectAll[1].ki.wVk = 'A';
        selectAll[2].ki.wVk = 'A'; selectAll[2].ki.dwFlags = KEYEVENTF_KEYUP;
        selectAll[3].ki.wVk = VK_CONTROL; selectAll[3].ki.dwFlags = KEYEVENTF_KEYUP;
        QCOMPARE(SendInput(4, selectAll, sizeof(INPUT)), UINT(4));
        QTest::qWait(100);
#endif
        fixtureReady = true;
    }
    static void nativeClick(const QPoint &position, bool twice = false) {
#ifdef Q_OS_WIN
        SetCursorPos(position.x(), position.y());
        INPUT events[2]{};
        for (int i = 0; i < 2; ++i) {
            events[i].type = INPUT_MOUSE;
            events[i].mi.dwFlags = i % 2 ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
        }
        QCOMPARE(SendInput(2, events, sizeof(INPUT)), UINT(2));
        if (twice) { QTest::qWait(80); QCOMPARE(SendInput(2, events, sizeof(INPUT)), UINT(2)); }
#endif
    }
    static void hotkey(int key) {
#ifdef Q_OS_WIN
        INPUT keys[8]{};
        const WORD sequence[] = {VK_CONTROL, VK_MENU, VK_SHIFT, WORD(key), WORD(key), VK_SHIFT, VK_MENU, VK_CONTROL};
        for (int i = 0; i < 8; ++i) {
            keys[i].type = INPUT_KEYBOARD; keys[i].ki.wVk = sequence[i];
            if (i >= 4) keys[i].ki.dwFlags = KEYEVENTF_KEYUP;
        }
        QCOMPARE(SendInput(8, keys, sizeof(INPUT)), UINT(8));
#endif
    }
private slots:
    void initTestCase() {
        if (QGuiApplication::platformName() != QLatin1String("windows")) QSKIP("Native desktop integration requires Windows.");
        QApplication::setQuitOnLastWindowClosed(false);
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        originalCursor = QCursor::pos();
        originalClipboard = std::make_unique<QMimeData>();
        const auto *mime = QApplication::clipboard()->mimeData();
        if (mime) for (const auto &format : mime->formats()) originalClipboard->setData(format, mime->data(format));
    }
    void cleanupTestCase() {
        if (originalClipboard) QApplication::clipboard()->setMimeData(originalClipboard.release());
        QCursor::setPos(originalCursor);
    }
    void cleanup() {
        if (fixture.state() != QProcess::NotRunning) {
            fixture.terminate();
            if (!fixture.waitForFinished(2000)) { fixture.kill(); fixture.waitForFinished(); }
        }
    }
    void shortcutValidation_data() {
        QTest::addColumn<QString>("input"); QTest::addColumn<QString>("expected");
        QTest::newRow("default") << QStringLiteral("Ctrl+Alt+Y") << QStringLiteral("Ctrl+Alt+Y");
        QTest::newRow("case") << QStringLiteral("ctrl+shift+f8") << QStringLiteral("Ctrl+Shift+F8");
        QTest::newRow("single-letter") << QStringLiteral("Y") << QString();
        QTest::newRow("shift-only") << QStringLiteral("Shift+A") << QString();
        QTest::newRow("multi-step") << QStringLiteral("Ctrl+K, Ctrl+C") << QString();
        QTest::newRow("debugger-reserved") << QStringLiteral("Ctrl+F12") << QString();
        QTest::newRow("modifier-only") << QStringLiteral("Ctrl+Alt") << QString();
        QTest::newRow("empty") << QString() << QString();
    }
    void shortcutValidation() {
        QFETCH(QString, input); QFETCH(QString, expected);
        QCOMPARE(DesktopIntegration::normalizedShortcut(input), expected);
    }
    void preferencesPersistWithoutTouchingProvider() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QSettings existing(directory.filePath("settings.ini"), QSettings::IniFormat);
        existing.setValue("provider-marker", "preserved"); existing.sync();
        {
            DesktopIntegration desktop(directory.path(), false);
            QVERIFY(desktop.selectionEnabled()); QVERIFY(desktop.closeToTray());
            desktop.setSelectionEnabled(false); desktop.setCloseToTray(false);
            QVERIFY(desktop.applyShortcuts(true, "Ctrl+Shift+F8", "Ctrl+Shift+F9"));
            QVERIFY(!desktop.applyShortcuts(true, "Ctrl+Y", "Ctrl+Y"));
            QCOMPARE(desktop.showShortcut(), QStringLiteral("Ctrl+Shift+F8"));
        }
        DesktopIntegration restored(directory.path(), false);
        QVERIFY(!restored.selectionEnabled()); QVERIFY(!restored.closeToTray());
        QCOMPARE(restored.showShortcut(), QStringLiteral("Ctrl+Shift+F8"));
        existing.sync(); QCOMPARE(existing.value("provider-marker").toString(), QStringLiteral("preserved"));
    }
    void shortcutConflictRollbackAndRelease() {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        DesktopIntegration desktop(directory.path());
        desktop.applyShortcuts(false, "Ctrl+Alt+Shift+F7", "Ctrl+Alt+Shift+F8");
        desktop.setSelectionEnabled(false); desktop.start();
        QVERIFY(desktop.applyShortcuts(true, "Ctrl+Alt+Shift+F7", "Ctrl+Alt+Shift+F8"));
        // Register a competing hotkey, then fail the SECOND of a new pair.
        QVERIFY(RegisterHotKey(nullptr, 0x5800, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F10));
        const bool accepted = desktop.applyShortcuts(true, "Ctrl+Alt+Shift+F9", "Ctrl+Alt+Shift+F10");
        UnregisterHotKey(nullptr, 0x5800);
        QVERIFY(!accepted); QVERIFY(!desktop.shortcutError().isEmpty());
        QCOMPARE(desktop.showShortcut(), QStringLiteral("Ctrl+Alt+Shift+F7"));
        QVERIFY(RegisterHotKey(nullptr, 0x5800, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F9));
        UnregisterHotKey(nullptr, 0x5800);
        QSignalSpy opened(&desktop, &DesktopIntegration::workspaceRequested);
        startFixture("empty"); if (!fixtureReady) return;
        hotkey(VK_F7); QTRY_COMPARE(opened.count(), 1);
        QVERIFY(desktop.applyShortcuts(true, "Ctrl+Alt+Shift+F8", "Ctrl+Alt+Shift+F7"));
        hotkey(VK_F8); QTRY_COMPARE(opened.count(), 2);
        QSignalSpy recorded(&desktop, &DesktopIntegration::shortcutRecorded);
        desktop.setProperty("recordingShortcut", true);
        hotkey(VK_F8); QTRY_COMPARE(recorded.count(), 1);
        QCOMPARE(opened.count(), 2); QCOMPARE(recorded.first()[0].toString(), desktop.showShortcut());
        desktop.setProperty("recordingShortcut", false);
        QVERIFY(desktop.applyShortcuts(false, desktop.showShortcut(), desktop.selectionShortcut()));
        QVERIFY(RegisterHotKey(nullptr, 0x5800, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F8));
        UnregisterHotKey(nullptr, 0x5800);
#endif
    }
    void trayCloseRestoreAndUnavailableFallback() {
        QTemporaryDir directory;
        DesktopIntegration desktop(directory.path()); QWindow window;
        desktop.applyShortcuts(false, "Ctrl+Alt+Y", "Ctrl+Alt+D"); desktop.setSelectionEnabled(false);
        desktop.setWindow(&window); desktop.start();
        window.showMaximized(); QTRY_COMPARE(window.visibility(), QWindow::Maximized);
        if (desktop.trayAvailable()) {
            QVERIFY(desktop.handleClose()); QVERIFY(!window.isVisible());
            desktop.showWindow(); QTRY_COMPARE(window.visibility(), QWindow::Maximized);
            window.showMinimized(); QTRY_COMPARE(window.visibility(), QWindow::Minimized);
            desktop.showWindow(); QTRY_COMPARE(window.visibility(), QWindow::Maximized);
        }
        desktop.setCloseToTray(false); QVERIFY(!desktop.handleClose());
        DesktopIntegration unavailable(directory.path(), false);
        unavailable.setWindow(&window); unavailable.setCloseToTray(true);
        QVERIFY(!unavailable.handleClose());
    }
    void accessibilityReadDoesNotTouchClipboard() {
        startFixture("text", true); if (!fixtureReady) return;
        QApplication::clipboard()->setText(QStringLiteral("old clipboard"));
        SelectionCapture capture;
        QSignalSpy ready(&capture, &SelectionCapture::captured), failed(&capture, &SelectionCapture::failed);
        capture.request(source, point, true);
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 5000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first()[0].toString()));
        QCOMPARE(ready.count(), 1); QCOMPARE(ready.first()[0].toString(), QStringLiteral("serendipity"));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("old clipboard"));
    }
    void globalSelectionHotkeyWithMonitoringEnabled() {
#ifdef Q_OS_WIN
        QTemporaryDir directory; DesktopIntegration desktop(directory.path());
        QVERIFY(desktop.applyShortcuts(true, "Ctrl+Alt+Shift+F7", "Ctrl+Alt+Shift+F8"));
        QSignalSpy query(&desktop, &DesktopIntegration::lookupRequested), notice(&desktop, &DesktopIntegration::toast);
        startFixture("text"); if (!fixtureReady) return;
        desktop.start(); QVERIFY(desktop.shortcutError().isEmpty()); hotkey(VK_F8);
        QTRY_VERIFY_WITH_TIMEOUT(!query.isEmpty() || !notice.isEmpty(), 5000);
        QVERIFY2(notice.isEmpty(), notice.isEmpty() ? "" : qUtf8Printable(notice.first()[0].toString()));
        QCOMPARE(query.count(), 1); QCOMPARE(query.first()[0].toString(), QStringLiteral("serendipity"));
#endif
    }
    void explicitCopyRestoresClipboardFormats() {
        startFixture("copy"); if (!fixtureReady) return;
        auto *original = new QMimeData;
        original->setText(QStringLiteral("original text")); original->setHtml(QStringLiteral("<b>original text</b>"));
        original->setData("application/x-yiyi-test", "original bytes");
        QApplication::clipboard()->setMimeData(original);
        SelectionCapture capture;
        QSignalSpy ready(&capture, &SelectionCapture::captured), failed(&capture, &SelectionCapture::failed);
        capture.request(source, point, false);
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 6000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first()[0].toString()));
        QCOMPARE(ready.count(), 1); QCOMPARE(ready.first()[0].toString(), QStringLiteral("selection from copy"));
        const auto *restored = QApplication::clipboard()->mimeData();
        QCOMPARE(restored->text(), QStringLiteral("original text")); QCOMPARE(restored->html(), QStringLiteral("<b>original text</b>"));
        QCOMPARE(restored->data("application/x-yiyi-test"), QByteArray("original bytes"));
    }
    void rejectedSelection_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("automatic");
        QTest::newRow("no-selection-no-stale-clipboard") << QStringLiteral("empty") << false;
        QTest::newRow("password") << QStringLiteral("password") << false;
        QTest::newRow("long") << QStringLiteral("long") << false;
        QTest::newRow("automatic-never-copies") << QStringLiteral("copy") << true;
    }
    void rejectedSelection() {
        QFETCH(QString, mode); QFETCH(bool, automatic);
        startFixture(mode); if (!fixtureReady) return;
        QApplication::clipboard()->setText(QStringLiteral("never translate this old clipboard"));
        SelectionCapture capture;
        QSignalSpy ready(&capture, &SelectionCapture::captured), failed(&capture, &SelectionCapture::failed);
        capture.request(source, point, automatic);
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 6000); QVERIFY(ready.isEmpty());
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("never translate this old clipboard"));
    }
    void canceledCaptureCannotDeliver() {
        startFixture("text"); if (!fixtureReady) return;
        SelectionCapture capture;
        QSignalSpy ready(&capture, &SelectionCapture::captured), failed(&capture, &SelectionCapture::failed);
        capture.request(source, point, true); capture.cancel(); QTest::qWait(500);
        QVERIFY(!capture.busy()); QVERIFY(ready.isEmpty()); QVERIFY(failed.isEmpty());
    }
    void clipboardRaceKeepsNewContent() {
        startFixture("copy"); if (!fixtureReady) return;
        QApplication::clipboard()->setText(QStringLiteral("original"));
        bool replaced = false;
        QObject connectionGuard;
        connect(&fixture, &QProcess::readyReadStandardOutput, &connectionGuard, [&] {
            if (fixture.readAllStandardOutput().contains("copied")) {
                QApplication::clipboard()->setText(QStringLiteral("newer clipboard")); replaced = true;
            }
        });
        SelectionCapture capture;
        QSignalSpy ready(&capture, &SelectionCapture::captured), failed(&capture, &SelectionCapture::failed);
        capture.request(source, point, false);
        QTRY_VERIFY_WITH_TIMEOUT(replaced || !failed.isEmpty(), 4000);
        QVERIFY2(replaced, failed.isEmpty() ? "Copy was not delivered" : qUtf8Printable(failed.first()[0].toString()));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 4000);
        QVERIFY(ready.isEmpty()); QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("newer clipboard"));
    }
    void draggingAndPausingSelection() {
#ifdef Q_OS_WIN
        QTemporaryDir directory; DesktopIntegration desktop(directory.path());
        desktop.applyShortcuts(false, "Ctrl+Alt+Y", "Ctrl+Alt+D");
        QSignalSpy query(&desktop, &DesktopIntegration::lookupRequested);
        QSignalSpy failures(desktop.findChild<SelectionCapture *>(), &SelectionCapture::failed);
        startFixture("text", true); if (!fixtureReady) return;
        desktop.start();
        // Start with a caret, not an existing selection: dragging selected
        // QTextEdit text starts a drag-and-drop operation instead of selecting.
        INPUT home[2]{}; home[0].type = home[1].type = INPUT_KEYBOARD;
        home[0].ki.wVk = home[1].ki.wVk = VK_HOME; home[1].ki.dwFlags = KEYEVENTF_KEYUP;
        QCOMPARE(SendInput(2, home, sizeof(INPUT)), UINT(2)); QTest::qWait(80);
        SetCursorPos(dragStart.x(), dragStart.y());
        INPUT event{}; event.type = INPUT_MOUSE; event.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        QCOMPARE(SendInput(1, &event, sizeof(INPUT)), UINT(1)); QTest::qWait(80);
        for (int step = 1; step <= 4; ++step) {
            const auto next = dragStart + (dragEnd - dragStart) * step / 4;
            SetCursorPos(next.x(), next.y()); QTest::qWait(30);
        }
        event.mi.dwFlags = MOUSEEVENTF_LEFTUP; QCOMPARE(SendInput(1, &event, sizeof(INPUT)), UINT(1));
        QTest::qWait(80);
        POINT actualCursor{}; GetCursorPos(&actualCursor);
        if (reinterpret_cast<quintptr>(GetForegroundWindow()) != source
            || GetAncestor(WindowFromPoint(POINT{dragEnd.x(), dragEnd.y()}), GA_ROOT) != reinterpret_cast<HWND>(source)
            || QPoint(actualCursor.x, actualCursor.y) != dragEnd) {
            QSKIP("Another desktop participant moved the pointer or covered the fixture during the drag.");
        }
        QTRY_VERIFY_WITH_TIMEOUT(desktop.selectionVisible() || !failures.isEmpty(), 5000);
        QVERIFY2(desktop.selectionVisible(), failures.isEmpty() ? "No selection event" : qUtf8Printable(failures.first()[0].toString()));
        QVERIFY(query.isEmpty());
        desktop.setSelectionEnabled(false); QVERIFY(!desktop.selectionVisible());
        nativeClick(point, true); QTest::qWait(500); QVERIFY(!desktop.selectionVisible());
#endif
    }
    void selectionBubbleQueriesOnlyOnClick() {
        QTemporaryDir directory; AppController controller(directory.path());
        StreamServer server(QStringLiteral("openai"));
        QVERIFY(controller.saveProvider({{"name", "Test"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "test"}}).isEmpty());
        DesktopIntegration desktop(directory.path()); desktop.applyShortcuts(false, "Ctrl+Alt+Y", "Ctrl+Alt+D");
        DocumentController documents(&controller); PdfController pdf(&controller); ScreenshotController capture;
        OfficeController office(&controller);
        FileImportController fileImport(&documents, &pdf, &office);
        VisionController vision(&controller);
        connect(&capture, &ScreenshotController::imageChanged, &vision, [&] { vision.setImage(capture.image()); });
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("App", &controller); engine.rootContext()->setContextProperty("Desktop", &desktop);
        engine.rootContext()->setContextProperty("Documents", &documents); engine.rootContext()->setContextProperty("Capture", &capture);
        engine.rootContext()->setContextProperty("Pdf", &pdf);
        engine.rootContext()->setContextProperty("FileImport", &fileImport);
        engine.rootContext()->setContextProperty("Office", &office);
        engine.rootContext()->setContextProperty("Vision", &vision);
        engine.rootContext()->setContextProperty("initialPage", "translate"); engine.rootContext()->setContextProperty("themeOverride", "light");
        engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml"))); QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        desktop.setWindow(window);
        connect(&desktop, &DesktopIntegration::lookupRequested, &controller, &AppController::lookupSelection);
        // Finish the first QML frame before hiding the app and installing the
        // global hook. Cold rendering must not overlap the synthetic clicks.
        QSignalSpy frames(window, &QQuickWindow::frameSwapped);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->update(); QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 5000);
        QVERIFY(desktop.selectionError().isEmpty());
        window->hide();
        desktop.start();
        startFixture("text", true); if (!fixtureReady) return;
        QApplication::clipboard()->setText(QStringLiteral("untouched"));
        auto *selection = desktop.findChild<SelectionCapture *>(); QVERIFY(selection);
        QSignalSpy failures(selection, &SelectionCapture::failed);
        QVERIFY(desktop.selectionEnabled());
#ifdef Q_OS_WIN
        QCOMPARE(reinterpret_cast<quintptr>(GetForegroundWindow()), source);
        QCOMPARE(reinterpret_cast<quintptr>(GetAncestor(WindowFromPoint(POINT{point.x(), point.y()}), GA_ROOT)), source);
#endif
        nativeClick(point, true);
#ifdef Q_OS_WIN
        QCOMPARE(reinterpret_cast<quintptr>(GetForegroundWindow()), source);
        QCOMPARE(reinterpret_cast<quintptr>(GetAncestor(WindowFromPoint(POINT{point.x(), point.y()}), GA_ROOT)), source);
#endif
        QTRY_VERIFY_WITH_TIMEOUT(desktop.selectionVisible() || !failures.isEmpty(), 6000);
        QVERIFY2(desktop.selectionVisible(), failures.isEmpty() ? "No selection event" : qUtf8Printable(failures.first()[0].toString()));
        QCOMPARE(server.requestCount, 0); QVERIFY(!window->isVisible());
#ifdef Q_OS_WIN
        QCOMPARE(reinterpret_cast<quintptr>(GetForegroundWindow()), source);
#endif
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("untouched"));
        auto *bubble = window->findChild<QQuickWindow *>(QStringLiteral("selectionBubble")); QVERIFY(bubble);
        QVERIFY(bubble->isVisible()); QVERIFY(QTest::qWaitForWindowExposed(bubble));
        const auto captureDir = qEnvironmentVariable("YIYI_DESKTOP_CAPTURE_DIR");
        if (!captureDir.isEmpty()) {
            QDir().mkpath(captureDir);
            QVERIFY(bubble->grabWindow().save(QDir(captureDir).filePath("selection-bubble.png")));
        }
#ifdef Q_OS_WIN
        POINT native{56, 21}; ClientToScreen(reinterpret_cast<HWND>(bubble->winId()), &native);
        nativeClick(QPoint(native.x, native.y));
#endif
        QTRY_VERIFY(!desktop.selectionVisible()); QTRY_VERIFY(window->isVisible());
        QTRY_VERIFY(server.ready); QCOMPARE(server.requestCount, 1);
        QCOMPARE(controller.property("inputText").toString(), QStringLiteral("serendipity"));
        server.sendText(QStringLiteral("POS: n.\nTRANSLATION: 意外之喜")); server.finish({});
        QTRY_VERIFY(!controller.busy()); QVERIFY(controller.error().isEmpty());
        QCOMPARE(controller.history().size(), 1);
        if (!captureDir.isEmpty()) {
            window->setProperty("currentPage", 3); QTest::qWait(250);
            QVERIFY(window->grabWindow().save(QDir(captureDir).filePath("desktop-settings.png")));
        }
        if (desktop.trayAvailable()) {
            QVERIFY(!window->close()); // QML rejects close after hiding in the tray.
            QVERIFY(!window->isVisible());
            desktop.showWindow(); QTRY_VERIFY(window->isVisible());
        }
        desktop.setCloseToTray(false);
        QVERIFY(window->close()); QVERIFY(!window->isVisible());
    }
    void singleInstanceActivation() {
        QTemporaryDir directory; SingleInstance primary;
        QCOMPARE(primary.acquire(directory.path()), SingleInstance::Primary);
        QSignalSpy activated(&primary, &SingleInstance::activated);
        QProcess second;
        second.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--instance-client"), directory.path()});
        QTRY_COMPARE_WITH_TIMEOUT(second.state(), QProcess::NotRunning, 5000);
        QCOMPARE(second.exitCode(), 0); QCOMPARE(activated.count(), 1);
    }
};

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--selection-probe") == 0) return SelectionCapture::runProbe(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--desktop-fixture") == 0) return runFixture(argc, argv);
    QApplication app(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--instance-client") == 0) {
        SingleInstance instance;
        return instance.acquire(QString::fromLocal8Bit(argv[2])) == SingleInstance::ActivatedExisting ? 0 : 1;
    }
    DesktopTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_desktop.moc"
