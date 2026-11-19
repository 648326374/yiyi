#include "appcontroller.h"
#include "pdfcontroller.h"
#include "secretstore.h"
#include <QGuiApplication>
#include <QDir>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QSettings>
#include <QTextStream>
#include <QNetworkProxyFactory>

// Explicitly invoked only. Uses a fresh profile and the supplied model endpoint.
int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    const auto args = application.arguments();
    if (args.size() == 3 && args[1] == "--list-providers") {
        QSettings settings(QDir(args[2]).filePath("settings.ini"), QSettings::IniFormat); QJsonArray names;
        for (const auto &v : QJsonDocument::fromJson(settings.value("providers").toByteArray()).array()) {
            const auto p = v.toObject(); names.append(QJsonObject{{"name", p["name"]}, {"protocol", p["protocol"]}, {"model", p["model"]}});
        }
        QTextStream(stdout) << QJsonDocument(names).toJson(); return 0;
    }
    QString source, outputPath, model; QVariantMap provider;
    if (args.size() == 7 && args[1] == "--configured-provider") {
        // Explicit opt-in: read only the named configuration; never change it
        // and never print its endpoint, encrypted secret or plaintext API key.
        QSettings settings(QDir(args[2]).filePath("settings.ini"), QSettings::IniFormat);
        for (const auto &v : QJsonDocument::fromJson(settings.value("providers").toByteArray()).array()) {
            const auto p = v.toObject(); if (p["name"].toString() != args[3]) continue;
            if (!provider.isEmpty()) return 2;
            QString error; const auto key = SecretStore::reveal(p["secret"].toString(), &error); if (!error.isEmpty()) return 2;
            provider = {{"name", "Configured scan check"}, {"protocol", p["protocol"].toString()}, {"baseUrl", p["baseUrl"].toString()},
                {"model", args[4]}, {"reasoningEffort", p["reasoningEffort"].toString()}, {"apiKey", key}};
        }
        if (provider.isEmpty()) return 2;
        source = args[5]; outputPath = args[6]; model = args[4]; QNetworkProxyFactory::setUseSystemConfiguration(true);
    } else if (args.size() == 5) {
        const QUrl endpoint(args[2]);
        if (endpoint.host() != "127.0.0.1" && endpoint.host() != "localhost" && endpoint.host() != "::1") return 2;
        source = args[1]; outputPath = args[4]; model = args[3];
        provider = {{"name", "Local scan check"}, {"protocol", "openai"}, {"baseUrl", args[2]}, {"model", model}};
    } else return 2;
    QTemporaryDir profile; if (!profile.isValid()) return 2;
    AppController app(profile.path()); PdfController pdf(&app);
    if (!app.saveProvider(provider).isEmpty()) return 2;
    provider.clear();
    pdf.setVisionProviderId(app.selectedProviderId());
    QDir output(outputPath); QDir().mkpath(output.path());
    int phase = 0; QElapsedTimer clock; QJsonObject result;
    auto finish = [&](const QString &error) {
        result["error"] = error; result["model"] = model;
        QSaveFile file(output.filePath("result.json"));
        if (file.open(QIODevice::WriteOnly)) { file.write(QJsonDocument(result).toJson()); file.commit(); }
        application.exit(error.isEmpty() ? 0 : 1);
    };
    QObject::connect(&pdf, &PdfController::changed, &application, [&] {
        if (!pdf.error().isEmpty()) { phase = 9; finish(pdf.error()); return; }
        if (phase == 0 && pdf.loaded()) {
            result["scan"] = pdf.scanMode(); clock.start();
            if (pdf.scanMode()) { phase = 1; QTimer::singleShot(0, &pdf, [&] { pdf.recognizePages(1, 1, 1800, true); }); }
            else { phase = 2; QTimer::singleShot(0, &pdf, [&] { pdf.start(true); }); }
        } else if (phase == 1 && !pdf.busy() && !pdf.scanSource().isEmpty()) {
            result["recognitionMs"] = clock.elapsed(); result["source"] = pdf.scanSource(); phase = 2;
            QTimer::singleShot(0, &pdf, [&] { clock.restart(); pdf.start(true); });
        } else if (phase == 2 && !pdf.busy() && pdf.total() > 0 && pdf.completed() == pdf.total()) {
            result["translationMs"] = clock.elapsed(); result["translation"] = pdf.scanTranslation(); result["blocks"] = QJsonArray::fromVariantList(pdf.blocks());
            result["layout"] = QJsonObject{{"regions", QJsonArray::fromVariantList(pdf.layoutRegions())}};
            if (!pdf.scanMode()) {
                QStringList sources, translations;
                for (const auto &v : pdf.blocks()) { sources.append(v.toMap()["source"].toString()); translations.append(v.toMap()["translation"].toString()); }
                result["source"] = sources.join("\n\n"); result["translation"] = translations.join("\n\n");
            }
            phase = 3;
            QTimer::singleShot(0, &pdf, [&] {
                phase = 4;
                if (!pdf.exportFile(QUrl::fromLocalFile(output.absoluteFilePath("comparison.pdf")))) finish("Export did not start");
            });
        } else if (phase == 4 && !pdf.processing()) {
            phase = 9; finish({});
        }
    });
    QTimer::singleShot(600000, &application, [&] { pdf.cancel(); finish("Timed out"); });
    if (!pdf.loadFile(QUrl::fromLocalFile(QFileInfo(source).absoluteFilePath()))) return 2;
    return application.exec();
}
