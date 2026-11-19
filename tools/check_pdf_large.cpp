#include "appcontroller.h"
#include "pdfcontroller.h"
#include "pdftaskstore.h"
#include <QGuiApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>

// Offline scalability exercise. The source is a generated fixture and all
// translations/scan regions are synthetic; no provider or personal profile.
int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    const auto args = application.arguments(); if (args.size() != 3) return 2;
    const auto source = QFileInfo(args[1]).absoluteFilePath(); const QDir output(QFileInfo(args[2]).absoluteFilePath());
    QDir().mkpath(output.path());
    const auto profile = output.filePath("profile");
    if (QFileInfo::exists(profile)) { QTextStream(stderr) << "Use a fresh output directory\n"; return 2; }
    AppController app(profile); auto pdf = std::make_unique<PdfController>(&app);
    QElapsedTimer elapsed, phaseClock, heartbeat; elapsed.start(); phaseClock.start(); heartbeat.start();
    QJsonObject report{{"source", source}, {"sourceBytes", QFileInfo(source).size()}};
    int phase = 0, seedPage = 0, pageCount = 0, expectedTranslations = 0;
    qint64 chars = 0, longestGap = 0, lastReport = -1000;
    bool finished = false, canceledExport = false;
    PdfTaskStore store; QString database;
    const auto write = [&](const QString &name, QJsonObject data) {
        data["elapsedMs"] = elapsed.elapsed(); data["phase"] = phase; data["maxEventLoopGapMs"] = longestGap;
        QSaveFile file(output.filePath(name)); const auto bytes = QJsonDocument(data).toJson();
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    };
    const auto finish = [&](const QString &error) {
        if (finished) return; finished = true;
        report["error"] = error; report["success"] = error.isEmpty();
        if (pdf) { report["completed"] = pdf->completed(); report["analyzed"] = pdf->analyzedPages(); }
        pdf.reset(); store.close(); write("report.json", report);
        QTextStream(stdout) << (error.isEmpty() ? "DONE" : "FAILED") << " " << elapsed.elapsed() << " ms " << error << Qt::endl;
        application.exit(error.isEmpty() ? 0 : 1);
    };
    const auto connectController = [&] {
        QObject::connect(pdf.get(), &PdfController::changed, &application, [&] {
            if (phase == 0 && pdf->loaded()) {
                report["firstUsableMs"] = elapsed.elapsed(); report["pagesAtFirstUsable"] = pdf->analyzedPages();
                pageCount = pdf->pageCount(); report["pageCount"] = pageCount; phase = 1; pdf->pauseAnalysis();
            }
            if (phase == 7 && !canceledExport && pdf->operationStatus().startsWith(QStringLiteral("正在导出")) && pdf->operationProgress() > 0) {
                canceledExport = true; pdf->cancel();
            }
        });
    };
    connectController();
    QTimer monitor; monitor.setInterval(25);
    QObject::connect(&monitor, &QTimer::timeout, &application, [&] {
        longestGap = qMax(longestGap, heartbeat.restart());
        if (finished) return;
        if (QFileInfo::exists(output.filePath("STOP"))) { if (pdf) pdf->cancel(); finish("Validation stopped by control file"); return; }
        if (elapsed.elapsed() > 1800000) { finish("Validation exceeded 30 minutes"); return; }
        if (elapsed.elapsed() - lastReport >= 1000) {
            auto progress = report;
            if (pdf) { progress["operation"] = pdf->operationStatus(); progress["analyzed"] = pdf->analyzedPages(); progress["completed"] = pdf->completed(); }
            write("progress.json", progress); lastReport = elapsed.elapsed();
        }
        if (pdf && !pdf->error().isEmpty() && phase != 7) { finish(pdf->error()); return; }
        if (phase == 7 && !pdf->processing() && !canceledExport && !pdf->error().isEmpty()) { finish(pdf->error()); return; }
        if (phase == 1 && !pdf->rendering()) {
            report["firstPreviewMs"] = elapsed.elapsed(); phase = 2; phaseClock.restart(); pdf->setPage(pageCount - 1); return;
        }
        if (phase == 2 && pdf->pageReady() && !pdf->rendering()) {
            report["lastPageJumpMs"] = phaseClock.elapsed(); report["pagesAfterJump"] = pdf->analyzedPages();
            phase = 3; pdf->setPage(0); pdf->resumeAnalysis(); return;
        }
        if (phase == 3 && pdf->analysisComplete() && !pdf->rendering()) {
            report["analysisMs"] = elapsed.elapsed(); report["nativeBlocks"] = pdf->total(); report["scanPages"] = pdf->scanPages();
            pdf.reset();
            const QDir tasks(QDir(profile).filePath("pdf-tasks")); const auto names = tasks.entryList({"*.sqlite"}, QDir::Files);
            if (names.size() != 1) { finish("Unexpected task database count"); return; }
            database = tasks.filePath(names.first()); if (!store.open(database)) { finish(store.error()); return; }
            phase = 4; return;
        }
        if (phase == 4) {
            const int last = qMin(pageCount, seedPage + 8);
            if (!store.begin()) { finish(store.error()); return; }
            while (seedPage < last) {
                auto page = store.page(seedPage); if (page.isEmpty()) { store.rollback(); finish("Missing analyzed page"); return; }
                auto blocks = page["blocks"].toArray();
                if (page["kind"] == "scan") {
                    const QJsonArray rect{40, 35, 515, 65};
                    blocks = QJsonArray{QJsonObject{{"id", QStringLiteral("scan-%1").arg(seedPage)}, {"source", "Synthetic scan heading"},
                        {"translation", QStringLiteral("扫描译文 %1").arg(seedPage + 1)}, {"rect", rect}, {"region", rect}, {"masks", QJsonArray{rect}},
                        {"size", 18}, {"color", QJsonArray{0, 0, 0}}, {"backgroundMode", "panel"}, {"panelColor", "#ffffff"}}};
                    page["scan"] = QJsonObject{{"blocks", blocks}, {"source", "Synthetic scan heading"}, {"translation", QStringLiteral("扫描译文 %1").arg(seedPage + 1)}};
                    ++expectedTranslations;
                } else {
                    for (int i = 0; i < blocks.size(); ++i) {
                        auto block = blocks[i].toObject(); chars += block["source"].toString().size();
                        if (block["fixed"].toBool() || block["keep"].toBool()) continue;
                        block["translation"] = QStringLiteral("中文译文 %1 段落 %2").arg(seedPage + 1).arg(i + 1); blocks[i] = block; ++expectedTranslations;
                    }
                    page["blocks"] = blocks;
                }
                if (!store.putPage(seedPage++, page)) { store.rollback(); finish(store.error()); return; }
            }
            if (!store.commit()) { finish(store.error()); return; }
            if (seedPage < pageCount) return;
            store.close(); report["extractedChars"] = chars; report["expectedTranslations"] = expectedTranslations;
            report["databaseBytes"] = QFileInfo(database).size(); report["database"] = database;
            phase = 5; phaseClock.restart(); pdf = std::make_unique<PdfController>(&app); connectController();
            if (!pdf->loadFile(QUrl::fromLocalFile(source))) finish(pdf->error()); return;
        }
        if (phase == 5 && pdf->analysisComplete() && !pdf->rendering()) {
            report["reopenMs"] = phaseClock.elapsed();
            if (pdf->completed() != expectedTranslations) { finish("Cached translations did not restore"); return; }
            phase = 6; pdf->setPage(pageCount - 1); return;
        }
        if (phase == 6 && !pdf->rendering()) {
            phase = 7;
            if (!pdf->exportFile(QUrl::fromLocalFile(output.filePath("cancelled.pdf")))) finish(pdf->error()); return;
        }
        if (phase == 7 && canceledExport && !pdf->processing()) {
            if (QFileInfo::exists(output.filePath("cancelled.pdf"))) { finish("Canceled export published a file"); return; }
            report["exportCancelPreservedProgress"] = pdf->completed() == expectedTranslations;
            phase = 8; phaseClock.restart();
            if (!pdf->exportFile(QUrl::fromLocalFile(output.filePath("translated.pdf")))) finish(pdf->error()); return;
        }
        if (phase == 8 && !pdf->processing()) {
            report["exportMs"] = phaseClock.elapsed(); report["outputBytes"] = QFileInfo(output.filePath("translated.pdf")).size();
            finish(QFileInfo::exists(output.filePath("translated.pdf")) ? QString() : QStringLiteral("Export missing"));
        }
    });
    if (!pdf->loadFile(QUrl::fromLocalFile(source))) return 2;
    monitor.start(); return application.exec();
}
