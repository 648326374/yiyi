#include "pdfengine.h"
#include <QGuiApplication>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QDir>
#include <cstdio>

int main(int argc, char **argv) {
    // No windows, hooks or personal settings are created in the worker.
    qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_WIN
    // The offscreen plugin does not discover Windows fonts automatically.
    // Math/Latin fallback must work in the portable app, not only in tests.
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", QDir(qEnvironmentVariable("WINDIR", "C:/Windows")).filePath("Fonts").toUtf8());
#endif
    QGuiApplication app(argc, argv);
    if (app.arguments().size() != 3) return 2;
    QFile input(app.arguments()[1]);
    if (!input.open(QIODevice::ReadOnly) || input.size() > 32 * 1024 * 1024) return 3;
    const auto request = QJsonDocument::fromJson(input.readAll());
    if (!request.isObject()) return 3;
    QElapsedTimer progressTimer;
    const auto progress = [&](const QJsonObject &value) {
        if (!request.object()["progress"].toBool()) return;
        if (progressTimer.isValid() && progressTimer.elapsed() < 100 && value["completed"] != value["total"]) return;
        progressTimer.start();
        const auto bytes = QJsonDocument(QJsonObject{{"progress", value}}).toJson(QJsonDocument::Compact) + '\n';
        fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout); fflush(stdout);
    };
    const auto result = PdfEngine::execute(request.object(), progress);
    QSaveFile file(app.arguments()[2]);
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return 4;
    return 0;
}
