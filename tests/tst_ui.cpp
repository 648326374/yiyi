#include "appcontroller.h"
#include "desktopintegration.h"
#include "documentcontroller.h"
#include "fileimportcontroller.h"
#include "officecontroller.h"
#include "pdfcontroller.h"
#include "pdffixture.h"
#include "screenshotcontroller.h"
#include "visioncontroller.h"
#include "streamserver.h"
#include <QtTest>
#include <QClipboard>
#include <QCryptographicHash>
#include <QCursor>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScreen>
#include <QStyleHints>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDrag>
#include <QWidget>
#include <atomic>
#include <thread>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <memory>

#ifdef Q_OS_WIN
class NativeFileDragSource : public QWidget {
public:
    QUrl file;
    bool completed = false, started = false;
    Qt::DropAction action = Qt::IgnoreAction;
    std::atomic_bool *finished = nullptr;
protected:
    void mousePressEvent(QMouseEvent *event) override { event->accept(); }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (started || !(event->buttons() & Qt::LeftButton)) return;
        started = true;
        QDrag drag(this); auto *mime = new QMimeData; mime->setUrls({file}); drag.setMimeData(mime);
        action = drag.exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
        completed = true; *finished = true;
    }
};
#endif

class UiTests : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> directory;
    std::unique_ptr<AppController> app;
    std::unique_ptr<DesktopIntegration> desktop;
    std::unique_ptr<DocumentController> documents;
    std::unique_ptr<PdfController> pdf;
    std::unique_ptr<FileImportController> fileImport;
    std::unique_ptr<OfficeController> office;
    std::unique_ptr<ScreenshotController> captureController;
    std::unique_ptr<VisionController> vision;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window = nullptr;
    QPoint originalCursorPosition;

    QObject *visualObject(QQuickItem *parent, const QString &name) {
        if (parent->objectName() == name) return parent;
        if (auto *found = parent->findChild<QObject *>(name)) return found;
        for (auto *child : parent->childItems()) if (auto *found = visualObject(child, name)) return found;
        return nullptr;
    }

    QObject *object(const char *name) {
        const auto text = QString::fromLatin1(name);
        if (auto *found = window->findChild<QObject *>(text)) return found;
        return visualObject(window->contentItem(), text);
    }
    void click(const char *name) {
        auto *item = qobject_cast<QQuickItem *>(object(name));
        QVERIFY2(item, name);
        QVERIFY2(item->isVisible(), name);
        QVERIFY2(item->isEnabled(), name);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        QCoreApplication::processEvents();
    }
    void menuClick(const char *name) {
        click("pdfMoreButton"); QTRY_VERIFY(object("pdfMoreMenu")->property("opened").toBool()); click(name);
    }
    void field(const char *name, const QString &value) {
        auto *item = object(name); QVERIFY2(item, name); QVERIFY(item->setProperty("text", value));
    }
    void wheel(QQuickItem *item, int amount, bool pixels = false) {
        const auto local = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        const auto global = window->mapToGlobal(local.toPoint());
        QWheelEvent event(local, global, pixels ? QPoint(0, amount) : QPoint(), pixels ? QPoint() : QPoint(0, amount), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(window, &event);
        QCoreApplication::processEvents();
    }
    void capture(const QString &name) {
        const auto path = qEnvironmentVariable("YIYI_UI_CAPTURE_DIR");
        if (path.isEmpty()) return;
        QTest::qWait(250);
        QDir().mkpath(path);
        QVERIFY(window->grabWindow().save(QDir(path).filePath(name + QStringLiteral(".png"))));
    }
    void movePointer(const QPoint &position) {
        // QTest's QWindow events do not move the OS cursor. Keep them in sync
        // when checking native hover behavior across popup and window updates.
        if (QGuiApplication::platformName() == QLatin1String("windows")) {
            window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
            QCursor::setPos(window->mapToGlobal(position));
            QTest::qWait(30);
        }
        QTest::mouseMove(window, position);
        QCoreApplication::processEvents();
    }
    QColor buttonFill(QQuickItem *button, const QImage &frame) {
        if (frame.isNull()) return {};
        const auto point = button->mapToScene(QPointF(5, button->height() / 2));
        return frame.pixelColor(qRound(point.x() * frame.width() / window->width()),
                                qRound(point.y() * frame.height() / window->height()));
    }
    QUrl importText(const QString &name, const QByteArray &bytes = "Original text") {
        QFile file(directory->filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return {};
        return QUrl::fromLocalFile(file.fileName());
    }
    bool enterFiles(const QMimeData &mime, const QPoint &position = QPoint(500, 330)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
        QDragEnterEvent event(QPointF(position), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
#else
        QDragEnterEvent event(position, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
#endif
        QCoreApplication::sendEvent(window, &event); QCoreApplication::processEvents(); return event.isAccepted();
    }
    bool dropFiles(const QMimeData &mime, const QPoint &position = QPoint(500, 330)) {
        QDropEvent event(QPointF(position), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event); QCoreApplication::processEvents();
        return event.isAccepted() && event.dropAction() == Qt::CopyAction;
    }
private slots:
    void fileDropRoutes_data() {
        QTest::addColumn<QString>("kind"); QTest::addColumn<bool>("compact");
        QTest::newRow("txt-light") << QStringLiteral("txt") << false;
        QTest::newRow("txt-dark-minimum") << QStringLiteral("txt") << true;
        QTest::newRow("pdf-light") << QStringLiteral("pdf") << false;
        QTest::newRow("pdf-dark-minimum") << QStringLiteral("pdf") << true;
    }
    void fileDropRoutes() {
        QFETCH(QString, kind); QFETCH(bool, compact);
        if (compact) { window->resize(900, 600); app->setTheme("dark"); }
        StreamServer server("openai");
        QVERIFY(app->saveProvider({{"name", "Offline"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "offline"}}).isEmpty());
        QUrl url;
        if (kind == "txt") url = importText(QStringLiteral("拖入 文档 #100%.TXT"));
        else { url = QUrl::fromLocalFile(directory->filePath(QStringLiteral("拖入 文档 #100%.PDF"))); makeSharedTextPdfFixture(url.toLocalFile()); }
        QMimeData mime; mime.setUrls({url}); QVERIFY(enterFiles(mime));
        QVERIFY(object("fileDropOverlay")->property("visible").toBool());
        QVERIFY(object("fileDropTitle")->property("text").toString().contains(QStringLiteral("松开")));
        QCOMPARE(object("fileDropDetail")->property("text").toString(), QFileInfo(url.toLocalFile()).fileName());
        capture("file-drop-" + QString::fromLatin1(QTest::currentDataTag()));
        QVERIFY(dropFiles(mime)); QVERIFY(!object("fileDropOverlay")->property("visible").toBool());
        QCOMPARE(window->property("currentPage").toInt(), kind == "txt" ? 4 : 5);
        if (kind == "txt") QCOMPARE(documents->sourceText(), QStringLiteral("Original text"));
        else { QTRY_VERIFY_WITH_TIMEOUT(pdf->loaded() && !pdf->processing(), 20000); QCOMPARE(pdf->pageCount(), 1); }
        QCOMPARE(server.requestCount, 0); QVERIFY(!documents->busy()); QVERIFY(!pdf->busy());
    }
    void fileDropRejectsInvalid_data() {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"multiple", "directory", "remote", "unsupported", "missing"}) QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void fileDropRejectsInvalid() {
        QFETCH(QString, kind);
        const auto old = importText("old.txt", "Keep this document"); QVERIFY(fileImport->requestOpen(old));
        click("navigation-0"); app->setProperty("inputText", "Keep this input");
        QList<QUrl> urls;
        if (kind == "multiple") urls = {old, importText("next.txt")};
        else if (kind == "directory") urls = {QUrl::fromLocalFile(directory->path())};
        else if (kind == "remote") urls = {QUrl("https://example.com/document.pdf")};
        else if (kind == "unsupported") urls = {importText("image.png")};
        else urls = {QUrl::fromLocalFile(directory->filePath("missing.txt"))};
        QMimeData mime; mime.setUrls(urls); QVERIFY(enterFiles(mime));
        QCOMPARE(object("fileDropTitle")->property("text").toString(), QStringLiteral("暂时无法导入"));
        QVERIFY(dropFiles(mime)); QCOMPARE(documents->sourceText(), QStringLiteral("Keep this document"));
        QCOMPARE(app->property("inputText").toString(), QStringLiteral("Keep this input")); QCOMPARE(window->property("currentPage").toInt(), 0);
        QVERIFY(!object("fileDropOverlay")->property("visible").toBool());
    }
    void fileDropLeaveAndBusyTask() {
        StreamServer server("openai");
        QVERIFY(app->saveProvider({{"name", "Offline"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "offline"}}).isEmpty());
        QVERIFY(fileImport->requestOpen(importText("old.txt"))); documents->start(); QTRY_VERIFY(server.ready);
        QMimeData mime; mime.setUrls({importText("next.txt", "Next document")}); QVERIFY(enterFiles(mime));
        QVERIFY(object("fileDropDetail")->property("text").toString().contains(QStringLiteral("停止 TXT")));
        QDragLeaveEvent leave; QCoreApplication::sendEvent(window, &leave); QCoreApplication::processEvents();
        QVERIFY(!object("fileDropOverlay")->property("visible").toBool()); QVERIFY(documents->busy());
        QVERIFY(enterFiles(mime)); QVERIFY(dropFiles(mime)); QCOMPARE(documents->sourceText(), QStringLiteral("Original text"));
        QCOMPARE(server.requestCount, 1); QVERIFY(documents->busy()); documents->cancel();
        QVERIFY(enterFiles(mime)); QVERIFY(object("fileDropTitle")->property("text").toString().contains(QStringLiteral("松开")));
        QVERIFY(dropFiles(mime)); QCOMPARE(documents->sourceText(), QStringLiteral("Next document"));
    }
    void fileDropEncodingAndPlainTextDrag() {
        click("navigation-4"); auto *encoding = object("documentEncoding");
        QVERIFY(encoding->setProperty("currentIndex", 3)); QVERIFY(QMetaObject::invokeMethod(encoding, "activated", Q_ARG(int, 3)));
        QCOMPARE(fileImport->textEncoding(), QStringLiteral("utf16le")); click("navigation-0");
        QMimeData mime; mime.setUrls({importText(QStringLiteral("编码.TXT"), QByteArray::fromHex("fffe2d4e8765"))});
        QVERIFY(enterFiles(mime)); QVERIFY(dropFiles(mime)); QCOMPARE(documents->sourceText(), QStringLiteral("中文"));
        QMimeData text; text.setText("ordinary selected text"); enterFiles(text);
        QVERIFY(!object("fileDropOverlay")->property("visible").toBool());
        QDragLeaveEvent leave; QCoreApplication::sendEvent(window, &leave);
    }
    void fileDropReplacementDialog() {
        StreamServer server("openai");
        QVERIFY(app->saveProvider({{"name", "Offline"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "offline"}}).isEmpty());
        QVERIFY(fileImport->requestOpen(importText("old.txt"))); click("documentTranslateButton"); QTRY_VERIFY(server.ready);
        server.sendText(QStringLiteral("旧译文")); server.finish({}); QTRY_VERIFY(!documents->busy());
        QMimeData mime; mime.setUrls({importText("new.txt", "New document")});
        QVERIFY(enterFiles(mime)); QVERIFY(dropFiles(mime)); QTRY_VERIFY(object("fileReplaceDialog")->property("opened").toBool());
        capture("file-drop-replace"); QCOMPARE(documents->resultText(), QStringLiteral("旧译文"));
        click("fileReplaceCancel"); QTRY_VERIFY(!object("fileReplaceDialog")->property("visible").toBool());
        QVERIFY(documents->hasUnexportedResult()); QCOMPARE(documents->sourceText(), QStringLiteral("Original text"));
        QVERIFY(enterFiles(mime)); QVERIFY(dropFiles(mime)); QTRY_VERIFY(object("fileReplaceDialog")->property("opened").toBool());
        click("fileReplaceDiscard"); QTRY_COMPARE(documents->sourceText(), QStringLiteral("New document"));
        QTRY_VERIFY(!object("fileReplaceDialog")->property("visible").toBool()); QVERIFY(documents->resultText().isEmpty());
    }
    void fileDialogUsesReplacementProtectionAndExport() {
        StreamServer server("openai");
        QVERIFY(app->saveProvider({{"name", "Offline"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "offline"}}).isEmpty());
        QVERIFY(fileImport->requestOpen(importText("old.txt"))); click("documentTranslateButton"); QTRY_VERIFY(server.ready);
        server.sendText(QStringLiteral("旧译文")); server.finish({}); QTRY_VERIFY(!documents->busy());
        const auto next = importText("next.txt", "Next document");
        auto *openDialog = object("documentOpenDialog"); QVERIFY(openDialog->setProperty("selectedFile", next));
        QVERIFY(QMetaObject::invokeMethod(openDialog, "accepted")); QTRY_VERIFY(object("fileReplaceDialog")->property("opened").toBool());
        click("fileReplaceExport"); auto *saveDialog = object("fileReplaceSaveDialog"); QTRY_VERIFY(saveDialog->property("visible").toBool());
        // Cancelling the save chooser keeps both the old result and pending import.
        QVERIFY(QMetaObject::invokeMethod(saveDialog, "reject")); QTRY_VERIFY(!saveDialog->property("visible").toBool());
        QVERIFY(fileImport->confirmationPending()); QCOMPARE(documents->resultText(), QStringLiteral("旧译文"));
        const auto destination = documents->suggestedOutput().toLocalFile();
        click("fileReplaceExport"); QTRY_VERIFY(saveDialog->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(saveDialog, "accept")); QTRY_COMPARE(documents->sourceText(), QStringLiteral("Next document"));
        QTRY_VERIFY(!object("fileReplaceDialog")->property("visible").toBool());
        QFile output(destination); QVERIFY(output.open(QIODevice::ReadOnly)); QCOMPARE(QString::fromUtf8(output.readAll()), QStringLiteral("旧译文"));
    }
    void fileDropDoesNotOverwriteModalEditor() {
        const auto path = directory->filePath("old.pdf"); makeSharedTextPdfFixture(path);
        QVERIFY(fileImport->requestOpen(QUrl::fromLocalFile(path))); QTRY_VERIFY_WITH_TIMEOUT(pdf->loaded() && !pdf->processing() && !pdf->rendering(), 20000);
        menuClick("pdfEditFirstBlock"); QTRY_VERIFY(object("pdfEditorDialog")->property("opened").toBool());
        field("pdfSourceEditor", "Unsaved proofreading");
        auto *editor = qobject_cast<QQuickItem *>(object("pdfSourceEditor")); QVERIFY(editor);
        const auto position = editor->mapToScene(QPointF(editor->width() / 2, editor->height() / 2)).toPoint();
        QVERIFY(QRect(QPoint(), window->size()).contains(position));
        QMimeData mime; mime.setUrls({importText("next.txt")}); enterFiles(mime, position); dropFiles(mime, position);
        QCOMPARE(object("pdfSourceEditor")->property("text").toString(), QStringLiteral("Unsaved proofreading"));
        QVERIFY(!object("fileDropOverlay")->property("visible").toBool()); QVERIFY(!fileImport->confirmationPending());
        QCOMPARE(pdf->fileName(), QStringLiteral("old.pdf")); QVERIFY(documents->sourceText().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(object("pdfEditorDialog"), "close")); QTRY_VERIFY(!object("pdfEditorDialog")->property("visible").toBool());
        QVERIFY(enterFiles(mime)); QVERIFY(dropFiles(mime)); QCOMPARE(window->property("currentPage").toInt(), 4);
    }
    void fileDropNativeWindows_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("txt") << QStringLiteral("txt"); QTest::newRow("pdf") << QStringLiteral("pdf");
    }
    void fileDropNativeWindows() {
#ifdef Q_OS_WIN
        if (!qEnvironmentVariableIntValue("YIYI_TEST_NATIVE_DROP") || QGuiApplication::platformName() != "windows")
            QSKIP("Set YIYI_TEST_NATIVE_DROP=1 on an interactive Windows desktop to exercise native OLE file dragging.");
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) QSKIP("The left mouse button is already in use.");
        QFETCH(QString, kind);
        QUrl url;
        if (kind == "txt") url = importText(QStringLiteral("原生 拖入 #100%.TXT"));
        else { url = QUrl::fromLocalFile(directory->filePath(QStringLiteral("原生 拖入 #100%.PDF"))); makeSharedTextPdfFixture(url.toLocalFile()); }
        struct RestorePointer {
            QPoint position = QCursor::pos();
            ~RestorePointer() { INPUT release{}; release.type = INPUT_MOUSE; release.mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1, &release, sizeof(INPUT)); QCursor::setPos(position); }
        } restore;
        const auto screen = window->screen()->availableGeometry();
        window->setPosition(screen.topLeft() + QPoint(20, 20)); window->showNormal(); window->raise();
        NativeFileDragSource source; source.file = url; source.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint); source.resize(120, 60);
        source.move(screen.bottomRight() - QPoint(130, 70)); source.show(); QVERIFY(QTest::qWaitForWindowExposed(source.windowHandle()));
        source.activateWindow(); source.raise(); QVERIFY(QTest::qWaitForWindowActive(source.windowHandle()));
        // Window activation can precede the compositor's show animation and
        // final placement. Inspect native hit targets after they have settled.
        QTest::qWait(200);
        // Native drag loops can suspend Qt timers. Drive OS input from a thread
        // that does not touch Qt objects or depend on the GUI event dispatcher.
        RECT targetRect{}, sourceRect{};
        const auto targetWindow = reinterpret_cast<HWND>(window->winId()), sourceWindow = reinterpret_cast<HWND>(source.winId());
        QVERIFY(GetWindowRect(targetWindow, &targetRect)); QVERIFY(GetWindowRect(sourceWindow, &sourceRect));
        // SendInput/SetCursorPos take physical pixels, including on scaled screens.
        const QPoint target((targetRect.left + targetRect.right) / 2, (targetRect.top + targetRect.bottom) / 2);
        const QPoint start((sourceRect.left + sourceRect.right) / 2, (sourceRect.top + sourceRect.bottom) / 2);
        if (GetAncestor(WindowFromPoint(POINT{target.x(), target.y()}), GA_ROOT) != targetWindow
            || GetAncestor(WindowFromPoint(POINT{start.x(), start.y()}), GA_ROOT) != sourceWindow)
            QSKIP("Another desktop window covers the native drag source or target.");
        const auto guiThread = GetCurrentThreadId();
        std::atomic_bool finished{false}, timedOut{false};
        source.finished = &finished;
        std::thread input([start, target, guiThread, &finished, &timedOut] {
            Sleep(100); SetCursorPos(start.x(), start.y());
            INPUT press{}; press.type = INPUT_MOUSE; press.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; SendInput(1, &press, sizeof(INPUT));
            // Continue moving inside the source before crossing windows. Qt's
            // Windows drag startup waits for the next mouse-move message.
            Sleep(100); SetCursorPos(start.x() + 10, start.y());
            Sleep(70); SetCursorPos(start.x() + 20, start.y());
            Sleep(70); SetCursorPos(start.x() + 30, start.y());
            for (int step = 1; step <= 20; ++step) {
                Sleep(15); SetCursorPos(start.x() + (target.x() - start.x()) * step / 20, start.y() + (target.y() - start.y()) * step / 20);
            }
            Sleep(200);
            INPUT release{}; release.type = INPUT_MOUSE; release.mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1, &release, sizeof(INPUT));
            for (int i = 0; i < 100 && !finished; ++i) Sleep(50);
            if (!finished) {
                timedOut = true;
                INPUT escape[2]{}; escape[0].type = escape[1].type = INPUT_KEYBOARD;
                escape[0].ki.wVk = escape[1].ki.wVk = VK_ESCAPE; escape[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, escape, sizeof(INPUT));
                PostThreadMessage(guiThread, WM_QUIT, 1, 0);
            }
        });
        QElapsedTimer elapsed; elapsed.start();
        while (!source.completed && elapsed.elapsed() < 8000) QTest::qWait(20);
        finished = true; input.join();
        QVERIFY2(!timedOut, qPrintable(QStringLiteral("Native drag timed out: started=%1, page=%2, action=%3, target=%4,%5, source=%6,%7")
            .arg(source.started).arg(window->property("currentPage").toInt()).arg(int(source.action)).arg(target.x()).arg(target.y()).arg(start.x()).arg(start.y())));
        QVERIFY(source.started); QVERIFY(source.completed);
        QTRY_COMPARE(window->property("currentPage").toInt(), kind == "txt" ? 4 : 5);
        QCOMPARE(source.action, Qt::CopyAction);
        if (kind == "txt") QCOMPARE(documents->sourceText(), QStringLiteral("Original text"));
        else QTRY_VERIFY_WITH_TIMEOUT(pdf->loaded() && !pdf->processing(), 20000);
        QVERIFY(QFile::exists(url.toLocalFile())); QVERIFY(!documents->busy()); QVERIFY(!pdf->busy());
#else
        QSKIP("Native file dragging is Windows-only.");
#endif
    }
    void pdfColumnDrag_data() {
        QTest::addColumn<QSize>("size"); QTest::newRow("default") << QSize(1000, 680); QTest::newRow("minimum") << QSize(900, 600);
    }
    void pdfColumnDrag() {
        QFETCH(QSize, size); window->resize(size); const auto source = directory->filePath("columns.pdf"); makeStructuredPdfFixture(source);
        click("navigation-5"); QVERIFY(pdf->loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf->loaded(), 20000); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        menuClick("pdfStructureButton"); QTRY_VERIFY(object("pdfStructureDialog")->property("opened").toBool());
        auto *mode = object("pdfStructureMode"); QVERIFY(mode->setProperty("currentIndex", 1)); QVERIFY(QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, 1)));
        QVariantMap column; for (const auto &v : pdf->layoutRegions()) if (v.toMap()["kind"] == "column") { column = v.toMap(); break; } QVERIFY(!column.isEmpty());
        const auto id = column["id"].toString(); const auto rectangle = column["rect"].toList(); click(("pdfStructureRegion_" + id).toUtf8().constData());
        auto *handle = qobject_cast<QQuickItem *>(object(("pdfStructureHandle_" + id + "_3").toUtf8().constData())); QVERIFY(handle);
        const auto from = handle->mapToScene(QPointF(handle->width() / 2, handle->height() / 2)).toPoint(), to = from - QPoint(6, 0);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from); QTest::mouseMove(window, to); QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to);
        QVERIFY(object("pdfStructureWidth")->property("text").toString().toDouble() < rectangle[2].toDouble());
        click("pdfStructureApply"); QVERIFY2(pdf->error().isEmpty(), qUtf8Printable(pdf->error()));
        QVariantList revised; for (const auto &v : pdf->layoutRegions()) if (v.toMap()["id"] == id) revised = v.toMap()["rect"].toList();
        QCOMPARE(revised[0], rectangle[0]); QVERIFY(revised[2].toDouble() < rectangle[2].toDouble());
        click("pdfStructureUndo"); click("pdfStructureClose"); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
    }
    void pdfBackgroundWorkflow_data() {
        QTest::addColumn<QSize>("size"); QTest::newRow("default") << QSize(1000, 680); QTest::newRow("minimum") << QSize(900, 600);
    }
    void pdfBackgroundWorkflow() {
        QFETCH(QSize, size); window->resize(size); const auto source = directory->filePath("background.pdf");
        const auto fixture = makeBackgroundFixture("texture"); saveBackgroundPdf(source, fixture);
        QFile original(source); QVERIFY(original.open(QIODevice::ReadOnly)); const auto hash = QString::fromLatin1(QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256).toHex());
        QJsonArray masks; QRectF box;
        for (const auto &line : fixture.lines) { const QRectF r(line.x() / 2.0, line.y() / 2.0, line.width() / 2.0, line.height() / 2.0);
            masks.append(QJsonArray{r.x(), r.y(), r.width(), r.height()}); box = box.isEmpty() ? r : box.united(r); }
        const QJsonObject block{{"id", "caption"}, {"source", fixture.source}, {"translation", QStringLiteral("阅读图片上的文字，保留背景。")},
            {"rect", QJsonArray{box.x(), box.y(), box.width(), box.height()}}, {"region", QJsonArray{box.x(), box.y(), box.width(), box.height() + 8}},
            {"masks", masks}, {"size", 15}, {"color", QJsonArray{255, 255, 255}}, {"scan", true}, {"background", QJsonObject{{"kind", "texture"}, {"textColor", QJsonArray{255, 255, 255}}}}};
        QDir().mkpath(directory->filePath("pdf-tasks")); QFile cache(directory->filePath("pdf-tasks/" + hash + ".json")); QVERIFY(cache.open(QIODevice::WriteOnly));
        cache.write(QJsonDocument(QJsonObject{{"version", 4}, {"hash", hash}, {"scans", QJsonArray{QJsonObject{{"page", 0}, {"scan", QJsonObject{{"source", block["source"]}, {"translation", block["translation"]}, {"blocks", QJsonArray{block}}}}}}}}).toJson()); cache.close();
        click("navigation-5"); QVERIFY(pdf->loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf->loaded(), 20000); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QCOMPARE(pdf->blocks().size(), 1); menuClick("pdfEditFirstBlock"); QTRY_VERIFY(object("pdfEditorDialog")->property("opened").toBool());
        auto selectMode = [&](int index) { auto *mode = object("pdfBackgroundMode"); mode->setProperty("currentIndex", index); QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, index)); };
        selectMode(1); QVERIFY(!object("pdfConfirmBackground")->property("enabled").toBool());
        for (const auto *name : {"pdfSourceEditor", "pdfTranslationEditor", "pdfBackgroundMode", "pdfConfirmBackground", "pdfApplyEditButton"}) {
            auto *item = qobject_cast<QQuickItem *>(object(name)); QVERIFY(item && item->isVisible());
            QVERIFY2(QRectF(QPointF(0, 0), window->size()).contains(item->mapRectToScene(item->boundingRect())), name);
            // Switching background mode adds a row; allow Qt Quick to finish
            // its deferred layout before measuring the available editor area.
            if (QByteArray(name).endsWith("Editor")) QTRY_VERIFY2_WITH_TIMEOUT(item->height() >= 75, qPrintable(QString::fromLatin1(name) + ": " + QString::number(item->height())), 2000);
        }
        capture("pdf-background-editor-" + QString::fromLatin1(QTest::currentDataTag())); click("pdfApplyEditButton");
        QTRY_VERIFY(!object("pdfEditorDialog")->property("opened").toBool()); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QVERIFY(pdf->blocks()[0].toMap()["backgroundPreviewed"].toBool());
        menuClick("pdfEditFirstBlock"); click("pdfConfirmBackground"); click("pdfApplyEditButton");
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000); QVERIFY(pdf->blocks()[0].toMap()["backgroundConfirmed"].toBool());
        menuClick("pdfEditFirstBlock"); field("pdfBlockFontSize", "14"); QVERIFY(!object("pdfConfirmBackground")->property("checked").toBool());
        click("pdfApplyEditButton"); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000); QVERIFY(!pdf->blocks()[0].toMap()["backgroundConfirmed"].toBool());
        menuClick("pdfEditFirstBlock"); selectMode(2); field("pdfPanelColor", "#143044"); click("pdfApplyEditButton");
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000); QCOMPARE(pdf->blocks()[0].toMap()["panelColor"].toString(), QStringLiteral("#143044"));
        menuClick("pdfEditFirstBlock"); selectMode(3); click("pdfApplyEditButton"); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QCOMPARE(QImage(pdf->originalPreview().toLocalFile()), QImage(pdf->translatedPreview().toLocalFile()));
        QCOMPARE(pdf->pagePreservedRegions(), 1); QVERIFY(pdf->scanTranslation().contains(QStringLiteral("保留背景")));
    }
    void pdfStructureWorkflow_data() {
        QTest::addColumn<QSize>("size"); QTest::newRow("default") << QSize(1000, 680); QTest::newRow("minimum") << QSize(900, 600);
    }
    void pdfStructureWorkflow() {
        QFETCH(QSize, size); window->resize(size); const auto source = directory->filePath("structure.pdf"); makeBorderlessPdfFixture(source);
        click("navigation-5"); QVERIFY(pdf->loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf->loaded(), 20000); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        menuClick("pdfStructureButton"); QTRY_VERIFY(object("pdfStructureDialog")->property("opened").toBool());
        const auto first = pdf->blocks()[0].toMap(); const auto firstName = ("pdfStructureRegion_" + first["id"].toString()).toUtf8(); click(firstName.constData());
        QVERIFY(object("pdfStructureOrder")->setProperty("value", 2)); click("pdfStructureMove"); QCOMPARE(pdf->blocks()[1].toMap()["id"], first["id"]);
        click("pdfStructureUndo"); QCOMPARE(pdf->blocks()[0].toMap()["id"], first["id"]);
        // Merge adjacent borderless header cells through the real controls.
        click("pdfStructureMultiple"); click(firstName.constData());
        const auto secondName = ("pdfStructureRegion_" + pdf->blocks()[1].toMap()["id"].toString()).toUtf8(); click(secondName.constData());
        auto *mergeButton = qobject_cast<QQuickItem *>(object("pdfStructureMerge")); QVERIFY(mergeButton);
        // Reveal the lower part of the settings pane at the minimum window size.
        wheel(mergeButton, -240); click("pdfStructureMerge"); QCOMPARE(pdf->blocks().size(), 12);
        click("pdfStructureUndo"); QCOMPARE(pdf->blocks().size(), 13);
        click("pdfStructureMultiple"); click(firstName.constData());
        click("pdfStructureSplit"); QTRY_VERIFY(object("pdfSplitDialog")->property("opened").toBool());
        auto *direction = object("pdfSplitDirection"); QVERIFY(direction->setProperty("currentIndex", 1)); QVERIFY(QMetaObject::invokeMethod(direction, "activated", Q_ARG(int, 1)));
        field("pdfSplitSourceA", "Part A"); field("pdfSplitSourceB", "Part B"); click("pdfConfirmSplit");
        QTRY_VERIFY(!object("pdfSplitDialog")->property("opened").toBool()); QCOMPARE(pdf->blocks().size(), 14);
        click("pdfStructureUndo"); QCOMPARE(pdf->blocks().size(), 13);
        auto *mode = object("pdfStructureMode"); QVERIFY(mode); QVERIFY(mode->setProperty("currentIndex", 2)); QVERIFY(QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, 2)));
        QString table; for (const auto &v : pdf->layoutRegions()) if (v.toMap()["kind"] == "table") table = v.toMap()["id"].toString(); QVERIFY(!table.isEmpty());
        const auto tableName = ("pdfStructureRegion_" + table).toUtf8(); click(tableName.constData());
        QVERIFY(object("pdfTableHeaders")->setProperty("value", 2)); click("pdfTableApplyHeaders");
        bool changed = false; for (const auto &v : pdf->layoutRegions()) if (v.toMap()["id"] == table) changed = v.toMap()["headerRows"].toInt() == 2; QVERIFY(changed);
        capture("pdf-structure-" + QString::fromLatin1(QTest::currentDataTag()));
        click("pdfStructureClose"); QTRY_VERIFY(!object("pdfStructureDialog")->property("opened").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
    }
    void pdfWorkflow_data() {
        QTest::addColumn<QSize>("size");
        QTest::newRow("default") << QSize(1000, 680);
        QTest::newRow("minimum") << QSize(900, 600);
    }
    void pdfWorkflow() {
        QFETCH(QSize, size); window->resize(size);
        const auto source = directory->filePath("sample.pdf"); makePdfFixture(source, 2);
        StreamServer server("openai");
        QVERIFY(app->saveProvider({{"name", "PDF test"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "pdf-test"}}).isEmpty());
        click("navigation-5"); QVERIFY(pdf->loadFile(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(pdf->loaded(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QVERIFY2(!pdf->originalPreview().isEmpty(), qUtf8Printable(pdf->error()));
        QCOMPARE(pdf->pageCount(), 2); QCOMPARE(pdf->total(), 6);
        QCOMPARE(pdf->pageTotal(), 3); QCOMPARE(pdf->pageCompleted(), 0);
        QVERIFY(object("pdfTranslatedPageStatus")->property("text").toString().contains(QStringLiteral("当前显示原文")));
        for (const auto *name : {"pdfImportButton", "pdfTranslateButton", "pdfNextPage", "pdfExportButton", "pdfPreviewArea"}) {
            auto *item = qobject_cast<QQuickItem *>(object(name)); QVERIFY(item); QVERIFY(item->isVisible());
            QVERIFY2(QRectF(QPointF(0, 0), window->size()).contains(item->mapRectToScene(item->boundingRect())), name);
        }
        object("pdfScope")->setProperty("currentIndex", 0); click("pdfTranslateButton");
        for (int i = 0; i < pdf->total(); ++i) {
            QTRY_VERIFY(server.ready);
            server.sendText(QStringLiteral("文档译文第 %1 段。").arg(i + 1)); server.finish({});
            server.ready = false; server.request.clear();
            QTRY_COMPARE(pdf->completed(), i + 1);
        }
        QTRY_VERIFY(!pdf->busy()); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QCOMPARE(pdf->pageCompleted(), 3);
        QCOMPARE(object("pdfTranslateButton")->property("text").toString(), QStringLiteral("已完成"));
        QVERIFY(object("pdfTranslatedPageStatus")->property("text").toString().contains(QStringLiteral("译文已生成")));
        click("pdfNextPage"); QCOMPARE(pdf->page(), 1); click("pdfPreviousPage"); QCOMPARE(pdf->page(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        // Exercise the actual hit region, not only the toolbar shortcut.
        QVERIFY(object("pdfBlock_p0-b1")); click("pdfBlock_p0-b1");
        QTRY_VERIFY(object("pdfEditorDialog")->property("opened").toBool());
        field("pdfTranslationEditor", QStringLiteral("点击段落修改后的正文。")); click("pdfApplyEditButton");
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        menuClick("pdfEditFirstBlock");
        field("pdfTranslationEditor", QStringLiteral("校对后的中文标题")); click("pdfApplyEditButton");
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QCOMPARE(pdf->blocks().first().toMap()["translation"].toString(), QStringLiteral("校对后的中文标题"));
        auto *zoom = object("pdfZoom"); QVERIFY(zoom); zoom->setProperty("currentIndex", 3);
        QVERIFY(QMetaObject::invokeMethod(zoom, "activated", Q_ARG(int, 3)));
        auto *viewer = qobject_cast<QQuickItem *>(object("pdfViewer")); QVERIFY(viewer);
        QTRY_VERIFY(viewer->property("contentWidth").toReal() > viewer->width() * 1.8);
        wheel(viewer, -120); QVERIFY(viewer->property("contentY").toReal() > 0);
        wheel(viewer, 120); QVERIFY(qAbs(viewer->property("contentY").toReal()) < .5);
        zoom->setProperty("currentIndex", 0); QVERIFY(QMetaObject::invokeMethod(zoom, "activated", Q_ARG(int, 0)));
        const auto output = directory->filePath("result.pdf"); QVERIFY(pdf->exportFile(QUrl::fromLocalFile(output)));
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->processing(), 20000); QVERIFY2(QFileInfo::exists(output), qUtf8Printable(pdf->error()));
        capture("pdf-light-" + QString::number(size.width())); app->setTheme("dark"); capture("pdf-dark-" + QString::number(size.width()));
    }
    void pdfRejectedResponseCanBeInspected() {
        window->resize(QSize(900, 600));
        const auto source = directory->filePath("rejected.pdf"); makePdfFixture(source, 1);
        StreamServer server("responses");
        QVERIFY(app->saveProvider({{"name", "PDF test"}, {"protocol", "responses"}, {"baseUrl", server.url()}, {"model", "pdf-test"}}).isEmpty());
        click("navigation-5"); QVERIFY(pdf->loadFile(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(pdf->analysisComplete(), 20000); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        const auto bad = QStringLiteral("<b>异常返回</b>\u0ab0");
        click("pdfTranslateButton"); QTRY_VERIFY(server.ready); server.sendText(bad); server.finish({});
        QTRY_VERIFY(pdf->activityStatus().contains(QStringLiteral("自动重试"))); click("pdfTranslateButton");
        QVERIFY(!pdf->busy()); QVERIFY(pdf->taskPending()); QCOMPARE(pdf->completed(), 0);
        menuClick("pdfEditFirstBlock"); QTRY_VERIFY(object("pdfEditorDialog")->property("opened").toBool());
        click("pdfShowRejectedResponse"); QTRY_VERIFY(object("pdfRejectedResponseDialog")->property("opened").toBool());
        auto *text = object("pdfRejectedResponseText"); QVERIFY(text); QVERIFY(text->property("readOnly").toBool());
        QCOMPARE(text->property("text").toString(), bad); QCOMPARE(text->property("textFormat").toInt(), 0);
        click("pdfCopyRejectedResponse"); QCOMPARE(QGuiApplication::clipboard()->text(), bad);
        for (const auto *name : {"pdfRejectedResponseText", "pdfCopyRejectedResponse", "pdfRejectedResponseClose"}) {
            auto *item = qobject_cast<QQuickItem *>(object(name)); QVERIFY(item && item->isVisible());
            QVERIFY2(QRectF(QPointF(0, 0), window->size()).contains(item->mapRectToScene(item->boundingRect())), name);
        }
        capture("pdf-rejected-light"); app->setTheme("dark"); capture("pdf-rejected-dark");
        click("pdfRejectedResponseClose"); QTRY_VERIFY(!object("pdfRejectedResponseDialog")->property("opened").toBool());
        QVERIFY(object("pdfTranslationEditor")->property("text").toString().isEmpty()); QCOMPARE(server.requestCount, 1);
    }
    void pdfExportWithOriginalFallback() {
        window->resize(QSize(900, 600)); const auto source = directory->filePath("fallback.pdf"); makePdfFixture(source, 1);
        click("navigation-5"); QVERIFY(pdf->loadFile(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(pdf->analysisComplete(), 20000); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QVERIFY(pdf->editBlock(pdf->blocks()[0].toMap()["id"].toString(), QString(12000, QChar(0x6587))));
        QVERIFY(pdf->editBlock(pdf->blocks()[1].toMap()["id"].toString(), QStringLiteral("正常译文")));
        QVERIFY(pdf->exportFile(QUrl::fromLocalFile(directory->filePath("usable.pdf")), true));
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->processing(), 20000);
        QTRY_VERIFY(object("pdfExportResult")->property("opened").toBool());
        QVERIFY(object("pdfExportResultDetails")->property("text").toString().contains(QStringLiteral("1 个区域保留原文")));
        capture("pdf-export-usable-light"); app->setTheme("dark"); capture("pdf-export-usable-dark");
        click("pdfExportViewIssues"); QTRY_VERIFY(object("pdfIssuesDialog")->property("opened").toBool()); QCOMPARE(pdf->reviewPages(), 1);
    }
    void pdfSkippedPageStatus_data() {
        QTest::addColumn<QSize>("size");
        QTest::newRow("default") << QSize(1000, 680);
        QTest::newRow("minimum") << QSize(900, 600);
    }
    void pdfSkippedPageStatus() {
        QFETCH(QSize, size); window->resize(size);
        const auto source = directory->filePath("mixed.pdf"); makePdfFixture(source, 2, false, 0);
        StreamServer server("openai");
        QVERIFY(app->saveProvider({{"name", "PDF test"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "pdf-test"}}).isEmpty());
        click("navigation-5"); QVERIFY(pdf->loadFile(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(pdf->analysisComplete(), 20000); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QCOMPARE(pdf->unsupportedPages(), 1); QCOMPARE(pdf->pageTotal(), 0);
        QVERIFY(pdf->pageStatus().contains(QStringLiteral("识别并翻译")));
        QCOMPARE(object("pdfTranslateButton")->property("text").toString(), QStringLiteral("识别并翻译"));
        QVERIFY(object("pdfTranslateButton")->property("enabled").toBool());
        click("pdfIssuesButton"); QTRY_VERIFY(object("pdfIssuesDialog")->property("opened").toBool());
        QVERIFY(object("pdfIssuePage_0")); click("pdfIssuePage_1"); QCOMPARE(pdf->page(), 1);
        click("pdfIssuesClose"); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        click("pdfTranslateButton");
        for (int i = 0; i < 3; ++i) {
            QTRY_VERIFY(server.ready); server.sendText(QStringLiteral("中文译文。")); server.finish({});
            server.ready = false; server.request.clear(); QTRY_COMPARE(pdf->completed(), i + 1);
        }
        QTRY_VERIFY(!pdf->busy()); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QCOMPARE(pdf->pendingPages(), 1); QCOMPARE(object("pdfTranslateButton")->property("text").toString(), QStringLiteral("已完成"));
        click("pdfPreviousPage"); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QCOMPARE(QImage(pdf->originalPreview().toLocalFile()), QImage(pdf->translatedPreview().toLocalFile()));
        capture("pdf-skipped-" + QString::number(size.width()));
        const auto gradient = directory->filePath("gradient-status.pdf"); makeShadingPdfFixture(gradient);
        QVERIFY(pdf->loadFile(QUrl::fromLocalFile(gradient)));
        QTRY_COMPARE_WITH_TIMEOUT(pdf->fileName(), QStringLiteral("gradient-status.pdf"), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QCOMPARE(pdf->unsupportedPages(), 0); QVERIFY(pdf->warning().isEmpty());
        click("pdfIssuesButton"); QTRY_VERIFY(object("pdfIssuesDialog")->property("opened").toBool());
        auto *notice = qobject_cast<QQuickItem *>(object("pdfPageDetail")); QVERIFY(notice && notice->isVisible());
        QVERIFY(notice->property("text").toString().contains(QStringLiteral("图像保留背景")));
        click("pdfIssuesClose"); QVERIFY(object("pdfTranslateButton")->property("enabled").toBool());
    }
    void pdfScanWorkflow_data() {
        QTest::addColumn<QSize>("size");
        QTest::newRow("default") << QSize(1000, 680); QTest::newRow("minimum") << QSize(900, 600);
    }
    void pdfScanWorkflow() {
        QFETCH(QSize, size); window->resize(size);
        const auto source = directory->filePath("scan.pdf"); makeScanPdfFixture(source);
        StreamServer server("openai");
        QVERIFY(app->saveProvider({{"name", "Local vision"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "vision-test"}}).isEmpty());
        pdf->setVisionProviderId(app->selectedProviderId()); click("navigation-5"); QVERIFY(pdf->loadFile(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(pdf->loaded(), 20000); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QVERIFY(pdf->scanMode()); QCOMPARE(server.requestCount, 0);
        click("pdfMoreButton"); QTRY_VERIFY(object("pdfMoreMenu")->property("opened").toBool());
        for (const auto *name : {"pdfRecognizeCurrent", "pdfRecognizeRange", "pdfCropButton", "pdfViewer", "pdfExportButton"}) {
            auto *item = qobject_cast<QQuickItem *>(object(name)); QVERIFY(item); QVERIFY(item->isVisible());
            QVERIFY2(QRectF(QPointF(0, 0), window->size()).contains(item->mapRectToScene(item->boundingRect())), name);
        }
        click("pdfRecognizeCurrent");
        for (const auto &text : {"Scanned document", "Please close the door.", "Keep the original page for comparison."}) {
            QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000); QVERIFY(server.request.contains("data:image"));
            QVERIFY(object("pdfTranslateButton")->property("enabled").toBool());
            QCOMPARE(object("pdfTranslateButton")->property("text").toString(), QStringLiteral("暂停"));
            server.sendText(text); server.finish({}); server.ready = false; server.request.clear();
        }
        QTRY_VERIFY(!pdf->busy()); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QCOMPARE(pdf->blocks().size(), 3); QCOMPARE(server.requestCount, 3);
        menuClick("pdfEditFirstBlock"); QTRY_VERIFY(object("pdfEditorDialog")->property("opened").toBool());
        field("pdfSourceEditor", QStringLiteral("Please open the door.")); click("pdfCopyBlockSource");
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Please open the door."));
        capture("pdf-scan-editor-" + QString::number(size.width()));
        field("pdfTranslationEditor", QStringLiteral("请打开门。")); click("pdfApplyEditButton");
        QTRY_VERIFY(!object("pdfEditorDialog")->property("opened").toBool()); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QCOMPARE(pdf->blocks().first().toMap()["source"].toString(), QStringLiteral("Please open the door."));
        menuClick("pdfTextView"); QTRY_VERIFY(qobject_cast<QQuickItem *>(object("pdfScanEditor"))->isVisible());
        QVERIFY(object("pdfScanEditor")->property("readOnly").toBool());
        click("pdfCopyScanText"); QCOMPARE(QGuiApplication::clipboard()->text(), pdf->scanSource());
        menuClick("pdfTextView"); click("pdfTranslateButton");
        for (int i = 0; i < 2; ++i) {
            QTRY_VERIFY(server.ready); QVERIFY(!server.request.contains("data:image"));
            server.sendText(QStringLiteral("保留原文页面。")); server.finish({}); server.ready = false; server.request.clear();
        }
        QTRY_VERIFY(!pdf->busy());
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        const auto output = directory->filePath("comparison.pdf"); QVERIFY(pdf->exportFile(QUrl::fromLocalFile(output)));
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->processing(), 20000); QVERIFY2(QFileInfo::exists(output), qUtf8Printable(pdf->error()));
        capture("pdf-scan-light-" + QString::number(size.width())); app->setTheme("dark"); capture("pdf-scan-dark-" + QString::number(size.width()));
        const auto savedSource = pdf->scanSource();
        menuClick("pdfCropButton"); QTRY_VERIFY(object("pdfCropDialog")->property("opened").toBool());
        auto *image = qobject_cast<QQuickItem *>(object("pdfCropImage")); QVERIFY(image);
        const auto from = image->mapToScene(QPointF(image->width() * .3, image->height() * .15)).toPoint();
        const auto to = image->mapToScene(QPointF(image->width() * .7, image->height() * .35)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from); QTest::mouseMove(window, to); QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to);
        server.ready = false; server.request.clear(); click("pdfRecognizeCrop"); QTRY_VERIFY(server.ready);
        server.sendText("window"); server.finish({}); QTRY_VERIFY(object("pdfRegionResult")->property("opened").toBool());
        QCOMPARE(object("pdfRegionEditor")->property("text").toString(), QStringLiteral("window")); QCOMPARE(pdf->scanSource(), savedSource);
    }
    void pdfCompactControls_data() {
        QTest::addColumn<QSize>("size"); QTest::newRow("default") << QSize(1000, 680); QTest::newRow("minimum") << QSize(900, 600);
    }
    void pdfCompactControls() {
        QFETCH(QSize, size); window->resize(size);
        const auto source = directory->filePath("compact.pdf"); makePdfFixture(source, 2);
        StreamServer server("openai");
        QVERIFY(app->saveProvider({{"name", "Offline UI"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "actual-pdf-model"}}).isEmpty());
        click("navigation-5"); QVERIFY(pdf->loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf->analysisComplete(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        auto *preview = qobject_cast<QQuickItem *>(object("pdfPreviewArea")); QVERIFY(preview); QVERIFY(preview->height() >= size.height() - 240);
        click("pdfSettingsButton"); QTRY_VERIFY(object("pdfSettingsDialog")->property("opened").toBool());
        for (const auto *name : {"pdfProvider", "pdfVisionProvider", "pdfRecognitionQuality", "pdfRequestTimeout", "pdfSettingsDone"}) {
            auto *item = qobject_cast<QQuickItem *>(object(name)); QVERIFY(item && item->isVisible());
            QVERIFY2(QRectF(QPointF(0, 0), window->size()).contains(item->mapRectToScene(item->boundingRect())), name);
        }
        auto *timeout = object("pdfRequestTimeout"); timeout->setProperty("currentIndex", 2);
        QVERIFY(QMetaObject::invokeMethod(timeout, "activated", Q_ARG(int, 2))); QCOMPARE(pdf->requestTimeoutMinutes(), 20);
        capture("pdf-settings-" + QString::number(size.width())); click("pdfSettingsDone");
        // Editable page number has no spin buttons and supports keyboard navigation.
        click("pdfPageNumber"); QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier); QTest::keyClick(window, Qt::Key_2); QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(pdf->page(), 1); click("pdfPreviousPage"); QCOMPARE(pdf->page(), 0);
        const auto id = pdf->blocks().first().toMap()["id"].toString();
        QVERIFY(pdf->reviseBlock(id, {{"source", "Title [unclear]"}})); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QCOMPARE(pdf->reviewPages(), 1); click("pdfIssuesButton"); QTRY_VERIFY(object("pdfIssuesDialog")->property("opened").toBool());
        // ListView positions its new delegates during the next render polish.
        auto *issueList = qobject_cast<QQuickItem *>(object("pdfIssueBlocks")); QVERIFY(issueList);
        issueList->ensurePolished(); QTest::qWait(60);
        capture("pdf-issues-" + QString::number(size.width()));
        click(("pdfIssueBlock_" + id).toUtf8().constData()); QTRY_VERIFY(object("pdfEditorDialog")->property("opened").toBool());
        QCOMPARE(object("pdfSourceEditor")->property("text").toString(), QStringLiteral("Title [unclear]"));
        field("pdfSourceEditor", "Confirmed title"); click("pdfApplyEditButton"); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000); QCOMPARE(pdf->reviewPages(), 0);
        menuClick("pdfRetranslatePage"); QTRY_VERIFY(server.ready);
        QCOMPARE(object("pdfTranslateButton")->property("text").toString(), QStringLiteral("暂停")); click("pdfTranslateButton");
        QVERIFY(!pdf->busy()); QVERIFY(pdf->taskPending()); QCOMPARE(object("pdfTranslateButton")->property("text").toString(), QStringLiteral("继续"));
        QCOMPARE(object("pdfActualModel")->property("text").toString(), QStringLiteral("模型 · actual-pdf-model"));
        menuClick("pdfDiscardTask"); QVERIFY(!pdf->taskPending());
        QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QTRY_COMPARE(object("pdfTranslatedPageImage")->property("status").toInt(), 1);
        menuClick("pdfReadingMode"); QTRY_VERIFY(preview->width() > size.width() - 100);
        capture("pdf-reading-light-" + QString::number(size.width())); app->setTheme("dark");
        capture("pdf-reading-dark-" + QString::number(size.width())); menuClick("pdfReadingMode");
        menuClick("pdfResetButton"); click("pdfConfirmReset"); QTRY_VERIFY(!pdf->processing()); QVERIFY(pdf->taskDescription().isEmpty());
    }
    void pdfUnifiedScanButton() {
        const auto source = directory->filePath("unified.pdf"); makeScanPdfFixture(source);
        StreamServer server("openai"); QVERIFY(app->saveProvider({{"name", "Offline vision"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "vision-model"}}).isEmpty());
        pdf->setVisionProviderId(app->selectedProviderId()); click("navigation-5"); QVERIFY(pdf->loadFile(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(pdf->loaded(), 20000); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QCOMPARE(object("pdfTranslateButton")->property("text").toString(), QStringLiteral("识别并翻译")); click("pdfTranslateButton");
        QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000); click("pdfTranslateButton"); QVERIFY(pdf->taskPending()); QVERIFY(!pdf->busy()); QVERIFY(pdf->error().isEmpty());
        server.ready = false; server.request.clear(); click("pdfTranslateButton");
        for (int i = 0; i < 6; ++i) {
            QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000); QCOMPARE(server.request.contains("data:image"), i < 3);
            server.sendText(i < 3 ? "Original text" : QStringLiteral("完整译文")); server.finish({}); server.ready = false; server.request.clear();
        }
        QTRY_VERIFY(!pdf->busy()); QTRY_VERIFY_WITH_TIMEOUT(!pdf->rendering(), 20000);
        QCOMPARE(pdf->completed(), 3); QVERIFY(!pdf->taskPending()); QCOMPARE(object("pdfTranslateButton")->property("text").toString(), QStringLiteral("已完成"));
    }
    void initTestCase() {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        QGuiApplication::setQuitOnLastWindowClosed(false);
        originalCursorPosition = QCursor::pos();
    }
    void cleanupTestCase() {
        if (QGuiApplication::platformName() == QLatin1String("windows")) QCursor::setPos(originalCursorPosition);
    }
    void init() {
        directory = std::make_unique<QTemporaryDir>(); QVERIFY(directory->isValid());
        if (QByteArray(QTest::currentTestFunction()).startsWith("libraryScrolling")) {
            Storage storage(directory->path());
            for (int i = 0; i < 80; ++i) {
                const QString text = i % 2 ? QStringLiteral("Short text") : QString(600, QLatin1Char('W'));
                QVERIFY(storage.add({{"input", QStringLiteral("Record %1").arg(i)}, {"output", text}, {"kind", "translation"}, {"source", "English"}, {"target", "Chinese"}, {"provider", "Test"}, {"model", "test"}}) > 0);
            }
        }
        app = std::make_unique<AppController>(directory->path());
        desktop = std::make_unique<DesktopIntegration>(directory->path(), false);
        documents = std::make_unique<DocumentController>(app.get());
        pdf = std::make_unique<PdfController>(app.get());
        office = std::make_unique<OfficeController>(app.get());
        fileImport = std::make_unique<FileImportController>(documents.get(), pdf.get(), office.get());
        captureController = std::make_unique<ScreenshotController>();
        vision = std::make_unique<VisionController>(app.get());
        connect(office.get(), &OfficeController::imageReviewRequested, captureController.get(), [this](const QImage &image) { if (!vision->busy()) captureController->reviewImage(image); });
        connect(captureController.get(), &ScreenshotController::imageChanged, vision.get(), [this] { vision->setImage(captureController->image()); });
        app->setTheme(QStringLiteral("light"));
        engine = std::make_unique<QQmlApplicationEngine>();
        engine->rootContext()->setContextProperty(QStringLiteral("App"), app.get());
        engine->rootContext()->setContextProperty(QStringLiteral("Desktop"), desktop.get());
        engine->rootContext()->setContextProperty(QStringLiteral("Documents"), documents.get());
        engine->rootContext()->setContextProperty(QStringLiteral("Pdf"), pdf.get());
        engine->rootContext()->setContextProperty(QStringLiteral("FileImport"), fileImport.get());
        engine->rootContext()->setContextProperty(QStringLiteral("Office"), office.get());
        engine->rootContext()->setContextProperty(QStringLiteral("Capture"), captureController.get());
        engine->rootContext()->setContextProperty(QStringLiteral("Vision"), vision.get());
        engine->rootContext()->setContextProperty(QStringLiteral("initialPage"), QStringLiteral("translate"));
        engine->rootContext()->setContextProperty(QStringLiteral("themeOverride"), QString());
        engine->load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
        QVERIFY(!engine->rootObjects().isEmpty());
        window = qobject_cast<QQuickWindow *>(engine->rootObjects().first()); QVERIFY(window);
        desktop->setWindow(window);
        captureController->setWindow(window);
        connect(desktop.get(), &DesktopIntegration::lookupRequested, app.get(), &AppController::lookupSelection);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTest::qWait(80);
    }
    void cleanup() { engine.reset(); fileImport.reset(); office.reset(); pdf.reset(); captureController.reset(); vision.reset(); documents.reset(); desktop.reset(); app.reset(); directory.reset(); window = nullptr; }

    void officeWorkflow_data() {
        QTest::addColumn<QString>("kind"); QTest::addColumn<QSize>("size"); QTest::addColumn<QString>("theme");
        QTest::newRow("word-default-light") << QStringLiteral("docx") << QSize(1000, 680) << QStringLiteral("light");
        QTest::newRow("word-minimum-dark") << QStringLiteral("docx") << QSize(900, 600) << QStringLiteral("dark");
        QTest::newRow("ppt-default-light") << QStringLiteral("pptx") << QSize(1000, 680) << QStringLiteral("light");
        QTest::newRow("ppt-minimum-dark") << QStringLiteral("pptx") << QSize(900, 600) << QStringLiteral("dark");
    }
    void officeWorkflow() {
        QFETCH(QString, kind); QFETCH(QSize, size); QFETCH(QString, theme);
        window->resize(size); app->setTheme(theme);
        const auto path = QStringLiteral(YIYI_SOURCE_DIR "/tests/fixtures/office/sample.") + kind;
        StreamServer server("openai");
        QVERIFY(app->saveProvider({{"name", "Office UI test"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "office-ui"}}).isEmpty());
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(path)}); QVERIFY(enterFiles(mime)); dropFiles(mime);
        QTRY_COMPARE(window->property("currentPage").toInt(), 4); QTRY_VERIFY(!office->processing()); QVERIFY2(office->loaded(), qPrintable(office->error()));
        for (const auto *name : {"officeImportButton", "officeOptionsButton", "officeSegmentList", "officeSourceEditor", "officeTranslationEditor", "officeApplyEdit", "officeExportButton", "officeTranslateButton"}) {
            auto *item = qobject_cast<QQuickItem *>(object(name)); QVERIFY2(item && item->isVisible(), name);
            QTRY_VERIFY2(QRectF(QPointF(0, 0), window->size()).contains(item->mapRectToScene(item->boundingRect())), name);
        }
        auto *editor = qobject_cast<QQuickItem *>(object("officeTranslationEditor")); QVERIFY(editor); editor->forceActiveFocus();
        field("officeTranslationEditor", QStringLiteral("校对中的译文")); QVERIFY(office->draftDirty());
        office->setFitText(!office->fitText()); QCOMPARE(editor->property("text").toString(), QStringLiteral("校对中的译文"));
        click("officeApplyEdit"); QVERIFY(!office->draftDirty()); QCOMPARE(office->completed(), 1);
        click("officeRetrySegment"); QTRY_VERIFY(server.ready); server.sendText(QStringLiteral("重译后的段落")); server.finish({}); QTRY_VERIFY(!office->busy());
        QCOMPARE(editor->property("text").toString(), QStringLiteral("重译后的段落"));
        capture("office-" + QString::fromLatin1(QTest::currentDataTag()));
        click("officeIssuesButton"); QTRY_VERIFY(object("officeIssuesDialog")->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape); QTRY_VERIFY(!object("officeIssuesDialog")->property("opened").toBool());
        click("documentTxtTab"); QVERIFY(object("documentImportButton")->property("visible").toBool());
        click("documentOfficeTab"); QCOMPARE(office->completed(), 1); QCOMPARE(editor->property("text").toString(), QStringLiteral("重译后的段落"));
        click("officeImagesButton"); QTRY_VERIFY(object("officeImagesDialog")->property("opened").toBool());
        click("officeReviewImage_0"); QTRY_VERIFY(object("screenshotDialog")->property("opened").toBool());
        QVERIFY(captureController->hasImage()); QCOMPARE(captureController->mode(), QStringLiteral("vision")); QVERIFY(!vision->busy());
        QTest::keyClick(window, Qt::Key_Escape); QTRY_VERIFY(!object("screenshotDialog")->property("opened").toBool());
    }
    void officeLanguageChangeUsesActualTaskSettings_data() {
        QTest::addColumn<QString>("kind"); QTest::addColumn<QString>("protocol");
        for (const auto *kind : {"docx", "pptx"}) for (const auto *protocol : {"openai", "responses"})
            QTest::newRow(qPrintable(QString::fromLatin1(kind) + '-' + protocol)) << QString::fromLatin1(kind) << QString::fromLatin1(protocol);
    }
    void officeLanguageChangeUsesActualTaskSettings() {
        QFETCH(QString, kind); QFETCH(QString, protocol); window->resize(900, 600);
        StreamServer server(protocol);
        QVERIFY(app->saveProvider({{"name", "Language UI test"}, {"protocol", protocol}, {"baseUrl", server.url()}, {"model", "offline-language"}}).isEmpty());
        const auto path = QUrl::fromLocalFile(QStringLiteral(YIYI_SOURCE_DIR "/tests/fixtures/office/sample.") + kind);
        QVERIFY(fileImport->requestOpen(path)); QTRY_VERIFY(!office->processing()); QVERIFY(office->loaded());
        const auto id = office->segments().first().toMap()["id"].toString();
        QVERIFY(office->retrySegment(id)); QTRY_VERIFY(server.ready); server.sendText(QStringLiteral("已有的中文译文")); server.finish({}); QTRY_VERIFY(!office->busy());
        app->setTargetLanguage(QStringLiteral("英语"));
        QVERIFY(office->loadFile(path)); QTRY_VERIFY(!office->processing());
        auto *target = object("officeTargetLanguage"); QVERIFY(target);
        QTRY_COMPARE(target->property("currentText").toString(), QStringLiteral("简体中文"));
        QCOMPARE(app->targetLanguage(), QStringLiteral("英语")); QCOMPARE(office->completed(), 1);
        auto chooseEnglish = [&] {
            target->setProperty("currentIndex", 2);
            QVERIFY(QMetaObject::invokeMethod(target, "activated", Q_ARG(int, 2)));
        };
        chooseEnglish(); QTRY_VERIFY(object("officeSettingsChangeDialog")->property("opened").toBool());
        QCOMPARE(office->completed(), 1); QCOMPARE(server.requestCount, 1);
        capture("office-language-confirm-" + QString::fromLatin1(QTest::currentDataTag()));
        click("officeSettingsChangeCancel"); QTRY_VERIFY(!object("officeSettingsChangeDialog")->property("opened").toBool());
        QCOMPARE(target->property("currentText").toString(), QStringLiteral("简体中文"));
        QCOMPARE(office->segments().first().toMap()["translation"].toString(), QStringLiteral("已有的中文译文"));
        chooseEnglish(); QTRY_VERIFY(object("officeSettingsChangeDialog")->property("opened").toBool());
        click("officeSettingsChangeConfirm"); QTRY_VERIFY(!object("officeSettingsChangeDialog")->property("opened").toBool());
        QCOMPARE(target->property("currentText").toString(), QStringLiteral("英语"));
        QVERIFY(office->taskSettings().isEmpty()); QCOMPARE(office->completed(), 0);
        server.ready = false; server.request.clear(); click("officeRetrySegment"); QTRY_VERIFY(server.ready);
        QVERIFY(server.request.contains(QStringLiteral("to 英语").toUtf8())); QCOMPARE(office->taskSettings()["target"].toString(), QStringLiteral("英语"));
        app->setTargetLanguage(QStringLiteral("日语")); QCOMPARE(target->property("currentText").toString(), QStringLiteral("英语"));
        server.sendText("A practical guide"); server.finish({}); QTRY_VERIFY(!office->busy());
        QCOMPARE(object("officeTranslationEditor")->property("text").toString(), QStringLiteral("A practical guide"));
        QCOMPARE(server.requestCount, 2);
        capture("office-language-english-" + QString::fromLatin1(QTest::currentDataTag()));
    }
    void officeCompletedTaskCanRestart_data() {
        QTest::addColumn<QString>("kind"); QTest::addColumn<QString>("protocol");
        for (const auto *kind : {"docx", "pptx"}) for (const auto *protocol : {"openai", "responses"})
            QTest::newRow(qPrintable(QString::fromLatin1(kind) + '-' + protocol)) << QString::fromLatin1(kind) << QString::fromLatin1(protocol);
    }
    void officeCompletedTaskCanRestart() {
        QFETCH(QString, kind); QFETCH(QString, protocol); window->resize(900, 600);
        StreamServer server(protocol);
        QVERIFY(app->saveProvider({{"name", "Completed task test"}, {"protocol", protocol}, {"baseUrl", server.url()}, {"model", "offline-completed"}}).isEmpty());
        const auto path = QUrl::fromLocalFile(QStringLiteral(YIYI_SOURCE_DIR "/tests/fixtures/office/sample.") + kind);
        QVERIFY(fileImport->requestOpen(path)); QTRY_VERIFY(!office->processing()); QVERIFY(office->loaded());
        const auto id = office->segments().first().toMap()["id"].toString();
        QVERIFY(office->retrySegment(id)); QTRY_VERIFY(server.ready); server.sendText(QStringLiteral("旧中文译文")); server.finish({}); QTRY_VERIFY(!office->busy());
        for (const auto &value : office->segments()) QVERIFY(office->editSegment(value.toMap()["id"].toString(), QStringLiteral("旧中文译文")));
        app->setTargetLanguage(QStringLiteral("英语"));
        QVERIFY(office->loadFile(path)); QTRY_VERIFY(!office->processing()); QCOMPARE(office->completed(), office->total()); QVERIFY(!office->canResume());
        auto *editor = qobject_cast<QQuickItem *>(object("officeTranslationEditor")); QVERIFY(editor); editor->forceActiveFocus();
        field("officeTranslationEditor", QStringLiteral("未保存的校对草稿")); QVERIFY(office->draftDirty());
        auto *translate = object("officeTranslateButton"); QVERIFY(translate);
        QVERIFY2(translate->property("enabled").toBool(), "A completed task must offer an enabled restart action.");
        QCOMPARE(translate->property("text").toString(), QStringLiteral("重新翻译"));
        capture("office-completed-" + QString::fromLatin1(QTest::currentDataTag()));
        click("officeTranslateButton"); QTRY_VERIFY(object("officeRestartDialog")->property("opened").toBool());
        QCOMPARE(office->completed(), office->total()); QVERIFY(office->draftDirty()); QCOMPARE(server.requestCount, 1);
        click("officeRestartCancel"); QTRY_VERIFY(!object("officeRestartDialog")->property("opened").toBool());
        QCOMPARE(office->completed(), office->total()); QCOMPARE(office->draftText(), QStringLiteral("未保存的校对草稿"));
        auto *target = object("officeTargetLanguage"); QVERIFY(target); QCOMPARE(target->property("currentText").toString(), QStringLiteral("简体中文"));
        target->setProperty("currentIndex", 2); QVERIFY(QMetaObject::invokeMethod(target, "activated", Q_ARG(int, 2)));
        QTRY_VERIFY(object("officeSettingsChangeDialog")->property("opened").toBool()); click("officeSettingsChangeConfirm");
        QTRY_VERIFY(!object("officeSettingsChangeDialog")->property("opened").toBool());
        QCOMPARE(office->completed(), 0); QVERIFY(!office->draftDirty()); QCOMPARE(target->property("currentText").toString(), QStringLiteral("英语"));
        QCOMPARE(translate->property("text").toString(), QStringLiteral("开始翻译")); QVERIFY(translate->property("enabled").toBool());
        // Stop after one confirmed paragraph, before a subsequent request is queued.
        connect(office.get(), &OfficeController::changed, office.get(), [this] { if (office->busy() && office->completed() == 1) office->cancel(); });
        server.ready = false; server.request.clear(); click("officeTranslateButton"); QTRY_VERIFY(server.ready);
        QVERIFY(server.request.contains(QStringLiteral("to 英语").toUtf8()));
        server.sendText("New English translation"); server.finish({}); QTRY_VERIFY(!office->busy()); QCOMPARE(office->completed(), 1);
        for (const auto &value : office->segments()) QVERIFY(office->editSegment(value.toMap()["id"].toString(), "Saved English translation"));
        app->setTargetLanguage(QStringLiteral("日语")); QCOMPARE(target->property("currentText").toString(), QStringLiteral("英语"));
        click("officeTranslateButton"); QTRY_VERIFY(object("officeRestartDialog")->property("opened").toBool());
        server.ready = false; server.request.clear(); click("officeRestartConfirm"); QTRY_VERIFY(server.ready);
        QVERIFY(server.request.contains(QStringLiteral("to 英语").toUtf8())); QCOMPARE(office->completed(), 0);
        QCOMPARE(office->taskSettings()["target"].toString(), QStringLiteral("英语"));
        server.sendText("Restarted English translation"); server.finish({}); QTRY_VERIFY(!office->busy());
        QCOMPARE(office->completed(), 1); QCOMPARE(server.requestCount, 3);
        QCOMPARE(object("officeTranslationEditor")->property("text").toString(), QStringLiteral("Restarted English translation"));
    }
    void officeIssueNavigationPreservesDraft() {
        QVERIFY(fileImport->requestOpen(QUrl::fromLocalFile(QStringLiteral(YIYI_SOURCE_DIR "/tests/fixtures/office/sample.docx"))));
        QTRY_VERIFY(!office->processing()); QVERIFY(office->loaded());
        auto *editor = qobject_cast<QQuickItem *>(object("officeTranslationEditor")); QVERIFY(editor); editor->forceActiveFocus();
        const auto source = object("officeSourceEditor")->property("text").toString();
        field("officeTranslationEditor", QStringLiteral("定位前的校对草稿")); QVERIFY(office->draftDirty());
        click("officeIssuesButton"); QTRY_VERIFY(object("officeIssuesDialog")->property("opened").toBool());
        click("officeIssueLocate_0"); QTRY_VERIFY(object("officeDiscardDialog")->property("opened").toBool());
        QCOMPARE(object("officeSourceEditor")->property("text").toString(), source);
        QVERIFY(QMetaObject::invokeMethod(object("officeDiscardDialog"), "reject"));
        QTRY_VERIFY(!object("officeDiscardDialog")->property("opened").toBool());
        QCOMPARE(editor->property("text").toString(), QStringLiteral("定位前的校对草稿")); QVERIFY(office->draftDirty());
        click("officeIssuesButton"); QTRY_VERIFY(object("officeIssuesDialog")->property("opened").toBool());
        click("officeIssueLocate_0"); QTRY_VERIFY(object("officeDiscardDialog")->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(object("officeDiscardDialog"), "accept"));
        QTRY_VERIFY(!object("officeDiscardDialog")->property("opened").toBool());
        QVERIFY(!office->draftDirty()); QCOMPARE(office->completed(), 1);
        QCOMPARE(office->segments().first().toMap()["translation"].toString(), QStringLiteral("定位前的校对草稿"));
        QVERIFY(object("officeSourceEditor")->property("text").toString() != source);
    }
    void documentWorkflow_data() {
        QTest::addColumn<QSize>("size");
        QTest::newRow("default") << QSize(1000, 680);
        QTest::newRow("minimum") << QSize(900, 600);
    }
    void documentWorkflow() {
        QFETCH(QSize, size); window->resize(size);
        StreamServer server(QStringLiteral("openai"));
        QVERIFY(app->saveProvider({{"name", "Local test"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "small-local-model"}}).isEmpty());
        const auto path = directory->filePath(QStringLiteral("sample.txt"));
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("Hello world."); file.close();
        click("navigation-4");
        QVERIFY(documents->loadFile(QUrl::fromLocalFile(path)));
        QTest::qWait(80);
        QCOMPARE(object("documentSource")->property("text").toString(), QStringLiteral("Hello world."));
        for (const auto *name : {"documentImportButton", "documentTranslateButton", "documentExportButton", "documentSource", "documentResult"}) {
            auto *item = qobject_cast<QQuickItem *>(object(name)); QVERIFY(item); QVERIFY(item->isVisible());
            QVERIFY2(QRectF(QPointF(0, 0), window->size()).contains(item->mapRectToScene(item->boundingRect())), name);
        }
        click("documentTranslateButton"); QTRY_VERIFY(server.ready); QVERIFY(app->documentBusy());
        server.sendText(QStringLiteral("你好，世界。"));
        QTRY_VERIFY(object("documentResult")->property("text").toString().contains(QStringLiteral("你好")));
        QVERIFY(!object("documentExportButton")->property("enabled").toBool());
        server.finish({}); QTRY_VERIFY(!documents->busy());
        QCOMPARE(documents->state(), QStringLiteral("completed")); QVERIFY(!app->documentBusy());
        QVERIFY(object("documentExportButton")->property("enabled").toBool());
        QVERIFY(app->history().isEmpty());
        capture(QStringLiteral("documents-") + QString::number(size.width()));
        app->setTheme(QStringLiteral("dark"));
        capture(QStringLiteral("documents-dark-") + QString::number(size.width()));
        click("documentResetButton"); QCOMPARE(documents->completed(), 0);
        QVERIFY(documents->resultText().isEmpty());
    }

    void ocrReviewUsesEditedText_data() {
        QTest::addColumn<bool>("hasImage");
        QTest::newRow("empty-capture") << false;
        QTest::newRow("retained-image") << true;
    }
    void ocrReviewUsesEditedText() {
        QFETCH(bool, hasImage);
        window->resize(900, 600);
        if (hasImage) {
            QImage image(400,100,QImage::Format_RGB32); image.fill(Qt::white);
            captureController->setMode("vision"); captureController->acceptImage(image); captureController->setMode("local");
        }
        auto *dialog = object("screenshotDialog"); QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open")); QTest::qWait(100);
        field("ocrEditor", QStringLiteral("Corrected OCR text"));
        QCOMPARE(captureController->text(), QStringLiteral("Corrected OCR text"));
        click("screenshotCopyButton"); QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Corrected OCR text"));
        captureController->setMode("vision"); field("ocrEditor", QStringLiteral("Independent AI draft"));
        captureController->setMode("local"); QCOMPARE(object("ocrEditor")->property("text").toString(), QStringLiteral("Corrected OCR text"));
        capture(QStringLiteral("ocr-review"));
        app->setTheme(QStringLiteral("dark")); capture(QStringLiteral("ocr-review-dark"));
        click("ocrUseButton");
        QCOMPARE(app->property("inputText").toString(), QStringLiteral("Corrected OCR text"));
        QVERIFY(!app->busy()); QVERIFY(!app->example()); QCOMPARE(window->property("currentPage").toInt(), 0);
        QVERIFY(app->history().isEmpty());
        QVERIFY(captureController->text().isEmpty()); QVERIFY(vision->resultText().isEmpty());
    }

    void visionScreenshotWorkflow_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("translate");
        for (const auto &protocol : {QStringLiteral("openai"), QStringLiteral("responses")})
            for (bool translate : {false, true})
                QTest::newRow(qPrintable(protocol + (translate ? "-translate" : "-use"))) << protocol << translate;
    }
    void visionScreenshotWorkflow() {
        QFETCH(QString, protocol); QFETCH(bool, translate);
        window->resize(900,600); StreamServer server(protocol), textServer("openai");
        QVERIFY(app->saveProvider({{"name","Text service"},{"protocol","openai"},{"baseUrl",textServer.url()},{"model","text-test"}}).isEmpty());
        const auto textId = app->selectedProviderId();
        QVERIFY(app->saveProvider({{"name","Vision service"},{"protocol",protocol},{"baseUrl",server.url()},{"model","vision-test"}}).isEmpty());
        vision->setProviderId(app->providers().last().toMap()["id"].toString()); app->selectProvider(textId);
        captureController->setMode("vision"); QImage image(480,100,QImage::Format_RGB32); image.fill(Qt::white);
        QPainter painter(&image); painter.setPen(Qt::black); QFont font("Segoe UI"); font.setPixelSize(28); painter.setFont(font); painter.drawText(image.rect(),Qt::AlignCenter,"Please close the door."); painter.end();
        captureController->acceptImage(image);
        auto *dialog = object("screenshotDialog"); QVERIFY(QMetaObject::invokeMethod(dialog,"open")); QTest::qWait(80);
        QCOMPARE(server.requestCount,0); QCOMPARE(textServer.requestCount,0);
        QVERIFY(object("screenshotTranslationProvider")->property("text").toString().contains("text-test"));
        for (const auto *name : {"screenshotMode","visionProvider","screenshotTarget","screenshotRecognizeButton","screenshotCopyButton","ocrEditor","ocrUseButton","ocrTranslateButton"}) {
            auto *item=qobject_cast<QQuickItem *>(object(name)); QVERIFY(item); QVERIFY(item->isVisible());
            QVERIFY2(QRectF(QPointF(0,0),window->size()).contains(item->mapRectToScene(item->boundingRect())),name);
        }
        click("screenshotRecognizeButton"); QTRY_VERIFY(server.ready); QVERIFY(vision->busy());
        server.sendText(QStringLiteral("Please close the door.")); QTRY_COMPARE(object("ocrEditor")->property("text").toString(), QStringLiteral("Please close the door."));
        QVERIFY(object("ocrEditor")->property("readOnly").toBool());
        for (const auto *name : {"screenshotCopyButton","ocrUseButton","ocrTranslateButton","screenshotMode"}) QVERIFY(!object(name)->property("enabled").toBool());
        for (const auto *name : {"screenshotCancelButton","screenshotCopyButton","ocrTranslateButton"}) {
            auto *item = qobject_cast<QQuickItem *>(object(name));
            QVERIFY(QRectF(QPointF(0,0),window->size()).contains(item->mapRectToScene(item->boundingRect())));
        }
        server.finish("Please close the door."); QTRY_VERIFY(vision->complete());
        QVERIFY(!object("ocrEditor")->property("readOnly").toBool()); QVERIFY(app->history().isEmpty()); QCOMPARE(textServer.requestCount,0);
        const auto corrected = QStringLiteral("Please close the window.\nKeep the original ENGLISH!");
        field("ocrEditor", corrected); QCOMPARE(vision->resultText(), corrected);
        captureController->setMode("local"); field("ocrEditor", "Local OCR correction.");
        click("screenshotCopyButton"); QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Local OCR correction."));
        captureController->setMode("vision"); QCOMPARE(object("ocrEditor")->property("text").toString(), corrected);
        click("screenshotCopyButton"); QCOMPARE(QGuiApplication::clipboard()->text(), corrected);
        QCOMPARE(server.requestCount,1); QCOMPARE(textServer.requestCount,0);
        capture("vision-review-light"); app->setTheme("dark"); capture("vision-review-dark");
        click(translate ? "ocrTranslateButton" : "ocrUseButton");
        QCOMPARE(app->property("inputText").toString(), corrected);
        QTRY_VERIFY(!captureController->hasImage()); QVERIFY(captureController->text().isEmpty()); QVERIFY(vision->resultText().isEmpty());
        if (translate) {
            QTRY_VERIFY(textServer.ready);
            const auto body = QJsonDocument::fromJson(textServer.request.mid(textServer.request.indexOf("\r\n\r\n") + 4)).object();
            QCOMPARE(body["model"].toString(), QStringLiteral("text-test"));
            const auto input = body["messages"].toArray().last().toObject()["content"];
            QVERIFY(input.isString()); QVERIFY(input.toString().contains(corrected)); QVERIFY(!textServer.request.contains("data:image"));
            textServer.sendText(QStringLiteral("请关窗。\n保留英文原文！")); textServer.finish({}); QTRY_VERIFY(!app->busy());
            QVERIFY(app->resultText().contains(QStringLiteral("请关窗"))); QCOMPARE(app->history().size(), 1);
        } else { QVERIFY(!app->busy()); QVERIFY(app->history().isEmpty()); QCOMPARE(textServer.requestCount,0); }
        QCOMPARE(server.requestCount,1);
    }
    void screenshotRetryRestoresDraft() {
        window->resize(900,600); StreamServer server("openai");
        QVERIFY(app->saveProvider({{"name","vision"},{"protocol","openai"},{"baseUrl",server.url()},{"model","vision-test"}}).isEmpty());
        captureController->setMode("vision"); QImage image(400,100,QImage::Format_RGB32); image.fill(Qt::white); captureController->acceptImage(image);
        auto *dialog = object("screenshotDialog"); QVERIFY(QMetaObject::invokeMethod(dialog,"open")); QTest::qWait(80);
        const auto corrected = QStringLiteral("My corrected original."); field("ocrEditor", corrected);
        click("screenshotRecognizeButton"); QTRY_VERIFY(server.ready);
        server.sendText("Partial new output"); QTRY_COMPARE(object("ocrEditor")->property("text").toString(), QStringLiteral("Partial new output"));
        click("screenshotCancelButton"); QVERIFY(!vision->busy());
        QCOMPARE(object("ocrEditor")->property("text").toString(), corrected);
        click("screenshotCopyButton"); QCOMPARE(QGuiApplication::clipboard()->text(), corrected);
        server.ready = false; server.request.clear(); click("screenshotRecognizeButton"); QTRY_VERIFY(server.ready);
        server.sendText("Truncated new output"); QTRY_COMPARE(object("ocrEditor")->property("text").toString(), QStringLiteral("Truncated new output"));
        server.interrupt(); QTRY_VERIFY(!vision->busy());
        QCOMPARE(object("ocrEditor")->property("text").toString(), corrected);
        click("screenshotCopyButton"); QCOMPARE(QGuiApplication::clipboard()->text(), corrected);
        captureController->acceptImage(image); QVERIFY(object("ocrEditor")->property("text").toString().isEmpty());
        QVERIFY(!object("screenshotCopyButton")->property("enabled").toBool());
        // Closing a live recognition discards the image and cannot restore a stale draft.
        server.ready = false; server.request.clear(); click("screenshotRecognizeButton"); QTRY_VERIFY(server.ready);
        server.sendText("Closing"); QTRY_COMPARE(object("ocrEditor")->property("text").toString(), QStringLiteral("Closing"));
        QVERIFY(QMetaObject::invokeMethod(dialog,"close")); QVERIFY(!vision->busy()); QVERIFY(!captureController->hasImage()); QVERIFY(vision->resultText().isEmpty());
    }

    void detailedDictionaryPreview_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("cancel");
        QTest::newRow("Chat complete") << QStringLiteral("openai") << false;
        QTest::newRow("Responses complete minimum") << QStringLiteral("responses") << false;
        QTest::newRow("Chat cancelled") << QStringLiteral("openai") << true;
        QTest::newRow("Responses cancelled minimum") << QStringLiteral("responses") << true;
    }

    void detailedDictionaryPreview() {
        QFETCH(QString, protocol); QFETCH(bool, cancel);
        StreamServer mock(protocol); QVERIFY(mock.server.isListening());
        if (protocol == QLatin1String("responses")) window->resize(900, 600);
        QCOMPARE(app->saveProvider({{"name", "Stream test"}, {"protocol", protocol}, {"baseUrl", mock.url()},
            {"model", "gpt-6.1-sol"}, {"reasoningEffort", "medium"}, {"dictionaryMode", "detailed"}}), QString());
        field("sourceInput", QStringLiteral("relationship")); click("translateButton");
        QTRY_VERIFY(mock.ready);
        QVERIFY(!qobject_cast<QQuickItem *>(object("resultScroll"))->isVisible());
        const auto prefix = QStringLiteral("{\"word\":\"relationship\",\"phonetic\":\"/rɪˈleɪʃənʃɪp/\",\"partOfSpeech\":\"noun · 名词\",\"definitions\":[{\"meaning\":\"关系；联系\",\"detail\":\"人与人或事物之间的联系。\"}");
        const auto suffix = QStringLiteral("],\"examples\":[{\"source\":\"They have a good relationship.\",\"translation\":\"他们关系很好。\"}]}");
        mock.sendText(prefix);
        QTRY_VERIFY(object("dictionaryMeaning-0"));
        QTRY_VERIFY(qobject_cast<QQuickItem *>(object("resultScroll"))->isVisible());
        QVERIFY(app->busy()); QCOMPARE(object("dictionaryMeaning-0")->property("text").toString(), QStringLiteral("关系；联系"));
        auto *status = qobject_cast<QQuickItem *>(object("generationStatus")); QVERIFY(status); QVERIFY(status->isVisible());
        QVERIFY(status->property("text").toString().contains(QStringLiteral("补全词条")));
        QVERIFY(QRectF(QPointF(0, 0), window->size()).contains(status->mapRectToScene(status->boundingRect())));
        QVERIFY(!object("copyResultButton")->property("enabled").toBool());
        QVERIFY(!object("favoriteButton")->property("enabled").toBool());
        QVERIFY(app->history().isEmpty());
        if (!cancel) capture(QStringLiteral("dictionary-progress-") + protocol);
        if (cancel) click("translateButton");
        else { mock.sendText(suffix); mock.finish(prefix + suffix); }
        QTRY_VERIFY(!app->busy());
        QVERIFY(!status->isVisible());
        auto *timing = qobject_cast<QQuickItem *>(object("requestTiming")); QVERIFY(timing);
        QTRY_VERIFY(timing->isVisible()); QVERIFY(timing->property("text").toString().contains(QStringLiteral("首段响应")));
        QVERIFY(QRectF(QPointF(0, 0), window->size()).contains(timing->mapRectToScene(timing->boundingRect())));
        QCOMPARE(mock.requestCount, 1);
        if (cancel) {
            QVERIFY(app->dictionary().isEmpty()); QVERIFY(app->history().isEmpty());
            QVERIFY(!qobject_cast<QQuickItem *>(object("resultScroll"))->isVisible());
        } else {
            QVERIFY2(app->error().isEmpty(), qPrintable(app->error()));
            QVERIFY(app->resultText().contains(QStringLiteral("他们关系很好。")));
            QCOMPARE(app->history().size(), 1);
            click("copyResultButton"); QCOMPARE(QGuiApplication::clipboard()->text(), app->resultText());
            click("favoriteButton"); QCOMPARE(app->favorites().size(), 1);
            capture(QStringLiteral("dictionary-completed-") + protocol);
        }
    }

    void dictionaryMalformedOptionalField_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("dictionaryMode");
        for (const auto protocol : {"openai", "responses"})
            for (const auto mode : {"brief", "detailed"})
                QTest::newRow(qPrintable(QStringLiteral("%1/%2").arg(QLatin1String(protocol), QLatin1String(mode))))
                    << QString::fromLatin1(protocol) << QString::fromLatin1(mode);
    }

    void dictionaryMalformedOptionalField() {
        QFETCH(QString, protocol); QFETCH(QString, dictionaryMode); StreamServer mock(protocol);
        QFile fixture(QFINDTESTDATA("fixtures/dictionary/hello-malformed-note.txt"));
        QVERIFY(fixture.open(QIODevice::ReadOnly)); const auto answer = QString::fromUtf8(fixture.readAll());
        QCOMPARE(app->saveProvider({{"name", "Local dictionary"}, {"protocol", protocol}, {"baseUrl", mock.url()},
            {"model", "small-local-model"}, {"dictionaryMode", dictionaryMode}}), QString());
        field("sourceInput", QStringLiteral("hello")); click("translateButton"); QTRY_VERIFY(mock.ready);
        const auto noteStart = answer.indexOf(QLatin1String(",\"note\"")); QVERIFY(noteStart > 0);
        mock.sendText(answer.left(noteStart));
        if (dictionaryMode == QLatin1String("detailed")) {
            QTRY_VERIFY(object("dictionaryMeaning-0"));
            QCOMPARE(object("dictionaryMeaning-0")->property("text").toString(), QStringLiteral("你好；喂"));
        }
        mock.sendText(answer.mid(noteStart)); mock.finish(answer); QTRY_VERIFY(!app->busy());
        QVERIFY2(app->error().isEmpty(), qPrintable(app->error())); QCOMPARE(mock.requestCount, 1);
        QCOMPARE(app->resultKind(), QStringLiteral("dictionary")); QTRY_VERIFY(object("dictionaryMeaning-0"));
        QCOMPARE(object("dictionaryMeaning-0")->property("text").toString(), QStringLiteral("你好；喂"));
        QCOMPARE(app->briefResult(), dictionaryMode == QLatin1String("brief"));
        QVERIFY(!app->resultText().contains(QLatin1String("\"definitions\""))); QCOMPARE(app->history().size(), 1);
        click("copyResultButton"); QCOMPARE(QGuiApplication::clipboard()->text(), app->resultText());
        click("favoriteButton"); QCOMPARE(app->favorites().size(), 1);
        capture(QStringLiteral("dictionary-recovered-") + protocol + QLatin1Char('-') + dictionaryMode);
    }

    void configureTranslateCopyAndFavorite_data() {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<int>("protocolIndex");
        QTest::addColumn<QString>("effort");
        QTest::addColumn<bool>("briefDictionary");
        QTest::addColumn<bool>("englishTarget");
        QTest::newRow("Chat Completions") << QStringLiteral("openai") << 0 << QString() << false << false;
        QTest::newRow("Responses") << QStringLiteral("responses") << 1 << QString() << false << false;
        QTest::newRow("Chat Completions low") << QStringLiteral("openai") << 0 << QStringLiteral("low") << false << false;
        QTest::newRow("Responses low") << QStringLiteral("responses") << 1 << QStringLiteral("low") << false << false;
        QTest::newRow("Brief dictionary Chat") << QStringLiteral("openai") << 0 << QString() << true << false;
        QTest::newRow("Brief dictionary Responses") << QStringLiteral("responses") << 1 << QString() << true << false;
        QTest::newRow("Chinese to English Chat") << QStringLiteral("openai") << 0 << QString() << true << true;
        QTest::newRow("Chinese to English Responses") << QStringLiteral("responses") << 1 << QString() << true << true;
    }

    void configureTranslateCopyAndFavorite() {
        QFETCH(QString, protocol); QFETCH(int, protocolIndex);
        QFETCH(QString, effort);
        QFETCH(bool, briefDictionary); QFETCH(bool, englishTarget);
        const auto briefAnswer = englishTarget ? QStringLiteral("POS: adj.\nTRANSLATION: interesting")
            : protocol == QLatin1String("openai") ? QStringLiteral("词性：v.\n释义：凝视") : QStringLiteral("词性：v.\n释义：看；注视");
        const auto expectedPos = englishTarget ? QStringLiteral("adj.") : QStringLiteral("v.");
        const auto expectedMeaning = englishTarget ? QStringLiteral("interesting")
            : protocol == QLatin1String("openai") ? QStringLiteral("凝视") : QStringLiteral("看");
        const auto expectedText = briefDictionary
            ? (englishTarget ? QStringLiteral("adj. interesting")
                : protocol == QLatin1String("openai") ? QStringLiteral("v. 凝视") : QStringLiteral("v. 看\n\nv. 注视"))
            : QStringLiteral("你好，世界。");
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QByteArray received;
        int requestCount = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            auto request = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [&, socket, request] {
                *request += socket->readAll();
                const auto headerEnd = request->indexOf("\r\n\r\n");
                if (headerEnd < 0 || socket->property("sent").toBool()) return;
                int length = 0;
                for (const auto &line : request->left(headerEnd).split('\n'))
                    if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                if (request->size() < headerEnd + 4 + length) return;
                socket->setProperty("sent", true);
                received = *request; ++requestCount;
                QByteArray body = protocol == QLatin1String("responses")
                    ? QStringLiteral("event: response.output_text.delta\ndata: {\"type\":\"response.output_text.delta\",\"delta\":\"你好，世界。\"}\n\nevent: response.completed\ndata: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"error\":null,\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"你好，世界。\"}]}]}}\n\n").toUtf8()
                    : QStringLiteral("data: {\"choices\":[{\"delta\":{\"content\":\"你好，世界。\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n").toUtf8();
                if (briefDictionary) {
                    const auto quoted = QJsonDocument(QJsonArray{briefAnswer}).toJson(QJsonDocument::Compact);
                    body.replace(QStringLiteral("你好，世界。").toUtf8(), quoted.mid(2, quoted.size() - 4));
                }
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
        click("translateButton");
        QCOMPARE(window->property("currentPage").toInt(), 3);
        auto *dialog = object("providerDialog"); QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        field("serviceNameField", QStringLiteral("UI test service"));
        field("endpointField", QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort()));
        field("modelField", QStringLiteral("ui-test-model"));
        field("apiKeyField", QStringLiteral("ui-secret"));
        QVERIFY(object("protocolPicker")->setProperty("currentIndex", protocolIndex));
        auto *reasoning = object("reasoningPicker"); QVERIFY(reasoning);
        QCOMPARE(reasoning->property("currentIndex").toInt(), 0);
        QVERIFY(reasoning->setProperty("currentIndex", effort.isEmpty() ? 0 : 2));
        if (protocol == QLatin1String("responses")) {
            QCOMPARE(object("protocolPicker")->property("currentText").toString(), QStringLiteral("OpenAI Responses"));
            QVERIFY(object("endpointHint")->property("text").toString().contains(QStringLiteral("/responses")));
        }
        click("testProviderButton");
        QTRY_VERIFY_WITH_TIMEOUT(!app->testBusy(), 5000);
        QVERIFY2(app->testSuccess(), qPrintable(app->testMessage()));
        QCOMPARE(requestCount, 1);
        const auto checkEffort = [&] {
            const auto payload = QJsonDocument::fromJson(received.mid(received.indexOf("\r\n\r\n") + 4)).object();
            if (effort.isEmpty()) {
                QVERIFY(!payload.contains("reasoning")); QVERIFY(!payload.contains("reasoning_effort"));
            } else if (protocol == QLatin1String("responses")) {
                QCOMPARE(payload.value("reasoning").toObject().value("effort").toString(), effort);
                QVERIFY(!payload.contains("reasoning_effort"));
            } else {
                QCOMPARE(payload.value("reasoning_effort").toString(), effort);
                QVERIFY(!payload.contains("reasoning"));
            }
        };
        checkEffort();
        QVERIFY(app->history().isEmpty());
        click("saveProviderButton");
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QCOMPARE(app->providers().size(), 1);
        QCOMPARE(app->providers().first().toMap().value("protocol").toString(), protocol);
        QCOMPARE(app->providers().first().toMap().value("reasoningEffort").toString(), effort);
        QCOMPARE(app->providers().first().toMap().value("dictionaryMode").toString(), QStringLiteral("brief"));
        QVERIFY(object("apiKeyField")->property("text").toString().isEmpty());
        window->setProperty("currentPage", 0);
        app->setMode(briefDictionary ? QStringLiteral("auto") : QStringLiteral("translation"));
        if (englishTarget) app->setTargetLanguage(QStringLiteral("英语"));
        const auto input = englishTarget ? QStringLiteral("有趣的") : briefDictionary ? QStringLiteral("look") : QStringLiteral("Hello, world.");
        field("sourceInput", input);
        QCOMPARE(app->property("inputText").toString(), input);
        QTest::qWait(50);
        click("translateButton");
        QTRY_COMPARE_WITH_TIMEOUT(app->history().size(), 1, 5000);
        QCOMPARE(app->resultText(), expectedText);
        if (briefDictionary) {
            QCOMPARE(app->resultKind(), QStringLiteral("dictionary"));
            QTRY_VERIFY(object("dictionaryMeaning-0"));
            QVERIFY(app->briefResult());
            QVERIFY(!qobject_cast<QQuickItem *>(object("dictionaryWord"))->isVisible());
            QCOMPARE(object("dictionaryPos-0")->property("text").toString(), expectedPos);
            QCOMPARE(object("dictionaryMeaning-0")->property("text").toString(), expectedMeaning);
            capture(QStringLiteral("brief-dictionary-") + protocol + (englishTarget ? QStringLiteral("-english") : QString()));
        }
        QVERIFY(received.contains("ui-test-model"));
        QCOMPARE(requestCount, 2);
        checkEffort();
        if (protocol == QLatin1String("responses")) {
            QVERIFY(received.startsWith("POST /v1/responses HTTP/1.1"));
            const auto payload = QJsonDocument::fromJson(received.mid(received.indexOf("\r\n\r\n") + 4)).object();
            QVERIFY(payload.contains("input")); QVERIFY(!payload.contains("messages"));
        }
        click("copyResultButton");
        QCOMPARE(QGuiApplication::clipboard()->text(), app->resultText());
        click("favoriteButton");
        QCOMPARE(app->favorites().size(), 1);
        window->setProperty("currentPage", 1);
        QTest::qWait(100);
        const auto recordName = QByteArray("historyRecord-") + QByteArray::number(app->history().first().toMap().value("id").toLongLong());
        click(recordName.constData());
        QCOMPARE(window->property("currentPage").toInt(), 0);
        QVERIFY(app->favorite());
        QCOMPARE(app->resultText(), expectedText);
        if (englishTarget) QCOMPARE(app->targetLanguage(), QStringLiteral("英语"));
        if (briefDictionary) QCOMPARE(app->dictionary().value("definitions").toList().first().toMap().value("partOfSpeech").toString(), expectedPos);
        click("theme-dark");
        QCOMPARE(app->theme(), QStringLiteral("dark"));
        QCOMPARE(window->color(), QColor(QStringLiteral("#131b18")));
    }

    void reasoningConfigurationEditsAndProtocolSwitches() {
        click("translateButton");
        auto *dialog = object("providerDialog"); QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        auto *picker = qobject_cast<QQuickItem *>(object("reasoningPicker")); QVERIFY(picker);
        QVERIFY(!picker->isVisible()); // The initial Ollama preset has no OpenAI-only option.
        QVERIFY(object("protocolPicker")->setProperty("currentIndex", 1));
        QTRY_VERIFY(picker->isVisible());
        QCOMPARE(picker->property("currentIndex").toInt(), 0);
        field("serviceNameField", QStringLiteral("Reasoning test"));
        field("endpointField", QStringLiteral("http://localhost:1234/v1"));
        field("modelField", QStringLiteral("gpt-6-luna"));
        field("apiKeyField", QStringLiteral("stored-test-key"));
        auto *scroll = object("providerFormScroll"); QVERIFY(scroll);
        auto *view = scroll->property("contentItem").value<QObject *>(); QVERIFY(view);
        QTest::qWait(100);
        view->setProperty("contentY", qMax(qreal(0), view->property("contentHeight").toReal() - view->property("height").toReal()));
        QTest::qWait(80);
        click("reasoningPicker");
        auto *popup = picker->property("popup").value<QObject *>(); QVERIFY(popup);
        QTRY_VERIFY(popup->property("opened").toBool());
        capture(QStringLiteral("reasoning-options"));
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!popup->property("opened").toBool());
        QCOMPARE(picker->property("currentValue").toString(), QStringLiteral("low"));
        capture(QStringLiteral("reasoning-low"));
        click("saveProviderButton");
        QTRY_VERIFY(!dialog->property("opened").toBool());
        const auto id = app->selectedProviderId();
        QCOMPARE(app->providerDraft(id).value("reasoningEffort").toString(), QStringLiteral("low"));
        const auto reopen = [&] {
            QVERIFY(QMetaObject::invokeMethod(dialog, "edit", Q_ARG(QVariant, QVariant(id)), Q_ARG(QVariant, QVariant(0))));
            QTRY_VERIFY(dialog->property("opened").toBool());
        };
        reopen();
        QCOMPARE(picker->property("currentValue").toString(), QStringLiteral("low"));
        QVERIFY(app->providerDraft(id).value("hasKey").toBool());
        QVERIFY(object("protocolPicker")->setProperty("currentIndex", 0));
        QCOMPARE(picker->property("currentValue").toString(), QStringLiteral("low"));
        for (int protocolIndex : {2, 3, 4}) {
            QVERIFY(object("protocolPicker")->setProperty("currentIndex", protocolIndex));
            QVERIFY(!picker->isVisible());
            QCOMPARE(picker->property("currentIndex").toInt(), 0);
            QVERIFY(object("protocolPicker")->setProperty("currentIndex", 1));
            QCOMPARE(picker->property("currentIndex").toInt(), 0);
            QVERIFY(picker->setProperty("currentIndex", 2));
        }
        auto *preset = object("presetPicker"); QVERIFY(preset);
        QVERIFY(preset->setProperty("currentIndex", 1)); // LM Studio: explicitly reset even though both protocols are compatible.
        QVERIFY(QMetaObject::invokeMethod(preset, "activated", Q_ARG(int, 1)));
        QCOMPARE(picker->property("currentIndex").toInt(), 0);
        QVERIFY(object("protocolPicker")->setProperty("currentIndex", 1));
        QVERIFY(picker->setProperty("currentIndex", 2));
        QVERIFY(preset->setProperty("currentIndex", 8));
        QVERIFY(QMetaObject::invokeMethod(preset, "activated", Q_ARG(int, 8)));
        QCOMPARE(picker->property("currentIndex").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("opened").toBool());
        reopen(); // Unsaved preset changes must not affect the stored provider or its key.
        QCOMPARE(picker->property("currentValue").toString(), QStringLiteral("low"));
        QVERIFY(picker->setProperty("currentIndex", 1));
        QVERIFY(object("reasoningHint")->property("text").toString().contains(QStringLiteral("需要模型支持")));
        click("saveProviderButton");
        QCOMPARE(app->providerDraft(id).value("reasoningEffort").toString(), QStringLiteral("none"));
        QVERIFY(app->providerDraft(id).value("hasKey").toBool());
        reopen();
        QCOMPARE(picker->property("currentValue").toString(), QStringLiteral("none"));
        QVERIFY(picker->setProperty("currentIndex", 0));
        click("saveProviderButton");
        QVERIFY(!app->providerDraft(id).contains("reasoningEffort"));
        reopen();
        QCOMPARE(picker->property("currentIndex").toInt(), 0);
    }

    void dictionaryModeSettingPersists_data() {
        QTest::addColumn<QSize>("size"); QTest::addColumn<QString>("theme");
        QTest::newRow("default light") << QSize(1000, 680) << QStringLiteral("light");
        QTest::newRow("minimum dark") << QSize(900, 600) << QStringLiteral("dark");
    }

    void dictionaryModeSettingPersists() {
        QFETCH(QSize, size); QFETCH(QString, theme);
        window->resize(size); app->setTheme(theme);
        click("translateButton");
        auto *dialog = object("providerDialog"); QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        auto *picker = qobject_cast<QQuickItem *>(object("dictionaryModePicker")); QVERIFY(picker);
        QCOMPARE(picker->property("currentValue").toString(), QStringLiteral("brief"));
        field("modelField", QStringLiteral("small-local-model"));
        auto *scroll = object("providerFormScroll"); QVERIFY(scroll);
        auto *view = scroll->property("contentItem").value<QObject *>(); QVERIFY(view);
        QTest::qWait(80);
        view->setProperty("contentY", qMax(qreal(0), view->property("contentHeight").toReal() - view->property("height").toReal()));
        QTest::qWait(80);
        const auto position = picker->mapToScene(QPointF(0, 0));
        QVERIFY(position.y() > 0); QVERIFY(position.y() + picker->height() < window->height());
        click("dictionaryModePicker");
        auto *popup = picker->property("popup").value<QObject *>(); QVERIFY(popup);
        QTRY_VERIFY(popup->property("opened").toBool());
        capture(QStringLiteral("dictionary-mode-options-") + theme);
        QTest::keyClick(window, Qt::Key_Down); QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!popup->property("opened").toBool());
        QCOMPARE(picker->property("currentValue").toString(), QStringLiteral("detailed"));
        click("saveProviderButton");
        QTRY_VERIFY(!dialog->property("opened").toBool());
        const auto id = app->selectedProviderId();
        QCOMPARE(app->providerDraft(id).value("dictionaryMode").toString(), QStringLiteral("detailed"));
        const auto reopen = [&] {
            QVERIFY(QMetaObject::invokeMethod(dialog, "edit", Q_ARG(QVariant, QVariant(id)), Q_ARG(QVariant, QVariant(0))));
            QTRY_VERIFY(dialog->property("opened").toBool());
        };
        reopen();
        QCOMPARE(picker->property("currentValue").toString(), QStringLiteral("detailed"));
        QVERIFY(object("protocolPicker")->setProperty("currentIndex", 1));
        QCOMPARE(picker->property("currentValue").toString(), QStringLiteral("detailed"));
        QVERIFY(picker->setProperty("currentIndex", 0));
        click("saveProviderButton");
        QCOMPARE(app->providerDraft(id).value("dictionaryMode").toString(), QStringLiteral("brief"));
        reopen();
        QCOMPARE(picker->property("currentValue").toString(), QStringLiteral("brief"));
        capture(QStringLiteral("dictionary-mode-brief-") + theme);
        QVERIFY(picker->setProperty("currentIndex", 1));
        auto *preset = object("presetPicker"); QVERIFY(preset);
        QVERIFY(preset->setProperty("currentIndex", 1));
        QVERIFY(QMetaObject::invokeMethod(preset, "activated", Q_ARG(int, 1)));
        QCOMPARE(picker->property("currentValue").toString(), QStringLiteral("brief"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QCOMPARE(app->providerDraft(id).value("model").toString(), QStringLiteral("small-local-model"));
    }

    void invalidProviderIsNotSaved() {
        click("translateButton");
        auto *dialog = object("providerDialog"); QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        field("endpointField", QStringLiteral("not-a-url"));
        click("saveProviderButton");
        QVERIFY(dialog->property("opened").toBool());
        QVERIFY(!dialog->property("formError").toString().isEmpty());
        QVERIFY(app->providers().isEmpty());
        click("testProviderButton");
        QVERIFY(!app->testBusy()); QVERIFY(!app->testMessage().isEmpty());
        QVERIFY(!app->testSuccess());
    }

    void primaryButtonContrast() {
        const auto button = object("translateButton"); QVERIFY(button);
        QCOMPARE(button->property("kind").toString(), QStringLiteral("primary"));
        QCOMPARE(button->property("foreground").value<QColor>(), QColor(Qt::white));
    }

    void scrollBoundaryStopsWithoutRebound() {
        auto *scroll = object("resultScroll"); QVERIFY(scroll);
        auto *flickable = scroll->property("contentItem").value<QObject *>(); QVERIFY(flickable);
        const qreal bottom = qMax(qreal(0), flickable->property("contentHeight").toReal() - flickable->property("height").toReal());
        QVERIFY(bottom > 10);
        flickable->setProperty("contentY", bottom - 2);
        QVERIFY(QMetaObject::invokeMethod(flickable, "flick", Q_ARG(qreal, 0.0), Q_ARG(qreal, -2000.0)));
        for (int i = 0; i < 35; ++i) {
            QTest::qWait(16);
            const qreal y = flickable->property("contentY").toReal();
            QVERIFY2(y <= bottom + 0.5, qPrintable(QStringLiteral("Content overshot its bottom by %1 pixels").arg(y - bottom)));
        }
        QVERIFY(qAbs(flickable->property("contentY").toReal() - bottom) <= 0.5);
        flickable->setProperty("contentY", 2);
        QVERIFY(QMetaObject::invokeMethod(flickable, "flick", Q_ARG(qreal, 0.0), Q_ARG(qreal, 2000.0)));
        for (int i = 0; i < 35; ++i) {
            QTest::qWait(16);
            QVERIFY(flickable->property("contentY").toReal() >= -0.5);
        }
        QVERIFY(qAbs(flickable->property("contentY").toReal()) <= 0.5);
    }

    void wheelChangesDirectionImmediately_data() {
        QTest::addColumn<QByteArray>("name");
        QTest::newRow("translation output") << QByteArray("resultScroll");
        QTest::newRow("source editor") << QByteArray("inputScroll");
        QTest::newRow("settings") << QByteArray("settingsScroll");
        QTest::newRow("provider form") << QByteArray("providerFormScroll");
    }

    void wheelChangesDirectionImmediately() {
        QFETCH(QByteArray, name);
        window->resize(1100, 680);
        QTest::qWait(80);
        if (name == "inputScroll") field("sourceInput", QStringLiteral("A paragraph of text.\n").repeated(80));
        else if (name == "settingsScroll") window->setProperty("currentPage", 3);
        else if (name == "providerFormScroll") {
            click("translateButton");
            QTRY_VERIFY(object("providerDialog")->property("opened").toBool());
        }
        QTest::qWait(80);
        auto *scroll = qobject_cast<QQuickItem *>(object(name.constData())); QVERIFY(scroll);
        auto *view = scroll->property("contentItem").value<QObject *>(); QVERIFY(view);
        const qreal bottom = qMax(qreal(0), view->property("contentHeight").toReal() - view->property("height").toReal());
        QVERIFY(bottom > 25);
        const qreal step = qMax(1, QGuiApplication::styleHints()->wheelScrollLines()) * 20;
        view->setProperty("contentY", bottom - 2);
        wheel(scroll, -120);
        QVERIFY(qAbs(view->property("contentY").toReal() - bottom) < 0.5);
        wheel(scroll, -120);
        QVERIFY(qAbs(view->property("contentY").toReal() - bottom) < 0.5);
        wheel(scroll, 120);
        QVERIFY(qAbs(view->property("contentY").toReal() - qMax(qreal(0), bottom - step)) < 0.5);
        view->setProperty("contentY", 2);
        wheel(scroll, 120);
        QVERIFY(qAbs(view->property("contentY").toReal()) < 0.5);
        wheel(scroll, 120);
        wheel(scroll, -120);
        QVERIFY(qAbs(view->property("contentY").toReal() - qMin(bottom, step)) < 0.5);
        view->setProperty("contentY", 0);
        wheel(scroll, -17, true);
        QVERIFY(qAbs(view->property("contentY").toReal() - 17) < 0.5);
        wheel(scroll, 7, true);
        QVERIFY(qAbs(view->property("contentY").toReal() - 10) < 0.5);
    }

    void scrollThumbReachesBothEnds() {
        auto *scroll = object("resultScroll"); QVERIFY(scroll);
        auto *view = scroll->property("contentItem").value<QObject *>(); QVERIFY(view);
        auto *bar = qobject_cast<QQuickItem *>(object("resultScrollBar")); QVERIFY(bar);
        auto *thumb = bar->property("contentItem").value<QQuickItem *>(); QVERIFY(thumb);
        const qreal bottom = view->property("contentHeight").toReal() - view->property("height").toReal();
        const auto drag = [this, bar, thumb](qreal endY) {
            const auto start = thumb->mapToScene(QPointF(thumb->width() / 2, thumb->height() / 2)).toPoint();
            const auto end = bar->mapToScene(QPointF(bar->width() / 2, endY)).toPoint();
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(window, end, 20);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
            QCoreApplication::processEvents();
        };
        drag(bar->height() - 1);
        QVERIFY(qAbs(view->property("contentY").toReal() - bottom) < 0.5);
        drag(1);
        QVERIFY(qAbs(view->property("contentY").toReal()) < 0.5);
    }

    void libraryScrollingHasStableExtent() {
        window->setProperty("currentPage", 1);
        QTest::qWait(80);
        auto *list = qobject_cast<QQuickItem *>(object("historyList")); QVERIFY(list);
        QCOMPARE(list->property("count").toInt(), 80);
        const qreal extent = list->property("contentHeight").toReal();
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QTest::qWait(80);
        QVERIFY(qAbs(list->property("contentHeight").toReal() - extent) < 0.5);
        const qreal bottom = list->property("originY").toReal() + extent - list->height();
        QVERIFY(qAbs(list->property("contentY").toReal() - bottom) < 0.5);
        wheel(list, -120);
        QVERIFY(qAbs(list->property("contentY").toReal() - bottom) < 0.5);
        wheel(list, 120);
        QVERIFY(list->property("contentY").toReal() < bottom - 1);
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
        QTest::qWait(80);
        QVERIFY(qAbs(list->property("contentHeight").toReal() - extent) < 0.5);
        const qreal start = list->property("contentY").toReal();
        wheel(list, 120);
        QVERIFY(qAbs(list->property("contentY").toReal() - start) < 0.5);
        wheel(list, -120);
        QVERIFY(list->property("contentY").toReal() > start + 1);
    }

    void themeHoverAndTooltipLifecycle_data() {
        QTest::addColumn<QString>("theme");
        QTest::newRow("light") << QStringLiteral("light");
        QTest::newRow("dark") << QStringLiteral("dark");
    }

    void themeHoverHasNoFlash_data() {
        QTest::addColumn<QString>("theme");
        QTest::newRow("light") << QStringLiteral("light");
        QTest::newRow("dark") << QStringLiteral("dark");
    }

    void themeHoverHasNoFlash() {
        QFETCH(QString, theme);
        window->setPosition(window->screen()->availableGeometry().center() - QPoint(window->width() / 2, window->height() / 2));
        app->setTheme(theme);
        movePointer(QPoint(400, 90));
        QTest::qWait(250);
        QList<QQuickItem *> buttons;
        for (const auto name : {"theme-light", "theme-dark", "theme-system"}) {
            auto *button = qobject_cast<QQuickItem *>(object(name)); QVERIFY(button);
            buttons.append(button);
        }
        // Sample the rendered fill throughout both entering and leaving each
        // button, including direct moves to a neighbour. Endpoints alone miss
        // a dark flash caused by interpolating RGB and alpha simultaneously.
        for (const auto index : {0, 1, 2, 1, 0, -1}) {
            const auto before = window->grabWindow(); QVERIFY(!before.isNull());
            if (index < 0) movePointer(QPoint(400, 90));
            else movePointer(buttons[index]->mapToScene(QPointF(buttons[index]->width() / 2, buttons[index]->height() / 2)).toPoint());
            QList<QList<QColor>> samples;
            for (int tick = 0; tick < 12; ++tick) {
                QTest::qWait(16);
                const auto frame = window->grabWindow(); QVERIFY(!frame.isNull());
                QList<QColor> colors;
                for (auto *button : buttons) colors.append(buttonFill(button, frame));
                samples.append(colors);
            }
            for (int i = 0; i < buttons.size(); ++i) {
                const auto first = buttonFill(buttons[i], before);
                const auto last = samples.last()[i];
                for (const auto &frame : samples) {
                    const auto color = frame[i];
                    const auto within = [](int value, int a, int b) { return value >= qMin(a, b) - 3 && value <= qMax(a, b) + 3; };
                    QVERIFY2(within(color.red(), first.red(), last.red()) && within(color.green(), first.green(), last.green()) && within(color.blue(), first.blue(), last.blue()),
                        qPrintable(QStringLiteral("%1 flashed %2 between %3 and %4")
                            .arg(buttons[i]->objectName(), color.name(), first.name(), last.name())));
                }
            }
            QCOMPARE(app->theme(), theme);
        }
    }

    void themeHoverAndTooltipLifecycle() {
        QFETCH(QString, theme);
        window->resize(1100, 680);
        window->setPosition(window->screen()->availableGeometry().center() - QPoint(window->width() / 2, window->height() / 2));
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
        app->setTheme(theme);
        QTest::qWait(250);
        click("sourceInput");
        movePointer(QPoint(400, 90));
        QTest::qWait(250);
        auto *editor = qobject_cast<QQuickItem *>(object("sourceInput")); QVERIFY(editor);
        QVERIFY(editor->hasActiveFocus());
        const auto name = (QStringLiteral("theme-") + theme).toLatin1();
        auto *button = qobject_cast<QQuickItem *>(object(name.constData())); QVERIFY(button);
        auto *background = button->property("background").value<QObject *>(); QVERIFY(background);
        auto *tip = object((name + "ToolTip").constData()); QVERIFY(tip);
        const auto restingColor = buttonFill(button, window->grabWindow());
        const auto position = button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
        movePointer(position);
        QTRY_VERIFY(button->property("hovered").toBool());
        QTest::qWait(150);
        QVERIFY(!tip->property("opened").toBool());
        const auto hoverColor = buttonFill(button, window->grabWindow());
        QVERIFY(hoverColor != restingColor);
        QTRY_VERIFY(tip->property("opened").toBool());
        for (int tick = 0; tick < 6; ++tick) {
            QTest::qWait(60);
            QVERIFY(button->property("hovered").toBool());
            QCOMPARE(buttonFill(button, window->grabWindow()), hoverColor);
        }
        auto *tipContent = tip->property("contentItem").value<QQuickItem *>(); QVERIFY(tipContent);
        const auto bounds = tipContent->mapRectToScene(tipContent->boundingRect());
        QVERIFY(QRectF(QPointF(0, 0), window->size()).contains(bounds));
        QVERIFY(bounds.bottom() < button->mapToScene(QPointF(0, 0)).y());
        capture(QStringLiteral("theme-hover-") + theme);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, position);
        QTRY_VERIFY(!tip->property("visible").toBool());
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, position);
        QCOMPARE(app->theme(), theme);
        QVERIFY(editor->hasActiveFocus());
        QCOMPARE(QQmlProperty(background, QStringLiteral("border.width")).read().toReal(), 0.0);
        QTest::qWait(700);
        QVERIFY(!tip->property("visible").toBool());
        movePointer(QPoint(400, 90));
        QTRY_VERIFY(!button->property("hovered").toBool());
        QTRY_COMPARE(buttonFill(button, window->grabWindow()), restingColor);

        auto *system = qobject_cast<QQuickItem *>(object("theme-system")); QVERIFY(system);
        auto *systemTip = object("theme-systemToolTip"); QVERIFY(systemTip);
        movePointer(system->mapToScene(QPointF(system->width() / 2, system->height() / 2)).toPoint());
        QTRY_VERIFY(system->property("hovered").toBool());
        QTRY_VERIFY2(systemTip->property("opened").toBool(), qPrintable(QStringLiteral("active=%1 hovered=%2 dismissed=%3 visible=%4")
            .arg(window->isActive()).arg(system->property("hovered").toBool())
            .arg(systemTip->property("dismissed").toBool()).arg(systemTip->property("visible").toBool())));
        QCOMPARE(systemTip->property("text").toString(), QStringLiteral("跟随系统"));
        QVERIFY(!tip->property("visible").toBool());
        capture(QStringLiteral("theme-system-hover-") + theme);
        click("theme-system");
        QCOMPARE(app->theme(), QStringLiteral("system"));
        QTRY_VERIFY(!systemTip->property("visible").toBool());
        movePointer(QPoint(400, 90));

        // Keyboard navigation still needs a visible focus indicator.
        button->forceActiveFocus(Qt::TabFocusReason);
        QTRY_COMPARE(QQmlProperty(background, QStringLiteral("border.width")).read().toReal(), 2.0);
        QTest::keyClick(window, Qt::Key_Space);
        QCOMPARE(app->theme(), theme);
    }

    void compactWindowLayout_data() {
        QTest::addColumn<QSize>("size");
        QTest::newRow("default") << QSize(1000, 680);
        QTest::newRow("minimum") << QSize(900, 600);
    }

    void desktopShortcutEditor() {
        window->resize(900, 600); window->setProperty("currentPage", 3); QTest::qWait(100);
        auto *scroll = qobject_cast<QQuickItem *>(object("settingsScroll")); QVERIFY(scroll);
        auto *view = scroll->property("contentItem").value<QObject *>(); QVERIFY(view);
        auto *show = qobject_cast<QQuickItem *>(object("showShortcutField")); QVERIFY(show);
        auto *selection = qobject_cast<QQuickItem *>(object("selectionShortcutField")); QVERIFY(selection);
        view->setProperty("contentY", show->mapToItem(scroll, QPointF()).y() - 80);
        QTest::qWait(100);
        click("showShortcutField"); QTRY_VERIFY(show->hasActiveFocus());
        QVERIFY(desktop->property("recordingShortcut").toBool());
        QTest::keyClick(window, Qt::Key_F8, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(show->property("shortcut").toString(), QStringLiteral("Ctrl+Shift+F8"));
        QVERIFY(!desktop->property("recordingShortcut").toBool());
        selection->setProperty("shortcut", QStringLiteral("Ctrl+Shift+F8"));
        click("saveShortcutsButton");
        QVERIFY(!desktop->shortcutError().isEmpty());
        QCOMPARE(desktop->showShortcut(), QStringLiteral("Ctrl+Alt+Y"));
        selection->setProperty("shortcut", QStringLiteral("Ctrl+Shift+F9"));
        click("saveShortcutsButton");
        QVERIFY(desktop->shortcutError().isEmpty());
        QCOMPARE(desktop->showShortcut(), QStringLiteral("Ctrl+Shift+F8"));
        QCOMPARE(desktop->selectionShortcut(), QStringLiteral("Ctrl+Shift+F9"));
        QTest::qWait(80);
        const auto frame = window->grabWindow(); QVERIFY(!frame.isNull());
        // Scrolled icons must not bleed into the title strip (including the
        // software renderer used on machines without a suitable GPU).
        for (int x = 216; x < 234; ++x) for (int y = 2; y < 12; ++y)
            QCOMPARE(frame.pixelColor(qRound(x * frame.width() / qreal(window->width())),
                                      qRound(y * frame.height() / qreal(window->height()))), window->color());
        capture(QStringLiteral("desktop-shortcuts-minimum"));
        show->forceActiveFocus();
        QTRY_VERIFY(desktop->property("recordingShortcut").toBool());
        window->hide(); QTest::qWait(80);
        QVERIFY(!desktop->property("recordingShortcut").toBool());
    }

    void compactWindowLayout() {
        QFETCH(QSize, size);
        QCOMPARE(window->size(), QSize(1000, 680));
        QCOMPARE(window->minimumSize(), QSize(900, 600));
        window->resize(size);
        QTest::qWait(100);
        QCOMPARE(window->size(), size);
        const QRectF bounds(QPointF(0, 0), size);
        for (const auto name : {"sourceCard", "resultCard", "inputScroll", "resultScroll", "translateButton", "window-close", "theme-system"}) {
            auto *item = qobject_cast<QQuickItem *>(object(name)); QVERIFY(item);
            QVERIFY2(item->isVisible() && bounds.contains(item->mapRectToScene(item->boundingRect())), name);
        }
        auto *input = qobject_cast<QQuickItem *>(object("inputScroll")); QVERIFY(input);
        QVERIFY2(input->height() >= 80, qPrintable(QStringLiteral("Input area too small: %1").arg(input->height())));
        auto *source = qobject_cast<QQuickItem *>(object("sourceCard")); QVERIFY(source);
        auto *result = qobject_cast<QQuickItem *>(object("resultCard")); QVERIFY(result);
        QVERIFY(!source->mapRectToScene(source->boundingRect()).intersects(result->mapRectToScene(result->boundingRect())));
        const auto label = QString::fromLatin1(QTest::currentDataTag());
        capture(QStringLiteral("compact-") + label + QStringLiteral("-light"));
        app->setTheme(QStringLiteral("dark"));
        capture(QStringLiteral("compact-") + label + QStringLiteral("-dark"));
        app->setTheme(QStringLiteral("light"));
        window->setProperty("currentPage", 2);
        capture(QStringLiteral("compact-") + label + QStringLiteral("-favorites"));
        window->setProperty("currentPage", 3);
        capture(QStringLiteral("compact-") + label + QStringLiteral("-settings"));
        window->setProperty("currentPage", 0);
        QTest::qWait(80);
        click("translateButton");
        QTRY_VERIFY(object("providerDialog")->property("opened").toBool());
        for (const auto name : {"testProviderButton", "saveProviderButton", "providerFormScroll"}) {
            auto *item = qobject_cast<QQuickItem *>(object(name)); QVERIFY(item);
            QVERIFY2(bounds.contains(item->mapRectToScene(item->boundingRect())), name);
        }
        capture(QStringLiteral("compact-") + label + QStringLiteral("-editor"));
    }

    void framelessWindowControlsAndSidebar() {
        QVERIFY(window->flags().testFlag(Qt::FramelessWindowHint));
        const auto original = window->geometry();
        auto *drag = qobject_cast<QQuickItem *>(object("sidebarDragArea")); QVERIFY(drag);
        click("navigation-3"); QCOMPARE(window->property("currentPage").toInt(), 3);
        QVERIFY(!drag->property("moveStarted").toBool());
        click("navigation-0");
        capture(QStringLiteral("window-light"));
        auto *space = qobject_cast<QQuickItem *>(object("sidebarDragSpace")); QVERIFY(space); QVERIFY(space->height() > 10);
        const auto point = space->mapToScene(QPointF(space->width() / 2, space->height() / 2)).toPoint();
        QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, point);
        QTRY_VERIFY(window->windowState() & Qt::WindowMaximized);
        QTest::qWait(100);
        QVERIFY(!qobject_cast<QQuickItem *>(object("resize-left"))->isVisible());
        auto *maximizeTip = object("window-maximizeToolTip"); QVERIFY(maximizeTip);
        QCOMPARE(maximizeTip->property("text").toString(), QStringLiteral("还原窗口"));
        if (QGuiApplication::platformName() == QLatin1String("windows")) {
            const QRect area = window->screen()->availableGeometry();
            QVERIFY2(area.adjusted(-1, -1, 1, 1).contains(window->geometry()), qPrintable(QStringLiteral("Maximized window %1,%2 %3x%4 exceeds work area %5,%6 %7x%8")
                .arg(window->x()).arg(window->y()).arg(window->width()).arg(window->height()).arg(area.x()).arg(area.y()).arg(area.width()).arg(area.height())));
        }
        capture(QStringLiteral("window-maximized"));
        click("window-maximize");
        QTRY_VERIFY(!(window->windowState() & Qt::WindowMaximized));
        QTest::qWait(80);
        QCOMPARE(window->size(), original.size());
        QTest::mouseMove(window, QPoint(400, 90));
        auto *maximizeBackground = object("window-maximize")->property("background").value<QObject *>();
        QVERIFY(maximizeBackground);
        QCOMPARE(QQmlProperty(maximizeBackground, QStringLiteral("border.width")).read().toReal(), 0.0);
        QCOMPARE(maximizeTip->property("text").toString(), QStringLiteral("最大化"));
        QVERIFY(qobject_cast<QQuickItem *>(object("resize-left"))->isVisible());
        for (const auto name : {"window-minimize", "window-maximize", "window-close"}) {
            auto *button = qobject_cast<QQuickItem *>(object(name)); QVERIFY(button);
            auto *background = button->property("background").value<QObject *>(); QVERIFY(background);
            const auto position = button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
            QTest::mouseMove(window, position);
            QTRY_VERIFY(button->property("hovered").toBool());
            QVERIFY(background->property("color").value<QColor>().alpha() > 0);
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, position);
            QTest::mouseMove(window, QPoint(400, 90));
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, QPoint(400, 90));
            QTRY_VERIFY(!button->property("down").toBool());
            QTRY_VERIFY(!button->property("hovered").toBool());
            QCOMPARE(background->property("color").value<QColor>().alpha(), 0);
            QCOMPARE(QQmlProperty(background, QStringLiteral("border.width")).read().toReal(), 0.0);
        }
        capture(QStringLiteral("window-after-click"));
        click("theme-dark");
        capture(QStringLiteral("window-dark"));
        click("window-minimize");
        QTRY_VERIFY(window->windowState() & Qt::WindowMinimized);
        window->showNormal(); QTest::qWait(100);
        click("window-close");
        QTRY_VERIFY(!window->isVisible());
    }
};

QTEST_MAIN(UiTests)
#include "tst_ui.moc"
