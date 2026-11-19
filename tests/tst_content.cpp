#include "appcontroller.h"
#include "documentcontroller.h"
#include "ocrservice.h"
#include "screenshotcontroller.h"
#include "visioncontroller.h"
#include "ocrpreprocess.h"
#include "streamserver.h"
#include <QtTest>
#include <QFile>
#include <QPainter>
#include <QTemporaryDir>
#include <QApplication>
#include <QScreen>
#include <QWidget>
#include <QWindow>

class OcrTextFixture : public QWidget {
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.fillRect(rect(), Qt::white); painter.setPen(Qt::black);
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 30));
        painter.drawText(QRect(50, 60, 800, 120), Qt::AlignVCenter, QStringLiteral("Hello world 123"));
    }
};

class ContentTests : public QObject {
    Q_OBJECT
    QUrl writeFile(const QTemporaryDir &directory, const QByteArray &bytes, const QString &name = QStringLiteral("input.txt")) {
        const auto path = directory.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return {};
        return QUrl::fromLocalFile(path);
    }
    void nextRequest(StreamServer &server) { server.ready = false; server.request.clear(); }
    QJsonObject requestBody(const StreamServer &server) {
        return QJsonDocument::fromJson(server.request.mid(server.request.indexOf("\r\n\r\n") + 4)).object();
    }
private slots:
    void visionSnapshotsAndCancellation() {
        QTemporaryDir directory; AppController app(directory.path()); VisionController vision(&app);
        StreamServer textServer("openai"), imageServer("openai");
        QVERIFY(app.saveProvider({{"name","text"},{"protocol","openai"},{"baseUrl",textServer.url()},{"model","text-model"}}).isEmpty());
        const auto textId = app.selectedProviderId();
        QVERIFY(app.saveProvider({{"name","vision"},{"protocol","openai"},{"baseUrl",imageServer.url()},{"model","image-model"},{"reasoningEffort","low"}}).isEmpty());
        const auto imageId = app.providers().last().toMap()["id"].toString(); vision.setProviderId(imageId); app.selectProvider(textId);
        QImage image(40,30,QImage::Format_RGB32); image.fill(Qt::white); vision.setImage(image);
        QTest::qWait(50); QCOMPARE(imageServer.requestCount, 0); QCOMPARE(textServer.requestCount, 0);
        app.setTargetLanguage(QStringLiteral("简体中文")); vision.recognize(); QVERIFY(app.visionBusy());
        QTRY_VERIFY(imageServer.ready); const auto body = requestBody(imageServer); QCOMPARE(body["model"].toString(), QStringLiteral("image-model")); QCOMPARE(body["reasoning_effort"].toString(), QStringLiteral("low"));
        const auto prompt = body["messages"].toArray().first().toObject()["content"].toString();
        QVERIFY(prompt.contains("Transcribe")); QVERIFY(prompt.contains("Do not translate")); QVERIFY(!prompt.contains(app.targetLanguage()));
        app.setTargetLanguage(QStringLiteral("日语")); vision.setProviderId(textId); QCOMPARE(vision.providerId(), imageId);
        app.setProperty("inputText", "hello"); app.translate(); QVERIFY(!app.busy()); app.testProvider(app.providerDraft(textId)); QVERIFY(!app.testBusy());
        QVERIFY(!app.importText("do not overwrite")); QCOMPARE(textServer.requestCount, 0);
        imageServer.sendText(QStringLiteral("Original text")); QTRY_COMPARE(vision.resultText(), QStringLiteral("Original text")); QVERIFY(!vision.complete());
        vision.cancel(); QVERIFY(!app.visionBusy()); QVERIFY(!vision.complete()); QVERIFY(vision.error().contains(QStringLiteral("取消")));
        QVERIFY(vision.resultText().isEmpty());
        vision.setImage(image); QVERIFY(vision.resultText().isEmpty()); QVERIFY(vision.error().isEmpty());
        nextRequest(imageServer); vision.recognize(); QTRY_VERIFY(imageServer.ready);
        imageServer.sendText(QStringLiteral("Original text")); imageServer.finish({}); QTRY_VERIFY(vision.complete());
        QVERIFY(vision.resultInfo().contains(QStringLiteral("image-model"))); QVERIFY(!vision.resultInfo().contains(QStringLiteral("日语"))); QVERIFY(app.history().isEmpty());
        app.removeProvider(imageId); vision.recognize(); QVERIFY(!vision.busy()); QVERIFY(!vision.error().isEmpty()); QCOMPARE(textServer.requestCount, 0);
        QVERIFY(!vision.complete());
        QCOMPARE(vision.resultText(), QStringLiteral("Original text"));
    }
    void visionRetryPreservesEditedSource_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("failure");
        for (const auto &protocol : {QStringLiteral("openai"), QStringLiteral("responses")})
            for (const auto &failure : {QStringLiteral("cancel"), QStringLiteral("disconnect"), QStringLiteral("empty")})
                QTest::newRow(qPrintable(protocol + '-' + failure)) << protocol << failure;
    }
    void visionRetryPreservesEditedSource() {
        QFETCH(QString, protocol); QFETCH(QString, failure);
        QTemporaryDir directory; AppController app(directory.path()); VisionController vision(&app); StreamServer server(protocol);
        QVERIFY(app.saveProvider({{"name","vision"},{"protocol",protocol},{"baseUrl",server.url()},{"model","image-model"}}).isEmpty());
        QImage image(40,30,QImage::Format_RGB32); image.fill(Qt::white); vision.setImage(image);
        vision.recognize(); QTRY_VERIFY(server.ready);
        server.sendText("Original text."); server.finish("Original text."); QTRY_VERIFY(vision.complete());
        const auto corrected = QStringLiteral("Corrected ENGLISH.\nKeep this paragraph!");
        vision.setResultText(corrected); const auto info = vision.resultInfo();
        nextRequest(server); vision.recognize(); QTRY_VERIFY(server.ready); QVERIFY(vision.busy());
        vision.setResultText("must not overwrite while recognizing");
        if (failure != "empty") { server.sendText("Incomplete"); QTRY_COMPARE(vision.resultText(), QStringLiteral("Incomplete")); }
        if (failure == "cancel") vision.cancel();
        else if (failure == "disconnect") server.interrupt();
        else server.finish({});
        QTRY_VERIFY(!vision.busy()); QVERIFY(!vision.complete()); QVERIFY(!vision.error().isEmpty());
        QCOMPARE(vision.resultText(), corrected); QCOMPARE(vision.resultInfo(), info); QVERIFY(!app.visionBusy());
        QVERIFY(vision.error().contains(QStringLiteral("保留"))); QVERIFY(app.history().isEmpty());
        nextRequest(server); vision.recognize(); QTRY_VERIFY(server.ready);
        server.sendText("A new complete source."); server.finish("A new complete source."); QTRY_VERIFY(vision.complete());
        QCOMPARE(vision.resultText(), QStringLiteral("A new complete source.")); QVERIFY(vision.error().isEmpty());
        vision.setImage(image); QVERIFY(vision.resultText().isEmpty()); QVERIFY(vision.resultInfo().isEmpty());
    }
    void visionProbeRejectsTextOnlyEndpoint() {
        QTemporaryDir directory; AppController app(directory.path()); StreamServer server("openai");
        app.testVisionProvider({{"name","test"},{"protocol","openai"},{"baseUrl",server.url()},{"model","vision"}});
        QTRY_VERIFY(server.ready); QVERIFY(app.testBusy());
        const auto body = requestBody(server); QVERIFY(body["messages"].toArray().last().toObject()["content"].isArray());
        server.sendText("OK"); server.finish({}); QTRY_VERIFY(!app.testBusy()); QVERIFY(!app.testSuccess()); QVERIFY(app.testMessage().contains(QStringLiteral("未通过")));
        app.resetTest(); nextRequest(server); app.testProvider({{"name","test"},{"protocol","openai"},{"baseUrl",server.url()},{"model","text"}});
        QTRY_VERIFY(server.ready); server.sendText("OK"); server.finish({}); QTRY_VERIFY(!app.testBusy()); QVERIFY(app.testSuccess());
    }
    void retainedImageAndPreprocessing() {
        ScreenshotController capture; capture.setMode("vision");
        QImage image(400,100,QImage::Format_RGB32); image.fill(QColor(20,20,20)); image.setPixelColor(200,50,Qt::white);
        capture.acceptImage(image); QVERIFY(capture.hasImage()); QVERIFY(!capture.busy()); QVERIFY(!capture.preview().isEmpty());
        capture.setMode("local"); QCOMPARE(capture.image(),image); QVERIFY(!capture.busy());
        capture.setText("Edited local source"); capture.setMode("vision"); capture.setMode("local");
        QCOMPARE(capture.text(), QStringLiteral("Edited local source"));
        const auto prepared = prepareOcrImage(image); QCOMPARE(prepared.size(),QSize(800,200)); QVERIFY(qGray(prepared.pixel(0,0)) > 220); QCOMPARE(image.pixelColor(0,0),QColor(20,20,20));
        capture.clearImage(); QVERIFY(!capture.hasImage()); QVERIFY(capture.preview().isEmpty()); QVERIFY(capture.text().isEmpty());
    }
    void localRetryPreservesEditedSource() {
        if (!qEnvironmentVariableIsSet("YIYI_TEST_NATIVE_OCR")) QSKIP("Opt-in native RapidOCR with synthetic text.");
        ScreenshotController capture; capture.setMode("vision");
        QImage blank(400,100,QImage::Format_RGB32); blank.fill(Qt::white); capture.acceptImage(blank); capture.setMode("local");
        const auto corrected = QStringLiteral("Keep my local correction."); capture.setText(corrected);
        capture.recognize(); QTRY_VERIFY_WITH_TIMEOUT(!capture.busy(), 10000);
        QVERIFY(!capture.error().isEmpty()); QCOMPARE(capture.text(), corrected);
        capture.recognize(); QVERIFY(capture.busy()); capture.cancel(); QVERIFY(!capture.busy()); QCOMPARE(capture.text(), corrected);
        QImage textImage(1000,200,QImage::Format_RGB32); textImage.fill(Qt::white);
        QPainter painter(&textImage); painter.setPen(Qt::black); painter.setFont(QFont("Segoe UI", 36));
        painter.drawText(textImage.rect(), Qt::AlignCenter, "Hello world 123"); painter.end();
        capture.setMode("vision"); capture.acceptImage(textImage); QVERIFY(capture.text().isEmpty()); capture.setMode("local");
        capture.setText(corrected); capture.recognize(); QTRY_VERIFY_WITH_TIMEOUT(!capture.busy(), 10000);
        QVERIFY2(capture.error().isEmpty(), qUtf8Printable(capture.error())); QVERIFY(capture.text().contains("Hello world 123"));
        capture.clearImage(); QVERIFY(capture.text().isEmpty());
    }
    void decodeEncodings_data() {
        QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<QString>("encoding"); QTest::addColumn<QString>("expected"); QTest::addColumn<bool>("valid");
        QTest::newRow("utf8") << QStringLiteral("你好 world\r\n").toUtf8() << QStringLiteral("auto") << QStringLiteral("你好 world\r\n") << true;
        QTest::newRow("utf8 BOM") << QByteArray::fromHex("efbbbf6869") << QStringLiteral("auto") << QStringLiteral("hi") << true;
        QTest::newRow("utf16 LE BOM") << QByteArray::fromHex("fffe604f7d59") << QStringLiteral("auto") << QStringLiteral("你好") << true;
        QTest::newRow("utf16 BE BOM") << QByteArray::fromHex("feff4f60597d") << QStringLiteral("auto") << QStringLiteral("你好") << true;
        QTest::newRow("utf16 no BOM") << QByteArray::fromHex("604f7d59") << QStringLiteral("utf16le") << QStringLiteral("你好") << true;
        QTest::newRow("GBK") << QByteArray::fromHex("c4e3bac3") << QStringLiteral("gb18030") << QStringLiteral("你好") << true;
        QTest::newRow("wrong encoding") << QByteArray::fromHex("c4e3bac3") << QStringLiteral("auto") << QString() << false;
        QTest::newRow("truncated utf8") << QByteArray::fromHex("e4bd") << QStringLiteral("utf8") << QString() << false;
        QTest::newRow("truncated utf16") << QByteArray::fromHex("604f7d") << QStringLiteral("utf16le") << QString() << false;
        QTest::newRow("orphan high surrogate") << QByteArray::fromHex("3dd8") << QStringLiteral("utf16le") << QString() << false;
        QTest::newRow("orphan low surrogate") << QByteArray::fromHex("42de") << QStringLiteral("utf16le") << QString() << false;
        QTest::newRow("binary") << QByteArray::fromHex("68690000") << QStringLiteral("utf8") << QString() << false;
        QTest::newRow("utf32") << QByteArray::fromHex("fffe000041000000") << QStringLiteral("auto") << QString() << false;
    }
    void decodeEncodings() {
        QFETCH(QByteArray, bytes); QFETCH(QString, encoding); QFETCH(QString, expected); QFETCH(bool, valid);
#ifndef Q_OS_WIN
        if (encoding == QLatin1String("gb18030")) QSKIP("GB18030 uses Windows code page support.");
#endif
        QString detected, error;
        const auto text = DocumentController::decode(bytes, encoding, &detected, &error);
        QCOMPARE(error.isEmpty(), valid); QCOMPARE(text, expected);
        if (valid) QVERIFY(!detected.isEmpty());
    }
    void splitPreservesAllSource() {
        const auto emoji = QString::fromUcs4(U"🙂");
        const auto source = QStringLiteral("  Hello world.  \n\n\t") + QString(999, QLatin1Char('x')) + emoji + QString(1500, QChar(0x4f60)) + QStringLiteral("\t\n\n");
        const auto parts = DocumentController::splitText(source);
        QString restored; int translated = 0;
        for (const auto &part : parts) {
            restored += part.text;
            if (part.translate) { QVERIFY(part.text.size() <= 1000); QVERIFY(!part.text.isEmpty()); QVERIFY(!part.text.back().isHighSurrogate()); QVERIFY(!part.text.front().isLowSurrogate()); ++translated; }
        }
        QCOMPARE(restored, source); QVERIFY(translated >= 4);
    }
    void importValidationPreservesLoadedDocument() {
        QTemporaryDir directory; AppController app(directory.path()); DocumentController doc(&app);
        QVERIFY(doc.loadFile(writeFile(directory, "Keep this text.")));
        const auto source = doc.sourceText();
        QVERIFY(!doc.loadFile(writeFile(directory, QByteArray(2 * 1024 * 1024 + 1, 'x'), "large.txt")));
        QVERIFY(!doc.loadFile(writeFile(directory, QByteArray(200001, 'x'), "long.txt")));
        QVERIFY(!doc.loadFile(writeFile(directory, QByteArray("line\n").repeated(501), "many.txt")));
        QVERIFY(!doc.loadFile(writeFile(directory, "    \n\t", "empty.txt")));
        QVERIFY(!doc.loadFile(writeFile(directory, "abc", "other.pdf")));
        QVERIFY(!doc.loadFile(QUrl(QStringLiteral("https://example.invalid/test.txt"))));
        QCOMPARE(doc.sourceText(), source); QCOMPARE(doc.state(), QStringLiteral("ready"));
    }
    void translateAndExport_data() {
        QTest::addColumn<QString>("protocol");
        QTest::newRow("Chat") << QStringLiteral("openai");
        QTest::newRow("Responses") << QStringLiteral("responses");
    }
    void translateAndExport() {
        QFETCH(QString, protocol);
        QTemporaryDir directory; AppController app(directory.path()); DocumentController doc(&app); StreamServer server(protocol);
        QVERIFY(app.saveProvider({{"name", "Local"}, {"protocol", protocol}, {"baseUrl", server.url()}, {"model", "small-model"}, {"reasoningEffort", "low"}}).isEmpty());
        const auto input = writeFile(directory, "  Hello.\r\n\r\nWorld.\r\n"); QVERIFY(doc.loadFile(input)); QCOMPARE(doc.total(), 2);
        doc.start(); QTRY_VERIFY(server.ready); QVERIFY(app.documentBusy());
        QCOMPARE(requestBody(server).value("model").toString(), QStringLiteral("small-model"));
        if (protocol == QLatin1String("responses")) QCOMPARE(requestBody(server).value("reasoning").toObject().value("effort").toString(), QStringLiteral("low"));
        else QCOMPARE(requestBody(server).value("reasoning_effort").toString(), QStringLiteral("low"));
        server.sendText(QStringLiteral("你好。\r\n朋友。")); server.finish(QStringLiteral("你好。\r\n朋友。")); nextRequest(server);
        QTRY_COMPARE(server.requestCount, 2); QTRY_VERIFY(server.ready);
        QCOMPARE(doc.completed(), 1); QCOMPARE(doc.resultText(), QStringLiteral("  你好。\n朋友。\n\n"));
        server.sendText(QStringLiteral("世界。")); server.finish(QStringLiteral("世界。"));
        QTRY_VERIFY(!doc.busy()); QCOMPARE(doc.state(), QStringLiteral("completed")); QVERIFY(!app.documentBusy());
        QCOMPARE(doc.resultText(), QStringLiteral("  你好。\n朋友。\n\n世界。\n")); QVERIFY(app.history().isEmpty());
        QVERIFY(!doc.saveFile(input));
        QFile original(input.toLocalFile()); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), QByteArray("  Hello.\r\n\r\nWorld.\r\n"));
        const auto output = QUrl::fromLocalFile(directory.filePath(QStringLiteral("translated.txt"))); QVERIFY(doc.saveFile(output));
        QFile translated(output.toLocalFile()); QVERIFY(translated.open(QIODevice::ReadOnly)); QCOMPARE(translated.readAll(), QStringLiteral("  你好。\r\n朋友。\r\n\r\n世界。\r\n").toUtf8());
    }
    void cancelResumeKeepsSnapshotAndCompletedSegments() {
        QTemporaryDir directory; AppController app(directory.path()); DocumentController doc(&app); StreamServer server(QStringLiteral("openai"));
        QVERIFY(app.saveProvider({{"name", "Original"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "original-model"}}).isEmpty());
        QVERIFY(doc.loadFile(writeFile(directory, "First\n\nSecond"))); doc.start(); QTRY_VERIFY(server.ready);
        QVERIFY(!requestBody(server).contains("reasoning_effort"));
        app.setProperty("inputText", "lookup"); app.translate(); QVERIFY(!app.busy());
        QVERIFY(!app.importText(QStringLiteral("OCR text")));
        QVERIFY(!doc.loadFile(writeFile(directory, "new", "new.txt")));
        server.sendText(QStringLiteral("第一段")); server.finish({}); nextRequest(server);
        QTRY_COMPARE(server.requestCount, 2); QTRY_VERIFY(server.ready);
        server.sendText(QStringLiteral("未完成")); QTRY_VERIFY(!doc.preview().isEmpty());
        doc.cancel(); QCOMPARE(doc.state(), QStringLiteral("paused")); QVERIFY(doc.preview().isEmpty()); QCOMPARE(doc.completed(), 1); QVERIFY(!app.documentBusy());
        QVERIFY(doc.saveFile(QUrl::fromLocalFile(directory.filePath("partial.txt"))));
        QCOMPARE(doc.resultText(), QStringLiteral("第一段\n\n"));
        app.setTargetLanguage(QStringLiteral("日语"));
        QVERIFY(app.saveProvider({{"name", "Changed"}, {"protocol", "openai"}, {"baseUrl", "http://127.0.0.1:1/v1"}, {"model", "changed-model"}}).isEmpty());
        nextRequest(server); doc.resume(); QTRY_COMPARE(server.requestCount, 3); QTRY_VERIFY(server.ready);
        const auto body = requestBody(server); QCOMPARE(body.value("model").toString(), QStringLiteral("original-model"));
        const auto messages = body.value("messages").toArray();
        QVERIFY(messages.first().toObject().value("content").toString().contains(QStringLiteral("简体中文")));
        QCOMPARE(messages.last().toObject().value("content").toString(), QStringLiteral("Second"));
        server.sendText(QStringLiteral("第二段")); server.finish({}); QTRY_VERIFY(!doc.busy());
        QCOMPARE(doc.resultText(), QStringLiteral("第一段\n\n第二段")); QCOMPARE(doc.completed(), 2); QCOMPARE(doc.state(), QStringLiteral("completed"));
    }
    void failureDoesNotRetryOrCommitPreview() {
        QTemporaryDir directory; AppController app(directory.path()); DocumentController doc(&app); StreamServer server(QStringLiteral("responses"));
        QVERIFY(app.saveProvider({{"name", "Test"}, {"protocol", "responses"}, {"baseUrl", server.url()}, {"model", "local"}}).isEmpty());
        QVERIFY(doc.loadFile(writeFile(directory, "First\nSecond"))); doc.start(); QTRY_VERIFY(server.ready);
        server.sendText(QStringLiteral("未完成的第一段")); server.interrupt();
        QTRY_VERIFY(!doc.busy()); QCOMPARE(doc.state(), QStringLiteral("failed")); QVERIFY(doc.preview().isEmpty()); QVERIFY(doc.resultText().isEmpty()); QCOMPARE(doc.completed(), 0);
        QTest::qWait(120); QCOMPARE(server.requestCount, 1); QVERIFY(!app.documentBusy());
        nextRequest(server); doc.resume(); QTRY_COMPARE(server.requestCount, 2); QTRY_VERIFY(server.ready); doc.cancel();
    }
    void screenshotScale_data() {
        QTest::addColumn<qreal>("scale");
        QTest::newRow("100 percent") << 1.0; QTest::newRow("125 percent") << 1.25; QTest::newRow("150 percent") << 1.5; QTest::newRow("200 percent") << 2.0;
    }
    void screenshotScale() {
        QFETCH(qreal, scale);
        QImage image(qRound(800 * scale), qRound(600 * scale), QImage::Format_RGB32); image.fill(Qt::white);
        QPainter painter(&image); painter.fillRect(QRect(qRound(100 * scale), qRound(80 * scale), qRound(200 * scale), qRound(120 * scale)), Qt::red); painter.end();
        const auto cropped = ScreenshotController::cropImage(image, QSize(800, 600), QRect(100, 80, 200, 120));
        QCOMPARE(cropped.size(), QSize(qRound(200 * scale), qRound(120 * scale))); QCOMPARE(cropped.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(cropped.pixelColor(cropped.width() - 1, cropped.height() - 1), QColor(Qt::red));
        QVERIFY(ScreenshotController::cropImage(image, QSize(800, 600), QRect(900, 700, 10, 10)).isNull());
    }
    void ocrSyntheticImage() {
        if (!qEnvironmentVariableIsSet("YIYI_TEST_NATIVE_OCR")) QSKIP("Opt-in native RapidOCR with synthetic text.");
        OcrService ocr; QSignalSpy finished(&ocr, &OcrService::finished);
        QImage image(1000, 200, QImage::Format_RGB32); image.fill(Qt::white);
        QPainter painter(&image); painter.setPen(Qt::black); painter.setFont(QFont(QStringLiteral("Segoe UI"), 36));
        painter.drawText(QRect(30, 20, 950, 160), Qt::AlignVCenter, QStringLiteral("Hello world 123")); painter.end();
        ocr.recognize(image);
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 35000);
        QVERIFY2(ocr.error().isEmpty(), qUtf8Printable(ocr.error())); QVERIFY(ocr.text().contains(QStringLiteral("Hello world 123"))); QVERIFY(!ocr.note().isEmpty()); QVERIFY(!ocr.busy());
        ocr.recognize(image); QVERIFY(ocr.busy()); ocr.cancel(); QVERIFY(!ocr.busy());
        finished.clear(); ocr.recognize(image);
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 35000);
        QVERIFY2(ocr.error().isEmpty(), qUtf8Printable(ocr.error())); QVERIFY(ocr.text().contains(QStringLiteral("Hello world 123")));
    }
    void nativeScreenSelection() {
        if (!qEnvironmentVariableIsSet("YIYI_TEST_NATIVE_OCR") || QGuiApplication::platformName() != QLatin1String("windows"))
            QSKIP("Opt-in screenshot interaction on an interactive Windows desktop.");
        ScreenshotController capture;
        OcrTextFixture fixture; fixture.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint); fixture.showFullScreen();
        QVERIFY(QTest::qWaitForWindowExposed(fixture.windowHandle()));
        QWidget main; main.setGeometry(60, 350, 300, 100); main.show();
        QVERIFY(QTest::qWaitForWindowExposed(main.windowHandle())); capture.setWindow(main.windowHandle());
        QSignalSpy review(&capture, &ScreenshotController::reviewRequested), restored(&capture, &ScreenshotController::restoreRequested);
        capture.capture(); QVERIFY(!main.windowHandle()->isVisible()); capture.cancel();
        QVERIFY(main.windowHandle()->isVisible()); QCOMPARE(restored.size(), 1); QVERIFY(review.isEmpty());
        QTest::qWait(80); capture.capture();
        QPointer<QWidget> overlay;
        auto findOverlay = [&] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (widget->objectName() == QLatin1String("screenshotSelectionOverlay") && widget->isVisible() && widget->screen() == fixture.screen()) return widget;
            return static_cast<QWidget *>(nullptr);
        };
        QTRY_VERIFY((overlay = findOverlay()));
        QTest::qWait(260);
        int visibleOverlays = 0;
        for (auto *widget : QApplication::topLevelWidgets()) if (widget->objectName() == QLatin1String("screenshotSelectionOverlay") && widget->isVisible()) ++visibleOverlays;
        QCOMPARE(visibleOverlays, QGuiApplication::screens().size());
        QTest::mousePress(overlay, Qt::LeftButton, Qt::NoModifier, QPoint(20, 40));
        QTest::mouseMove(overlay, QPoint(qMin(900, overlay->width() - 20), 200));
        QTest::mouseRelease(overlay, Qt::LeftButton, Qt::NoModifier, QPoint(qMin(900, overlay->width() - 20), 200));
        QTRY_VERIFY_WITH_TIMEOUT(!capture.busy(), 35000);
        QVERIFY2(capture.error().isEmpty(), qUtf8Printable(capture.error())); QVERIFY2(capture.text().contains(QStringLiteral("Hello world 123")), qUtf8Printable(capture.text()));
        QCOMPARE(review.size(), 1); QCOMPARE(restored.size(), 2); QVERIFY(main.windowHandle()->isVisible());
        capture.capture(); QTRY_VERIFY((overlay = findOverlay()));
        QTest::keyClick(overlay, Qt::Key_Escape); QTRY_VERIFY(!capture.busy()); QVERIFY(main.windowHandle()->isVisible());
        QCOMPARE(review.size(), 1); QCOMPARE(restored.size(), 3);
    }
};
QTEST_MAIN(ContentTests)
#include "tst_content.moc"
