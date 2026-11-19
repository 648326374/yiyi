// Opt-in, offline package round trip. Synthetic translations, no model calls.
#include "officedocument.h"
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cstdio>

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv); const auto args = app.arguments();
    if (args.size() != 4) return 2;
    auto document = OfficeDocument::load(args[1]);
    if (!document.valid()) { fprintf(stderr, "%s\n", qPrintable(document.error)); return 3; }
    QHash<QString, QString> translations; QJsonArray locations;
    int index = 0;
    for (const auto &s : document.segments) {
        translations[s.id] = QStringLiteral("译文 %1：").arg(++index) + s.source;
        locations.append(QJsonObject{{"source", s.source}, {"location", s.location}, {"part", s.part}});
    }
    QString error;
    if (!document.save(args[2], translations, false, 12, &error)) { fprintf(stderr, "%s\n", qPrintable(error)); return 4; }
    const auto result = OfficeDocument::load(args[2]);
    if (!result.valid() || result.segments.size() != document.segments.size()) return 5;
    for (int i = 0; i < result.segments.size(); ++i) if (!result.segments[i].source.contains(QStringLiteral("译文 %1：").arg(i + 1))) return 6;
    const QJsonObject report{{"source", args[1]}, {"destination", args[2]}, {"kind", document.kind}, {"segments", locations},
        {"roundTrip", true}, {"pages", document.pages}, {"media", document.imageCount}, {"aiCalls", 0}, {"notices", QJsonArray::fromVariantList(document.notices)}};
    const auto json = QJsonDocument(report).toJson(); QSaveFile file(args[3]);
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit()) return 7;
    printf("%d segments, %d pages; round trip passed; AI calls: 0\n", int(document.segments.size()), document.pages);
    return 0;
}
