#include "appcontroller.h"
#include "pdfcontroller.h"
#include "secretstore.h"
#include <QGuiApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QLockFile>
#include <QNetworkProxyFactory>
#include <QElapsedTimer>
#include <QTextStream>
#include <QDateTime>
#include <memory>

// Explicit whole-book exercise of the production controller. SQLite checkpoints
// survive restarts in an isolated profile. Credentials use the application's
// DPAPI store; progress and logs never contain credentials or endpoints.
int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName("Yiyi");
    QCoreApplication::setApplicationName("YiyiBookCheck");
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOptions({
        {"config-dir", "Read the configured provider from this application profile.", "path"},
        {"provider", "Provider name; defaults to the selected provider.", "name"},
        {"source", "PDF to translate in full.", "path"},
        {"output", "Durable isolated profile, progress and output directory.", "path"},
        {"model", "Explicit model override for this isolated run.", "model"},
        {"reasoning", "Explicit reasoning override for this isolated run.", "effort"},
        {"first-page", "First page of an explicitly assigned range (one based).", "page"},
        {"last-page", "Last page of an explicitly assigned range (one based).", "page"},
        {"no-export", "Retain the completed range in SQLite for a later full-book merge."},
        {"rescan-range", "Revisit the range from its first page, retaining completed translations."},
        {"run", "Submit real requests using the configured credentials."},
        {"reset-empty-task", "Reset task settings only if no translations are completed."}
    });
    parser.process(application);
    if (!parser.isSet("config-dir")) parser.showHelp(2);
    QSettings settings(QDir(parser.value("config-dir")).filePath("settings.ini"), QSettings::IniFormat);
    QJsonObject configured;
    for (const auto &v : QJsonDocument::fromJson(settings.value("providers").toByteArray()).array()) {
        const auto p = v.toObject();
        const bool match = parser.isSet("provider") ? p["name"].toString() == parser.value("provider")
            : p["id"].toString() == settings.value("selectedProvider").toString();
        if (!match) continue;
        if (!configured.isEmpty()) { QTextStream(stderr) << "Ambiguous provider\n"; return 2; }
        configured = p;
    }
    if (configured.isEmpty()) { QTextStream(stderr) << "Configured provider not found\n"; return 2; }
    QJsonObject description;
    for (const auto *key : {"name", "protocol", "model", "reasoningEffort"}) description[key] = configured[key];
    QTextStream(stdout) << QJsonDocument(description).toJson(QJsonDocument::Compact) << Qt::endl;
    if (!parser.isSet("run")) return 0;
    if (!parser.isSet("source") || !parser.isSet("output")) return 2;
    const auto source = QFileInfo(parser.value("source")).absoluteFilePath();
    if (!QFileInfo::exists(source)) return 2;
    QDir output(QFileInfo(parser.value("output")).absoluteFilePath());
    if (!QDir().mkpath(output.path())) return 2;
    QLockFile lock(output.filePath("run.lock")); lock.setStaleLockTime(0);
    if (!lock.tryLock()) { QTextStream(stderr) << "A book run already holds this output directory\n"; return 2; }
    if (QFileInfo::exists(output.filePath("STOP"))) { QTextStream(stderr) << "STOP file is present\n"; return 2; }
    QString credentialError;
    const auto key = SecretStore::reveal(configured["secret"].toString(), &credentialError);
    if (!credentialError.isEmpty()) { QTextStream(stderr) << "Cannot unlock configured credential\n"; return 2; }
    QVariantMap provider{{"id", configured["id"].toString()}, {"name", configured["name"].toString()},
        {"protocol", configured["protocol"].toString()}, {"baseUrl", configured["baseUrl"].toString()},
        {"model", parser.isSet("model") ? parser.value("model") : configured["model"].toString()},
        {"reasoningEffort", parser.isSet("reasoning") ? parser.value("reasoning") : configured["reasoningEffort"].toString()}, {"apiKey", key}};
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    AppController app(output.filePath("profile"));
    if (!app.saveProvider(provider).isEmpty()) { QTextStream(stderr) << "Cannot prepare configured provider\n"; return 2; }
    const auto model = provider["model"].toString(), effort = provider["reasoningEffort"].toString();
    provider.clear(); configured = {}; app.setTargetLanguage(QStringLiteral("简体中文"));
    const bool rangeMode = parser.isSet("first-page") || parser.isSet("last-page");
    const int rangeFirst = parser.value("first-page").toInt() - 1, rangeLast = parser.value("last-page").toInt() - 1;
    QString taskPath;
    if (rangeMode) {
        const QDir tasks(QDir(app.dataDirectory()).filePath("pdf-tasks"));
        const auto names = tasks.entryList({"*.sqlite"}, QDir::Files);
        if (rangeFirst < 0 || rangeLast < rangeFirst || names.size() != 1 || !parser.isSet("no-export") || parser.isSet("reset-empty-task")) return 2;
        taskPath = tasks.filePath(names.first()); PdfTaskStore store(0);
        if (!store.open(taskPath)) return 2;
        auto metadata = store.metadata();
        if (rangeLast >= metadata["pageCount"].toInt() || QFileInfo(metadata["stamp"].toObject()["path"].toString()).absoluteFilePath() != source) return 2;
        auto workflow = metadata["workflow"].toObject();
        if (workflow["kind"] == "retranslate") return 2;
        const bool sameRange = workflow["kind"] == "translate" && workflow["first"].toInt() == rangeFirst && workflow["last"].toInt() == rangeLast;
        if (!sameRange) workflow = {{"kind", "translate"}, {"first", rangeFirst}, {"last", rangeLast}, {"cursor", rangeFirst}, {"pixels", 2400}};
        if (parser.isSet("rescan-range")) workflow["cursor"] = rangeFirst;
        workflow["state"] = "paused"; workflow.remove("error"); metadata["workflow"] = workflow;
        if (!store.setMetadata(metadata)) return 2;
    }
    auto pdf = std::make_unique<PdfController>(&app); pdf->setVisionProviderId(app.selectedProviderId());
    const auto destination = output.filePath(QStringLiteral("DirectX12-简体中文.pdf"));
    QElapsedTimer elapsed; elapsed.start();
    QString phase = "loading"; bool finished = false, reset = parser.isSet("reset-empty-task");
    int lastCompleted = -1, lastTranslatedPages = -1;
    QJsonObject report{{"source", source}, {"destination", destination}, {"pid", QCoreApplication::applicationPid()},
        {"startedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"model", model}, {"reasoningEffort", effort}, {"realAi", true}};
    report["partialRange"] = rangeMode;
    if (rangeMode) { report["firstPage"] = rangeFirst + 1; report["lastPage"] = rangeLast + 1; }
    auto rangeSummary = [&] {
        QJsonObject totals{{"total", 0}, {"completed", 0}, {"pending", 0}, {"pages", 0}};
        if (!rangeMode) return totals;
        PdfTaskStore reader(0);
        if (!reader.open(taskPath, true)) { totals["error"] = reader.error(); return totals; }
        for (const auto &v : reader.summaries()) {
            const auto s = v.toObject(); const int page = s["number"].toInt();
            if (page < rangeFirst || page > rangeLast) continue;
            totals["pages"] = totals["pages"].toInt() + 1;
            totals["total"] = totals["total"].toInt() + s["total"].toInt();
            totals["completed"] = totals["completed"].toInt() + s["completed"].toInt();
            if (!s["analyzed"].toBool() || (s["scan"].toBool() && !s["hasBlocks"].toBool()) || s["total"].toInt() != s["completed"].toInt()
                || !s["analysisError"].toString().isEmpty() || (!s["scan"].toBool() && !s["warning"].toString().isEmpty())) totals["pending"] = totals["pending"].toInt() + 1;
        }
        if (!reader.error().isEmpty()) totals["error"] = reader.error();
        return totals;
    };
    auto checkpoint = [&] {
        auto progress = report;
        progress["updatedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        progress["phase"] = phase; progress["elapsedMs"] = elapsed.elapsed(); progress["finished"] = finished;
        if (rangeMode) progress["range"] = rangeSummary();
        if (pdf) {
            progress["pageCount"] = pdf->pageCount(); progress["analyzedPages"] = pdf->analyzedPages();
            progress["completedBlocks"] = pdf->completed(); progress["totalBlocks"] = pdf->total();
            progress["translatedPages"] = pdf->translatedPages(); progress["pendingPages"] = pdf->pendingPages();
            progress["reviewPages"] = pdf->reviewPages(); progress["activity"] = pdf->activityStatus(); progress["error"] = pdf->error();
        }
        QSaveFile file(output.filePath("progress.json")); const auto bytes = QJsonDocument(progress).toJson();
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    };
    auto finish = [&](const QString &error) {
        if (finished) return;
        finished = true; phase = error.isEmpty() ? "complete" : "stopped";
        report["success"] = error.isEmpty(); report["stopReason"] = error;
        checkpoint();
        QTextStream(stdout) << (error.isEmpty() ? "DONE " : "STOPPED ") << error << Qt::endl;
        pdf.reset(); application.exit(error.isEmpty() ? 0 : 1);
    };
    QTimer monitor; monitor.setInterval(1000);
    QObject::connect(&monitor, &QTimer::timeout, &application, [&] {
        if (finished) return;
        if (!checkpoint()) { pdf->cancel(); finish("Cannot persist progress"); return; }
        if (QFileInfo::exists(output.filePath("STOP"))) { pdf->cancel(); finish("Paused by control file; completed translations retained"); return; }
        if (pdf->translatedPages() != lastTranslatedPages || pdf->completed() / 25 != lastCompleted / 25) {
            lastCompleted = pdf->completed(); lastTranslatedPages = pdf->translatedPages();
            QTextStream(stdout) << "PROGRESS " << lastCompleted << "/" << pdf->total() << " blocks; "
                << lastTranslatedPages << "/" << pdf->pageCount() << " translated pages" << Qt::endl;
        }
        if (pdf->busy() || pdf->processing() || pdf->rendering()) return;
        if (phase == "loading" && pdf->loaded()) {
            if (rangeMode) pdf->pauseAnalysis();
            if (reset) {
                if (pdf->completed() != 0) { finish("Refusing to reset a task that already has translations"); return; }
                reset = false; phase = "resetting"; pdf->resetTranslations(); return;
            }
            // A saved failure is retried only by this explicit tool invocation.
            phase = "translating"; pdf->translate(false, 2400); return;
        }
        if (phase == "resetting") { phase = "translating"; pdf->translate(false, 2400); return; }
        if (!pdf->error().isEmpty()) { finish(pdf->error()); return; }
        if (phase == "translating") {
            const auto scope = rangeSummary();
            const bool incomplete = rangeMode ? scope.contains("error") || scope["pages"].toInt() != rangeLast - rangeFirst + 1
                    || scope["pending"].toInt() != 0 || scope["total"] != scope["completed"]
                : !pdf->analysisComplete() || pdf->completed() != pdf->total() || pdf->pendingPages() != 0;
            if (pdf->taskPending() || incomplete) {
                finish("Translation stopped with unfinished pages or blocks"); return;
            }
            if (parser.isSet("no-export")) { finish({}); return; }
            phase = "exporting";
            if (!pdf->exportFile(QUrl::fromLocalFile(destination))) finish("Export did not start");
            return;
        }
        if (phase == "exporting") {
            if (!QFileInfo::exists(destination) || QFileInfo(destination).size() == 0) { finish("Export did not create a PDF"); return; }
            finish({});
        }
    });
    if (!pdf->loadFile(QUrl::fromLocalFile(source))) { finish("Cannot open PDF"); return 2; }
    checkpoint(); monitor.start();
    return application.exec();
}
