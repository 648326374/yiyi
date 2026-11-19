#include "appcontroller.h"
#include "desktopintegration.h"
#include "documentcontroller.h"
#include "fileimportcontroller.h"
#include "officecontroller.h"
#include "pdfcontroller.h"
#include "screenshotcontroller.h"
#include "visioncontroller.h"
#include "singleinstance.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QMessageBox>
#include <QNetworkProxyFactory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <cstring>

int main(int argc, char *argv[])
{
    if (argc > 1 && std::strcmp(argv[1], "--selection-probe") == 0)
        return SelectionCapture::runProbe(argc, argv);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Yiyi"));
    QCoreApplication::setApplicationName(QStringLiteral("Yiyi"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.11.3"));
    app.setApplicationDisplayName(QStringLiteral("译意"));
    app.setWindowIcon(QIcon(QStringLiteral(":/assets/logo.svg")));
    app.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QCommandLineParser parser;
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({
        {QStringLiteral("data-dir"), "Override local data directory for isolated testing.", "path"},
        {QStringLiteral("smoke-test"), "Open the interface and exit automatically."},
        {QStringLiteral("screenshot"), "Save a screenshot before exiting.", "path"},
        {QStringLiteral("page"), "Initial page: translate, history, favorites, settings, editor, documents, pdf.", "name", "translate"},
        {QStringLiteral("pdf"), "Open a local PDF without submitting a translation.", "path"},
        {QStringLiteral("document"), "Open a local TXT, DOCX or PPTX without submitting a translation.", "path"},
        {QStringLiteral("theme"), "Override theme for this run: light, dark, system.", "name"},
        {QStringLiteral("no-desktop"), "Disable tray, global shortcuts and selection monitoring for isolated testing."}
    });
    parser.process(app);
    const bool desktopEnabled = !parser.isSet(QStringLiteral("no-desktop")) && !parser.isSet(QStringLiteral("smoke-test")) && !parser.isSet(QStringLiteral("screenshot"));
    const auto dataDirectory = parser.isSet(QStringLiteral("data-dir")) ? QDir(parser.value(QStringLiteral("data-dir"))).absolutePath()
        : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    SingleInstance instance;
    if (desktopEnabled) {
        const auto result = instance.acquire(dataDirectory);
        if (result == SingleInstance::ActivatedExisting) return 0;
        if (result == SingleInstance::Failed) { QMessageBox::warning(nullptr, QStringLiteral("译意"), instance.error()); return 1; }
    }
    AppController controller(dataDirectory);
    DesktopIntegration desktop(controller.dataDirectory(), desktopEnabled);
    DocumentController documents(&controller);
    PdfController pdf(&controller);
    OfficeController office(&controller);
    FileImportController fileImport(&documents, &pdf, &office);
    ScreenshotController capture;
    VisionController vision(&controller);
    QObject::connect(&office, &OfficeController::imageReviewRequested, &capture, [&capture, &vision](const QImage &image) { if (!vision.busy()) capture.reviewImage(image); });
    QObject::connect(&capture, &ScreenshotController::imageChanged, &vision, [&] { vision.setImage(capture.image()); });
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("Desktop"), &desktop);
    engine.rootContext()->setContextProperty(QStringLiteral("Documents"), &documents);
    engine.rootContext()->setContextProperty(QStringLiteral("Pdf"), &pdf);
    engine.rootContext()->setContextProperty(QStringLiteral("FileImport"), &fileImport);
    engine.rootContext()->setContextProperty(QStringLiteral("Office"), &office);
    engine.rootContext()->setContextProperty(QStringLiteral("Capture"), &capture);
    engine.rootContext()->setContextProperty(QStringLiteral("Vision"), &vision);
    engine.rootContext()->setContextProperty(QStringLiteral("initialPage"), parser.isSet(QStringLiteral("pdf")) ? QStringLiteral("pdf")
        : parser.isSet(QStringLiteral("document")) ? QStringLiteral("documents") : parser.value(QStringLiteral("page")));
    engine.rootContext()->setContextProperty(QStringLiteral("themeOverride"), parser.value(QStringLiteral("theme")));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 1;
    desktop.setWindow(qobject_cast<QQuickWindow *>(engine.rootObjects().first()));
    capture.setWindow(qobject_cast<QQuickWindow *>(engine.rootObjects().first()));
    QObject::connect(&capture, &ScreenshotController::restoreRequested, &desktop, &DesktopIntegration::showWindow);
    QObject::connect(&desktop, &DesktopIntegration::lookupRequested, &controller, &AppController::lookupSelection);
    QObject::connect(&instance, &SingleInstance::activated, &desktop, &DesktopIntegration::showWindow);
    desktop.start();
    if (parser.isSet(QStringLiteral("pdf"))) pdf.loadFile(QUrl::fromLocalFile(QFileInfo(parser.value(QStringLiteral("pdf"))).absoluteFilePath()));
    if (parser.isSet(QStringLiteral("document"))) fileImport.requestOpen(QUrl::fromLocalFile(QFileInfo(parser.value(QStringLiteral("document"))).absoluteFilePath()));
    if (QGuiApplication::platformName() != QLatin1String("offscreen")) {
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
            const auto area = window->screen()->availableGeometry();
            window->resize(qMin(window->width(), area.width() - 40), qMin(window->height(), area.height() - 48));
            window->setPosition(area.center() - QPoint(window->width() / 2, window->height() / 2));
        }
    }
    const auto screenshot = parser.value(QStringLiteral("screenshot"));
    QTimer smokeTimer;
    QElapsedTimer smokeElapsed;
    if (parser.isSet(QStringLiteral("smoke-test")) || !screenshot.isEmpty()) {
        smokeElapsed.start();
        smokeTimer.setInterval(100);
        QObject::connect(&smokeTimer, &QTimer::timeout, &app, [&] {
            if (smokeElapsed.elapsed() > 45000) { app.exit(4); return; }
            if (smokeElapsed.elapsed() < 1800) return;
            if (parser.isSet(QStringLiteral("document"))) {
                if (office.processing()) return;
                const auto kind = QFileInfo(parser.value(QStringLiteral("document"))).suffix().toLower();
                if (kind == "txt" ? documents.sourceText().isEmpty() : !office.loaded()) { app.exit(4); return; }
            }
            // A book can take longer than the initial UI animation to render.
            // Capture the restored document, not a loading indicator.
            if (parser.isSet(QStringLiteral("pdf"))) {
                if (pdf.processing() || pdf.rendering()) return;
                if (!pdf.loaded() || !pdf.error().isEmpty()) { app.exit(4); return; }
            }
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            if (!window) { app.exit(2); return; }
            if (parser.isSet(QStringLiteral("pdf"))) {
                const auto findVisual = [](auto &&self, QQuickItem *item, const QString &name) -> QObject * {
                    if (item->objectName() == name) return item;
                    for (auto *child : item->childItems()) if (auto *found = self(self, child, name)) return found;
                    return nullptr;
                };
                for (const auto *name : {"pdfOriginalPageImage", "pdfTranslatedPageImage"}) {
                    const auto *preview = findVisual(findVisual, window->contentItem(), QString::fromLatin1(name));
                    if (!preview) return;
                    const int status = preview->property("status").toInt();
                    if (status == 3) { app.exit(3); return; } // QML Image.Error
                    if (status != 1) return; // QML Image.Ready
                }
            }
            if (!screenshot.isEmpty()) {
                const QImage image = window->grabWindow();
                if (image.isNull() || !image.save(screenshot)) { app.exit(3); return; }
            }
            app.quit();
        });
        smokeTimer.start();
    }
    return app.exec();
}
