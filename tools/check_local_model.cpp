#include "appcontroller.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Check real dictionary requests with an isolated profile and an explicitly selected local model."));
    parser.addHelpOption();
    parser.addOptions({
        {QStringLiteral("url"), "Local Chat Completions base URL.", "url", "http://127.0.0.1:8080/v1"},
        {QStringLiteral("model"), "Model ID returned by /v1/models (required).", "id"},
        {QStringLiteral("target"), "Target language.", "language", QStringLiteral("简体中文")},
        {QStringLiteral("detailed"), "Request a full dictionary entry instead of brief meanings."},
        {QStringLiteral("translation"), "Use text translation instead of dictionary lookup."}
    });
    parser.addPositionalArgument(QStringLiteral("terms"), QStringLiteral("Words or phrases to check."), QStringLiteral("[terms... ]"));
    parser.process(application);
    QTextStream out(stdout), err(stderr);
    const auto host = QUrl(parser.value(QStringLiteral("url"))).host();
    if (parser.value(QStringLiteral("model")).isEmpty() || (host != QLatin1String("localhost") && host != QLatin1String("127.0.0.1") && host != QLatin1String("::1"))) {
        err << "Specify --model and a loopback URL. This tool does not call remote or paid services.\n";
        return 2;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    AppController controller(directory.path());
    const auto error = controller.saveProvider({{"name", "Local model check"}, {"protocol", "openai"},
        {"baseUrl", parser.value(QStringLiteral("url"))}, {"model", parser.value(QStringLiteral("model"))},
        {"dictionaryMode", parser.isSet(QStringLiteral("detailed")) ? "detailed" : "brief"}});
    if (!error.isEmpty()) { err << error << '\n'; return 2; }
    controller.setTargetLanguage(parser.value(QStringLiteral("target")));
    controller.setMode(parser.isSet(QStringLiteral("translation")) ? QStringLiteral("translation") : QStringLiteral("dictionary"));
    auto terms = parser.positionalArguments();
    if (terms.isEmpty()) terms = {QStringLiteral("look"), QStringLiteral("book"), QStringLiteral("light"), QStringLiteral("run"), QStringLiteral("look up"), QStringLiteral("serendipity"), QStringLiteral("qzwvblorp")};
    bool success = true;
    for (const auto &term : terms) {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&controller, &AppController::busyChanged, &loop, [&] { if (!controller.busy()) loop.quit(); });
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] { controller.cancel(); loop.quit(); });
        controller.setProperty("inputText", term);
        QElapsedTimer watch;
        watch.start();
        timer.start(30000);
        controller.translate();
        if (controller.busy()) loop.exec();
        timer.stop();
        const QVariantMap record{{"term", term}, {"elapsedMs", watch.elapsed()}, {"target", controller.targetLanguage()},
            {"resultKind", controller.resultKind()}, {"text", controller.resultText()},
            {"dictionary", controller.dictionary()}, {"error", controller.error()}};
        out << QString::fromUtf8(QJsonDocument::fromVariant(record).toJson(QJsonDocument::Compact)) << '\n';
        out.flush();
        success = success && controller.error().isEmpty() && !controller.resultText().isEmpty();
    }
    return success ? 0 : 1;
}
