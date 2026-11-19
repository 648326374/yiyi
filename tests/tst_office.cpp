#include "appcontroller.h"
#include "documentcontroller.h"
#include "fileimportcontroller.h"
#include "officedocument.h"
#include "officecontroller.h"
#include "pdfcontroller.h"
#include "streamserver.h"
#include "miniz.h"
#include <QtTest>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QPdfWriter>
#include <QPainter>

namespace {
QByteArray bytes(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QString fixture(const QString &name) { return QStringLiteral(YIYI_SOURCE_DIR "/tests/fixtures/office/") + name; }
QString copyFixture(QTemporaryDir &dir, const QString &name, const QString &target = {}) {
    const auto path = dir.filePath(target.isEmpty() ? name : target); return QFile::copy(fixture(name), path) ? path : QString();
}
QByteArray part(const QString &path, const char *name) {
    const auto data = bytes(path); mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data.constData(), size_t(data.size()), 0)) return {};
    size_t size = 0; void *content = mz_zip_reader_extract_file_to_heap(&zip, name, &size, 0);
    const auto result = content ? QByteArray(static_cast<const char *>(content), qsizetype(size)) : QByteArray();
    mz_free(content); mz_zip_reader_end(&zip); return result;
}
bool configure(AppController &app, StreamServer &server) {
    return app.saveProvider({{"name", "Offline office test"}, {"protocol", server.protocol}, {"baseUrl", server.url()}, {"model", "office-test"}, {"apiKey", "offline-secret"}}).isEmpty();
}
}
class OfficeTests : public QObject {
    Q_OBJECT
private slots:
    void editableRoundTrip_data() {
        QTest::addColumn<QString>("kind"); QTest::newRow("word") << QStringLiteral("docx"); QTest::newRow("powerpoint") << QStringLiteral("pptx");
    }
    void editableRoundTrip() {
        QFETCH(QString, kind); QTemporaryDir dir;
        const auto source = copyFixture(dir, "sample." + kind, QStringLiteral("原稿 空格 #100%.") + kind.toUpper());
        const auto original = bytes(source); const auto document = OfficeDocument::load(source);
        QVERIFY2(document.valid(), qPrintable(document.error)); QCOMPARE(document.kind, kind); QVERIFY(document.segments.size() > 6); QVERIFY(document.imageCount > 0);
        if (kind == "pptx") QCOMPARE(document.pages, 2);
        QHash<QString, QString> translations;
        for (const auto &s : document.segments) translations[s.id] = QStringLiteral("译文测试：") + s.source;
        const auto destination = dir.filePath(QStringLiteral("译文 #100%.") + kind); QString error;
        QVERIFY2(document.save(destination, translations, false, 12, &error), qPrintable(error));
        const auto translated = OfficeDocument::load(destination); QVERIFY2(translated.valid(), qPrintable(translated.error));
        QCOMPARE(translated.segments.size(), document.segments.size());
        for (const auto &s : translated.segments) QVERIFY2(s.source.contains(QStringLiteral("译文测试：")), qPrintable(s.source));
        QCOMPARE(bytes(source), original);
        const auto styles = kind == "docx" ? "word/styles.xml" : "ppt/slideMasters/slideMaster1.xml";
        QCOMPARE(part(source, styles), part(destination, styles));
        const auto media = kind == "docx" ? "word/media/image1.png" : "ppt/media/image1.png";
        QCOMPARE(part(source, media), part(destination, media));
        const auto rels = kind == "docx" ? "word/_rels/document.xml.rels" : "ppt/slides/_rels/slide1.xml.rels";
        QCOMPARE(part(source, rels), part(destination, rels));
        if (kind == "docx") {
            const auto body = part(destination, "word/document.xml");
            QVERIFY(body.contains("xmlns:w14=")); QVERIFY(body.contains("xmlns:wp14="));
        }
        const auto capture = qEnvironmentVariable("YIYI_OFFICE_CAPTURE_DIR");
        if (!capture.isEmpty()) { QDir().mkpath(capture); QVERIFY(QFile::copy(destination, QDir(capture).filePath("translated." + kind))); }
    }
    void fieldsRevisionsAndExplicitBreaks() {
        const auto fields = OfficeDocument::load(fixture("fields.docx")); QVERIFY2(fields.valid(), qPrintable(fields.error));
        QStringList sources; for (const auto &s : fields.segments) sources << s.source;
        QVERIFY(sources.contains("Normal paragraph")); QVERIFY(!sources.contains("Generated contents")); QVERIFY(!sources.contains("Tracked paragraph"));
        const auto normal = OfficeDocument::load(fixture("sample.docx")); QVERIFY(normal.valid());
        for (const auto &s : normal.segments) QVERIFY(s.source != "Before tabAfter tabAfter break");
    }
    void sourceAndInvalidExportProtected() {
        QTemporaryDir dir; const auto source = copyFixture(dir, "sample.docx"); const auto document = OfficeDocument::load(source); QVERIFY(document.valid());
        const auto original = bytes(source); QString error;
        QVERIFY(!document.save(source, {}, true, 12, &error)); QCOMPARE(bytes(source), original);
        QVERIFY(!document.save(dir.filePath("wrong.pptx"), {}, true, 12, &error));
        QVERIFY(!document.save(dir.filePath("missing/out.docx"), {}, true, 12, &error));
        QVERIFY(QFile::remove(source)); QVERIFY(!document.save(source, {}, true, 12, &error)); QVERIFY(!QFile::exists(source));
    }
    void storiesAndChartDataPreserved() {
        const auto word = OfficeDocument::load(fixture("sample.docx")); QVERIFY(word.valid());
        QStringList sources; for (const auto &s : word.segments) sources << s.source;
        QVERIFY(sources.contains("Internal report")); QVERIFY(sources.contains("A useful footnote.")); QVERIFY(sources.contains("A useful endnote."));
        const auto ppt = OfficeDocument::load(fixture("sample.pptx")); QVERIFY(ppt.valid());
        const OfficeSegment *title = nullptr;
        for (const auto &s : ppt.segments) if (s.source == "Quarterly performance") title = &s;
        QVERIFY(title); QTemporaryDir dir; QString error; const auto output = dir.filePath("chart.pptx");
        QVERIFY(ppt.save(output, {{title->id, QStringLiteral("季度业绩")}}, false, 12, &error));
        QCOMPARE(part(fixture("sample.pptx"), "ppt/embeddings/Microsoft_Excel_Sheet1.xlsx"), part(output, "ppt/embeddings/Microsoft_Excel_Sheet1.xlsx"));
        QVERIFY(part(output, "ppt/charts/chart1.xml").contains("Alpha"));
        bool warning = false; for (const auto &v : ppt.notices) if (v.toMap()["message"].toString().contains(QStringLiteral("工作簿"))) warning = true;
        QVERIFY(warning);
    }
    void autoFitKeepsCompleteTextAndMinimumFont() {
        const auto ppt = OfficeDocument::load(fixture("sample.pptx")); QVERIFY(ppt.valid()); const auto &s = ppt.segments.first();
        const QString longText(1000, QChar(0x4e2d)); QHash<QString, QString> translations{{s.id, longText}};
        const auto review = ppt.issues(translations, true, 14); bool overflow = false;
        for (const auto &item : review) if (item.toMap()["message"].toString().contains(QStringLiteral("超出"))) overflow = true;
        QVERIFY(overflow); QTemporaryDir dir; QString error; const auto path = dir.filePath("fit.pptx");
        QVERIFY(ppt.save(path, translations, true, 14, &error)); const auto result = OfficeDocument::load(path); QVERIFY(result.valid());
        QCOMPARE(result.segments.first().source, longText);
        QDomDocument xml; QVERIFY(xml.setContent(part(path, "ppt/slides/slide1.xml")));
        const auto properties = xml.elementsByTagName("a:rPr"); QVERIFY(!properties.isEmpty());
        const int size = properties.at(0).toElement().attribute("sz").toInt(); QVERIFY(size >= 1400); QVERIFY(size < 3000);
    }
    void corruptAndOldFormat() {
        QTemporaryDir dir; const auto wrong = dir.filePath("broken.docx"); QFile file(wrong); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("invalid file"); file.close();
        QVERIFY(!OfficeDocument::load(wrong).valid()); QVERIFY(!OfficeDocument::load(dir.filePath("old.doc")).valid());
        const auto mismatched = copyFixture(dir, "sample.docx", "mismatch.pptx"); QVERIFY(!OfficeDocument::load(mismatched).valid());
    }
    void plainRequestsPauseAndCache_data() {
        QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QStringLiteral("openai"); QTest::newRow("responses") << QStringLiteral("responses");
    }
    void plainRequestsPauseAndCache() {
        QFETCH(QString, protocol); QTemporaryDir dir; AppController app(dir.path()); StreamServer server(protocol); QVERIFY(configure(app, server));
        const auto source = copyFixture(dir, "sample.docx"); QString id;
        {
            OfficeController office(&app); QVERIFY(office.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(!office.processing(), 10000); QVERIFY(office.loaded());
            QCOMPARE(server.requestCount, 0); id = office.segments().first().toMap()["id"].toString();
            office.start(); QTRY_VERIFY(server.ready); QVERIFY(app.documentBusy()); QVERIFY(!server.request.contains("<w:"));
            const auto pause = connect(&office, &OfficeController::changed, &office, [&] { if (office.completed() == 1 && office.busy()) office.cancel(); });
            server.sendText(QStringLiteral("实用指南")); server.finish(QStringLiteral("实用指南")); QTRY_COMPARE(office.completed(), 1); QTRY_VERIFY(!office.busy()); disconnect(pause);
            QVERIFY(office.hasUnexportedResult()); QVERIFY(!app.documentBusy());
            QVERIFY(!bytes(dir.filePath("office-tasks/" + OfficeDocument::load(source).hash + ".json")).contains("offline-secret"));
        }
        OfficeController restored(&app); QVERIFY(restored.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY(!restored.processing());
        QCOMPARE(restored.completed(), 1); QCOMPARE(restored.segments().first().toMap()["translation"].toString(), QStringLiteral("实用指南"));
        QVERIFY(restored.hasUnexportedResult());
        server.ready = false; server.request.clear(); QVERIFY(restored.retrySegment(id)); QTRY_VERIFY(server.ready);
        server.sendText(QStringLiteral("尚未完成")); QTRY_VERIFY(!restored.preview().isEmpty()); restored.cancel();
        QCOMPARE(restored.segments().first().toMap()["translation"].toString(), QStringLiteral("实用指南"));
        QVERIFY(restored.editSegment(id, QStringLiteral("校对后的指南")));
        const auto output = dir.filePath("result.docx"); QVERIFY(restored.saveFile(QUrl::fromLocalFile(output))); QTRY_VERIFY(!restored.processing()); QVERIFY2(restored.error().isEmpty(), qPrintable(restored.error()));
        QVERIFY(!restored.hasUnexportedResult()); QVERIFY(OfficeDocument::load(output).segments.first().source.contains(QStringLiteral("校对后的指南")));
    }
    void longParagraphUsesBoundedPlainRequests() {
        QTemporaryDir dir; AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server)); OfficeController office(&app);
        QVERIFY(office.loadFile(QUrl::fromLocalFile(fixture("long.docx")))); QTRY_VERIFY(!office.processing()); QVERIFY(office.loaded());
        const auto id = office.segments().first().toMap()["id"].toString(); QVERIFY(office.retrySegment(id));
        int requests = 0;
        while (office.busy() && requests < 10) {
            QTRY_VERIFY(server.ready);
            const auto request = QJsonDocument::fromJson(server.request.mid(server.request.indexOf("\r\n\r\n") + 4)).object();
            const auto input = request["messages"].toArray().last().toObject()["content"].toString(); QVERIFY(input.size() <= 1000); QVERIFY(!input.contains("<w:"));
            server.sendText(QStringLiteral("长段译文")); server.finish({}); server.ready = false; server.request.clear(); ++requests;
            QTest::qWait(20);
        }
        QTRY_VERIFY(!office.busy()); QVERIFY(requests > 1); QCOMPARE(office.completed(), 1);
        QVERIFY(office.segments().first().toMap()["translation"].toString().count(QStringLiteral("长段译文")) == requests);
    }
    void busyGuardsAndBrokenReplacement() {
        QTemporaryDir dir; AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server));
        OfficeController office(&app); DocumentController txt(&app); PdfController pdf(&app); FileImportController imports(&txt, &pdf, &office);
        QVERIFY(imports.requestOpen(QUrl::fromLocalFile(fixture("sample.pptx")))); QTRY_VERIFY(!office.processing()); QVERIFY(office.loaded());
        office.start(); QTRY_VERIFY(server.ready); QVERIFY(!imports.officeBlockReason().isEmpty()); QVERIFY(!imports.requestOpen(QUrl::fromLocalFile(fixture("sample.docx"))));
        QVERIFY(!office.editSegment(office.segments().first().toMap()["id"].toString(), "Unexpected")); office.cancel();
        const auto original = office.fileName(); QFile broken(dir.filePath("broken.docx")); QVERIFY(broken.open(QIODevice::WriteOnly)); broken.write("broken"); broken.close();
        QVERIFY(imports.requestOpen(QUrl::fromLocalFile(broken.fileName()))); QTRY_VERIFY(!office.processing()); QVERIFY(!office.error().isEmpty()); QCOMPARE(office.fileName(), original);
        app.setDocumentBusy(true); office.start(); QVERIFY(!office.busy()); app.setDocumentBusy(false);
    }
    void keepResetAndServiceChanges() {
        QTemporaryDir dir; AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server)); OfficeController office(&app);
        QVERIFY(office.loadFile(QUrl::fromLocalFile(fixture("sample.docx")))); QTRY_VERIFY(!office.processing());
        const auto id = office.segments().first().toMap()["id"].toString(); QVERIFY(office.keepOriginal(id)); QCOMPARE(office.completed(), 1); QVERIFY(office.segments().first().toMap()["kept"].toBool());
        QVERIFY(office.resetTranslations()); QCOMPARE(office.completed(), 0); QVERIFY(!office.hasUnexportedResult());
        QVERIFY(!office.editSegment(id, "   ")); QVERIFY(!office.editSegment("missing", "test"));
        office.setRendererPath(dir.filePath("missing.exe")); QVERIFY(!office.generateLayoutPreview()); QVERIFY(!office.processing());
    }
    void draftRestorationAndExport() {
        QTemporaryDir dir; AppController app(dir.path()); const auto source = copyFixture(dir, "sample.docx"); QString id;
        {
            OfficeController office(&app); QVERIFY(office.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY(!office.processing());
            id = office.segments().first().toMap()["id"].toString(); office.stageEdit(id, QStringLiteral("未提交的校对草稿"));
            QVERIFY(office.draftDirty()); QVERIFY(office.hasUnexportedResult()); QCOMPARE(office.completed(), 0);
        }
        OfficeController restored(&app); QVERIFY(restored.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY(!restored.processing());
        QCOMPARE(restored.draftId(), id); QCOMPARE(restored.draftText(), QStringLiteral("未提交的校对草稿"));
        const auto output = dir.filePath("draft.docx"); QVERIFY(restored.saveFile(QUrl::fromLocalFile(output))); QTRY_VERIFY(!restored.processing());
        QVERIFY2(restored.error().isEmpty(), qPrintable(restored.error())); QVERIFY(!restored.draftDirty()); QVERIFY(!restored.hasUnexportedResult());
        QCOMPARE(OfficeDocument::load(output).segments.first().source, QStringLiteral("未提交的校对草稿")); QVERIFY(QFile::exists(output + ".issues.txt"));
    }
    void officeReplacement_data() {
        QTest::addColumn<QString>("choice"); for (const auto &choice : {"cancel", "discard", "export"}) QTest::newRow(choice) << QString::fromLatin1(choice);
    }
    void officeReplacement() {
        QFETCH(QString, choice); QTemporaryDir dir; AppController app(dir.path()); OfficeController office(&app); DocumentController txt(&app); PdfController pdf(&app); FileImportController imports(&txt, &pdf, &office);
        const auto source = copyFixture(dir, "sample.docx"), next = copyFixture(dir, "sample.pptx");
        QVERIFY(imports.requestOpen(QUrl::fromLocalFile(source))); QTRY_VERIFY(!office.processing());
        const auto id = office.segments().first().toMap()["id"].toString(); office.stageEdit(id, QStringLiteral("原文档校对"));
        QVERIFY(imports.requestOpen(QUrl::fromLocalFile(next))); QVERIFY(imports.confirmationPending()); QCOMPARE(imports.replacementKind(), QStringLiteral("docx"));
        if (choice == "cancel") { imports.cancelReplacement(); QCOMPARE(office.kind(), QStringLiteral("docx")); QVERIFY(office.draftDirty()); }
        else if (choice == "discard") { QVERIFY(imports.confirmReplacement()); QTRY_VERIFY(!office.processing()); QCOMPARE(office.kind(), QStringLiteral("pptx")); }
        else {
            QVERIFY(!imports.exportAndReplace(QUrl::fromLocalFile(next))); QVERIFY(imports.confirmationPending());
            QVERIFY(imports.exportAndReplace(QUrl::fromLocalFile(dir.filePath("missing/out.docx")))); QTRY_VERIFY(!office.processing()); QVERIFY(imports.confirmationPending()); QCOMPARE(office.kind(), QStringLiteral("docx"));
            const auto output = dir.filePath("saved.docx"); QVERIFY(imports.exportAndReplace(QUrl::fromLocalFile(output)));
            QTRY_VERIFY(!imports.confirmationPending()); QTRY_VERIFY(!office.processing()); QCOMPARE(office.kind(), QStringLiteral("pptx"));
            QCOMPARE(OfficeDocument::load(output).segments.first().source, QStringLiteral("原文档校对"));
        }
    }
    void restoredTaskDetectsChangedService() {
        QTemporaryDir dir; AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server));
        {
            OfficeController office(&app); QVERIFY(office.loadFile(QUrl::fromLocalFile(fixture("sample.docx")))); QTRY_VERIFY(!office.processing()); office.start(); QTRY_VERIFY(server.ready); office.cancel();
        }
        auto draft = app.providerDraft(app.selectedProviderId()); draft["model"] = "different-model"; QVERIFY(app.saveProvider(draft).isEmpty());
        OfficeController restored(&app); QVERIFY(restored.loadFile(QUrl::fromLocalFile(fixture("sample.docx")))); QTRY_VERIFY(!restored.processing()); restored.start();
        QVERIFY(!restored.busy()); QVERIFY(restored.error().contains(QStringLiteral("配置已变化"))); QCOMPARE(server.requestCount, 1);
    }
    void restoredTaskSettingsMatchTranslationRequests_data() {
        QTest::addColumn<QString>("protocol");
        QTest::newRow("chat") << QStringLiteral("openai");
        QTest::newRow("responses") << QStringLiteral("responses");
    }
    void restoredTaskSettingsMatchTranslationRequests() {
        QFETCH(QString, protocol); QTemporaryDir dir; AppController app(dir.path()); StreamServer server(protocol); QVERIFY(configure(app, server));
        const auto source = copyFixture(dir, "sample.docx"); QString id;
        {
            OfficeController office(&app); QVERIFY(office.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY(!office.processing());
            QVERIFY(office.taskSettings().isEmpty()); id = office.segments().first().toMap()["id"].toString();
            QVERIFY(office.retrySegment(id)); QTRY_VERIFY(server.ready);
            server.sendText(QStringLiteral("原有中文译文")); server.finish({}); QTRY_VERIFY(!office.busy());
            QCOMPARE(office.taskSettings()["target"].toString(), QStringLiteral("简体中文"));
            QVERIFY(!office.taskSettings().contains("apiKey"));
        }
        app.setTargetLanguage(QStringLiteral("英语")); app.setTone(QStringLiteral("正式"));
        OfficeController restored(&app); QVERIFY(restored.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY(!restored.processing());
        QCOMPARE(restored.taskSettings()["target"].toString(), QStringLiteral("简体中文"));
        QCOMPARE(restored.taskSettings()["tone"].toString(), QStringLiteral("自然"));
        QCOMPARE(restored.taskSettings()["providerId"].toString(), app.selectedProviderId());
        QCOMPARE(app.targetLanguage(), QStringLiteral("英语")); QCOMPARE(restored.completed(), 1);
        server.ready = false; server.request.clear(); QVERIFY(restored.retrySegment(id)); QTRY_VERIFY(server.ready);
        QVERIFY(server.request.contains(QStringLiteral("to 简体中文").toUtf8())); restored.cancel();
        QVERIFY(restored.resetTranslations()); QVERIFY(restored.taskSettings().isEmpty()); QCOMPARE(restored.completed(), 0);
        server.ready = false; server.request.clear(); QVERIFY(restored.retrySegment(id)); QTRY_VERIFY(server.ready);
        QVERIFY(server.request.contains(QStringLiteral("to 英语").toUtf8())); QVERIFY(server.request.contains(QStringLiteral("正式").toUtf8()));
        server.sendText("A practical guide"); server.finish({}); QTRY_VERIFY(!restored.busy());
        QCOMPARE(restored.taskSettings()["target"].toString(), QStringLiteral("英语"));
        QCOMPARE(restored.segments().first().toMap()["translation"].toString(), QStringLiteral("A practical guide"));
    }
    void cacheWriteFailureRetainsCurrentDocument() {
        QTemporaryDir dir; AppController app(dir.path()); QFile blocker(dir.filePath("office-tasks")); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.write("blocked"); blocker.close();
        OfficeController office(&app); QVERIFY(office.loadFile(QUrl::fromLocalFile(fixture("sample.docx")))); QTRY_VERIFY(!office.processing());
        const auto id = office.segments().first().toMap()["id"].toString(); QVERIFY(!office.editSegment(id, QStringLiteral("内存中保留的译文"))); QCOMPARE(office.completed(), 1);
        QVERIFY(!office.loadFile(QUrl::fromLocalFile(fixture("sample.pptx")))); QCOMPARE(office.kind(), QStringLiteral("docx"));
        QCOMPARE(office.segments().first().toMap()["translation"].toString(), QStringLiteral("内存中保留的译文"));
    }
    void embeddedImageReviewDoesNotCallModel() {
        QTemporaryDir dir; AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server)); OfficeController office(&app);
        QVERIFY(office.loadFile(QUrl::fromLocalFile(fixture("sample.docx")))); QTRY_VERIFY(!office.processing()); QVERIFY(!office.images().isEmpty());
        QSignalSpy review(&office, &OfficeController::imageReviewRequested);
        QVERIFY(office.reviewImage(office.images().first().toMap()["id"].toString())); QCOMPARE(review.size(), 1); QVERIFY(!qvariant_cast<QImage>(review.first().first()).isNull());
        QVERIFY(!office.reviewImage("missing")); QCOMPARE(server.requestCount, 0); QVERIFY(!office.busy());
    }
    void imageOnlyDocumentCanBeReviewed() {
        const auto document = OfficeDocument::load(fixture("images-only.docx")); QVERIFY2(document.valid(), qPrintable(document.error));
        QCOMPARE(document.segments.size(), 0); QVERIFY(!document.images.isEmpty()); QString error;
        QVERIFY(!document.image(document.images.first().toMap()["id"].toString(), &error).isNull());
    }
    void optionalRendererContractAndTemporaryProfile() {
        QTemporaryDir dir; AppController app(dir.path()); OfficeController office(&app);
        QVERIFY(office.loadFile(QUrl::fromLocalFile(fixture("sample.pptx")))); QTRY_VERIFY(!office.processing());
        office.setRendererPath(QCoreApplication::applicationFilePath()); QVERIFY(office.generateLayoutPreview()); QVERIFY(office.processing());
        QVERIFY(!office.loadFile(QUrl::fromLocalFile(fixture("sample.docx"))));
        QTRY_VERIFY_WITH_TIMEOUT(!office.processing(), 15000); QVERIFY2(office.error().isEmpty(), qPrintable(office.error()));
        QVERIFY(!office.layoutPreview().isEmpty()); QVERIFY(bytes(office.layoutPreview().toLocalFile()).startsWith("%PDF"));
        office.setMinimumFont(14); QVERIFY(office.layoutPreview().isEmpty());
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv); const auto args = app.arguments();
    // Simulate the external renderer's public command line contract. This
    // validates process arguments/lifecycle, not LibreOffice's layout engine.
    if (args.contains("--convert-to")) {
        const auto out = args.indexOf("--outdir");
        if (!args.contains("--headless") || out < 0 || out + 1 >= args.size() || !OfficeDocument::load(args.last()).valid()) return 2;
        bool isolatedProfile = false;
        for (const auto &arg : args) if (arg.startsWith("-env:UserInstallation=file:")) isolatedProfile = true;
        if (!isolatedProfile) return 3;
        const auto outputPath = QDir(args.at(out + 1)).filePath("preview.pdf");
        QPdfWriter writer(outputPath); QPainter painter(&writer);
        painter.drawText(100, 100, "Synthetic layout preview fixture"); painter.end(); return 0;
    }
    OfficeTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "tst_office.moc"
