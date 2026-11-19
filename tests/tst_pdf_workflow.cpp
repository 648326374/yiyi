#include "appcontroller.h"
#include "pdfcontroller.h"
#include "pdfengine.h"
#include "pdffixture.h"
#include "streamserver.h"
#include <QtTest>
#include <QDir>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <memory>

class PdfWorkflowTests : public QObject {
    Q_OBJECT
    static void reply(StreamServer &server, const QString &text) {
        server.sendText(text); server.finish(text); server.ready = false; server.request.clear();
    }
    static QStringList translations(const PdfController &pdf) {
        QStringList result; for (const auto &v : pdf.blocks()) result << v.toMap()["translation"].toString(); return result;
    }
    static QString configure(AppController &app, StreamServer &server, const QString &model = "offline-model") {
        return app.saveProvider({{"name", "Offline test"}, {"protocol", server.protocol}, {"baseUrl", server.url()}, {"model", model}, {"apiKey", "workflow-test-secret"}});
    }
private slots:
    void abnormalResponseRetriesWithoutChangingModel_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("bad");
        for (const auto &protocol : {QStringLiteral("openai"), QStringLiteral("responses")}) {
            QTest::newRow(qPrintable(protocol + "-script")) << protocol << QStringLiteral("标题\u0ab0");
            QTest::newRow(qPrintable(protocol + "-marker")) << protocol << QStringLiteral("标题】【。");
        }
    }
    void abnormalResponseRetriesWithoutChangingModel() {
        QFETCH(QString, protocol); QFETCH(QString, bad);
        QTemporaryDir dir; const auto source = dir.filePath("retry.pdf"); makePdfFixture(source, 2);
        AppController app(dir.path()); StreamServer server(protocol); QVERIFY(configure(app, server, "gpt-6-luna").isEmpty());
        auto draft = app.providerDraft(app.selectedProviderId()); draft["reasoningEffort"] = "medium"; QVERIFY(app.saveProvider(draft).isEmpty());
        PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.analysisComplete(), 20000);
        pdf.setPage(1); pdf.translate(false); QTRY_VERIFY(server.ready); reply(server, bad);
        QTRY_VERIFY(pdf.activityStatus().contains(QStringLiteral("自动重试 1 / 2")));
        QVERIFY(pdf.busy()); QVERIFY(pdf.error().isEmpty()); QCOMPARE(pdf.page(), 1); QCOMPARE(pdf.completed(), 0);
        QVERIFY(pdf.activityStatus().contains(QStringLiteral("第 1 页")));
        QTRY_VERIFY(server.ready);
        const auto request = QJsonDocument::fromJson(server.request.mid(server.request.indexOf("\r\n\r\n") + 4)).object();
        QCOMPARE(request["model"].toString(), QStringLiteral("gpt-6-luna"));
        QCOMPARE(protocol == "responses" ? request["reasoning"].toObject()["effort"].toString() : request["reasoning_effort"].toString(), QStringLiteral("medium"));
        QVERIFY(server.request.contains("failed validation"));
        QVERIFY(!server.request.contains(bad.toUtf8()));
        reply(server, QStringLiteral("正确标题"));
        for (int i = 0; i < 5; ++i) { QTRY_VERIFY(server.ready); reply(server, QStringLiteral("正确译文")); }
        QTRY_VERIFY(!pdf.busy()); QCOMPARE(server.requestCount, 7); QCOMPARE(pdf.completed(), 6); QCOMPARE(pdf.reviewPages(), 0); QVERIFY(!pdf.taskPending());
        pdf.setPage(0); QVERIFY(pdf.blocks().first().toMap()["rejectedTranslation"].toString().isEmpty());
    }
    void rejectedBlockDoesNotStopLaterPagesAndSurvivesRestart_data() {
        QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QStringLiteral("openai"); QTest::newRow("responses") << QStringLiteral("responses");
    }
    void rejectedBlockDoesNotStopLaterPagesAndSurvivesRestart() {
        QFETCH(QString, protocol);
        QTemporaryDir dir; const auto source = dir.filePath("deferred.pdf"); makePdfFixture(source, 2);
        AppController app(dir.path()); StreamServer server(protocol); QVERIFY(configure(app, server).isEmpty());
        const auto bad = QStringLiteral("译文\u0ab0"); QString id;
        {
            PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.analysisComplete(), 20000);
            id = pdf.blocks().first().toMap()["id"].toString(); pdf.translate(false);
            for (int i = 0; i < 3; ++i) { QTRY_VERIFY(server.ready); reply(server, bad); }
            // The failed first paragraph must not prevent the next page.
            for (int i = 0; i < 5; ++i) { QTRY_VERIFY(server.ready); reply(server, QStringLiteral("后续段落已完成")); }
            QTRY_VERIFY(!pdf.busy()); QCOMPARE(server.requestCount, 8); QCOMPARE(pdf.completed(), 5); QVERIFY(pdf.taskPending());
            QCOMPARE(pdf.reviewPages(), 1); QCOMPARE(pdf.pendingPages(), 1); QVERIFY(pdf.error().contains(QStringLiteral("仍有 1 段")));
            const auto b = pdf.blocks().first().toMap(); QVERIFY(b["translation"].toString().isEmpty()); QCOMPARE(b["rejectedTranslation"].toString(), bad); QCOMPARE(b["translationAttempts"].toInt(), 3);
            QCOMPARE(pdf.pageIssues().first().toMap()["blockId"].toString(), id);
            pdf.setPage(1); QCOMPARE(pdf.pageCompleted(), 3);
        }
        PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        QCOMPARE(pdf.completed(), 5); QCOMPARE(pdf.reviewPages(), 1); QVERIFY(pdf.taskPending());
        QCOMPARE(pdf.blocks().first().toMap()["rejectedTranslation"].toString(), bad);
        pdf.resumeTask(); QTRY_VERIFY(server.ready); reply(server, QStringLiteral("重试成功"));
        QTRY_VERIFY(!pdf.busy()); QCOMPARE(server.requestCount, 9); QCOMPARE(pdf.completed(), 6); QCOMPARE(pdf.reviewPages(), 0);
        QVERIFY(!pdf.taskPending()); QVERIFY(pdf.error().isEmpty());
    }
    void pauseCancelsScheduledValidationRetry() {
        QTemporaryDir dir; const auto source = dir.filePath("pause.pdf"); makePdfFixture(source, 1);
        AppController app(dir.path()); StreamServer server("responses"); QVERIFY(configure(app, server).isEmpty());
        PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        pdf.translate(true); QTRY_VERIFY(server.ready); reply(server, QStringLiteral("错误】【。"));
        QTRY_VERIFY(pdf.activityStatus().contains(QStringLiteral("自动重试"))); pdf.cancel();
        QTest::qWait(1200); QCOMPARE(server.requestCount, 1); QVERIFY(!pdf.busy()); QVERIFY(pdf.taskPending());
        pdf.resumeTask();
        for (int i = 0; i < 3; ++i) { QTRY_VERIFY(server.ready); reply(server, QStringLiteral("恢复后的译文")); }
        QTRY_VERIFY(!pdf.busy()); QCOMPARE(server.requestCount, 4); QCOMPARE(pdf.completed(), 3); QCOMPARE(pdf.reviewPages(), 0);
    }
    void repeatedRejectionsPauseInsteadOfSpendingIndefinitely() {
        QTemporaryDir dir; const auto source = dir.filePath("unavailable.pdf"); makePdfFixture(source, 2);
        AppController app(dir.path()); StreamServer server("responses"); QVERIFY(configure(app, server).isEmpty());
        PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        pdf.translate(false);
        for (int i = 0; i < 9; ++i) { QTRY_VERIFY(server.ready); reply(server, QStringLiteral("错误\u0ab0")); }
        QTRY_VERIFY(!pdf.busy()); QVERIFY(pdf.error().contains(QStringLiteral("连续 3 段"))); QVERIFY(pdf.taskPending());
        QTest::qWait(100); QCOMPARE(server.requestCount, 9); QCOMPARE(pdf.completed(), 0); QCOMPARE(pdf.pageIssues().size(), 3);
    }
    void rejectedRetranslationKeepsWholeOldPage() {
        QTemporaryDir dir; const auto source = dir.filePath("atomic.pdf"); makePdfFixture(source, 1);
        AppController app(dir.path()); StreamServer server("responses"); QVERIFY(configure(app, server).isEmpty());
        PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        for (const auto &v : pdf.blocks()) if (!v.toMap()["fixed"].toBool()) QVERIFY(pdf.editBlock(v.toMap()["id"].toString(), QStringLiteral("已有译文")));
        const auto old = translations(pdf); QVERIFY(pdf.retranslatePage()); QTRY_VERIFY(server.ready); reply(server, QStringLiteral("暂存的新标题"));
        for (int i = 0; i < 3; ++i) { QTRY_VERIFY(server.ready); reply(server, QStringLiteral("错误\u0ab0")); }
        QTRY_VERIFY(!pdf.busy()); QCOMPARE(translations(pdf), old); QVERIFY(pdf.taskPending());
        pdf.resumeTask(); for (int i = 0; i < 2; ++i) { QTRY_VERIFY(server.ready); reply(server, QStringLiteral("新的正文")); }
        QTRY_VERIFY(!pdf.busy()); QVERIFY(!pdf.taskPending()); QCOMPARE(server.requestCount, 6); QCOMPARE(translations(pdf).first(), QStringLiteral("暂存的新标题"));
    }
    void pdfWaitPreferencePersistsWithoutChangingTask() {
        QTemporaryDir dir; const auto source = dir.filePath("timeout.pdf"); makePdfFixture(source, 1);
        AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server, "gpt-6-luna").isEmpty());
        auto provider = app.providerDraft(app.selectedProviderId()); provider["reasoningEffort"] = "medium"; QVERIFY(app.saveProvider(provider).isEmpty());
        {
            PdfController pdf(&app); QCOMPARE(pdf.requestTimeoutMinutes(), 10); pdf.setRequestTimeoutMinutes(20); QCOMPARE(pdf.requestTimeoutMinutes(), 20);
            QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000); pdf.translate(true); QTRY_VERIFY(server.ready);
            const auto body = QJsonDocument::fromJson(server.request.mid(server.request.indexOf("\r\n\r\n") + 4)).object();
            QCOMPARE(body["model"].toString(), QStringLiteral("gpt-6-luna")); QCOMPARE(body["reasoning_effort"].toString(), QStringLiteral("medium"));
            QVERIFY(pdf.activityStatus().contains(QStringLiteral("等待模型响应")));
            pdf.setRequestTimeoutMinutes(5); QCOMPARE(pdf.requestTimeoutMinutes(), 20); pdf.cancel(); QVERIFY(pdf.taskPending());
            pdf.setRequestTimeoutMinutes(30); QCOMPARE(pdf.requestTimeoutMinutes(), 30);
        }
        PdfController pdf(&app); QCOMPARE(pdf.requestTimeoutMinutes(), 30); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000); QVERIFY(pdf.taskPending()); QCOMPARE(pdf.taskModel(), QStringLiteral("gpt-6-luna"));
        server.ready = false; server.request.clear(); pdf.resumeTask();
        for (int i = 0; i < 3; ++i) { QTRY_VERIFY(server.ready); reply(server, QStringLiteral("完整译文")); }
        QTRY_VERIFY(!pdf.busy()); QCOMPARE(pdf.completed(), 3); QVERIFY(pdf.error().isEmpty());
    }
    void mediumPdfWaitsPastOldDeadline() {
        if (!qEnvironmentVariableIsSet("YIYI_TEST_SLOW_AI")) QSKIP("Opt-in 95-second silent model response; uses only localhost.");
        QTemporaryDir dir; const auto source = dir.filePath("slow.pdf"); makePdfFixture(source, 1);
        AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server, "gpt-6-luna").isEmpty());
        auto provider = app.providerDraft(app.selectedProviderId()); provider["reasoningEffort"] = "medium"; QVERIFY(app.saveProvider(provider).isEmpty());
        PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        const auto first = pdf.blocks().first().toMap(); QVERIFY(pdf.editBlock(first["id"].toString(), QStringLiteral("原有标题")));
        QVERIFY(pdf.retranslatePage()); QTRY_VERIFY(server.ready);
        const auto request = QJsonDocument::fromJson(server.request.mid(server.request.indexOf("\r\n\r\n") + 4)).object();
        QCOMPARE(request["model"].toString(), QStringLiteral("gpt-6-luna")); QCOMPARE(request["reasoning_effort"].toString(), QStringLiteral("medium"));
        QTest::qWait(95000); QVERIFY(pdf.busy()); QVERIFY(pdf.error().isEmpty()); QCOMPARE(server.requestCount, 1);
        QCOMPARE(pdf.blocks().first().toMap()["translation"].toString(), QStringLiteral("原有标题"));
        reply(server, QStringLiteral("新的完整标题"));
        for (int i = 0; i < 2; ++i) { QTRY_VERIFY(server.ready); reply(server, QStringLiteral("完整正文")); }
        QTRY_VERIFY(!pdf.busy()); QVERIFY(pdf.error().isEmpty()); QCOMPARE(pdf.completed(), 3); QVERIFY(!pdf.taskPending());
        QCOMPARE(pdf.blocks().first().toMap()["translation"].toString(), QStringLiteral("新的完整标题"));
    }
    void extractionUpgradeRevisitsEarlierPendingText() {
        QTemporaryDir dir; const auto source = dir.filePath("upgrade.pdf"); makePdfFixture(source, 2);
        AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server).isEmpty());
        {
            PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.analysisComplete(), 20000);
            pdf.translate(false);
            for (int i = 0; i < 3; ++i) { QTRY_VERIFY(server.ready); reply(server, QStringLiteral("已有译文")); }
            QTRY_VERIFY(server.ready); pdf.cancel(); QCOMPARE(pdf.completed(), 3);
        }
        const auto info = PdfEngine::execute({{"action", "info"}, {"source", source}});
        PdfTaskStore store; QVERIFY(store.open(dir.filePath("pdf-tasks/" + info["hash"].toString() + ".sqlite")));
        QCOMPARE(store.metadata()["workflow"].toObject()["cursor"].toInt(), 1);
        auto page = store.page(0); page["extractionVersion"] = -1; page["baseLayoutHash"] = "old-extraction";
        auto blocks = page["blocks"].toArray(); auto block = blocks[1].toObject(); block.remove("translation"); block["keep"] = true; block["protected"] = true;
        blocks[1] = block; page["blocks"] = blocks; QVERIFY(store.putPage(0, page)); store.close();
        server.ready = false; server.request.clear();
        PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.analysisComplete(), 20000);
        QCOMPARE(pdf.completed(), 2); QVERIFY(pdf.taskPending()); pdf.resumeTask();
        for (int i = 0; i < 4; ++i) { QTRY_VERIFY(server.ready); reply(server, QStringLiteral("补全译文")); }
        QTRY_VERIFY(!pdf.busy()); QVERIFY2(pdf.error().isEmpty(), qUtf8Printable(pdf.error()));
        QCOMPARE(pdf.completed(), 6); QVERIFY(!pdf.taskPending());
        QCOMPARE(pdf.blocks().first().toMap()["translation"].toString(), QStringLiteral("已有译文"));
    }
    void scanThenTranslate_data() {
        QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QStringLiteral("openai"); QTest::newRow("responses") << QStringLiteral("responses");
    }
    void scanThenTranslate() {
        QFETCH(QString, protocol); QTemporaryDir dir; const auto source = dir.filePath("scan.pdf"); makeScanPdfFixture(source, 2);
        AppController app(dir.path()); StreamServer server(protocol); QVERIFY(configure(app, server).isEmpty()); PdfController pdf(&app);
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.analysisComplete(), 20000);
        QVERIFY(!pdf.taskPending());
        bool unlocked = false; connect(&pdf, &PdfController::changed, &pdf, [&] { if (pdf.busy() && !app.pdfBusy()) unlocked = true; });
        pdf.translate(false);
        for (int page = 0; page < 2; ++page) {
            for (const auto &text : {"Scanned document", "Please close the door.", "Keep the original page for comparison."}) {
                QTRY_VERIFY2_WITH_TIMEOUT(server.ready, qPrintable(QStringLiteral("page %1: %2; %3").arg(page).arg(pdf.activityStatus(), pdf.error())), 20000); QVERIFY(pdf.recognizing()); QVERIFY(server.request.contains("data:image")); reply(server, text);
            }
            for (int i = 0; i < 3; ++i) {
                QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000); QVERIFY(!pdf.recognizing()); QVERIFY(!server.request.contains("data:image")); reply(server, QStringLiteral("第 %1 页译文。").arg(page + 1));
            }
        }
        QTRY_VERIFY(!pdf.busy()); QCOMPARE(pdf.completed(), 6); QCOMPARE(pdf.recognizedPages(), 2); QCOMPARE(server.requestCount, 12);
        QVERIFY(!pdf.taskPending()); QVERIFY(pdf.error().isEmpty()); QVERIFY(!app.pdfBusy()); QVERIFY(!unlocked);
    }
    void pauseAtRecognitionBoundaryAndReopen_data() {
        QTest::addColumn<bool>("recognizing"); QTest::newRow("last-region") << true; QTest::newRow("stage-transition") << false;
    }
    void pauseAtRecognitionBoundaryAndReopen() {
        QFETCH(bool, recognizing); QTemporaryDir dir; const auto source = dir.filePath("scan.pdf"); makeScanPdfFixture(source, 2);
        AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server).isEmpty());
        {
            PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.analysisComplete(), 20000);
            bool paused = false;
            connect(&pdf, &PdfController::changed, &pdf, [&] {
                if (!paused && pdf.busy() && pdf.recognizing() == recognizing && pdf.recognizedPages() == 1) { paused = true; pdf.cancel(); }
            });
            pdf.translate(true);
            for (const auto &text : {"Title", "First paragraph.", "Second paragraph."}) { QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000); reply(server, text); }
            QTRY_VERIFY(!pdf.busy()); QVERIFY(paused); QVERIFY(pdf.taskPending()); QVERIFY(pdf.error().isEmpty());
            QTest::qWait(100); QCOMPARE(server.requestCount, 3); QCOMPARE(pdf.completed(), 0); QVERIFY(!app.pdfBusy());
        }
        PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        QVERIFY(pdf.taskPending()); QCOMPARE(pdf.taskScope(), QStringLiteral("第 1 页")); QVERIFY(!pdf.busy());
        pdf.setPage(1); pdf.resumeTask();
        for (int i = 0; i < 3; ++i) { QTRY_VERIFY(server.ready); QVERIFY(!server.request.contains("data:image")); reply(server, QStringLiteral("完整译文。")); }
        QTRY_VERIFY(!pdf.busy()); QCOMPARE(pdf.completed(), 3); QCOMPARE(pdf.pageCompleted(), 0); QCOMPARE(pdf.recognizedPages(), 1); QVERIFY(!pdf.taskPending());
    }
    void recognitionFailureRetainsCompletedPages() {
        QTemporaryDir dir; const auto source = dir.filePath("scan.pdf"); makeScanPdfFixture(source, 2);
        AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server).isEmpty()); PdfController pdf(&app);
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.analysisComplete(), 20000); pdf.translate(false);
        for (int i = 0; i < 6; ++i) { QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000); reply(server, i < 3 ? QStringLiteral("Original paragraph") : QStringLiteral("已保存译文")); }
        QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000); server.sendText("Incomplete"); server.interrupt(); server.ready = false; server.request.clear();
        QTRY_VERIFY(!pdf.busy()); QVERIFY(!pdf.error().isEmpty()); QVERIFY(pdf.taskPending()); QCOMPARE(pdf.completed(), 3); QCOMPARE(pdf.recognizedPages(), 1);
        const auto old = translations(pdf); pdf.resumeTask();
        for (int i = 0; i < 6; ++i) { QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000); reply(server, i < 3 ? QStringLiteral("Original paragraph") : QStringLiteral("新页译文")); }
        QTRY_VERIFY(!pdf.busy()); QCOMPARE(pdf.completed(), 6); QCOMPARE(translations(pdf), old); QVERIFY(pdf.error().isEmpty());
    }
    void retranslationIsAtomic_data() {
        QTest::addColumn<QString>("failure");
        for (const auto *failure : {"pause", "disconnect", "empty"}) QTest::newRow(failure) << QString::fromLatin1(failure);
    }
    void retranslationIsAtomic() {
        QFETCH(QString, failure); QTemporaryDir dir; const auto source = dir.filePath("source.pdf"); makePdfFixture(source, 2);
        AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server).isEmpty()); QStringList firstPage, otherPage;
        {
            PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.analysisComplete(), 20000);
            for (int page = 0; page < 2; ++page) {
                pdf.setPage(page); for (const auto &v : pdf.blocks()) if (!v.toMap()["fixed"].toBool()) QVERIFY(pdf.editBlock(v.toMap()["id"].toString(), QStringLiteral("原有校对译文 %1").arg(page + 1)));
                if (page == 0) firstPage = translations(pdf); else otherPage = translations(pdf);
            }
            pdf.setPage(0); QVERIFY(pdf.retranslatePage()); QTRY_VERIFY(server.ready); reply(server, QStringLiteral("新标题"));
            QTRY_VERIFY(server.ready); QCOMPARE(translations(pdf), firstPage); // First result stays staged.
            if (failure != "empty") server.sendText(QStringLiteral("未完成"));
            if (failure == "pause") pdf.cancel(); else if (failure == "empty") server.finish({}); else server.interrupt();
            server.ready = false; server.request.clear(); QTRY_VERIFY(!pdf.busy());
            QCOMPARE(translations(pdf), firstPage); QCOMPARE(pdf.completed(), 6); QVERIFY(pdf.taskPending()); QCOMPARE(pdf.error().isEmpty(), failure == "pause");
            pdf.setPage(1); QCOMPARE(translations(pdf), otherPage);
        }
        PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        QVERIFY(pdf.taskPending()); QCOMPARE(translations(pdf), firstPage); pdf.setPage(1); pdf.resumeTask();
        for (int i = 0; i < 2; ++i) { QTRY_VERIFY(server.ready); reply(server, QStringLiteral("新正文")); }
        QTRY_VERIFY(!pdf.busy()); QVERIFY(!pdf.taskPending()); QCOMPARE(translations(pdf), otherPage);
        pdf.setPage(0); QCOMPARE(translations(pdf).first(), QStringLiteral("新标题")); QVERIFY(translations(pdf) != firstPage); QCOMPARE(server.requestCount, 4);
    }
    void pausedRetranslationRespectsUserEdits() {
        QTemporaryDir dir; const auto source = dir.filePath("source.pdf"); makePdfFixture(source, 1);
        AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server).isEmpty()); PdfController pdf(&app);
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        QVERIFY(pdf.retranslatePage()); QTRY_VERIFY(server.ready); reply(server, QStringLiteral("过时暂存"));
        QTRY_VERIFY(server.ready); pdf.cancel(); server.ready = false; server.request.clear();
        const auto first = pdf.blocks().first().toMap(); QVERIFY(pdf.reviseBlock(first["id"].toString(), {{"source", "Updated source"}, {"translation", QStringLiteral("人工修订")}}));
        pdf.resumeTask(); QTRY_VERIFY(server.ready); QVERIFY(server.request.contains("Updated source")); reply(server, QStringLiteral("更新标题"));
        for (int i = 0; i < 2; ++i) { QTRY_VERIFY(server.ready); reply(server, QStringLiteral("更新正文")); }
        QTRY_VERIFY(!pdf.busy()); QCOMPARE(translations(pdf).first(), QStringLiteral("更新标题")); QCOMPARE(server.requestCount, 5);
    }
    void actualModelAndSecrets() {
        QTemporaryDir dir; const auto source = dir.filePath("source.pdf"); makePdfFixture(source, 1);
        AppController app(dir.path()); StreamServer original("openai"), other("openai"); QVERIFY(configure(app, original, "original-model").isEmpty()); PdfController pdf(&app);
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        pdf.translate(true); QTRY_VERIFY(original.ready); pdf.cancel(); original.ready = false; original.request.clear();
        const auto savedId = pdf.taskProviderId(); QVERIFY(configure(app, other, "other-model").isEmpty());
        app.selectProvider(app.providers().last().toMap()["id"].toString()); app.setTargetLanguage(QStringLiteral("英语"));
        QCOMPARE(app.modelName(), QStringLiteral("other-model"));
        QCOMPARE(pdf.taskModel(), QStringLiteral("original-model")); QCOMPARE(pdf.taskTarget(), QStringLiteral("简体中文")); QCOMPARE(pdf.taskProviderId(), savedId);
        pdf.resumeTask(); QTRY_VERIFY(original.ready); QVERIFY(original.request.contains("original-model")); QCOMPARE(other.requestCount, 0); pdf.cancel();
        const QDir tasks(dir.filePath("pdf-tasks")); PdfTaskStore saved; QVERIFY(saved.open(tasks.filePath(tasks.entryList({"*.sqlite"}).first()), true));
        QVERIFY(!QJsonDocument(saved.metadata()).toJson().contains("workflow-test-secret"));
        auto draft = app.providerDraft(savedId); draft["model"] = "changed"; QVERIFY(app.saveProvider(draft).isEmpty()); pdf.resumeTask();
        QVERIFY(!pdf.busy()); QVERIFY(pdf.error().contains(QStringLiteral("已改变"))); QVERIFY(pdf.taskPending());
    }
    void pauseBeforeFirstRequest() {
        QTemporaryDir dir; const auto source = dir.filePath("cancel.pdf"); makePdfFixture(source, 1);
        AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server).isEmpty()); PdfController pdf(&app);
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        bool paused = false;
        connect(&pdf, &PdfController::changed, &pdf, [&] { if (!paused && pdf.busy() && pdf.activityStatus().contains("0 / 3")) { paused = true; pdf.cancel(); } });
        pdf.translate(true); QTRY_VERIFY(paused); QTest::qWait(100); QCOMPARE(server.requestCount, 0); QVERIFY(!pdf.busy()); QVERIFY(pdf.taskPending());
        pdf.resumeTask(); QTRY_VERIFY(server.ready); pdf.cancel(); QCOMPARE(server.requestCount, 1);
    }
    void mixedPagesShareOneTask() {
        QTemporaryDir dir; const auto source = dir.filePath("mixed.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QPageSize::A4)); writer.setPageMargins(QMarginsF()); QPainter painter(&writer);
            QFont font("Times New Roman"); font.setPixelSize(20); painter.setFont(font);
            for (int p = 0; p < 3; ++p) {
                if (p) writer.newPage();
                if (p != 1) { painter.drawText(QPointF(50, 80), "Native paragraph for translation."); continue; }
                QImage page(1200, 1698, QImage::Format_RGB32); page.fill(Qt::white); QPainter ink(&page);
                font.setPixelSize(48); ink.setFont(font); ink.drawText(QPointF(100, 170), "Scanned document");
                font.setPixelSize(32); ink.setFont(font); ink.drawText(QPointF(100, 280), "Please close the door.");
                ink.drawText(QPointF(100, 340), "Keep the original page for comparison."); ink.end(); painter.drawImage(QRectF(0, 0, 595, 842), page);
            }
        }
        AppController app(dir.path()); StreamServer server("openai"); QVERIFY(configure(app, server).isEmpty()); PdfController pdf(&app);
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.analysisComplete(), 20000); QCOMPARE(pdf.scanPages(), 1);
        pdf.translate(false); int imageRequests = 0, textRequests = 0;
        for (int i = 0; i < 20; ++i) {
            QTRY_VERIFY2_WITH_TIMEOUT(server.ready || !pdf.busy(), qPrintable(pdf.error()), 20000); if (!pdf.busy()) break;
            const bool image = server.request.contains("data:image"); if (image) ++imageRequests; else ++textRequests;
            reply(server, image ? QStringLiteral("Original paragraph.") : QStringLiteral("混合文档译文。"));
        }
        QTRY_VERIFY(!pdf.busy()); QVERIFY(pdf.error().isEmpty()); QVERIFY(imageRequests > 0); QCOMPARE(textRequests, pdf.total());
        QCOMPARE(pdf.completed(), pdf.total()); QCOMPARE(pdf.pendingPages(), 0); QCOMPARE(pdf.recognizedPages(), 1);
    }
    void issueIndexAndLegacySummaryMigration() {
        QTemporaryDir dir; const auto source = dir.filePath("issues.pdf"); makePdfFixture(source, 2);
        AppController app(dir.path());
        {
            PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.analysisComplete(), 20000);
            const auto id = pdf.blocks().first().toMap()["id"].toString();
            QVERIFY(pdf.editBlock(id, QStringLiteral("旧译文"))); QVERIFY(pdf.reviseBlock(id, {{"source", "Title [Unclear]"}}));
            QCOMPARE(pdf.reviewPages(), 1); QCOMPARE(pdf.pendingPages(), 2); QCOMPARE(pdf.pageIssues().size(), 1);
            const auto item = pdf.pageIssues().first().toMap(); QCOMPARE(item["blockId"].toString(), id); QVERIFY(item["reason"].toString().contains(QStringLiteral("原文已变动")));
            const auto rows = pdf.issuePages(); QCOMPARE(rows.size(), 2); QCOMPARE(rows.first().toMap()["severity"].toString(), QStringLiteral("review"));
        }
        const QDir tasks(dir.filePath("pdf-tasks")); const auto path = tasks.filePath(tasks.entryList({"*.sqlite"}).first());
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "legacy-issues"); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery query(db); QVERIFY(query.exec("SELECT number, summary FROM pages")); QList<QPair<int, QJsonObject>> rows;
            while (query.next()) { auto summary = QJsonDocument::fromJson(query.value(1).toByteArray()).object(); summary.remove("issueVersion"); summary.remove("issues"); summary.remove("review"); rows.append({query.value(0).toInt(), summary}); }
            query.finish();
            for (const auto &row : rows) { query.prepare("UPDATE pages SET summary=? WHERE number=?"); query.addBindValue(QJsonDocument(row.second).toJson(QJsonDocument::Compact)); query.addBindValue(row.first); QVERIFY(query.exec()); }
            db.close();
        }
        QSqlDatabase::removeDatabase("legacy-issues");
        PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded() && !pdf.issueIndexing(), 20000);
        QCOMPARE(pdf.reviewPages(), 1); QVERIFY(pdf.pageStatus().contains(QStringLiteral("待校对")));
        const auto id = pdf.blocks().first().toMap()["id"].toString();
        QVERIFY(pdf.reviseBlock(id, {{"source", QStringLiteral("已确认标题")}, {"translation", QStringLiteral("已校对译文")}, {"confirmTranslation", true}}));
        QTRY_VERIFY_WITH_TIMEOUT(!pdf.rendering(), 20000); QCOMPARE(pdf.reviewPages(), 0); QVERIFY(pdf.pageIssues().isEmpty());
        for (const auto &marker : {"[unclear]", "[UNREADABLE]", "［不清楚］", "[不清晰]"}) {
            const QJsonObject b{{"id", "x"}, {"source", QString::fromUtf8(marker)}, {"translation", "ok"}}; QCOMPARE(PdfTaskStore::blockIssues(b).size(), 1);
        }
        QJsonObject b{{"id", "x"}, {"backgroundReview", true}, {"layoutError", "Does not fit"}};
        QCOMPARE(PdfTaskStore::blockIssues(b).size(), 2); b["backgroundConfirmed"] = true; QCOMPARE(PdfTaskStore::blockIssues(b).size(), 1);
    }
};
QTEST_MAIN(PdfWorkflowTests)
#include "tst_pdf_workflow.moc"
