#include "appcontroller.h"
#include "visioncontroller.h"
#include "ocrservice.h"
#include <QGuiApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>

static int distance(QString a, QString b) {
    a.remove(QRegularExpression(QStringLiteral("\\s+"))); b.remove(QRegularExpression(QStringLiteral("\\s+")));
    QList<int> row(b.size()+1); for (int j=0;j<row.size();++j) row[j]=j;
    for (int i=1;i<=a.size();++i) { int previous=row[0]; row[0]=i; for (int j=1;j<=b.size();++j) { int old=row[j]; row[j]=qMin(row[j]+1,qMin(row[j-1]+1,previous+(a[i-1]!=b[j-1]))); previous=old; } }
    return row.last();
}
int main(int argc,char **argv) {
    QGuiApplication application(argc,argv); QCommandLineParser parser; parser.addHelpOption();
    parser.addOptions({{"output","Artifact directory","path"},{"model","Explicit local llama.cpp model ID; enables two real image requests","id"},{"skip-ocr","Only run local vision checks"}}); parser.process(application);
    const auto folder=QDir(parser.value("output")).absolutePath(); QDir().mkpath(folder); QJsonArray records;
    auto save=[&](const QJsonObject &record){ records.append(record); QFile file(QDir(folder).filePath("results.json")); if(file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(records).toJson()); QTextStream(stdout)<<QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact))<<Qt::endl; };
    struct Sample { const char *name; QString text; int pixels; bool dark; bool chinese; };
    const QList<Sample> samples{
        {"english-small",QStringLiteral("Please close the door.\nTranslation should preserve the meaning."),13,false,false},
        {"chinese-small",QStringLiteral("请先识别截图中的文字。\n校对后再翻译，避免遗漏内容。"),14,false,true},
        {"mixed-dark",QStringLiteral("本地 OCR / Local AI\nHello world 123\n点击按钮开始翻译"),13,true,true},
        {"english-tiny-dark",QStringLiteral("Settings > Translation\nModel: Qwen3.5-2B\nPlease select the target language."),11,true,false},
        {"chinese-dark",QStringLiteral("截图翻译支持中英文。\n识别结果可以手动修改。"),18,true,true},
        {"multiline",QStringLiteral("First line\nSecond line\nThird line"),22,false,false}
    };
    if (!parser.isSet("skip-ocr")) for (const auto &sample:samples) {
        QImage image(650,130,QImage::Format_RGB32); image.fill(sample.dark?QColor("#20262b"):Qt::white);
        QPainter painter(&image); painter.setPen(sample.dark?Qt::white:Qt::black); QFont font(sample.chinese?"Microsoft YaHei UI":"Segoe UI"); font.setPixelSize(sample.pixels); painter.setFont(font);
        painter.drawText(QRect(15,12,620,110),Qt::AlignLeft|Qt::AlignTop,sample.text); painter.end(); image.save(QDir(folder).filePath(QString::fromLatin1(sample.name)+".png"));
        for(int run=0;run<2;++run) {
            OcrService ocr; QEventLoop loop; QObject::connect(&ocr,&OcrService::finished,&loop,&QEventLoop::quit); QElapsedTimer clock; clock.start();
            ocr.recognize(image); if(ocr.busy()) loop.exec();
            auto normalized=sample.text; normalized.remove(QRegularExpression(QStringLiteral("\\s+")));
            save({{"sample",sample.name},{"engine","rapid"},{"run",run+1},{"elapsedMs",clock.elapsed()},{"expected",sample.text},{"text",ocr.text()},{"error",ocr.error()},{"editsIgnoringWhitespace",distance(sample.text,ocr.text())},{"referenceCharacters",normalized.size()}});
        }
    }
    if(parser.isSet("model")) {
        QTemporaryDir profile; AppController app(profile.path()); VisionController vision(&app);
        const auto error=app.saveProvider({{"name","Local image validation"},{"protocol","openai"},{"baseUrl","http://127.0.0.1:8080/v1"},{"model",parser.value("model")}});
        if(!error.isEmpty()) return 2;
        QEventLoop loop; QTimer timeout; timeout.setSingleShot(true); QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
        QObject::connect(&app,&AppController::testChanged,&loop,[&]{if(!app.testBusy())loop.quit();}); QElapsedTimer clock; clock.start(); timeout.start(180000);
        app.testVisionProvider(app.providerDraft(app.selectedProviderId())); if(app.testBusy())loop.exec(); timeout.stop();
        save({{"kind","vision-probe"},{"success",app.testSuccess()},{"message",app.testMessage()},{"elapsedMs",clock.elapsed()}}); app.resetTest();
        QImage image(500,120,QImage::Format_RGB32); image.fill(Qt::white); QPainter painter(&image); QFont font("Segoe UI"); font.setPixelSize(30); painter.setFont(font); painter.setPen(Qt::black); painter.drawText(image.rect(),Qt::AlignCenter,"Please close the door."); painter.end(); image.save(QDir(folder).filePath("vision-source.png"));
        vision.setImage(image); QObject::connect(&vision,&VisionController::changed,&loop,[&]{if(!vision.busy())loop.quit();}); clock.restart(); timeout.start(180000); vision.recognize(); if(vision.busy())loop.exec(); timeout.stop();
        const auto expected = QStringLiteral("Please close the door.");
        const bool exact = vision.complete() && vision.resultText().trimmed() == expected;
        save({{"kind","vision-recognition"},{"success",exact},{"expected",expected},{"text",vision.resultText()},{"error",vision.error()},{"elapsedMs",clock.elapsed()}}); if(!exact)return 1;
    }
    return 0;
}
