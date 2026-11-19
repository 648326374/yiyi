#include "ocrpreprocess.h"
#include <QCoreApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QSaveFile>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3) return 2;
    QJsonObject result;
    const QImage original(args[1]);
    if (original.isNull() || qint64(original.width()) * original.height() > 64000000) return 3;
    const auto image = prepareOcrImage(original);
    const auto runtime = QDir(app.applicationDirPath()).filePath(QStringLiteral("ocr"));
#ifdef Q_OS_WIN
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    AddDllDirectory(reinterpret_cast<LPCWSTR>(runtime.utf16()));
#endif
    QLibrary library(QDir(runtime).filePath(QStringLiteral("YiyiOcr.dll")));
    using Recognize = int (*)(const unsigned char *, int, char *, int, float *);
    const auto recognize = reinterpret_cast<Recognize>(library.resolve("YiyiOcr"));
    if (!recognize || !QDir::setCurrent(runtime)) result["code"] = "rapid_missing";
    else {
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG");
        QByteArray output(1024 * 1024, '\0'); float confidence = 0;
        const int size = recognize(reinterpret_cast<const unsigned char *>(png.constData()), png.size(), output.data(), output.size(), &confidence);
        if (size < 0) result["code"] = "rapid_failed";
        else { result["text"] = QString::fromUtf8(output.constData(), size).trimmed(); result["confidence"] = double(confidence); }
    }
    QSaveFile file(args[2]);
    if (!file.open(QIODevice::WriteOnly)) return 4;
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) return 5;
    return result.contains("code") ? 1 : 0;
}
