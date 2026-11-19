#include "appcontroller.h"
#include "documentcontroller.h"
#include "fileimportcontroller.h"
#include "pdfcontroller.h"
#include "pdffixture.h"
#include "streamserver.h"
#include <QtTest>
#include <QTemporaryDir>

class FileImportTests : public QObject {
    Q_OBJECT
    QUrl write(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return {};
        return QUrl::fromLocalFile(file.fileName());
    }
    QByteArray read(const QUrl &url) {
        QFile file(url.toLocalFile()); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    void configure(AppController &app, StreamServer &server) {
        QVERIFY(app.saveProvider({{"name", "Offline test"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "offline-model"}}).isEmpty());
    }
private slots:
    void localFilesAndRejectedInputs() {
        QTemporaryDir dir; AppController app(dir.path()); DocumentController txt(&app); PdfController pdf(&app); FileImportController imports(&txt, &pdf);
        const auto text = write(dir, QStringLiteral("中文 空格 #100%.TXT"), "Original text");
        const auto book = write(dir, "book.PdF", "placeholder");
        QCOMPARE(imports.inspect({text})["kind"].toString(), QStringLiteral("txt"));
        QCOMPARE(imports.inspect({book})["kind"].toString(), QStringLiteral("pdf"));
        QCOMPARE(imports.inspect({text})["fileName"].toString(), QStringLiteral("中文 空格 #100%.TXT"));
        QVERIFY(imports.requestDrop({text})); QCOMPARE(txt.sourceText(), QStringLiteral("Original text"));
        QSignalSpy opened(&imports, &FileImportController::opened), errors(&imports, &FileImportController::toast);
        const QList<QList<QUrl>> invalid{{}, {text, book}, {QUrl::fromLocalFile(dir.path())}, {QUrl("https://example.com/file.txt")},
            {write(dir, "other.xlsx", "text")}, {QUrl::fromLocalFile(dir.filePath("missing.txt"))}};
        for (const auto &urls : invalid) {
            QVERIFY(imports.inspect(urls).contains("error")); QVERIFY(!imports.requestDrop(urls));
            QCOMPARE(txt.sourceText(), QStringLiteral("Original text"));
        }
        QCOMPARE(opened.size(), 0); QCOMPARE(errors.size(), invalid.size()); QVERIFY(!app.busy()); QVERIFY(!pdf.loaded());
    }
    void selectedEncodingAndFailedReadsPreserveDocument() {
        QTemporaryDir dir; AppController app(dir.path()); DocumentController txt(&app); PdfController pdf(&app); FileImportController imports(&txt, &pdf);
        imports.setTextEncoding("gb18030");
        QVERIFY(imports.requestOpen(write(dir, QStringLiteral("编码.TXT"), QByteArray::fromHex("d6d0cec4"))));
        QCOMPARE(txt.sourceText(), QStringLiteral("中文"));
        imports.setTextEncoding("utf8"); imports.setTextEncoding("unsupported"); QCOMPARE(imports.textEncoding(), QStringLiteral("utf8"));
        for (const auto &bytes : {QByteArray::fromHex("fffe41"), QByteArray(), QByteArray(2 * 1024 * 1024 + 1, 'x'), QByteArray(200001, 'x')}) {
            QVERIFY(!imports.requestOpen(write(dir, "invalid.txt", bytes))); QCOMPARE(txt.sourceText(), QStringLiteral("中文"));
        }
    }
    void unsavedReplacement_data() {
        QTest::addColumn<QString>("choice");
        for (const auto &choice : {"cancel", "discard", "export"}) QTest::newRow(choice) << QString::fromLatin1(choice);
    }
    void unsavedReplacement() {
        QFETCH(QString, choice);
        QTemporaryDir dir; AppController app(dir.path()); DocumentController txt(&app); PdfController pdf(&app); FileImportController imports(&txt, &pdf);
        StreamServer server("openai"); configure(app, server);
        const auto original = write(dir, "original.txt", "Original text"), next = write(dir, "next.TXT", "Next document");
        QVERIFY(imports.requestOpen(original)); txt.start(); QTRY_VERIFY(server.ready);
        server.sendText(QStringLiteral("原文译文")); server.finish({}); QTRY_VERIFY(!txt.busy()); QVERIFY(txt.hasUnexportedResult());
        QVERIFY(imports.requestOpen(next)); QVERIFY(imports.confirmationPending()); QCOMPARE(imports.pendingFileName(), QStringLiteral("next.TXT"));
        QVERIFY(!imports.requestOpen(write(dir, "third.txt", "Other document"))); QCOMPARE(imports.pendingFileName(), QStringLiteral("next.TXT"));
        QCOMPARE(txt.resultText(), QStringLiteral("原文译文"));
        if (choice == "cancel") {
            imports.cancelReplacement(); QVERIFY(!imports.confirmationPending()); QCOMPARE(txt.sourceText(), QStringLiteral("Original text")); QVERIFY(txt.hasUnexportedResult());
        } else if (choice == "discard") {
            QVERIFY(imports.confirmReplacement()); QCOMPARE(txt.sourceText(), QStringLiteral("Next document")); QVERIFY(!txt.hasUnexportedResult());
        } else {
            QVERIFY(!imports.exportAndReplace(original)); QVERIFY(!imports.exportAndReplace(next));
            QVERIFY(QFile::remove(next.toLocalFile())); QVERIFY(!imports.exportAndReplace(next)); QVERIFY(!QFile::exists(next.toLocalFile()));
            QCOMPARE(write(dir, "next.TXT", "Next document"), next);
            QVERIFY(!imports.exportAndReplace(QUrl::fromLocalFile(dir.filePath("missing/output.txt"))));
            QVERIFY(imports.confirmationPending()); QVERIFY(txt.hasUnexportedResult()); QCOMPARE(txt.resultText(), QStringLiteral("原文译文"));
            const auto output = QUrl::fromLocalFile(dir.filePath(QStringLiteral("已完成 译文.txt")));
            QVERIFY(imports.exportAndReplace(output)); QCOMPARE(QString::fromUtf8(read(output)), QStringLiteral("原文译文"));
            QCOMPARE(txt.sourceText(), QStringLiteral("Next document")); QVERIFY(!txt.hasUnexportedResult());
        }
        QCOMPARE(read(original), QByteArray("Original text")); QCOMPARE(read(next), QByteArray("Next document")); QCOMPARE(server.requestCount, 1);
    }
    void replacementRevalidatesFileAndSnapshotsEncoding() {
        QTemporaryDir dir; AppController app(dir.path()); DocumentController txt(&app); PdfController pdf(&app); FileImportController imports(&txt, &pdf);
        StreamServer server("openai"); configure(app, server);
        QVERIFY(imports.requestOpen(write(dir, "old.txt", "Old document"))); txt.start(); QTRY_VERIFY(server.ready);
        server.sendText(QStringLiteral("旧译文")); server.finish({}); QTRY_VERIFY(!txt.busy());
        const auto next = write(dir, "next.txt", QByteArray::fromHex("d6d0cec4"));
        imports.setTextEncoding("gb18030"); QVERIFY(imports.requestOpen(next)); imports.setTextEncoding("utf8");
        QVERIFY(imports.confirmReplacement()); QCOMPARE(txt.sourceText(), QStringLiteral("中文"));
        server.ready = false; server.request.clear(); txt.start(); QTRY_VERIFY(server.ready); server.sendText("Chinese"); server.finish({}); QTRY_VERIFY(!txt.busy());
        QVERIFY(imports.requestOpen(next)); QVERIFY(QFile::remove(next.toLocalFile()));
        QVERIFY(!imports.confirmReplacement()); QCOMPARE(txt.sourceText(), QStringLiteral("中文")); QCOMPARE(txt.resultText(), QStringLiteral("Chinese"));
    }
    void partialExportTracksFurtherTranslation() {
        QTemporaryDir dir; AppController app(dir.path()); DocumentController txt(&app); PdfController pdf(&app); FileImportController imports(&txt, &pdf);
        StreamServer server("openai"); configure(app, server);
        QVERIFY(imports.requestOpen(write(dir, "old.txt", "First paragraph\n\nSecond paragraph")));
        const auto pause = connect(&txt, &DocumentController::changed, &txt, [&] { if (txt.completed() == 1 && txt.busy()) txt.cancel(); });
        txt.start(); QTRY_VERIFY(server.ready); server.sendText(QStringLiteral("第一段")); server.finish({}); QTRY_COMPARE(txt.completed(), 1); QTRY_VERIFY(!txt.busy());
        disconnect(pause); QVERIFY(txt.hasUnexportedResult());
        QVERIFY(txt.saveFile(QUrl::fromLocalFile(dir.filePath("partial.txt")))); QVERIFY(!txt.hasUnexportedResult());
        server.ready = false; server.request.clear(); txt.resume(); QTRY_VERIFY(server.ready); server.sendText(QStringLiteral("第二段")); server.finish({});
        QTRY_COMPARE(txt.completed(), 2); QVERIFY(txt.hasUnexportedResult());
        QVERIFY(imports.requestOpen(write(dir, "next.txt", "Next text"))); QVERIFY(imports.confirmationPending()); imports.cancelReplacement();
        QVERIFY(txt.saveFile(QUrl::fromLocalFile(dir.filePath("full.txt")))); QVERIFY(!txt.hasUnexportedResult());
        QVERIFY(imports.requestOpen(QUrl::fromLocalFile(dir.filePath("next.txt")))); QVERIFY(!imports.confirmationPending());
    }
    void runningTasksRejectReplacement() {
        QTemporaryDir dir; AppController app(dir.path()); DocumentController txt(&app); PdfController pdf(&app); FileImportController imports(&txt, &pdf);
        StreamServer server("openai"); configure(app, server);
        QVERIFY(imports.requestOpen(write(dir, "old.txt", "Old document"))); txt.start(); QTRY_VERIFY(server.ready);
        QVERIFY(!imports.textBlockReason().isEmpty()); QVERIFY(!imports.requestOpen(write(dir, "new.txt", "New document"))); QVERIFY(txt.busy());
        QCOMPARE(txt.sourceText(), QStringLiteral("Old document")); txt.cancel(); QVERIFY(imports.textBlockReason().isEmpty());
        const auto book = QUrl::fromLocalFile(dir.filePath("book.pdf")); makeSharedTextPdfFixture(book.toLocalFile(), "Original paragraph");
        const auto next = QUrl::fromLocalFile(dir.filePath("next.pdf")); makeSharedTextPdfFixture(next.toLocalFile());
        QVERIFY(imports.requestOpen(book)); QVERIFY(!imports.pdfBlockReason().isEmpty()); QVERIFY(!imports.requestOpen(next));
        QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded() && !pdf.processing(), 20000);
        server.ready = false; server.request.clear(); pdf.start(true); QTRY_VERIFY(server.ready);
        QVERIFY(!imports.pdfBlockReason().isEmpty()); QVERIFY(!imports.requestOpen(next)); QVERIFY(pdf.busy()); QCOMPARE(pdf.fileName(), QStringLiteral("book.pdf")); pdf.cancel();
        QVERIFY(pdf.editBlock(pdf.blocks().first().toMap()["id"].toString(), QStringLiteral("原文段落")));
        QVERIFY(pdf.exportFile(QUrl::fromLocalFile(dir.filePath("export.pdf")), true)); QVERIFY(!imports.requestOpen(next)); pdf.cancel();
    }
    void pdfSwitchSavesProgressAndInvalidPdfPreservesIt() {
        QTemporaryDir dir; AppController app(dir.path()); DocumentController txt(&app); PdfController pdf(&app); FileImportController imports(&txt, &pdf);
        const auto book = QUrl::fromLocalFile(dir.filePath(QStringLiteral("原书 #1.PDF"))); makeSharedTextPdfFixture(book.toLocalFile(), "Original paragraph");
        const auto sourceBytes = read(book);
        QVERIFY(imports.requestDrop({book})); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded() && !pdf.processing(), 20000);
        const auto id = pdf.blocks().first().toMap()["id"].toString(); QVERIFY(pdf.editBlock(id, QStringLiteral("原文段落")));
        const auto next = QUrl::fromLocalFile(dir.filePath("next.pdf")); makeSharedTextPdfFixture(next.toLocalFile());
        QVERIFY(imports.requestOpen(next)); QTRY_VERIFY_WITH_TIMEOUT(pdf.fileName() == "next.pdf" && !pdf.processing(), 20000);
        QVERIFY(imports.requestOpen(book)); QTRY_VERIFY_WITH_TIMEOUT(pdf.fileName() != "next.pdf" && !pdf.processing(), 20000);
        QCOMPARE(pdf.blocks().first().toMap()["translation"].toString(), QStringLiteral("原文段落"));
        QVERIFY(imports.requestOpen(write(dir, "corrupt.pdf", "invalid pdf"))); QTRY_VERIFY_WITH_TIMEOUT(!pdf.error().isEmpty() && !pdf.processing(), 20000);
        QCOMPARE(pdf.fileName(), QStringLiteral("原书 #1.PDF")); QCOMPARE(pdf.blocks().first().toMap()["translation"].toString(), QStringLiteral("原文段落"));
        QCOMPARE(read(book), sourceBytes);
    }
};
QTEST_MAIN(FileImportTests)
#include "tst_fileimport.moc"
