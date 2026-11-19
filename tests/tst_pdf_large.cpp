#include "pdfengine.h"
#include "pdftaskstore.h"
#include "pdfcache.h"
#include "pdfcontroller.h"
#include "appcontroller.h"
#include "streamserver.h"
#include "pdffixture.h"
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QLibrary>
#include <fpdf_doc.h>
#include <fpdf_annot.h>

// The unused stream is valid PDF data. It exercises file access beyond the old
// size limit without introducing a huge decoded image into a unit test.
static bool makeBook(const QString &path, int pages, qint64 padding = 0, bool navigation = false) {
    QFile file(path); if (!file.open(QIODevice::WriteOnly)) return false;
    QList<qint64> offsets{0};
    auto write = [&](const QByteArray &bytes) { return file.write(bytes) == bytes.size(); };
    auto object = [&](const QByteArray &body) {
        const int id = offsets.size(); offsets.append(file.pos());
        return write(QByteArray::number(id) + " 0 obj\n" + body + "\nendobj\n");
    };
    const int extra = 4 + 2 * pages, lastPage = 4 + 2 * (pages - 1);
    const auto ref = [](int id) { return QByteArray::number(id) + " 0 R"; };
    const QByteArray catalog = navigation ? " /Outlines " + ref(extra) + " /PageMode /UseOutlines /Names << /Dests << /Names [(end) [" + ref(lastPage) + " /Fit]] >> >>" : QByteArray{};
    if (!write("%PDF-1.7\n") || !object("<< /Type /Catalog /Pages 2 0 R" + catalog + " >>")) return false;
    QByteArray kids; for (int i = 0; i < pages; ++i) kids += QByteArray::number(4 + 2 * i) + " 0 R ";
    if (!object("<< /Type /Pages /Count " + QByteArray::number(pages) + " /Kids [" + kids + "] >>")
        || !object("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")) return false;
    for (int i = 0; i < pages; ++i) {
        const QByteArray annotations = navigation && i == 0 ? " /Annots [" + ref(extra + 2) + ' ' + ref(extra + 3) + ' ' + ref(extra + 4) + ']' : QByteArray{};
        if (!object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 3 0 R >> >> /Contents " + ref(5 + 2 * i) + annotations + " >>")) return false;
        const QByteArray text = "BT /F1 16 Tf 1 0 0 1 40 780 Tm (Sample document " + QByteArray::number(i + 1) + ") Tj ET";
        if (!object("<< /Length " + QByteArray::number(text.size()) + " >>\nstream\n" + text + "\nendstream")) return false;
    }
    if (navigation) {
        if (!object("<< /Type /Outlines /First " + ref(extra + 1) + " /Last " + ref(extra + 1) + " /Count 1 >>")
            || !object("<< /Title (Last chapter) /Parent " + ref(extra) + " /Dest [" + ref(lastPage) + " /Fit] >>")
            || !object("<< /Type /Annot /Subtype /Link /Rect [40 700 200 725] /Border [0 0 0] /Dest [" + ref(lastPage) + " /Fit] >>")
            || !object("<< /Type /Annot /Subtype /Link /Rect [40 650 200 675] /Border [0 0 0] /Dest (end) >>")
            || !object("<< /Type /Annot /Subtype /Text /Rect [500 750 520 770] /Contents (Preserved note) >>")) return false;
    }
    if (padding) {
        const int id = offsets.size(); offsets.append(file.pos());
        if (!write(QByteArray::number(id) + " 0 obj\n<< /Length " + QByteArray::number(padding) + " >>\nstream\n")
            || !file.seek(file.pos() + padding - 1) || !write("\nendstream\nendobj\n")) return false;
    }
    const qint64 xref = file.pos();
    if (!write("xref\n0 " + QByteArray::number(offsets.size()) + "\n0000000000 65535 f \n")) return false;
    for (int i = 1; i < offsets.size(); ++i) if (!write(QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n")) return false;
    return write("trailer\n<< /Size " + QByteArray::number(offsets.size()) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n");
}

// Read navigation through PDFium independently of the qpdf merge implementation.
static QJsonObject readNavigation(const QString &path) {
    QLibrary library(QCoreApplication::applicationDirPath() + "/pdf/pdfium.dll");
#define LOAD(name) const auto name = reinterpret_cast<decltype(&::name)>(library.resolve(#name)); if (!name) return {};
    LOAD(FPDF_InitLibrary) LOAD(FPDF_DestroyLibrary) LOAD(FPDF_LoadDocument) LOAD(FPDF_CloseDocument)
    LOAD(FPDF_LoadPage) LOAD(FPDF_ClosePage) LOAD(FPDFBookmark_GetFirstChild) LOAD(FPDFBookmark_GetTitle)
    LOAD(FPDFBookmark_GetDest) LOAD(FPDFDest_GetDestPageIndex) LOAD(FPDFLink_Enumerate) LOAD(FPDFLink_GetDest)
    LOAD(FPDFPage_GetAnnotCount) LOAD(FPDFPage_GetAnnot) LOAD(FPDFAnnot_GetStringValue) LOAD(FPDFPage_CloseAnnot)
#undef LOAD
    FPDF_InitLibrary();
    const auto doc = FPDF_LoadDocument(path.toUtf8().constData(), nullptr);
    if (!doc) { FPDF_DestroyLibrary(); return {}; }
    const auto bookmark = FPDFBookmark_GetFirstChild(doc, nullptr);
    char16_t title[128]{}; FPDFBookmark_GetTitle(bookmark, title, sizeof(title));
    QJsonObject result{{"title", QString::fromUtf16(title)}, {"bookmarkPage", FPDFDest_GetDestPageIndex(doc, FPDFBookmark_GetDest(doc, bookmark))}};
    const auto page = FPDF_LoadPage(doc, 0);
    if (page) {
        result["annotations"] = FPDFPage_GetAnnotCount(page);
        FPDF_LINK link = nullptr; int position = 0; QJsonArray destinations;
        while (FPDFLink_Enumerate(page, &position, &link)) destinations.append(FPDFDest_GetDestPageIndex(doc, FPDFLink_GetDest(doc, link)));
        result["links"] = destinations;
        const auto note = FPDFPage_GetAnnot(page, 2); char16_t contents[128]{};
        if (note) { FPDFAnnot_GetStringValue(note, "Contents", reinterpret_cast<unsigned short *>(contents), sizeof(contents)); FPDFPage_CloseAnnot(note); }
        result["note"] = QString::fromUtf16(contents); FPDF_ClosePage(page);
    }
    FPDF_CloseDocument(doc); FPDF_DestroyLibrary(); return result;
}

class PdfLargeTests : public QObject {
    Q_OBJECT
private slots:
    void jumpedTableWaitsForPrecedingHeaderChain() {
        QTemporaryDir directory; const auto source = directory.filePath("long-table.pdf"); makeContinuedTableFixture(source, 19);
        AppController app(directory.path()); PdfController pdf(&app); StreamServer server("openai");
        QVERIFY(app.saveProvider({{"name", "offline table"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "test"}}).isEmpty());
        bool paused = false;
        connect(&pdf, &PdfController::changed, &pdf, [&] { if (pdf.loaded() && !paused) { paused = true; pdf.pauseAnalysis(); } });
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        pdf.setPage(18); QTRY_VERIFY_WITH_TIMEOUT(pdf.pageReady(), 20000); QVERIFY(!pdf.analysisComplete());
        pdf.start(true); QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000);
        QVERIFY(pdf.analysisComplete());
        QVERIFY(server.request.contains("Part")); QVERIFY(server.request.contains("Amplifier"));
        server.sendText(QStringLiteral("放大器")); server.finish({}); server.ready = false; server.request.clear();
        QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000);
        QVERIFY2(server.request.contains("Current"), server.request.constData()); QVERIFY(server.request.contains("12 mA"));
        pdf.cancel(); QVERIFY(!pdf.busy());
    }
    void exportCheckpointsPreserveTextAndPreview() {
        QTemporaryDir directory; const auto source = directory.filePath("checkpoints.pdf");
        QVERIFY(makeBook(source, 65, 0, true));
        const QJsonObject navigation{{"title", "Last chapter"}, {"bookmarkPage", 64}, {"annotations", 3},
            {"links", QJsonArray{64, 64}}, {"note", "Preserved note"}};
        QCOMPARE(readNavigation(source), navigation);
        const auto info = PdfEngine::execute({{"action", "info"}, {"source", source}});
        PdfTaskStore store; QVERIFY(store.open(directory.filePath("task.sqlite"))); QVERIFY(store.setMetadata(info));
        QJsonObject last;
        for (int first = 0; first < 65; first += 8) {
            const auto batch = PdfEngine::execute({{"action", "inspectPages"}, {"source", source}, {"document", info}, {"first", first}});
            QVERIFY2(!batch.contains("error"), qUtf8Printable(batch["error"].toString()));
            for (const auto &value : batch["pages"].toArray()) {
                auto page = value.toObject(); const int number = page["number"].toInt(); auto blocks = page["blocks"].toArray();
                auto block = blocks[0].toObject(); block["translation"] = QStringLiteral("中文译文 %1").arg(number + 1); blocks[0] = block; page["blocks"] = blocks;
                QVERIFY(store.putPage(number, page)); last = page;
            }
        }
        const auto output = directory.filePath("translated.pdf");
        QFile target(output); QVERIFY(target.open(QIODevice::WriteOnly)); target.write("previous output"); target.close();
        int checkpoints = 0; QString lastStage;
        const auto result = PdfEngine::execute({{"action", "export"}, {"source", source}, {"taskDatabase", store.path()}, {"destination", output}}, [&](const QJsonObject &event) {
            const auto stage = event["stage"].toString(); if (stage == "checkpoint" && stage != lastStage) ++checkpoints; lastStage = stage;
        });
        QVERIFY2(result["saved"].toBool(), qUtf8Printable(result["error"].toString())); QCOMPARE(checkpoints, 3);
        QCOMPARE(readNavigation(output), navigation);
        const auto exported = PdfEngine::execute({{"action", "info"}, {"source", output}}); QCOMPARE(exported["pageCount"].toInt(), 65);
        for (int p : {0, 31, 32, 63, 64}) {
            const auto batch = PdfEngine::execute({{"action", "inspectPages"}, {"source", output}, {"document", exported}, {"first", p}, {"count", 1}});
            const auto page = batch["pages"].toArray()[0].toObject();
            QVERIFY2(page["text"].toString().contains(QStringLiteral("中文译文 %1").arg(p + 1)), qUtf8Printable(page["text"].toString()));
            QCOMPARE(page["width"].toDouble(), 595.0); QCOMPARE(page["height"].toDouble(), 842.0);
        }
        const auto preview = PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", info}, {"pageData", last}, {"page", 64}, {"pixels", 300},
            {"original", directory.filePath("original.png")}, {"translated", directory.filePath("expected.png")}});
        QVERIFY(!preview.contains("error"));
        const auto page = PdfEngine::execute({{"action", "inspectPages"}, {"source", output}, {"document", exported}, {"first", 64}, {"count", 1}})["pages"].toArray()[0].toObject();
        const auto reopened = PdfEngine::execute({{"action", "render"}, {"source", output}, {"document", exported}, {"pageData", page}, {"page", 64}, {"pixels", 300},
            {"original", directory.filePath("actual.png")}, {"translated", directory.filePath("unchanged.png")}});
        QVERIFY(!reopened.contains("error")); QCOMPARE(QImage(directory.filePath("actual.png")), QImage(directory.filePath("expected.png")));
        QCOMPARE(PdfEngine::execute({{"action", "info"}, {"source", source}})["hash"], info["hash"]);
        QVERIFY(QDir(directory.path()).entryList({".Yiyi-export-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
    void failedExportAfterCheckpointLeavesExistingTarget() {
        QTemporaryDir directory; const auto source = directory.filePath("failure.pdf"); QVERIFY(makeBook(source, 33));
        const auto document = PdfEngine::execute({{"action", "inspect"}, {"source", source}}); const auto output = directory.filePath("saved.pdf");
        QFile file(output); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("existing target"); file.close();
        bool reached = false;
        const auto result = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", document}, {"destination", output}}, [&](const QJsonObject &event) {
            if (event["stage"] == "checkpoint") reached = true;
            if (event["stage"] == "export" && event["completed"].toInt() == 33) throw QStringLiteral("Simulated failure after checkpoint");
        });
        QVERIFY(reached); QVERIFY(result.contains("error")); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("existing target"));
        QVERIFY(QDir(directory.path()).entryList({".Yiyi-export-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
    void resetIsIncrementalAndResumesAfterReopen() {
        QTemporaryDir directory; const auto source = directory.filePath("reset.pdf"); QVERIFY(makeBook(source, 25));
        const auto document = PdfEngine::execute({{"action", "inspect"}, {"source", source}});
        PdfTaskStore store; QVERIFY(store.open(directory.filePath("pdf-tasks/" + document["hash"].toString() + ".sqlite")));
        auto metadata = document; metadata.remove("pages"); QVERIFY(store.setMetadata(metadata));
        for (const auto &value : document["pages"].toArray()) {
            auto page = value.toObject(); auto blocks = page["blocks"].toArray(); auto block = blocks[0].toObject();
            block["translation"] = QStringLiteral("校对译文"); blocks[0] = block; page["blocks"] = blocks; QVERIFY(store.putPage(page["number"].toInt(), page));
        }
        store.close(); AppController app(directory.path());
        {
            PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
            QCOMPARE(pdf.completed(), 25); pdf.resetTranslations();
            QVERIFY(pdf.resetPending()); QVERIFY(pdf.resetting()); QCOMPARE(pdf.completed(), 17);
            pdf.cancel(); QVERIFY(pdf.resetPending()); QVERIFY(!pdf.resetting());
        }
        PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded() && !pdf.resetPending(), 20000);
        QCOMPARE(pdf.completed(), 0); QCOMPARE(pdf.total(), 25); QVERIFY(pdf.taskDescription().isEmpty());
        pdf.setPage(24); QVERIFY(pdf.blocks().first().toMap()["translation"].toString().isEmpty());
        QVERIFY(pdf.blocks().first().toMap()["source"].toString().contains("25"));
    }
    void cancelExportPreservesExistingTarget() {
        QTemporaryDir directory; const auto source = directory.filePath("cancel-export.pdf"); QVERIFY(makeBook(source, 96));
        AppController app(directory.path()); PdfController pdf(&app);
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.analysisComplete(), 30000);
        QVERIFY(pdf.editBlock(pdf.blocks().first().toMap()["id"].toString(), QStringLiteral("保留校对译文")));
        const auto output = directory.filePath("target.pdf"); QFile target(output); QVERIFY(target.open(QIODevice::WriteOnly)); target.write("prior output"); target.close();
        bool canceled = false;
        connect(&pdf, &PdfController::changed, &pdf, [&] {
            if (!canceled && pdf.operationStatus().startsWith(QStringLiteral("正在导出")) && pdf.operationProgress() > 0) { canceled = true; pdf.cancel(); }
        });
        QVERIFY(pdf.exportFile(QUrl::fromLocalFile(output))); QTRY_VERIFY_WITH_TIMEOUT(canceled, 20000);
        QVERIFY(!pdf.processing()); QCOMPARE(pdf.completed(), 1);
        QVERIFY(target.open(QIODevice::ReadOnly)); QCOMPARE(target.readAll(), QByteArray("prior output"));
        QVERIFY(QDir(directory.path()).entryList({".Yiyi-pdf-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
    void firstPageJumpPauseAndResume() {
        QTemporaryDir directory; const auto source = directory.filePath("601页.pdf");
        QVERIFY(makeBook(source, 601, 55 * 1024 * 1024));
        AppController app(directory.path()); QString id;
        {
            PdfController pdf(&app); bool paused = false;
            connect(&pdf, &PdfController::changed, &pdf, [&] {
                if (pdf.loaded() && !paused) { paused = true; pdf.pauseAnalysis(); }
            });
            QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source)));
            QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
            QVERIFY(pdf.analyzedPages() > 0 && pdf.analyzedPages() <= 8); QVERIFY(!pdf.analysisComplete());
            QVERIFY(!pdf.analyzing()); QVERIFY(!pdf.processing());
            id = pdf.blocks().first().toMap()["id"].toString(); QVERIFY(pdf.editBlock(id, QStringLiteral("首章校对")));
            pdf.setPage(600); QVERIFY(!pdf.pageReady()); QVERIFY(pdf.pageStatus().contains(QStringLiteral("尚未解析")));
            QTRY_VERIFY_WITH_TIMEOUT(pdf.pageReady(), 20000);
            QTRY_VERIFY_WITH_TIMEOUT(!pdf.rendering(), 20000);
            QVERIFY2(pdf.error().isEmpty(), qUtf8Printable(pdf.error()));
            QVERIFY(pdf.analyzedPages() <= 10); QVERIFY(!pdf.analyzing());
            QVERIFY(pdf.blocks().first().toMap()["source"].toString().contains("601"));
            QVERIFY(pdf.editBlock(pdf.blocks().first().toMap()["id"].toString(), QStringLiteral("末章校对")));
            pdf.resumeAnalysis(); QTRY_VERIFY_WITH_TIMEOUT(pdf.analyzedPages() > 10, 20000); pdf.pauseAnalysis();
            pdf.setPage(0); QCOMPARE(pdf.blocks().first().toMap()["translation"].toString(), QStringLiteral("首章校对"));
            QCOMPARE(pdf.completed(), 2);
        }
        // A partially upgraded task must validate each individual page rather
        // than accepting the database metadata version for every saved page.
        const auto metadata = PdfEngine::execute({{"action", "info"}, {"source", source}});
        PdfTaskStore store; QVERIFY(store.open(directory.filePath("pdf-tasks/" + metadata["hash"].toString() + ".sqlite")));
        auto last = store.page(600); last["extractionVersion"] = 1; QVERIFY(store.putPage(600, last)); store.close();
        PdfController pdf(&app); bool paused = false;
        connect(&pdf, &PdfController::changed, &pdf, [&] { if (pdf.loaded() && !paused) { paused = true; pdf.pauseAnalysis(); } });
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        QCOMPARE(pdf.blocks().first().toMap()["translation"].toString(), QStringLiteral("首章校对"));
        QCOMPARE(pdf.completed(), 1);
        pdf.setPage(600); QVERIFY(!pdf.pageReady());
        QTRY_VERIFY_WITH_TIMEOUT(pdf.pageReady(), 20000);
        QCOMPARE(pdf.blocks().first().toMap()["translation"].toString(), QStringLiteral("末章校对"));
        QCOMPARE(pdf.completed(), 2);
    }
    void cancelBeforeFirstPageCanResume() {
        QTemporaryDir directory; const auto source = directory.filePath("cancel.pdf"); QVERIFY(makeBook(source, 12));
        AppController app(directory.path()); PdfController pdf(&app); bool canceled = false;
        connect(&pdf, &PdfController::changed, &pdf, [&] {
            if (!canceled && pdf.pageCount() && !pdf.loaded()) { canceled = true; pdf.cancel(); }
        });
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(canceled, 20000);
        QVERIFY(!pdf.loaded()); QVERIFY(!pdf.processing()); QVERIFY(!pdf.analyzing());
        pdf.resumeAnalysis(); QTRY_VERIFY_WITH_TIMEOUT(pdf.analysisComplete(), 20000);
        QCOMPARE(pdf.analyzedPages(), 12); QVERIFY2(pdf.error().isEmpty(), qUtf8Printable(pdf.error()));
    }
    void translationIncludesLaterPages() {
        QTemporaryDir directory; const auto source = directory.filePath("queue.pdf"); QVERIFY(makeBook(source, 19));
        AppController app(directory.path()); PdfController pdf(&app); StreamServer server("openai");
        QVERIFY(app.saveProvider({{"name", "offline fixture"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "test"}}).isEmpty());
        bool paused = false;
        connect(&pdf, &PdfController::changed, &pdf, [&] { if (pdf.loaded() && !paused) { paused = true; pdf.pauseAnalysis(); } });
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        QVERIFY(!pdf.analysisComplete()); pdf.start();
        for (int p = 0; p < 19; ++p) {
            QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000);
            QVERIFY(server.request.contains("Sample document " + QByteArray::number(p + 1)));
            server.sendText(QStringLiteral("文档 %1").arg(p + 1)); server.finish({});
            server.ready = false; server.request.clear();
        }
        QTRY_VERIFY_WITH_TIMEOUT(!pdf.busy(), 20000);
        QCOMPARE(server.requestCount, 19); QCOMPARE(pdf.completed(), 19); QVERIFY(pdf.analysisComplete());
        QVERIFY2(pdf.error().isEmpty(), qUtf8Printable(pdf.error()));
    }
    void taskStoreExceedsOldCacheLimit() {
        QTemporaryDir directory; const auto path = directory.filePath("任务.sqlite");
        const QString text(2048, QLatin1Char('a'));
        QJsonArray blocks;
        for (int i = 0; i < 160; ++i) blocks.append(QJsonObject{{"id", QString::number(i)}, {"source", text}, {"translation", text}});
        {
            PdfTaskStore store(1024); QVERIFY2(store.open(path), qUtf8Printable(store.error()));
            QVERIFY(store.setMetadata({{"hash", "fixture"}, {"pageCount", 64}, {"version", 1}}));
            for (int i = 0; i < 64; ++i) QVERIFY2(store.putPage(i, {{"number", i}, {"analyzed", true}, {"blocks", blocks}, {"width", 595}, {"height", 842}}), qUtf8Printable(store.error()));
            QCOMPARE(store.summaries().size(), 64);
            QCOMPARE(store.page(0)["blocks"].toArray()[159].toObject()["translation"].toString(), text);
        }
        QVERIFY(QFileInfo(path).size() > 32 * 1024 * 1024);
        PdfTaskStore restored(1024); QVERIFY(restored.open(path));
        QCOMPARE(restored.metadata()["hash"].toString(), "fixture");
        QCOMPARE(restored.page(63)["blocks"].toArray()[159].toObject()["source"].toString(), text);
        auto changed = restored.page(32); auto edits = changed["blocks"].toArray(); auto block = edits[0].toObject();
        block["translation"] = QStringLiteral("校对后的译文"); edits[0] = block; changed["blocks"] = edits;
        QVERIFY(restored.putPage(32, changed)); restored.close();
        QVERIFY(restored.open(path, true));
        QCOMPARE(restored.page(32)["blocks"].toArray()[0].toObject()["translation"].toString(), QStringLiteral("校对后的译文"));
        QCOMPARE(restored.page(31)["blocks"].toArray()[0].toObject()["translation"].toString(), text);
        QCOMPARE(restored.summaries()[32].toObject()["completed"].toInt(), 160);
    }
    void taskTransactionsAndConcurrentReader() {
        QTemporaryDir directory; const auto path = directory.filePath("task.sqlite");
        PdfTaskStore writer, reader(0); QVERIFY(writer.open(path)); QVERIFY(writer.setMetadata({{"hash", "fixture"}}));
        QVERIFY(writer.putPage(0, {{"value", "original"}})); QVERIFY(reader.open(path, true));
        QVERIFY(writer.begin()); QVERIFY(writer.putPage(0, {{"value", "uncommitted"}}));
        QCOMPARE(reader.page(0)["value"].toString(), "original");
        writer.rollback(); QCOMPARE(writer.page(0)["value"].toString(), "original");
        QVERIFY(writer.begin()); QVERIFY(writer.putPage(0, {{"value", "committed"}})); QVERIFY(writer.commit());
        QCOMPARE(reader.page(0)["value"].toString(), "committed");
        QVERIFY(!reader.putPage(0, {{"value", "forbidden"}}));
        QCOMPARE(writer.page(0)["value"].toString(), "committed");
    }
    void reextractionRetainsUnambiguousTranslations() {
        const QJsonObject saved{{"blocks", QJsonArray{
            QJsonObject{{"id", "p0-b0"}, {"source", "Original paragraph"}, {"translation", QStringLiteral("已有译文")}},
            QJsonObject{{"id", "p0-b1"}, {"source", "Repeated label"}, {"translation", QStringLiteral("标签一")}},
            QJsonObject{{"id", "p0-b2"}, {"source", "Repeated label"}, {"translation", QStringLiteral("标签二")}},
            QJsonObject{{"id", "p0-b3"}, {"source", "Stale paragraph"}, {"translation", QStringLiteral("待更新")}, {"stale", true}}
        }}};
        const QJsonObject fresh{{"blocks", QJsonArray{
            QJsonObject{{"id", "p0-b0"}, {"source", "Newly recovered paragraph"}},
            QJsonObject{{"id", "p0-b1"}, {"source", "Original paragraph"}},
            QJsonObject{{"id", "p0-b5"}, {"source", "Repeated label"}},
            QJsonObject{{"id", "p0-b6"}, {"source", "Stale paragraph"}}
        }}};
        const auto result = PdfCache::reconcile(fresh, saved)["blocks"].toArray();
        QVERIFY(result[0].toObject()["translation"].toString().isEmpty());
        QCOMPARE(result[1].toObject()["translation"].toString(), QStringLiteral("已有译文"));
        QVERIFY(result[2].toObject()["translation"].toString().isEmpty());
        QCOMPARE(result[3].toObject()["translation"].toString(), QStringLiteral("待更新"));
        QVERIFY(result[3].toObject()["stale"].toBool());
    }
    void reextractionUpdatesGeneratedBoundsAndPreservesUserEdits() {
        const QJsonArray oldRegion{10, 10, 100, 25}, newRegion{10, 10, 120, 40}, editedRegion{12, 12, 90, 22};
        const QJsonObject original{{"id", "p0-b0"}, {"source", "Title"}, {"size", 12},
            {"rect", QJsonArray{10, 10, 80, 10}}, {"region", oldRegion}, {"objects", QJsonArray{0}}};
        const auto digest = [](const QJsonArray &blocks) { return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(blocks).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()); };
        auto translated = original; translated["translation"] = QStringLiteral("标题"); translated["stale"] = false; translated["layoutError"] = "Old overflow";
        auto generated = original; generated["region"] = newRegion;
        const QJsonObject fresh{{"width", 200}, {"height", 200}, {"blocks", QJsonArray{generated}}, {"baseLayoutHash", digest({generated})}};
        QJsonObject saved{{"width", 200}, {"height", 200}, {"blocks", QJsonArray{translated}}, {"baseLayoutHash", digest({original})}};
        auto restored = PdfCache::reconcile(fresh, saved)["blocks"].toArray()[0].toObject();
        QCOMPARE(restored["region"].toArray(), newRegion); QCOMPARE(restored["translation"].toString(), QStringLiteral("标题"));
        translated["region"] = editedRegion; saved["blocks"] = QJsonArray{translated};
        restored = PdfCache::reconcile(fresh, saved)["blocks"].toArray()[0].toObject();
        QCOMPARE(restored["region"].toArray(), editedRegion); QCOMPARE(restored["translation"].toString(), QStringLiteral("标题"));
    }
    void migrateLegacyPagesAndEdits() {
        QTemporaryDir directory; const auto source = directory.filePath("source.pdf"); QVERIFY(makeBook(source, 2));
        const auto document = PdfEngine::execute({{"action", "inspect"}, {"source", source}});
        QVERIFY2(!document.contains("error"), qUtf8Printable(document["error"].toString()));
        const auto pages = document["pages"].toArray(); auto first = pages[0].toObject(); auto second = pages[1].toObject();
        auto blocks = first["blocks"].toArray(); auto edited = blocks[0].toObject();
        edited["originalSource"] = edited["source"]; edited["source"] = "Corrected source title";
        edited["translation"] = QStringLiteral("校对后的标题"); edited["serif"] = true; blocks[0] = edited;
        auto scanBlock = second["blocks"].toArray()[0].toObject();
        scanBlock["masks"] = QJsonArray{scanBlock["rect"]}; scanBlock["translation"] = QStringLiteral("扫描页译文");
        scanBlock["backgroundMode"] = "repair"; scanBlock["backgroundConfirmed"] = true; scanBlock["backgroundPreviewed"] = true;
        QJsonObject scan{{"source", scanBlock["source"]}, {"translation", scanBlock["translation"]}, {"blocks", QJsonArray{scanBlock}},
                         {"layout", second["layout"]}, {"draftSource", "Preserved draft"}};
        const QJsonObject cache{{"version", 4}, {"hash", document["hash"]},
            {"snapshot", QJsonObject{{"target", "Simplified Chinese"}, {"provider", QJsonObject{{"model", "test-model"}, {"apiKey", "must-not-migrate"}}}}},
            {"layouts", QJsonArray{QJsonObject{{"page", 0}, {"blocks", blocks}, {"layout", first["layout"]}, {"baseLayoutHash", first["baseLayoutHash"]}}}},
            {"scans", QJsonArray{QJsonObject{{"page", 1}, {"scan", scan}}}}};
        const auto bytes = QJsonDocument(cache).toJson(); const auto legacy = directory.filePath("legacy.json");
        QFile file(legacy); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        PdfTaskStore store; QVERIFY(store.open(directory.filePath("task.sqlite")));
        auto metadata = document; metadata.remove("pages"); QVERIFY(store.setMetadata(metadata)); QString error;
        QVERIFY2(PdfCache::importLegacy(legacy, store, &error), qUtf8Printable(error));
        QVERIFY(!store.page(0)["analyzed"].toBool());
        QVERIFY(!QJsonDocument(store.metadata()).toJson().contains("must-not-migrate"));
        const auto restored = PdfCache::reconcile(first, store.page(0));
        QCOMPARE(restored["blocks"].toArray(), blocks);
        const auto scanRestored = PdfCache::reconcile(second, store.page(1))["scan"].toObject();
        QCOMPARE(scanRestored["blocks"].toArray()[0].toObject(), scanBlock);
        QCOMPARE(scanRestored["draftSource"].toString(), "Preserved draft");
        QVERIFY(store.putPage(0, restored));
        QVERIFY(PdfCache::importLegacy(legacy, store, &error));
        QCOMPARE(store.page(0), restored); // Migration cannot overwrite newer work.
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes);
        auto mismatched = first; mismatched["baseLayoutHash"] = "new-extractor";
        auto changedBlocks = first["blocks"].toArray(); auto changed = changedBlocks[0].toObject(); changed["source"] = "Different paragraph";
        changedBlocks[0] = changed; mismatched["blocks"] = changedBlocks;
        const auto rejected = PdfCache::reconcile(mismatched, restored);
        QVERIFY(rejected["blocks"].toArray()[0].toObject()["translation"].toString().isEmpty());
    }
    void migrateOriginalTranslationCache() {
        QTemporaryDir directory; const auto source = directory.filePath("source.pdf"); QVERIFY(makeBook(source, 1));
        const auto document = PdfEngine::execute({{"action", "inspect"}, {"source", source}});
        const auto page = document["pages"].toArray()[0].toObject(); const auto block = page["blocks"].toArray()[0].toObject();
        QJsonObject cache{{"version", 1}, {"hash", document["hash"]}, {"translations", QJsonArray{QJsonObject{{"id", block["id"]}, {"source", block["source"]}, {"translation", QStringLiteral("旧版译文")}}}}};
        QFile file(directory.filePath("legacy.json")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(cache).toJson()); file.close();
        PdfTaskStore store; QVERIFY(store.open(directory.filePath("task.sqlite")));
        auto metadata = document; metadata.remove("pages"); QVERIFY(store.setMetadata(metadata)); QString error;
        QVERIFY2(PdfCache::importLegacy(file.fileName(), store, &error), qUtf8Printable(error));
        QCOMPARE(PdfCache::reconcile(page, store.page(0))["blocks"].toArray()[0].toObject()["translation"].toString(), QStringLiteral("旧版译文"));
        PdfTaskStore different; QVERIFY(different.open(directory.filePath("different.sqlite")));
        metadata["hash"] = "different-source"; QVERIFY(different.setMetadata(metadata));
        QVERIFY(!PdfCache::importLegacy(file.fileName(), different, &error));
        QVERIFY(different.summaries().isEmpty()); QVERIFY(!different.metadata()["legacyImported"].toBool());
    }
    void largeFileAndPagedInspection() {
        QTemporaryDir directory; const auto source = directory.filePath("大文档.pdf");
        QVERIFY(makeBook(source, 601, 55 * 1024 * 1024));
        int hashEvents = 0;
        const auto info = PdfEngine::execute({{"action", "info"}, {"source", source}}, [&](const QJsonObject &event) { if (event["stage"] == "fingerprint") ++hashEvents; });
        QVERIFY2(!info.contains("error"), qUtf8Printable(info["error"].toString()));
        QCOMPARE(info["pageCount"].toInt(), 601); QVERIFY(hashEvents > 1); QVERIFY(!info.contains("pages"));
        int pageEvents = 0;
        const auto batch = PdfEngine::execute({{"action", "inspectPages"}, {"source", source}, {"document", info}, {"quickSourceCheck", true}, {"first", 599}, {"count", 8}},
            [&](const QJsonObject &event) { if (event["stage"] == "inspect") ++pageEvents; else QFAIL("Unchanged page requests must not re-hash the entire PDF"); });
        QVERIFY2(!batch.contains("error"), qUtf8Printable(batch["error"].toString()));
        QCOMPARE(pageEvents, 2); QCOMPARE(batch["pages"].toArray().size(), 2);
        auto page = batch["pages"].toArray()[1].toObject(); QCOMPARE(page["number"].toInt(), 600);
        QVERIFY(page["text"].toString().contains("601"));
        auto blocks = page["blocks"].toArray(); QVERIFY(!blocks.isEmpty()); auto first = blocks[0].toObject();
        first["translation"] = QStringLiteral("大文档验证"); blocks[0] = first; page["blocks"] = blocks;
        const auto preview = PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", info}, {"pageData", page}, {"page", 600}, {"quickSourceCheck", true},
            {"original", directory.filePath("original.png")}, {"translated", directory.filePath("translated.png")}});
        QVERIFY2(!preview.contains("error"), qUtf8Printable(preview["error"].toString()));
        QVERIFY(!preview["statuses"].toArray().isEmpty()); QVERIFY(preview["statuses"].toArray()[0].toObject()["fits"].toBool());
        QVERIFY(QFileInfo::exists(directory.filePath("translated.png")));
        QFile changed(source); QVERIFY(changed.open(QIODevice::Append)); changed.write("\n"); changed.close();
        const auto stale = PdfEngine::execute({{"action", "inspectPages"}, {"source", source}, {"document", info}, {"quickSourceCheck", true}, {"first", 0}});
        QVERIFY(stale["error"].toString().contains(QStringLiteral("已变化")));
    }
};
QTEST_MAIN(PdfLargeTests)
#include "tst_pdf_large.moc"
