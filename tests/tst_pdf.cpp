#include "pdfengine.h"
#include "pdfcontroller.h"
#include "appcontroller.h"
#include "pdffixture.h"
#include "streamserver.h"
#include "pdflayout.h"
#include "pdfbackground.h"
#include "pdfscanlayout.h"
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPainterPath>
#include <QTemporaryDir>

class PdfTests : public QObject {
    Q_OBJECT
    QJsonObject inspect(const QString &source) { return PdfEngine::execute({{"action", "inspect"}, {"source", source}}); }
    QJsonObject translated(QJsonObject doc) {
        auto pages = doc["pages"].toArray();
        for (int p = 0; p < pages.size(); ++p) {
            auto page = pages[p].toObject(); auto blocks = page["blocks"].toArray();
            for (int b = 0; b < blocks.size(); ++b) {
                auto block = blocks[b].toObject(); if (block["fixed"].toBool()) continue;
                block["translation"] = block["source"].toString().startsWith("Sample") ? QStringLiteral("示例文档 %1").arg(p + 1)
                    : block["source"].toString().startsWith("This") ? QStringLiteral("本文档保留图片、标题与水平分隔线。请将整个段落翻译成中文。")
                    : QStringLiteral("第二段在页面上有独立的位置。译文应保持在分隔线上方。");
                blocks[b] = block;
            }
            page["blocks"] = blocks; pages[p] = page;
        }
        doc["pages"] = pages; return doc;
    }
private slots:
    void exportPreservesPatternArtwork_data() {
        QTest::addColumn<bool>("nested"); QTest::newRow("page") << false; QTest::newRow("nested-form") << true;
    }
    void exportPreservesPatternArtwork() {
        QFETCH(bool, nested); QTemporaryDir dir; const auto source = dir.filePath("pattern.pdf"); makePatternPdfFixture(source, nested);
        auto doc = inspect(source); auto pages = doc["pages"].toArray(); QCOMPARE(pages.size(), 1);
        auto page = pages[0].toObject(); auto blocks = page["blocks"].toArray(); QCOMPARE(blocks.size(), 1);
        auto block = blocks[0].toObject(); block["translation"] = QStringLiteral("保留纹理图示"); blocks[0] = block;
        page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto output = dir.filePath("translated.pdf"), before = dir.filePath("before.png"), after = dir.filePath("after.png");
        const auto strict = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}});
        QVERIFY(strict["error"].toString().contains(QStringLiteral("纹理"))); QVERIFY(!QFile::exists(output));
        const auto saved = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}, {"preserveFailed", true}});
        QVERIFY2(saved["saved"].toBool(), qUtf8Printable(saved["error"].toString())); QCOMPARE(saved["preserved"].toArray().size(), 1);
        const auto result = inspect(output); QVERIFY(result["pages"].toArray()[0].toObject()["text"].toString().contains("Pattern illustration"));
        const auto preview = PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", 0}, {"pixels", 900}, {"original", before}, {"translated", dir.filePath("preview.png")}});
        QVERIFY(!preview.contains("error")); QVERIFY(!preview["statuses"].toArray()[0].toObject()["fits"].toBool());
        QVERIFY(!PdfEngine::execute({{"action", "render"}, {"source", output}, {"document", result}, {"page", 0}, {"pixels", 900}, {"original", after}, {"translated", dir.filePath("copy.png")}}).contains("error"));
        QCOMPARE(QImage(before), QImage(after)); QCOMPARE(QImage(before), QImage(dir.filePath("preview.png")));
        QFile report(output + ".issues.txt"); QVERIFY(report.open(QIODevice::ReadOnly)); QVERIFY(QString::fromUtf8(report.readAll()).contains(QStringLiteral("纹理填充")));
    }
    void exportSplitContentStreams() {
        QTemporaryDir dir; const auto source = dir.filePath("split.pdf"); makeSplitContentPdfFixture(source);
        auto doc = inspect(source); auto pages = doc["pages"].toArray(); QCOMPARE(pages.size(), 1);
        auto page = pages[0].toObject(); auto blocks = page["blocks"].toArray(); QCOMPARE(blocks.size(), 2);
        for (int i = 0; i < blocks.size(); ++i) { auto b = blocks[i].toObject(); b["translation"] = i ? QStringLiteral("第二段正文") : QStringLiteral("标题文字"); blocks[i] = b; }
        page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto output = dir.filePath("translated.pdf");
        const auto saved = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}});
        QVERIFY2(saved["saved"].toBool(), qUtf8Printable(saved["error"].toString()));
        const auto result = inspect(output); const auto translatedPage = result["pages"].toArray()[0].toObject();
        for (const auto &v : translatedPage["blocks"].toArray()) {
            const auto b = v.toObject(); QVERIFY2(b["rect"].toArray()[0].toDouble() < 40, qUtf8Printable(b["source"].toString()));
        }
        QVERIFY(translatedPage["text"].toString().contains(QStringLiteral("标题文字")));
        QVERIFY(translatedPage["text"].toString().contains(QStringLiteral("第二段正文")));
        const auto before = dir.filePath("before.png"), after = dir.filePath("after.png");
        QVERIFY(!PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", 0}, {"pixels", 900}, {"original", before}, {"translated", dir.filePath("preview.png")}}).contains("error"));
        QVERIFY(!PdfEngine::execute({{"action", "render"}, {"source", output}, {"document", result}, {"page", 0}, {"pixels", 900}, {"original", after}, {"translated", dir.filePath("copy.png")}}).contains("error"));
        // The path is below both text blocks. Its geometry and stroke must
        // survive serialization, independently of the in-memory preview.
        QCOMPARE(QImage(before).copy(0, 750, 900, 450), QImage(after).copy(0, 750, 900, 450));
    }
    void denseContentsAndIndexExport() {
        QTemporaryDir dir; const auto source = dir.filePath("contents.pdf"); makePdfFixture(source, 1);
        auto doc = inspect(source); auto pages = doc["pages"].toArray(); auto page = pages[0].toObject(); auto blocks = page["blocks"].toArray();
        auto toc = blocks[0].toObject(); toc["source"] = "Introduction........xxvi Next chapter........12";
        toc["translation"] = QStringLiteral("简介") + QString(180, '.') + "xxvi\n" + QStringLiteral("下一章") + QString(180, '.') + "12";
        toc["styledSource"] = ""; toc["size"] = 9; toc["region"] = QJsonArray{40, 40, 250, 22}; blocks[0] = toc;
        auto index = blocks[1].toObject(); index["source"] = "linkage, 166–169, 209–212";
        index["translation"] = QStringLiteral("链接，166–169，209–212"); index["styledSource"] = "";
        index["role"] = "index"; index["size"] = 8; index["region"] = QJsonArray{40, 110, 150, 8}; blocks[1] = index;
        page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto output = dir.filePath("translated.pdf");
        const auto result = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}});
        QVERIFY2(result["saved"].toBool(), qUtf8Printable(result["error"].toString()));
        const auto text = inspect(output)["pages"].toArray()[0].toObject()["text"].toString();
        for (const auto &part : {QStringLiteral("简介"), QStringLiteral("下一章"), QStringLiteral("链接"), QStringLiteral("xxvi"), QStringLiteral("166"), QStringLiteral("212")}) QVERIFY2(text.contains(part), qUtf8Printable(text));
        QVERIFY(!PdfLayout::changedNumbers("linkage, 166–169, 209–212", QStringLiteral("链接，166–170，209–212"), true).isEmpty());
    }
    void exportReportsAllPagesBeforeWriting() {
        QTemporaryDir dir; const auto source = dir.filePath("failures.pdf"); makePdfFixture(source, 2);
        AppController app(dir.path()); PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(pdf.analysisComplete(), 20000); QTRY_VERIFY_WITH_TIMEOUT(!pdf.rendering(), 20000);
        for (int page = 0; page < 2; ++page) {
            pdf.setPage(page); QVERIFY(pdf.editBlock(pdf.blocks()[0].toMap()["id"].toString(), QString(12000, QChar(0x6587))));
        }
        const auto output = dir.filePath("existing.pdf"); QFile file(output); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("existing output"); file.close();
        QVERIFY(pdf.exportFile(QUrl::fromLocalFile(output))); QTRY_VERIFY_WITH_TIMEOUT(!pdf.processing(), 20000);
        QVERIFY(pdf.error().contains(QStringLiteral("2 页、2 个区域"))); QCOMPARE(pdf.reviewPages(), 2);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("existing output"));
        QCOMPARE(pdf.completed(), 2);
        file.close();
        pdf.setPage(0); const auto kept = pdf.blocks()[0].toMap()["translation"].toString();
        QVERIFY(pdf.editBlock(pdf.blocks()[1].toMap()["id"].toString(), QStringLiteral("能够正常排版的译文")));
        QSignalSpy exported(&pdf, &PdfController::exported);
        QVERIFY(pdf.exportFile(QUrl::fromLocalFile(output), true)); QTRY_VERIFY_WITH_TIMEOUT(!pdf.processing(), 20000);
        QVERIFY2(pdf.error().isEmpty(), qUtf8Printable(pdf.error())); QCOMPARE(exported.size(), 1);
        QCOMPARE(exported.first()[1].toInt(), 2); QCOMPARE(exported.first()[2].toInt(), 2);
        QCOMPARE(pdf.completed(), 3); QCOMPARE(pdf.blocks()[0].toMap()["translation"].toString(), kept);
        const auto saved = inspect(output)["pages"].toArray(); QCOMPARE(saved.size(), 2);
        QVERIFY(saved[0].toObject()["text"].toString().contains(QStringLiteral("能够正常排版的译文")));
        QVERIFY(saved[0].toObject()["text"].toString().contains("Sample")); QVERIFY(saved[1].toObject()["text"].toString().contains("Sample"));
        QFile report(output + ".issues.txt"); QVERIFY(report.open(QIODevice::ReadOnly)); const auto details = QString::fromUtf8(report.readAll());
        QVERIFY(details.contains(QStringLiteral("第 1 页"))); QVERIFY(details.contains(QStringLiteral("第 2 页")));
    }
    void portableWorkerFindsFallbackFonts() {
        QTemporaryDir dir; const auto source = dir.filePath("symbols.pdf"); makePdfFixture(source, 1);
        auto doc = inspect(source); auto pages = doc["pages"].toArray(); auto page = pages[0].toObject(); auto blocks = page["blocks"].toArray();
        auto b = blocks[1].toObject(); b["translation"] = QStringLiteral("字符 č 与指数 2⁶⁴ 和 2ⁿ，以及下标 x₁\u200b。"); blocks[1] = b;
        page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const QJsonObject request{{"action", "checkLayout"}, {"source", source}, {"document", doc}, {"pages", QJsonArray{0}}};
        const auto input = dir.filePath("request.json"), output = dir.filePath("response.json");
        QFile file(input); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(request).toJson()); file.close();
        auto env = QProcessEnvironment::systemEnvironment(); env.remove("QT_QPA_FONTDIR");
        QProcess worker; worker.setProcessEnvironment(env); worker.start(QDir(QCoreApplication::applicationDirPath()).filePath("yiyi_pdf_worker.exe"), {input, output});
        QVERIFY(worker.waitForFinished(20000)); QCOMPARE(worker.exitCode(), 0);
        QFile result(output); QVERIFY(result.open(QIODevice::ReadOnly)); const auto json = QJsonDocument::fromJson(result.readAll()).object();
        QVERIFY2(!json.contains("error"), qUtf8Printable(json["error"].toString()));
        for (const auto &v : json["pages"].toArray()[0].toObject()["statuses"].toArray()) QVERIFY2(v.toObject()["fits"].toBool(), qUtf8Printable(v.toObject()["error"].toString()));
    }
    void translatedCodePreservesTokens() {
        const QString source = "const char* url = \"http://example.test\"; // Address\nvec3 v = vec3(1.0); /* Initialize vector */";
        QVERIFY(PdfLayout::changedCode(source, "const char* url = \"http://example.test\"; // 地址\nvec3 v = vec3(1.0); /* 初始化向量 */").isEmpty());
        QVERIFY(!PdfLayout::changedCode(source, "const char* url = \"http://example.test\"; // 地址\nvec3 v = vec3(2.0);").isEmpty());
        QVERIFY(!PdfLayout::changedCode(source, "const char* url = \"http://different.test\";\nvec3 v = vec3(1.0);").isEmpty());
        QVERIFY(!PdfLayout::changedCode("float x; // name", "floatx; // 名称").isEmpty());
        QVERIFY(!PdfLayout::changedCode("x + +y;", "x++y;").isEmpty());
        QVERIFY(!PdfLayout::changedCode("#define X 1\nfloat y;", "#define X 1 float y;").isEmpty());
        QVERIFY(!PdfLayout::changedCode("#define F (x)", "#define F(x)").isEmpty());
        QVERIFY(PdfLayout::changedCode("float x = 1.0; // value", "float  x=1.0; // 数值").isEmpty());
        QVERIFY(PdfLayout::changedNumbers("page 10", QStringLiteral("第10页")).isEmpty());
        QVERIFY(PdfLayout::changedNumbers("rendering to 89-92", QStringLiteral("渲染至89-92")).isEmpty());
        QVERIFY(!PdfLayout::changedNumbers("page 10", QStringLiteral("第11页")).isEmpty());
    }
    void codeExcerptUsesOneRequest() {
        QTemporaryDir dir; const auto source = dir.filePath("code.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(500, 400), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter p(&writer); QFont font("Courier New"); font.setPixelSize(10); p.setFont(font);
            const QStringList lines{"// This input block must match the corresponding", "vertex shader version.", "void main() {", "    vec3 v = vec3(1.0); // Initialize vector", "}",
                "mat3 M = mat3(vec3(1,0,0), // An inline comment in a matrix constructor", "    vec3(0,1,0),", "    vec3(0,0,1));",
                "// Structure declaration, to use as a sample", "struct Light", "{ vec3 position; };",
                "// Tell OpenGL how to locate the second attribute (texture", "coordinates) inside the buffer", "glEnableVertexAttribArray(1);",
                "/* Some explanation:", "The values use column vectors and matrix operations.", "*/", "// Now define the input interface block for this shader", "in VertexData", "{ vec3 position; };"};
            for (int i = 0; i < lines.size(); ++i) p.drawText(QPointF(40, 60 + i * 12), lines[i]);
        }
        AppController app(dir.path()); PdfController pdf(&app); StreamServer server("openai");
        QVERIFY(app.saveProvider({{"name", "code"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "test"}}).isEmpty());
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000); QCOMPARE(pdf.total(), 1);
        const auto block = pdf.blocks().first().toMap(); const auto original = block["source"].toString();
        QVERIFY2(original.contains("corresponding vertex shader version."), qUtf8Printable(original)); QVERIFY(original.contains("\n    vec3"));
        QVERIFY(original.contains("constructor\n"));
        QVERIFY(original.contains("sample\nstruct Light\n"));
        QVERIFY(original.contains("(texture coordinates) inside the buffer\n"));
        QVERIFY(original.contains("shader\nin VertexData\n"));
        QVERIFY(original.contains("/* Some explanation:\nThe values"));
        pdf.start(true); QTRY_VERIFY(server.ready);
        const auto body = QJsonDocument::fromJson(server.request.mid(server.request.indexOf("\r\n\r\n") + 4)).object();
        QCOMPARE(body["messages"].toArray().last().toObject()["content"].toString(), original);
        auto translation = original; translation.replace("This input block must match the corresponding vertex shader version.", QStringLiteral("输入块必须与对应的顶点着色器版本匹配。")); translation.replace("Initialize vector", QStringLiteral("初始化向量"));
        server.sendText(translation); server.finish(translation); QTRY_VERIFY(!pdf.busy()); QCOMPARE(pdf.completed(), 1); QCOMPARE(server.requestCount, 1); QVERIFY(pdf.error().isEmpty());
    }
    void hangingCodeCommentIsNotProgramText() {
        QTemporaryDir dir; const auto source = dir.filePath("hanging-comment.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(620, 950), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter p(&writer); QFont font("Courier New"); font.setPixelSize(10); p.setFont(font);
            const QStringList lines{"// Indices [0, NUM_DIR_LIGHTS) are directional", "lights;", "// indices [NUM_DIR_LIGHTS,",
                "NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;", "Light gLights[MaxLights];", "// A long unfinished comment followed by a real statement", "lights;", "float4 gFogColor;",
                "// In order to copy CPU memory data into our default", "buffer, we need", "buffer Data { float4 position; };",
                "// Alpha channel stores shininess at per-pixel", "level.", "const float shininess = 1.0f;",
                "/// Examples of AnimationClips are “Walk”, “Run”,", "“Attack”, “Defend”.",
                "DXGI_MODE_SCALING Scaling; // How the image is", "stretched",
                "// Mark the visible mirror pixels in the stencil", "buffer with the value 1",
                "// This triangle shares the following three", "vertices,",
                "D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, //", "filter",
                "// Note that we must supply a different per-pass", "constant buffer—one",
                "// An array of textures, which is only supported in", "shader model 5.1+.",
                "0);// base shader register arguments are bound to", "for this root parameter",
                "// The grid is the widest object with a width of", "20 and depth of",
                "// The texture array above will occupy registers t0,", "t1, …, t6 in",
                "// Create descriptor to mip level 0 of entire resource", "using the",
                "float FovAngleY, // vertical field of view angle", "in radians",
                "XMFLOAT3 Origin; // Origin of the frustum", "(and projection).",
                "// Shadows.hlsl by Frank Luna (C) 2015 All Rights", "Reserved.",
                "// Name Index Mask Register SysValue", "Format Used",
                "XMVECTOR XM_CALLCONV XMVector3NormalizeEst( //", "Returns estimated v/||v||",
                "// Clamp out of bound samples that occur at right", "image borders.",
                "// This thread group runs N threads. To get the", "extra 2*BlurRadius",
                "// z_ndc = A + B/viewZ, where gProj[2,2]=A and", "gProj[3,2]=B.",
                "// Cube map pass cbuffers are stored in elements", "1-6.",
                "// We cannot have an array entry in a constant", "buffer that gets",
                "// Our offset vectors are fixed and uniformly", "distributed (so that",
                "// lies on the ray of q, so there exists a t such", "that r = t*q."};
            for (int i = 0; i < lines.size(); ++i) p.drawText(QPointF(QList<int>{1, 3, 9, 12, 15, 17, 19, 21, 23}.contains(i) ? 40 : 80, 60 + i * 12), lines[i]);
        }
        const auto doc = inspect(source); QVERIFY(!doc.contains("error"));
        QStringList text;
        for (const auto &v : doc["pages"].toArray()[0].toObject()["blocks"].toArray()) text << v.toObject()["source"].toString();
        const auto original = text.join('\n');
        QVERIFY2(original.contains("are directional lights;"), qUtf8Printable(original));
        QVERIFY(original.contains("// indices [NUM_DIR_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;"));
        QVERIFY2(QRegularExpression("\\n\\s*Light gLights\\[MaxLights\\];").match(original).hasMatch(), qUtf8Printable(original));
        QVERIFY2(QRegularExpression("real statement\\n\\s*lights;\\n\\s*float4 gFogColor;").match(original).hasMatch(), qUtf8Printable(original));
        QVERIFY(original.contains("default buffer, we need\n"));
        QVERIFY(original.contains("per-pixel level.\n"));
        QVERIFY(original.contains("buffer Data { float4 position; };"));
        QVERIFY(original.contains(QStringLiteral("“Run”, “Attack”, “Defend”.")));
        QVERIFY(original.contains("// How the image is stretched\n"));
        QVERIFY(original.contains("stencil buffer with the value 1\n"));
        QVERIFY2(original.contains("following three vertices,\n"), qUtf8Printable(original));
        QVERIFY(original.contains("MIP_POINT, // filter"));
        QVERIFY(original.contains(QStringLiteral("per-pass constant buffer—one")));
        QVERIFY(original.contains("supported in shader model 5.1+."));
        QVERIFY(original.contains("bound to for this root parameter"));
        QVERIFY(original.contains("a width of 20 and depth of"));
        QVERIFY(original.contains(QStringLiteral("registers t0, t1, …, t6 in")));
        QVERIFY(original.contains("entire resource using the"));
        QVERIFY(original.contains("view angle in radians"));
        QVERIFY(original.contains("frustum (and projection)."));
        QVERIFY(original.contains("All Rights Reserved."));
        QVERIFY(original.contains("SysValue Format Used"));
        QVERIFY(original.contains("XMVector3NormalizeEst( // Returns estimated v/||v||"));
        QVERIFY(original.contains("// Clamp out of bound samples that occur at right image borders."));
        QVERIFY(original.contains("// This thread group runs N threads. To get the extra 2*BlurRadius"));
        QVERIFY(original.contains("// z_ndc = A + B/viewZ, where gProj[2,2]=A and gProj[3,2]=B."));
        QVERIFY(original.contains("// Cube map pass cbuffers are stored in elements 1-6."));
        QVERIFY(original.contains("// We cannot have an array entry in a constant buffer that gets"));
        QVERIFY(original.contains("// Our offset vectors are fixed and uniformly distributed (so that"));
        QVERIFY(original.contains("// lies on the ray of q, so there exists a t such that r = t*q."));
        auto translation = original;
        translation.replace("are directional lights;", QStringLiteral("是平行光；"));
        translation.replace("are point lights;", QStringLiteral("是点光源；"));
        QVERIFY(PdfLayout::changedCode(original, translation).isEmpty());
        translation.replace("gLights[MaxLights]", "gLights[1]");
        QVERIFY(!PdfLayout::changedCode(original, translation).isEmpty());
    }
    void closedUnderlinesRemainTranslatable() {
        QTemporaryDir dir; const auto source = dir.filePath("underlined-links.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(500, 300), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter p(&writer); p.setBrush(Qt::NoBrush);
            const auto link = [&](const QString &text, double x, double y, int size, double penWidth, double offset = 0) {
                QFont font("Times New Roman"); font.setPixelSize(size); p.setFont(font); p.setPen(QPen(Qt::blue, penWidth)); p.drawText(QPointF(x, y), text);
                const double width = QFontMetricsF(font).horizontalAdvance(text);
                const double underlineY = y + (offset == 0 ? size / 12.0 : offset);
                QPainterPath underline; underline.moveTo(x, underlineY); underline.lineTo(x + width, underlineY);
                underline.lineTo(x, underlineY); underline.closeSubpath(); p.drawPath(underline);
            };
            link("Typography", 40, 60, 18, .75); link("Go", 40, 105, 18, .75);
            p.save(); p.scale(.25, .25); link("Scaled typography", 160, 600, 72, 3); p.restore();
            link("Small capitals", 40, 180, 18, .75, -.75);
            QFont font("Times New Roman"); font.setPixelSize(18); p.setFont(font); p.setPen(QPen(Qt::black, 1));
            p.drawText(QPointF(40, 210), "Diagram crossing"); p.drawLine(QPointF(75, 194), QPointF(75, 216));
        }
        const auto doc = inspect(source); QVERIFY(!doc.contains("error"));
        QMap<QString, QJsonObject> blocks;
        for (const auto &v : doc["pages"].toArray()[0].toObject()["blocks"].toArray()) blocks[v.toObject()["source"].toString()] = v.toObject();
        for (const auto &text : {"Typography", "Go", "Scaled typography", "Small capitals"}) {
            QVERIFY2(blocks.contains(text), text); QVERIFY2(!blocks[text]["keep"].toBool(), text); QVERIFY(!blocks[text]["fixed"].toBool());
        }
        QVERIFY(blocks["Diagram crossing"]["keep"].toBool());
    }
    void inlineLinkUnderlinesDoNotProtectParagraph() {
        QTemporaryDir dir; const auto source = dir.filePath("inline-link.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(500, 300), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter p(&writer); QFont font("Times New Roman"); font.setPixelSize(14); p.setFont(font); p.setPen(QPen(Qt::black, .7));
            p.drawText(QPointF(40, 60), "Figure 1 shows a screenshot of the documentation.");
            const double left = 40 + QFontMetricsF(font).horizontalAdvance("Figure ");
            p.drawLine(QPointF(left, 61), QPointF(left + 7, 61));
            p.drawText(QPointF(40, 100), "This paragraph contains a link on its second line");
            p.drawText(QPointF(40, 117), "and explains the function shown in Figure 2.");
            const double end = 40 + QFontMetricsF(font).horizontalAdvance("and explains the function shown in Figure ");
            p.drawLine(QPointF(end, 118), QPointF(end + 7, 118));
            p.drawText(QPointF(40, 180), "A rule crossing the text");
            p.drawLine(QPointF(40, 175), QPointF(190, 175));
        }
        const auto doc = inspect(source); QVERIFY(!doc.contains("error")); bool linked = false, crossed = false;
        for (const auto &v : doc["pages"].toArray()[0].toObject()["blocks"].toArray()) {
            const auto b = v.toObject(); const auto text = b["source"].toString();
            if (text.startsWith("Figure") || text.startsWith("This paragraph")) {
                QVERIFY2(!b["keep"].toBool(), qUtf8Printable(text));
                if (text.startsWith("This paragraph")) { QVERIFY(text.contains("Figure 2.")); linked = true; }
            }
            if (text.startsWith("A rule")) { QVERIFY(b["keep"].toBool()); crossed = true; }
        }
        QVERIFY(linked); QVERIFY(crossed);
    }
    void inlineArtworkKeepsTranslatableTextFragments() {
        QTemporaryDir dir; const auto source = dir.filePath("inline-artwork.pdf");
        const QRectF artwork(140, 65, 34, 18);
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(500, 300), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter p(&writer); QFont font("Times New Roman"); font.setPixelSize(14); p.setFont(font);
            p.drawText(QPointF(40, 60), "This paragraph explains the following vector formula.");
            p.drawText(QPointF(40, 78), "The vector"); p.drawText(QPointF(185, 78), "has a unit length.");
            p.drawText(QPointF(40, 96), "These words continue after the inline formula.");
            QImage formula(68, 36, QImage::Format_RGB32); formula.fill(QColor("#38764b")); p.drawImage(artwork, formula);
        }
        auto doc = inspect(source); QVERIFY(!doc.contains("error")); auto pages = doc["pages"].toArray(); auto page = pages[0].toObject(); auto blocks = page["blocks"].toArray();
        QCOMPARE(blocks.size(), 4);
        for (int i = 0; i < blocks.size(); ++i) {
            auto b = blocks[i].toObject(); QVERIFY2(!b["keep"].toBool(), qUtf8Printable(b["source"].toString()));
            QVERIFY2(!PdfLayout::rect(b["region"]).intersects(artwork), qUtf8Printable(QJsonDocument(b).toJson()));
            b["translation"] = b["source"].toString() == "The vector" ? QStringLiteral("这里所示的向量") : QStringLiteral("示例译文"); blocks[i] = b;
        }
        page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto original = dir.filePath("before.png"), translated = dir.filePath("after.png");
        const auto result = PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", 0}, {"pixels", 1000}, {"original", original}, {"translated", translated}});
        QVERIFY(!result.contains("error"));
        for (const auto &s : result["statuses"].toArray()) QVERIFY2(s.toObject()["fits"].toBool(), qUtf8Printable(QJsonDocument(s.toObject()).toJson()));
        QCOMPARE(QImage(original).copy(QRect(282, 132, 64, 32)), QImage(translated).copy(QRect(282, 132, 64, 32)));
    }
    void textLayoutCheckMatchesNativePreview() {
        QTemporaryDir dir; const auto source = dir.filePath("check-layout.pdf"); makePdfFixture(source, 1);
        auto doc = translated(inspect(source)); auto pages = doc["pages"].toArray(); auto page = pages[0].toObject(); auto blocks = page["blocks"].toArray();
        auto block = blocks[1].toObject(); block["translation"] = QString(4000, QChar(0x6587)); blocks[1] = block; page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto checked = PdfEngine::execute({{"action", "checkLayout"}, {"source", source}, {"document", doc}, {"pages", QJsonArray{0}}});
        QVERIFY2(!checked.contains("error"), qUtf8Printable(checked["error"].toString())); QVERIFY(checked["textLayoutOnly"].toBool());
        const auto statuses = checked["pages"].toArray()[0].toObject()["statuses"].toArray(); QVERIFY(!statuses[1].toObject()["fits"].toBool());
        const auto rendered = PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", 0},
            {"original", dir.filePath("before.png")}, {"translated", dir.filePath("after.png")}});
        QCOMPARE(rendered["statuses"].toArray(), statuses);
        QVERIFY(PdfEngine::execute({{"action", "checkLayout"}, {"source", source}, {"document", doc}, {"pages", QJsonArray{1}}}).contains("error"));
    }
    void mathematicalFallbackPreservesUnicode() {
        QTemporaryDir dir; const auto source = dir.filePath("symbols.pdf"); makePdfFixture(source, 1);
        auto doc = translated(inspect(source)); auto pages = doc["pages"].toArray(); auto page = pages[0].toObject(); auto blocks = page["blocks"].toArray();
        auto b = blocks[1].toObject(); b["serif"] = true;
        const auto symbols = QStringLiteral("角度 ϕ，逆矩阵 A⁻¹；网址 ˜example；普通 φ 与波浪号 ~ 仍保持区别。");
        b["translation"] = symbols; blocks[1] = b; page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto output = dir.filePath("symbols-translated.pdf");
        const auto exported = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}});
        QVERIFY2(exported["saved"].toBool(), qUtf8Printable(exported["error"].toString()));
        const auto text = inspect(output)["pages"].toArray()[0].toObject()["text"].toString();
        for (const auto ch : QStringLiteral("ϕ⁻¹˜φ~")) QVERIFY2(text.contains(ch), qUtf8Printable(text));
        const auto artifacts = qEnvironmentVariable("YIYI_PDF_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) { QDir().mkpath(artifacts); QFile::copy(output, QDir(artifacts).filePath("math-symbols.pdf")); }
        const char32_t unsupported = 0x10ffff;
        b["translation"] = symbols + QString::fromUcs4(&unsupported, 1); blocks[1] = b; page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto check = PdfEngine::execute({{"action", "checkLayout"}, {"source", source}, {"document", doc}, {"pages", QJsonArray{0}}});
        bool rejected = false;
        for (const auto &v : check["pages"].toArray()[0].toObject()["statuses"].toArray()) {
            const auto s = v.toObject(); if (s["id"] == b["id"]) rejected = !s["fits"].toBool() && s["error"].toString().contains("10ffff");
        }
        QVERIFY(rejected);
    }
    void nativeScriptsStayWithTheirMathematicalBase() {
        QTemporaryDir dir; const auto source = dir.filePath("native-scripts.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(600, 260), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter p(&writer); QFont normal("Times New Roman"); normal.setPixelSize(14);
            QFont small("Times New Roman"); small.setPixelSize(10);
            double x = 40, baseline = 65;
            const auto text = [&](const QString &s, bool script = false, double offset = 0) {
                const auto font = script ? small : normal; p.setFont(font); p.drawText(QPointF(x, baseline + offset), s); x += QFontMetricsF(font).horizontalAdvance(s);
            };
            text("The inverse is (AB)"); text(QStringLiteral("−1"), true, -5); text(" = B"); text(QStringLiteral("−1"), true, -5);
            text("A"); text(QStringLiteral("−1"), true, -5); text(" and the component is x"); text("2", true, 4);
            text(" and r"); text("z", true, 4); text(".");
            x = 40; baseline = 110;
            text("The next paragraph lists v"); text("0,", true, 4);
            text(" v"); text("1,", true, 4); text(" and v"); text("2", true, 4); text(".");
        }
        auto doc = inspect(source); QVERIFY(!doc.contains("error")); auto pages = doc["pages"].toArray(); auto page = pages[0].toObject(); auto blocks = page["blocks"].toArray();
        QCOMPARE(blocks.size(), 2); auto b = blocks[0].toObject();
        auto plain = b["source"].toString(); plain.remove(' ');
        QVERIFY2(plain.contains(QStringLiteral("(AB)⁻¹=B⁻¹A⁻¹")), qUtf8Printable(plain));
        QVERIFY2(plain.contains(QStringLiteral("x₂")), qUtf8Printable(plain));
        QVERIFY2(plain.contains("r_{z}"), qUtf8Printable(plain));
        QVERIFY(b["styledSource"].toString().contains("<sub>z</sub>"));
        QVERIFY2(blocks[1].toObject()["source"].toString().contains(QStringLiteral("v₀,")), qUtf8Printable(blocks[1].toObject()["source"].toString()));
        QVERIFY(blocks[1].toObject()["source"].toString().contains(QStringLiteral("v₁,")));
        b["translation"] = QStringLiteral("逆矩阵满足 (AB)⁻¹ = B⁻¹A⁻¹，分量为 x₂ 和 r<sub>z</sub>。"); blocks[0] = b; page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto destination = dir.filePath("translated.pdf");
        const auto result = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", destination}});
        QVERIFY2(result["saved"].toBool(), qUtf8Printable(result["error"].toString()));
        auto output = inspect(destination)["pages"].toArray()[0].toObject()["text"].toString(); output.remove(QRegularExpression("\\s"));
        QVERIFY2(output.contains(QStringLiteral("(AB)⁻¹=B⁻¹A⁻¹")), qUtf8Printable(output));
        QCOMPARE(output.count(QStringLiteral("⁻¹")), 3);
        QVERIFY(output.contains(QStringLiteral("x₂")));
        QVERIFY2(output.contains("rz"), qUtf8Printable(output));
        QVERIFY(!output.contains("<sub>"));
        const auto exportedBlocks = inspect(destination)["pages"].toArray()[0].toObject()["blocks"].toArray();
        bool hasSubscript = false;
        for (const auto &v : exportedBlocks) if (v.toObject()["styledSource"].toString().contains("<sub>z</sub>")) hasSubscript = true;
        const auto artifacts = qEnvironmentVariable("YIYI_PDF_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) { QDir().mkpath(artifacts); QFile::copy(destination, QDir(artifacts).filePath("native-scripts.pdf")); }
        QVERIFY2(hasSubscript, qUtf8Printable(QJsonDocument(exportedBlocks).toJson(QJsonDocument::Compact)));
    }
    void firstLineIndentKeepsCompleteParagraphs() {
        QTemporaryDir dir; const auto source = dir.filePath("first-line-indent.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(600, 300), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter p(&writer); QFont font("Times New Roman"); font.setPixelSize(14); p.setFont(font);
            p.drawText(QPointF(65, 60), "This first line is indented, and its sentence continues");
            p.drawText(QPointF(36, 78), "on the next line without losing its meaning.");
            p.drawText(QPointF(65, 96), "A new paragraph starts with another first-line indent");
            p.drawText(QPointF(36, 114), "and must remain separate from the paragraph above.");
        }
        const auto doc = inspect(source); QVERIFY(!doc.contains("error")); const auto blocks = doc["pages"].toArray()[0].toObject()["blocks"].toArray();
        QCOMPARE(blocks.size(), 2);
        QVERIFY(blocks[0].toObject()["source"].toString().contains("continues on the next line"));
        QVERIFY(blocks[1].toObject()["source"].toString().contains("indent and must remain separate"));
    }
    void widelyJustifiedProseIsNotAColumnPerWord() {
        QTemporaryDir dir; const auto source = dir.filePath("justified.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(600, 300), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter p(&writer); QFont font("Times New Roman"); font.setPixelSize(14); p.setFont(font);
            const QStringList words{"After", "we", "have", "filled", "out", "a"};
            for (int i = 0; i < words.size(); ++i) p.drawText(QPointF(65 + i * 94, 60), words[i]);
            p.drawText(QPointF(36, 78), "descriptor, we create an object using the following method:");
            QFont code("Courier New"); code.setPixelSize(14); p.setFont(code);
            p.drawText(QPointF(36, 110), "Device::CreateGraphicsPipelineState");
            const double right = 36 + QFontMetricsF(code).horizontalAdvance("Device::CreateGraphicsPipelineState");
            p.setFont(font); p.drawText(QPointF(right + 3, 110), "method:");
            const QStringList ending{"the", "surrounding", "environment", "as", "well"};
            for (int i = 0; i < ending.size(); ++i) p.drawText(QPointF(36 + i * 115, 155), ending[i]);
        }
        const auto doc = inspect(source); QVERIFY(!doc.contains("error")); const auto blocks = doc["pages"].toArray()[0].toObject()["blocks"].toArray();
        QCOMPARE(blocks.size(), 3);
        QVERIFY2(blocks[0].toObject()["source"].toString().contains("After we have filled out a descriptor"), qUtf8Printable(QJsonDocument(doc).toJson()));
        QVERIFY(!blocks[1].toObject()["fixed"].toBool()); QVERIFY(!blocks[1].toObject()["code"].toBool());
        QCOMPARE(blocks[2].toObject()["source"].toString(), QStringLiteral("the surrounding environment as well"));
    }
    void proseInCodeFontIsTranslatable() {
        QTemporaryDir dir; const auto source = dir.filePath("code-prose.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(500, 400), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter p(&writer); QFont font("Courier New"); font.setPixelSize(9); p.setFont(font);
            const QStringList lines{"mat4 scale = mat4(1);", "For the other model, use a different matrix:", "mat4 scale = mat4(2);"};
            for (int i = 0; i < lines.size(); ++i) p.drawText(QPointF(40, 60 + i * 12), lines[i]);
        }
        const auto doc = inspect(source); const auto blocks = doc["pages"].toArray()[0].toObject()["blocks"].toArray(); QCOMPARE(blocks.size(), 3);
        QVERIFY(blocks[0].toObject()["fixed"].toBool()); QVERIFY(blocks[2].toObject()["fixed"].toBool());
        QVERIFY(!blocks[1].toObject()["fixed"].toBool()); QVERIFY(!blocks[1].toObject()["code"].toBool());
    }
    void wrappedProseInCodeFontIsTranslatable() {
        QTemporaryDir dir; const auto source = dir.filePath("wrapped-prose.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(600, 400), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter p(&writer); QFont font("Courier New"); font.setPixelSize(9); p.setFont(font);
            const QStringList lines{"Constructs a translation matrix from components in a", "vector:",
                "XMMATRIX XM_CALLCONV XMMatrixTranslationFromVector(", "  FXMVECTOR Offset);",
                "The CD3DX12_GPU_DESCRIPTOR_HANDLE type has the same", "Offset methods.",
                "will be labeled with IDs from 0, 1, ..., n-1, where", "n is the number of vertices in the draw call. For",
                "a DrawIndexed call, the vertex IDs correspond to", "the vertex index values.",
                "An animation is a list of key frames sorted by time:", "struct Keyframe { float TimePos; };",
                "associated final bone transforms if the render-item is", "animated by a skinned mesh:", "float4 gFogColor;",
                "just D3D12_COMPARISON_FUNC_LESS."};
            for (int i = 0; i < lines.size(); ++i) p.drawText(QPointF(40, 60 + i * 12), lines[i]);
        }
        const auto doc = inspect(source); QVERIFY(!doc.contains("error"));
        QStringList prose, code;
        for (const auto &v : doc["pages"].toArray()[0].toObject()["blocks"].toArray()) {
            const auto b = v.toObject(); (b["fixed"].toBool() ? code : prose).append(b["source"].toString());
        }
        const auto text = prose.join('\n'), program = code.join('\n');
        for (const auto &part : {"Constructs a translation matrix from components in a vector:", "The CD3DX12_GPU_DESCRIPTOR_HANDLE", "Offset methods.", "will be labeled with IDs", "An animation is a list", "associated final bone transforms"})
            QVERIFY2(text.contains(part), qUtf8Printable(text));
        QVERIFY(program.contains("XMMATRIX XM_CALLCONV XMMatrixTranslationFromVector("));
        QVERIFY(program.contains("struct Keyframe { float TimePos; };"));
        QVERIFY(program.contains("float4 gFogColor;"));
        QVERIFY(text.contains("just D3D12_COMPARISON_FUNC_LESS."));
    }
    void alphabeticalIndexKeepsEntriesAndColumnOrder() {
        QTemporaryDir dir; const auto source = dir.filePath("index.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(500, 400), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter p(&writer); QFont font("Arial"); font.setPixelSize(10); p.setFont(font);
            for (int col = 0; col < 2; ++col) {
                p.drawText(QPointF(40 + col * 220, 50), col == 0 ? "I" : "M");
                for (int row = 0; row < 12; ++row) p.drawText(QPointF(40 + col * 220, 70 + row * 12), QString("%1 topic %2").arg(col == 0 ? "Input" : "Matrix").arg(row + 1));
            }
        }
        const auto doc = inspect(source); const auto page = doc["pages"].toArray()[0].toObject(); const auto blocks = page["blocks"].toArray();
        QCOMPARE(blocks.size(), 26);
        QCOMPARE(blocks[0].toObject()["source"].toString(), QStringLiteral("I")); QVERIFY(blocks[0].toObject()["fixed"].toBool());
        QCOMPARE(blocks[13].toObject()["source"].toString(), QStringLiteral("M")); QVERIFY(blocks[13].toObject()["fixed"].toBool());
        for (int row = 0; row < 12; ++row) {
            QCOMPARE(blocks[row + 1].toObject()["source"].toString().remove(' '), QString("Inputtopic%1").arg(row + 1));
            QCOMPARE(blocks[row + 14].toObject()["source"].toString().remove(' '), QString("Matrixtopic%1").arg(row + 1));
        }
        for (const auto &region : page["layout"].toObject()["regions"].toArray()) QVERIFY(region.toObject()["kind"] != "table");
    }
    void mixedScanRemovesExactNativeText() {
        QTemporaryDir dir; const auto source = dir.filePath("mixed.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(300, 400), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter p(&writer); QFont font("Times New Roman"); font.setPixelSize(12); font.setItalic(true); p.setFont(font);
            p.drawText(QPointF(200, 45), "Chapter 5"); p.drawLine(QPointF(30, 47), QPointF(270, 47));
            font.setItalic(false); p.setFont(font); p.drawText(QPointF(140, 350), "[ 42 ]");
            p.drawLine(QPointF(30, 345), QPointF(130, 345)); p.drawLine(QPointF(175, 345), QPointF(270, 345));
        }
        auto doc = inspect(source); auto pages = doc["pages"].toArray(); auto page = pages[0].toObject(); auto blocks = page["blocks"].toArray();
        for (int i = 0; i < blocks.size(); ++i) { auto b = blocks[i].toObject(); if (!b["fixed"].toBool()) b["translation"] = QStringLiteral("章"); blocks[i] = b; }
        page["scan"] = QJsonObject{{"blocks", blocks}}; pages[0] = page; doc["pages"] = pages;
        const auto original = dir.filePath("original.png"), result = dir.filePath("translated.png");
        const auto status = PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", 0}, {"pixels", 900}, {"original", original}, {"translated", result}});
        QVERIFY2(!status.contains("error"), qUtf8Printable(status["error"].toString()));
        for (const auto &v : status["statuses"].toArray()) QVERIFY(v.toObject()["fits"].toBool());
        auto ink = [](const QImage &image, const QRect &area, int cutoff = 180) { int count = 0; for (int y = area.top(); y <= area.bottom(); ++y) for (int x = area.left(); x <= area.right(); ++x) if (qGray(image.pixel(x, y)) < cutoff) ++count; return count; };
        const QImage before(original), after(result);
        QVERIFY(ink(before, QRect(650, 100, 110, 34)) > 50);
        QVERIFY(ink(after, QRect(650, 100, 110, 34)) < 10); // no English header ghost
        const auto beforeLine = ink(before, QRect(90, 136, 500, 12), 245), afterLine = ink(after, QRect(90, 136, 500, 12), 245);
        QVERIFY2(beforeLine > 350 && afterLine > beforeLine * .7, qPrintable(QString("separator ink: %1 -> %2").arg(beforeLine).arg(afterLine)));
        QVERIFY(ink(after, QRect(420, 1010, 100, 50)) > 60); // original fixed page number survives
        const auto output = dir.filePath("translated.pdf");
        QVERIFY(!PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}}).contains("error"));
        QVERIFY(inspect(output)["pages"].toArray()[0].toObject()["text"].toString().contains(QStringLiteral("章")));
    }
    void gradientBackgroundSurvivesExport() {
        QTemporaryDir dir; const auto source = dir.filePath("gradient.pdf"); makeShadingPdfFixture(source);
        auto doc = inspect(source); auto pages = doc["pages"].toArray(); auto page = pages[0].toObject(); QVERIFY(page["rasterBackground"].toBool());
        QVERIFY(page["warning"].toString().isEmpty());
        AppController app(dir.filePath("profile")); PdfController controller(&app);
        QVERIFY(controller.loadFile(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(controller.loaded(), 20000); QTRY_VERIFY_WITH_TIMEOUT(!controller.rendering(), 20000);
        QCOMPARE(controller.unsupportedPages(), 0);
        QVERIFY(controller.warning().isEmpty()); QVERIFY(controller.pageNotice().contains(QStringLiteral("图像保留背景")));
        QVERIFY(controller.pageTotal() > 0);
        for (const auto &v : controller.blocks()) { const auto b = v.toMap(); if (!b["fixed"].toBool() && !b["keep"].toBool()) QVERIFY(controller.editBlock(b["id"].toString(), QStringLiteral("渐变图形"))); }
        QTRY_VERIFY_WITH_TIMEOUT(!controller.rendering(), 20000);
        QCOMPARE(controller.completed(), controller.total());
        QVERIFY(!controller.pageStatus().contains(QStringLiteral("本页未翻译")));
        QVERIFY(!controller.status().contains(QStringLiteral("页未处理")));
        auto blocks = page["blocks"].toArray(); for (int i = 0; i < blocks.size(); ++i) { auto b = blocks[i].toObject(); b["translation"] = QStringLiteral("渐变图形"); blocks[i] = b; } page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto original = dir.filePath("original.png"), preview = dir.filePath("preview.png");
        QVERIFY(!PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", 0}, {"pixels", 900}, {"original", original}, {"translated", preview}}).contains("error"));
        const QImage before(original), after(preview);
        for (const auto &point : {QPoint(180, 420), QPoint(420, 570), QPoint(660, 720)}) {
            const auto a = before.pixelColor(point), b = after.pixelColor(point);
            QVERIFY(qAbs(a.red() - b.red()) < 4 && qAbs(a.green() - b.green()) < 4 && qAbs(a.blue() - b.blue()) < 4);
        }
        const auto output = dir.filePath("translated.pdf");
        QVERIFY(!PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}}).contains("error"));
        QVERIFY(inspect(output)["pages"].toArray()[0].toObject()["text"].toString().contains(QStringLiteral("渐变图形")));
    }
    void punctuationRemainsSearchable() {
        QTemporaryDir dir; const auto source = dir.filePath("punctuation.pdf"); makeSharedTextPdfFixture(source, "Long enough source text for punctuation", "Helvetica");
        auto doc = inspect(source); auto pages = doc["pages"].toArray(); auto page = pages[0].toObject(); auto blocks = page["blocks"].toArray(); QCOMPARE(blocks.size(), 1);
        auto b = blocks[0].toObject(); const QString target = QStringLiteral("示例——全文 – 字符／标点"); b["translation"] = target; b["serif"] = true; blocks[0] = b; page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto output = dir.filePath("translated.pdf");
        QVERIFY(!PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}}).contains("error"));
        auto text = inspect(output)["pages"].toArray()[0].toObject()["text"].toString(); text.remove(QRegularExpression(QStringLiteral("\\s")));
        auto expected = target; expected.remove(' ');
        QVERIFY2(text.contains(expected), qUtf8Printable(text));
    }
    void nestedTextAndSharedForms() {
        QTemporaryDir dir; const auto source = dir.filePath("nested.pdf"); makeNestedPdfFixture(source); auto doc = inspect(source);
        QVERIFY2(!doc.contains("error"), qUtf8Printable(doc["error"].toString())); auto pages = doc["pages"].toArray(); QCOMPARE(pages.size(), 2);
        auto page = pages[0].toObject(); QVERIFY2(page["warning"].toString().isEmpty(), qUtf8Printable(page["warning"].toString()));
        auto blocks = page["blocks"].toArray(); QCOMPARE(blocks.size(), 1); auto block = blocks[0].toObject();
        QCOMPARE(block["source"].toString(), QStringLiteral("Nested sample text")); QVERIFY(!block["keep"].toBool());
        QCOMPARE(block["size"].toDouble(), 12.0);
        QVERIFY(PdfLayout::rect(block["rect"]).left() > 44 && PdfLayout::rect(block["rect"]).left() < 46);
        block["translation"] = QStringLiteral("嵌套对象译文"); blocks[0] = block; page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto output = dir.filePath("translated.pdf"); const auto saved = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}});
        QVERIFY2(saved["saved"].toBool(), qUtf8Printable(saved["error"].toString())); const auto reopened = inspect(output); const auto resultPages = reopened["pages"].toArray();
        const auto text = resultPages[0].toObject()["text"].toString(); QVERIFY2(text.contains(QStringLiteral("嵌套对象译文")), qUtf8Printable(text)); QVERIFY(!text.contains("Nested sample text"));
        QVERIFY(resultPages[1].toObject()["text"].toString().contains("Nested sample text"));
        for (int p = 0; p < 2; ++p) {
            const auto original = dir.filePath(QString::number(p) + "-original.png"), preview = dir.filePath(QString::number(p) + "-preview.png"), exported = dir.filePath(QString::number(p) + "-exported.png");
            QVERIFY(!PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", p}, {"original", original}, {"translated", preview}}).contains("error"));
            QVERIFY(!PdfEngine::execute({{"action", "render"}, {"source", output}, {"document", reopened}, {"page", p}, {"original", exported}, {"translated", dir.filePath("unused.png")}}).contains("error"));
            QCOMPARE(QImage(preview), QImage(exported));
            if (p == 1) QCOMPARE(QImage(original), QImage(exported));
            else QCOMPARE(QImage(original).copy(140, 1250, 120, 110), QImage(exported).copy(140, 1250, 120, 110));
        }
    }
    void neighboringDiagramLabels() {
        QTemporaryDir dir; const auto source = dir.filePath("diagram.pdf");
        {
            QPdfWriter pdf(source); pdf.setResolution(72); pdf.setPageSize(QPageSize(QSizeF(300, 400), QPageSize::Point)); pdf.setPageMargins(QMarginsF());
            QPainter p(&pdf); p.fillRect(QRectF(20, 40, 55, 20), QColor("#bbbbbb")); p.fillRect(QRectF(83, 40, 40, 20), QColor("#bbbbbb"));
            QFont font("Arial"); font.setPixelSize(8); p.setFont(font); p.setPen(Qt::black);
            p.drawText(QPointF(23, 53), "Vertex Data"); p.drawText(QPointF(86, 53), "Textures");
            p.fillRect(QRectF(150, 80, 12, 105), QColor("#bbbbbb"));
            const QString label = "RASTERIZER"; for (int i = 0; i < label.size(); ++i) p.drawText(QPointF(153, 90 + i * 9), label.mid(i, 1));
        }
        const auto doc = inspect(source); QVERIFY(!doc.contains("error")); QStringList labels;
        for (const auto &v : doc["pages"].toArray()[0].toObject()["blocks"].toArray()) { const auto b = v.toObject(); labels.append(b["source"].toString()); QVERIFY(!b["keep"].toBool()); }
        QVERIFY2(labels.contains("Vertex Data") && labels.contains("Textures") && labels.contains("RASTERIZER"), qUtf8Printable(labels.join(" | ")));
    }
    void sharedObjectKeepsFixedNumber_data() {
        QTest::addColumn<QByteArray>("labelText"); QTest::addColumn<QByteArray>("fontName"); QTest::addColumn<QString>("translation");
        QTest::newRow("name") << QByteArray("Name") << QByteArray("Helvetica") << QStringLiteral("姓名");
        QTest::newRow("toc") << QByteArray("Preface") << QByteArray("Helvetica-Bold") << QStringLiteral("前言");
    }
    void sharedObjectKeepsFixedNumber() {
        QFETCH(QByteArray, labelText); QFETCH(QByteArray, fontName); QFETCH(QString, translation);
        QTemporaryDir dir; const auto source = dir.filePath("shared-number.pdf");
        makeSharedTextPdfFixture(source, labelText + "                    42", fontName);
        auto doc = inspect(source); auto pages = doc["pages"].toArray(); auto page = pages[0].toObject(); auto blocks = page["blocks"].toArray(); QCOMPARE(blocks.size(), 2);
        const int labelIndex = blocks[0].toObject()["source"] == QString::fromLatin1(labelText) ? 0 : 1;
        auto label = blocks[labelIndex].toObject(); const auto number = blocks[1 - labelIndex].toObject(); QVERIFY(number["fixed"].toBool());
        bool shared = false; for (const auto &v : label["objects"].toArray()) if (number["objects"].toArray().contains(v)) shared = true; QVERIFY(shared);
        QVERIFY(PdfLayout::rect(label["region"]).right() < PdfLayout::rect(number["rect"]).left());
        label["translation"] = translation; blocks[labelIndex] = label; page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto output = dir.filePath("translated.pdf"); const auto saved = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}});
        QVERIFY2(saved["saved"].toBool(), qUtf8Printable(saved["error"].toString()));
        const auto text = inspect(output)["pages"].toArray()[0].toObject()["text"].toString();
        QVERIFY(text.contains(translation)); QVERIFY(!text.contains(QString::fromLatin1(labelText))); QCOMPARE(text.count("42"), 1);
    }
    void tableRulesDoNotInflateScannedFont() {
        QTemporaryDir dir; const auto source = dir.filePath("table.pdf"); makeStructuredPdfFixture(source, 2, true, false, false); const auto doc = inspect(source);
        const auto result = PdfEngine::execute({{"action", "recognitionImage"}, {"source", source}, {"document", doc}, {"page", 0},
            {"region", QJsonArray{0, 0, 1, 1}}, {"pixels", 1800}, {"detectLayout", true}, {"image", dir.filePath("recognition.png")}});
        QVERIFY(!result.contains("error")); double smallest = 1000, largest = 0; int cells = 0;
        for (const auto &v : result["blocks"].toArray()) { const auto b = v.toObject(); if (b["role"] != "cell") continue;
            ++cells; smallest = qMin(smallest, b["size"].toDouble()); largest = qMax(largest, b["size"].toDouble()); }
        QCOMPARE(cells, 12); QVERIFY2(largest / smallest < 1.25, qUtf8Printable(QStringLiteral("Cell fonts %1..%2").arg(smallest).arg(largest)));
    }
    void detectorOnBackgrounds_data() {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {QString("solid"), QString("gradient"), QString("texture"), QString("orange")}) QTest::newRow(qPrintable(kind)) << kind;
        if (QFileInfo::exists(qEnvironmentVariable("YIYI_PDF_PHOTO_FILE"))) QTest::newRow("photo") << QString("photo");
    }
    void detectorOnBackgrounds() {
        QFETCH(QString, kind); const auto fixture = makeBackgroundFixture(kind); QTemporaryDir dir;
        const auto source = dir.filePath("background.pdf"); saveBackgroundPdf(source, fixture); const auto doc = inspect(source);
        const auto result = PdfEngine::execute({{"action", "recognitionImage"}, {"source", source}, {"document", doc}, {"page", 0},
            {"region", QJsonArray{0, 0, 1, 1}}, {"pixels", 1800}, {"detectLayout", true}, {"image", dir.filePath("recognition.png")}});
        QVERIFY2(!result.contains("error"), qUtf8Printable(result["error"].toString()));
        for (const auto &line : fixture.lines) {
            const QRectF expected(line.x() / 2.0, line.y() / 2.0, line.width() / 2.0, line.height() / 2.0); double coverage = 0;
            for (const auto &v : result["blocks"].toArray()) for (const auto &m : v.toObject()["masks"].toArray()) {
                const auto intersection = PdfLayout::rect(m).intersected(expected); coverage += intersection.width() * intersection.height();
            }
            QVERIFY2(coverage >= expected.width() * expected.height() * .75, qUtf8Printable(QJsonDocument(result).toJson()));
        }
        const auto artifacts = qEnvironmentVariable("YIYI_PDF_ARTIFACT_DIR"); if (!artifacts.isEmpty()) {
            QDir folder(QDir(artifacts).filePath("detector-" + kind)); QDir().mkpath(folder.path());
            QFile::remove(folder.filePath("source.pdf")); QFile::copy(source, folder.filePath("source.pdf"));
            QFile json(folder.filePath("detected.json")); QVERIFY(json.open(QIODevice::WriteOnly)); json.write(QJsonDocument(result).toJson());
        }
    }
    void backgroundFailureKeepsOtherRegions() {
        auto fixture = makeBackgroundFixture("texture");
        { QPainter p(&fixture.withText); for (int x = 0; x < 1200; x += 16) p.fillRect(QRect(x, 140, 16, 100), x % 32 ? QColor("#242f46") : QColor("#9a6522"));
            QFont font("Arial"); font.setPixelSize(38); font.setBold(true); p.setFont(font); p.setPen(Qt::white); p.drawText(QPoint(90, 220), "Read the words on this picture."); }
        QTemporaryDir dir; const auto source = dir.filePath("mixed.pdf"); saveBackgroundPdf(source, fixture);
        auto doc = inspect(source); auto pages = doc["pages"].toArray(); auto page = pages[0].toObject(); QJsonArray blocks;
        for (int i = 0; i < fixture.lines.size(); ++i) { const auto line = fixture.lines[i]; const QRectF r(line.x() / 2.0, line.y() / 2.0, line.width() / 2.0, line.height() / 2.0);
            blocks.append(QJsonObject{{"id", QString::number(i)}, {"source", "Read this line."}, {"translation", QStringLiteral("读取这一行文字。")}, {"rect", PdfLayout::jsonRect(r)}, {"region", PdfLayout::jsonRect(r)},
                {"masks", QJsonArray{PdfLayout::jsonRect(r)}}, {"size", 15}, {"scan", true}, {"backgroundMode", i ? "panel" : "auto"}, {"color", QJsonArray{0, 0, 0}}});
        }
        page["scan"] = QJsonObject{{"blocks", blocks}}; pages[0] = page; doc["pages"] = pages;
        const auto before = dir.filePath("before.png"), after = dir.filePath("after.png");
        const auto rendered = PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", 0}, {"original", before}, {"translated", after}});
        QVERIFY(!rendered.contains("error")); const auto statuses = rendered["statuses"].toArray(); QCOMPARE(statuses.size(), 2);
        QVERIFY(!statuses[0].toObject()["error"].toString().isEmpty()); QVERIFY(statuses[1].toObject()["fits"].toBool());
        const QImage a(before), b(after); QVERIFY(a != b);
        // Compare at the erasure resolution: PDF rasterization itself resamples
        // the whole image, so preview pixels need not be bitwise identical.
        QJsonArray failures; const auto cleaned = PdfScanLayout::eraseText(fixture.withText, QSizeF(600, 360), blocks, &failures);
        QCOMPARE(failures.size(), 1); QCOMPARE(cleaned.copy(fixture.lines[0]), fixture.withText.copy(fixture.lines[0]));
        QVERIFY(cleaned.copy(fixture.lines[1]) != fixture.withText.copy(fixture.lines[1]));
        auto block = blocks[0].toObject(); block["backgroundMode"] = "original"; blocks[0] = block;
        page["scan"] = QJsonObject{{"blocks", blocks}}; pages[0] = page; doc["pages"] = pages;
        const auto output = dir.filePath("translated.pdf"); const auto saved = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}});
        QVERIFY2(saved["saved"].toBool(), qUtf8Printable(saved["error"].toString()));
        QVERIFY(inspect(output)["pages"].toArray()[0].toObject()["text"].toString().contains(QStringLiteral("读取这一行文字")));
    }
    void shortHeadingGapDoesNotInterleaveColumns() {
        QJsonArray items;
        auto add = [&](const QString &text, double x, double y, double w) { items.append(QJsonObject{{"source", text}, {"rect", QJsonArray{x, y, w, 10}}}); };
        add("Spanning heading", 50, 35, 490); add("Left paragraph starts", 50, 53, 210); add("Left paragraph continues", 50, 68, 210);
        add("Left second paragraph", 50, 130, 210); add("Right paragraph starts", 330, 53, 210); add("Right paragraph continues", 330, 68, 210); add("Right second paragraph", 330, 130, 210);
        const auto layout = PdfLayout::analyze(items, {}, QSizeF(595, 842), "test"); QStringList order; for (const auto &v : layout.items) order.append(v.toObject()["source"].toString());
        QVERIFY(order.indexOf("Left second paragraph") < order.indexOf("Right paragraph starts")); QCOMPARE(order.first(), QStringLiteral("Spanning heading"));
    }
    void backgroundConfirmationAndCache() {
        QTemporaryDir dir; const auto path = dir.filePath("photo.pdf"); const auto fixture = makeBackgroundFixture("texture"); saveBackgroundPdf(path, fixture);
        const auto doc = inspect(path); QJsonArray masks; QRectF box;
        for (const auto &line : fixture.lines) { QRectF r(line.x() / 2.0, line.y() / 2.0, line.width() / 2.0, line.height() / 2.0); masks.append(PdfLayout::jsonRect(r)); box = box.isEmpty() ? r : box.united(r); }
        const QJsonObject block{{"id", "photo-text"}, {"source", fixture.source}, {"translation", QStringLiteral("阅读图片上的文字，保留背景。")}, {"rect", PdfLayout::jsonRect(box)}, {"region", PdfLayout::jsonRect(box.adjusted(0, 0, 0, 8))},
            {"masks", masks}, {"size", 15}, {"color", QJsonArray{255, 255, 255}}, {"scan", true}, {"background", PdfBackground::describe(fixture.withText, fixture.lines.first())}};
        QDir().mkpath(dir.filePath("pdf-tasks")); QFile cache(dir.filePath("pdf-tasks/" + doc["hash"].toString() + ".json")); QVERIFY(cache.open(QIODevice::WriteOnly));
        cache.write(QJsonDocument(QJsonObject{{"version", 4}, {"hash", doc["hash"]}, {"scans", QJsonArray{QJsonObject{{"page", 0}, {"scan", QJsonObject{{"source", block["source"]}, {"translation", block["translation"]}, {"blocks", QJsonArray{block}}}}}}}}).toJson()); cache.close();
        {
            AppController app(dir.path()); PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(path))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
            QCOMPARE(pdf.blocks().size(), 1); QVERIFY(!pdf.reviseBlock("photo-text", {{"backgroundMode", "repair"}, {"confirmBackground", true}}));
            QVERIFY(pdf.reviseBlock("photo-text", {{"backgroundMode", "repair"}})); QTRY_VERIFY_WITH_TIMEOUT(!pdf.rendering(), 20000);
            QVERIFY2(pdf.blocks()[0].toMap()["backgroundPreviewed"].toBool(), qUtf8Printable(pdf.blocks()[0].toMap()["layoutError"].toString()));
            QVERIFY(pdf.reviseBlock("photo-text", {{"confirmBackground", true}})); QTRY_VERIFY_WITH_TIMEOUT(!pdf.rendering(), 20000);
            QVERIFY(pdf.blocks()[0].toMap()["backgroundConfirmed"].toBool());
            QVERIFY(pdf.exportFile(QUrl::fromLocalFile(dir.filePath("confirmed.pdf")))); QTRY_VERIFY_WITH_TIMEOUT(!pdf.processing(), 20000); QVERIFY2(QFileInfo::exists(dir.filePath("confirmed.pdf")), qUtf8Printable(pdf.error()));
            QVERIFY(pdf.reviseBlock("photo-text", {{"translation", QStringLiteral("修改后的图片译文")}})); QVERIFY(!pdf.blocks()[0].toMap()["backgroundConfirmed"].toBool());
            QVERIFY(pdf.reviseBlock("photo-text", {{"backgroundMode", "panel"}, {"panelColor", "#143044"}, {"textColorMode", "light"}}));
        }
        {
            AppController app(dir.path()); PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(path))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
            const auto restored = pdf.blocks()[0].toMap(); QCOMPARE(restored["backgroundMode"].toString(), QStringLiteral("panel")); QCOMPARE(restored["panelColor"].toString(), QStringLiteral("#143044"));
            QCOMPARE(restored["translation"].toString(), QStringLiteral("修改后的图片译文"));
        }
    }
    void backgroundCleaning_data() {
        QTest::addColumn<QString>("kind"); QTest::addColumn<bool>("light");
        QTest::newRow("solid-light") << QString("solid") << true; QTest::newRow("solid-dark") << QString("solid") << false;
        QTest::newRow("gradient-light") << QString("gradient") << true; QTest::newRow("texture-light") << QString("texture") << true; QTest::newRow("texture-dark") << QString("texture") << false;
        if (QFileInfo::exists(qEnvironmentVariable("YIYI_PDF_PHOTO_FILE"))) QTest::newRow("photo-light") << QString("photo") << true;
    }
    void backgroundCleaning() {
        QFETCH(QString, kind); QFETCH(bool, light); const auto fixture = makeBackgroundFixture(kind, light); auto cleaned = fixture.withText; QString error;
        const auto description = PdfBackground::describe(fixture.withText, fixture.lines.first());
        QCOMPARE(description["textColor"].toArray()[0].toInt(), light ? 255 : 0);
        if (kind == "texture") {
            auto autoClean = fixture.withText; QVERIFY(!PdfBackground::erase(autoClean, fixture.withText, fixture.lines, "auto", &error)); QCOMPARE(autoClean, fixture.withText);
        }
        QVERIFY2(PdfBackground::erase(cleaned, fixture.withText, fixture.lines, kind == "texture" || kind == "photo" ? "repair" : "auto", &error), qUtf8Printable(error));
        double before = 0, after = 0; int pixels = 0;
        for (const auto &line : fixture.lines) for (int y = line.top(); y <= line.bottom(); ++y) for (int x = line.left(); x <= line.right(); ++x) {
            const auto target = fixture.clean.pixel(x, y); const auto a = fixture.withText.pixel(x, y), b = cleaned.pixel(x, y);
            before += std::abs(qRed(a) - qRed(target)) + std::abs(qGreen(a) - qGreen(target)) + std::abs(qBlue(a) - qBlue(target));
            after += std::abs(qRed(b) - qRed(target)) + std::abs(qGreen(b) - qGreen(target)) + std::abs(qBlue(b) - qBlue(target)); ++pixels;
        }
        QVERIFY2(after < before * (kind == "texture" || kind == "photo" ? .8 : .1), qUtf8Printable(QStringLiteral("Before %1, after %2").arg(before / pixels).arg(after / pixels)));
        QCOMPARE(cleaned.copy(900, 400, 280, 300), fixture.clean.copy(900, 400, 280, 300));
        const auto artifacts = qEnvironmentVariable("YIYI_PDF_ARTIFACT_DIR"); if (!artifacts.isEmpty()) { QDir folder(QDir(artifacts).filePath(QString::fromLatin1(QTest::currentDataTag()))); QDir().mkpath(folder.path()); fixture.withText.save(folder.filePath("original.png")); fixture.clean.save(folder.filePath("ground-truth.png")); cleaned.save(folder.filePath("repaired.png")); }
    }
    void backgroundModesAndExport() {
        QTemporaryDir dir; const auto path = dir.filePath("picture.pdf"); const auto fixture = makeBackgroundFixture("texture"); saveBackgroundPdf(path, fixture);
        auto doc = inspect(path); auto pages = doc["pages"].toArray(); auto page = pages[0].toObject();
        QJsonArray masks; QRectF box;
        for (const auto &line : fixture.lines) { const QRectF r(line.x() / 2.0, line.y() / 2.0, line.width() / 2.0, line.height() / 2.0); masks.append(PdfLayout::jsonRect(r)); box = box.isEmpty() ? r : box.united(r); }
        QJsonObject block{{"id", "picture-caption"}, {"source", fixture.source}, {"translation", QStringLiteral("阅读图片上的文字，保留原有背景。")}, {"rect", PdfLayout::jsonRect(box)}, {"region", PdfLayout::jsonRect(box.adjusted(0, 0, 0, 8))},
            {"masks", masks}, {"size", 15}, {"color", QJsonArray{255, 255, 255}}, {"scan", true}};
        auto manifest = [&](const QString &mode, bool confirmed = false) {
            auto b = block; b["backgroundMode"] = mode; b["backgroundConfirmed"] = confirmed; b["panelColor"] = "#143044";
            page["scan"] = QJsonObject{{"source", b["source"]}, {"translation", b["translation"]}, {"blocks", QJsonArray{b}}}; pages[0] = page; doc["pages"] = pages; return doc;
        };
        const auto untouched = manifest("original");
        const auto original = dir.filePath("original.png"), translated = dir.filePath("translated.png");
        auto render = [&](const QJsonObject &value) { return PdfEngine::execute({{"action", "render"}, {"source", path}, {"document", value}, {"page", 0}, {"original", original}, {"translated", translated}}); };
        QVERIFY(!render(untouched).contains("error")); QCOMPARE(QImage(original), QImage(translated));
        const auto repair = manifest("repair"); const auto rendered = render(repair); QVERIFY2(!rendered.contains("error"), qUtf8Printable(rendered["error"].toString()));
        QVERIFY(rendered["statuses"].toArray()[0].toObject()["backgroundReview"].toBool()); QVERIFY(QImage(original) != QImage(translated));
        auto saved = PdfEngine::execute({{"action", "export"}, {"source", path}, {"document", repair}, {"destination", dir.filePath("unconfirmed.pdf")}});
        QVERIFY(saved["error"].toString().contains(QStringLiteral("尚未确认"))); QVERIFY(!QFileInfo::exists(dir.filePath("unconfirmed.pdf")));
        for (const auto &mode : {QString("repair"), QString("panel")}) {
            const auto value = manifest(mode, true); const auto out = dir.filePath(mode + ".pdf"); QVERIFY(!render(value).contains("error")); const QImage preview(translated);
            saved = PdfEngine::execute({{"action", "export"}, {"source", path}, {"document", value}, {"destination", out}}); QVERIFY2(saved["saved"].toBool(), qUtf8Printable(saved["error"].toString()));
            const auto reopened = inspect(out); QCOMPARE(reopened["pages"].toArray().size(), 1); QVERIFY(reopened["pages"].toArray()[0].toObject()["text"].toString().contains(QStringLiteral("保留原有背景")));
            QVERIFY(!PdfEngine::execute({{"action", "render"}, {"source", out}, {"document", reopened}, {"page", 0}, {"original", dir.filePath("reopen.png")}, {"translated", dir.filePath("unused.png")}}).contains("error"));
            QCOMPARE(preview, QImage(dir.filePath("reopen.png")));
        }
        QCOMPARE(inspect(path)["hash"], doc["hash"]);
    }
    void complexTables_data() {
        QTest::addColumn<bool>("scan"); QTest::addColumn<bool>("borderless"); QTest::addColumn<bool>("vertical");
        QTest::newRow("native-merged") << false << false << false; QTest::newRow("scan-merged") << true << false << false;
        QTest::newRow("native-borderless") << false << true << false; QTest::newRow("scan-borderless") << true << true << false;
        QTest::newRow("native-vertical") << false << false << true; QTest::newRow("scan-vertical") << true << false << true;
    }
    void complexTables() {
        QFETCH(bool, scan); QFETCH(bool, borderless); QFETCH(bool, vertical); QTemporaryDir dir; const auto path = dir.filePath("table.pdf");
        if (borderless) makeBorderlessPdfFixture(path, scan); else makeStructuredPdfFixture(path, 2, scan, !vertical, false, vertical);
        const auto doc = inspect(path); QVERIFY(!doc.contains("error")); const auto page = doc["pages"].toArray()[0].toObject();
        QJsonObject data = page;
        if (scan) data = PdfEngine::execute({{"action", "recognitionImage"}, {"source", path}, {"document", doc}, {"page", 0}, {"region", QJsonArray{0, 0, 1, 1}},
            {"pixels", 2200}, {"detectLayout", true}, {"image", dir.filePath("recognition.png")}});
        QVERIFY2(!data.contains("error"), qUtf8Printable(data["error"].toString()));
        int tables = 0, cells = 0, wideCells = 0, tallCells = 0;
        for (const auto &v : data["layout"].toObject()["regions"].toArray()) { const auto r = v.toObject();
            if (r["kind"] == "table") { ++tables; QCOMPARE(r["inferred"].toBool(), borderless); if (!scan && !borderless) QCOMPARE(r["headerRows"].toInt(), 2); }
            if (r["kind"] == "cell") { ++cells; if (r["columnSpan"].toInt() > 1) ++wideCells; if (r["rowSpan"].toInt() > 1) ++tallCells; }
        }
        QVERIFY2(tables == 1, qUtf8Printable(QJsonDocument(data).toJson())); QCOMPARE(cells, borderless ? 12 : 11); QCOMPARE(wideCells, borderless || vertical ? 0 : 1); QCOMPARE(tallCells, vertical ? 1 : 0);
        if (!scan && !borderless && !vertical) {
            const auto blocks = data["blocks"].toArray(); QJsonObject current;
            for (const auto &v : blocks) if (v.toObject()["source"] == "12 mA") current = v.toObject();
            QVERIFY(!current.isEmpty());
            const auto context = PdfLayout::cellContext(current, blocks, data["layout"].toObject()["regions"].toArray());
            QVERIFY(context.contains("Measurements")); QVERIFY(context.contains("Current rating")); QVERIFY(context.contains("Controller"));
        }
    }
    void continuedTableHeaders() {
        QTemporaryDir dir; const auto path = dir.filePath("continued.pdf"); makeContinuedTableFixture(path); const auto doc = inspect(path);
        QVERIFY2(!doc.contains("error"), qUtf8Printable(doc["error"].toString())); const auto pages = doc["pages"].toArray(); QCOMPARE(pages.size(), 2);
        const auto page = pages[1].toObject(); const auto regions = page["layout"].toObject()["regions"].toArray(); QJsonObject table, block;
        for (const auto &v : regions) if (v.toObject()["kind"] == "table") table = v.toObject();
        QCOMPARE(table["headerRows"].toInt(), 0); QCOMPARE(table["continuedFrom"].toObject()["page"].toInt(-1), 0); QVERIFY(!table["inheritedHeaders"].toArray().isEmpty());
        for (const auto &v : page["blocks"].toArray()) if (v.toObject()["source"] == "12 mA") { block = v.toObject(); break; }
        const auto context = PdfLayout::cellContext(block, page["blocks"].toArray(), regions); QVERIFY(context.contains("Current")); QVERIFY(context.contains("Amplifier"));
    }
    void structuralEditsAndCache() {
        QTemporaryDir dir; const auto path = dir.filePath("table.pdf"); makeBorderlessPdfFixture(path); QString newId;
        {
            AppController app(dir.path()); PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(path))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
            const auto initial = pdf.blocks(); QVERIFY(initial.size() >= 12); const auto first = initial[0].toMap()["id"].toString(), second = initial[1].toMap()["id"].toString();
            QVERIFY(pdf.editBlock(first, QStringLiteral("已校对的表头")));
            QVERIFY2(pdf.mergeBlocks({first, second}), qUtf8Printable(pdf.error())); QCOMPARE(pdf.blocks().size(), initial.size() - 1); QVERIFY(pdf.canUndoLayout());
            QVERIFY(pdf.blocks()[0].toMap()["draftTranslation"].toString().contains(QStringLiteral("已校对")));
            QCOMPARE(pdf.blocks()[0].toMap()["columnSpan"].toInt(), 2); QVERIFY(pdf.undoLayout()); QCOMPARE(pdf.blocks().size(), initial.size());
            QCOMPARE(pdf.blocks()[0].toMap()["translation"].toString(), QStringLiteral("已校对的表头"));
            QVERIFY(pdf.mergeBlocks({first, second})); newId = pdf.blocks()[0].toMap()["id"].toString();
            QVERIFY(pdf.editBlock(newId, QStringLiteral("部件与电流"))); QVERIFY(pdf.moveBlock(newId, pdf.blocks().size() - 1));
        }
        {
            AppController app(dir.path()); PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(path))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
            QCOMPARE(pdf.blocks().last().toMap()["id"].toString(), newId); QCOMPARE(pdf.blocks().last().toMap()["translation"].toString(), QStringLiteral("部件与电流"));
            QCOMPARE(pdf.blocks().last().toMap()["columnSpan"].toInt(), 2);
            bool found = false; for (const auto &v : pdf.layoutRegions()) if (v.toMap()["id"] == pdf.blocks().last().toMap()["container"]) found = true; QVERIFY(found);
        }
    }
    void nativeSplitIsAtomicAndRestores() {
        QTemporaryDir dir; const auto path = dir.filePath("structured.pdf"); makeStructuredPdfFixture(path); QString a, b;
        {
            AppController app(dir.path()); PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(path))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
            const auto title = pdf.blocks()[0].toMap(); const auto rect = title["region"].toList();
            QVERIFY2(pdf.splitBlock(title["id"].toString(), true, rect[0].toDouble() + rect[2].toDouble() / 2, "First title", "Second title"), qUtf8Printable(pdf.error()));
            a = pdf.blocks()[0].toMap()["id"].toString(); b = pdf.blocks()[1].toMap()["id"].toString();
            QVERIFY(pdf.editBlock(a, QStringLiteral("第一标题"))); QVERIFY(pdf.exportFile(QUrl::fromLocalFile(dir.filePath("incomplete.pdf"))));
            QTRY_VERIFY_WITH_TIMEOUT(!pdf.processing(), 20000); QVERIFY(!QFileInfo::exists(dir.filePath("incomplete.pdf")));
            QVERIFY(pdf.editBlock(b, QStringLiteral("第二标题"))); QVERIFY(pdf.exportFile(QUrl::fromLocalFile(dir.filePath("complete.pdf"))));
            QTRY_VERIFY_WITH_TIMEOUT(!pdf.processing(), 20000); QVERIFY2(QFileInfo::exists(dir.filePath("complete.pdf")), qUtf8Printable(pdf.error()));
            const auto text = inspect(dir.filePath("complete.pdf"))["pages"].toArray()[0].toObject()["text"].toString();
            QVERIFY(text.contains(QStringLiteral("第一标题"))); QVERIFY(text.contains(QStringLiteral("第二标题"))); QVERIFY(!text.contains("A report with columns"));
        }
        {
            AppController app(dir.path()); PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(path))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
            QCOMPARE(pdf.blocks()[0].toMap()["id"].toString(), a); QCOMPARE(pdf.blocks()[1].toMap()["id"].toString(), b); QCOMPARE(pdf.blocks()[1].toMap()["translation"].toString(), QStringLiteral("第二标题"));
        }
    }
    void tableTranslationContextAndNumbers() {
        QTemporaryDir dir; const auto path = dir.filePath("table.pdf"); makeStructuredPdfFixture(path, 2, false, true, false);
        AppController app(dir.path()); PdfController pdf(&app); StreamServer server("openai");
        QVERIFY(app.saveProvider({{"name", "table"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "test"}}).isEmpty());
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(path))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000); QString id;
        for (const auto &v : pdf.blocks()) if (v.toMap()["source"] == "12 mA") id = v.toMap()["id"].toString(); QVERIFY(!id.isEmpty());
        QVERIFY(pdf.editBlock(id, "12 mA")); QVERIFY(pdf.retryBlock(id)); QTRY_VERIFY(server.ready);
        QVERIFY(server.request.contains("Measurements")); QVERIFY(server.request.contains("Current rating")); QVERIFY(server.request.contains("Controller"));
        server.sendText("21 mA"); server.finish("21 mA"); QTRY_VERIFY(!pdf.busy()); QVERIFY(pdf.error().contains(QStringLiteral("数字")));
        for (const auto &v : pdf.blocks()) if (v.toMap()["id"] == id) QCOMPARE(v.toMap()["translation"].toString(), QStringLiteral("12 mA"));
    }
    void structuredPage_data() {
        QTest::addColumn<int>("columns"); QTest::addColumn<bool>("scan");
        QTest::newRow("native-two") << 2 << false; QTest::newRow("native-three") << 3 << false;
        QTest::newRow("scan-two") << 2 << true; QTest::newRow("scan-three") << 3 << true;
    }
    void structuredPage() {
        QFETCH(int, columns); QFETCH(bool, scan); QTemporaryDir dir; const auto source = dir.filePath("structured.pdf");
        makeStructuredPdfFixture(source, columns, scan, false, !scan);
        auto doc = inspect(source); QVERIFY2(!doc.contains("error"), qUtf8Printable(doc["error"].toString()));
        auto pages = doc["pages"].toArray(); auto page = pages[0].toObject(); QJsonObject layout; QJsonArray blocks;
        if (scan) {
            auto recognition = PdfEngine::execute({{"action", "recognitionImage"}, {"source", source}, {"document", doc}, {"page", 0},
                {"region", QJsonArray{0, 0, 1, 1}}, {"pixels", 2200}, {"detectLayout", true}, {"image", dir.filePath("recognition.png")}});
            QVERIFY2(!recognition.contains("error"), qUtf8Printable(recognition["error"].toString())); blocks = recognition["blocks"].toArray(); layout = recognition["layout"].toObject();
        } else { blocks = page["blocks"].toArray(); layout = page["layout"].toObject(); }
        const auto regions = layout["regions"].toArray(); int tables = 0, cells = 0, cols = 0, translatedCount = 0;
        for (const auto &v : regions) { const auto r = v.toObject(); if (r["kind"] == "table") ++tables; if (r["kind"] == "cell") ++cells; if (r["kind"] == "column") ++cols; }
        const auto detail = QJsonDocument(QJsonObject{{"page", page}, {"layout", layout}, {"blocks", blocks}}).toJson();
        if (!qEnvironmentVariable("YIYI_PDF_ARTIFACT_DIR").isEmpty()) {
            const QDir folder(QDir(qEnvironmentVariable("YIYI_PDF_ARTIFACT_DIR")).filePath(QString::fromLatin1(QTest::currentDataTag()))); QDir().mkpath(folder.path());
            QFile::remove(folder.filePath("source.pdf")); QFile::copy(source, folder.filePath("source.pdf"));
            QFile data(folder.filePath("detected.json")); if (data.open(QIODevice::WriteOnly)) data.write(detail);
        }
        QCOMPARE(tables, 1); QCOMPARE(cells, 12); QVERIFY2(cols >= columns, detail.constData());
        double previousColumn = -1; int columnParagraphs = 0;
        for (int i = 0; i < blocks.size(); ++i) {
            auto b = blocks[i].toObject(); const auto box = PdfLayout::rect(b["rect"]), area = PdfLayout::rect(b["region"]);
            QVERIFY2(!b["keep"].toBool(), qUtf8Printable(b["preserveReason"].toString()));
            if (box.top() > 70 && box.bottom() < 190) {
                QVERIFY2(box.left() + 2 >= previousColumn, detail.constData()); previousColumn = box.left(); ++columnParagraphs;
                const auto parent = PdfLayout::rect(b["containerRect"]); QVERIFY(parent.adjusted(-.01, -.01, .01, .01).contains(area));
            }
            if (b["role"] == "cell") QVERIFY(PdfLayout::rect(b["containerRect"]).contains(area));
            if (b["fixed"].toBool()) continue;
            if (scan) b["source"] = "Recognized text";
            b["translation"] = b["source"].toString().startsWith("Column") ? QStringLiteral("第 %1 栏的译文，文字按段落排列，保持在各自栏内。").arg(b["source"].toString().section(' ', 1, 1))
                : b["source"].toString().contains(QRegularExpression("\\d")) ? b["source"].toString() : QStringLiteral("清晰的译文");
            blocks[i] = b; ++translatedCount;
        }
        QVERIFY(columnParagraphs >= columns); QVERIFY(translatedCount >= 10);
        if (!scan) {
            QStringList sources; for (const auto &v : blocks) sources.append(v.toObject()["source"].toString());
            QVERIFY(sources.contains("12 mA")); QVERIFY(sources.contains("3.3 V")); QVERIFY(!sources.contains("1 2 mA"));
        }
        if (scan) page["scan"] = QJsonObject{{"blocks", blocks}, {"layout", layout}}; else page["blocks"] = blocks;
        pages[0] = page; doc["pages"] = pages;
        const auto preview = dir.filePath("translated.png"), original = dir.filePath("original.png"), out = dir.filePath("export.pdf");
        const auto render = PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", 0}, {"pixels", 1500}, {"original", original}, {"translated", preview}});
        QVERIFY2(!render.contains("error"), qUtf8Printable(render["error"].toString()));
        for (const auto &v : render["statuses"].toArray()) QVERIFY2(v.toObject()["fits"].toBool(), qUtf8Printable(QJsonDocument(v.toObject()).toJson()));
        const auto saved = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", out}});
        QVERIFY2(saved["saved"].toBool(), qUtf8Printable(saved["error"].toString()));
        const auto reopened = inspect(out); QCOMPARE(reopened["pages"].toArray().size(), 1); QVERIFY(reopened["pages"].toArray()[0].toObject()["text"].toString().contains(QStringLiteral("清晰的译文")));
        QVERIFY(!PdfEngine::execute({{"action", "render"}, {"source", out}, {"document", reopened}, {"page", 0}, {"pixels", 1500}, {"original", dir.filePath("reopen.png")}, {"translated", dir.filePath("unused.png")}}).contains("error"));
        QCOMPARE(QImage(preview), QImage(dir.filePath("reopen.png")));
        const QImage before(original), after(preview); const double scale = before.width() / 595.0;
        for (const auto &p : {QPointF(100, 270), QPointF(215, 360), QPointF(400, 420), QPointF(400, 210)}) {
            const auto a = before.pixelColor(qRound(p.x() * scale), qRound(p.y() * scale)), b = after.pixelColor(qRound(p.x() * scale), qRound(p.y() * scale));
            QVERIFY2(std::abs(a.lightness() - b.lightness()) < 35, qUtf8Printable(QStringLiteral("Rule changed at %1,%2").arg(p.x()).arg(p.y())));
        }
        if (!scan) QCOMPARE(before.copy(QRect(qRound(470 * scale), qRound(495 * scale), 100, 100)), after.copy(QRect(qRound(470 * scale), qRound(495 * scale), 100, 100)));
        else {
            // This tail of "Current rating" lies beyond the shorter Chinese
            // replacement. The grid's connected outline must not protect it.
            const QRect tail(qRound(283 * scale), qRound(318 * scale), qRound(9 * scale), qRound(9 * scale)); int oldInk = 0, remainingInk = 0;
            for (int y = tail.top(); y <= tail.bottom(); ++y) for (int x = tail.left(); x <= tail.right(); ++x) {
                if (qGray(before.pixel(x, y)) < 180) ++oldInk;
                if (qGray(after.pixel(x, y)) < 180) ++remainingInk;
            }
            QVERIFY(oldInk > 10); QCOMPARE(remainingInk, 0);
            for (const auto &area : {QRectF(165, 30, 240, 28), QRectF(170, 642, 170, 32)}) {
                const QRect pixels(qRound(area.x() * scale), qRound(area.y() * scale), qRound(area.width() * scale), qRound(area.height() * scale));
                int ink = 0; for (int y = pixels.top(); y <= pixels.bottom(); ++y) for (int x = pixels.left(); x <= pixels.right(); ++x) if (qGray(after.pixel(x, y)) < 180) ++ink;
                QCOMPARE(ink, 0); // Letter baselines are not decorative rules.
            }
        }
        const auto artifacts = qEnvironmentVariable("YIYI_PDF_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
            const QDir folder(QDir(artifacts).filePath(QString::fromLatin1(QTest::currentDataTag()))); QDir().mkpath(folder.path());
            for (const auto &name : {"source.pdf", "translated.pdf", "original.png", "translated.png"}) QFile::remove(folder.filePath(name));
            QFile::copy(source, folder.filePath("source.pdf")); QFile::copy(out, folder.filePath("translated.pdf")); QFile::copy(original, folder.filePath("original.png")); QFile::copy(preview, folder.filePath("translated.png"));
            QFile details(folder.filePath("layout.json")); if (details.open(QIODevice::WriteOnly)) details.write(QJsonDocument(doc).toJson());
        }
    }
    void tableNumberValidation() {
        QVERIFY(PdfLayout::changedNumbers("12 mA / 3.3 V", QStringLiteral("12 mA / 3.3 V")).isEmpty());
        QVERIFY(!PdfLayout::changedNumbers("12 mA / 3.3 V", "21 mA / 3.3 V").isEmpty());
        QVERIFY(!PdfLayout::changedNumbers("50%", "50").isEmpty());
        QVERIFY(PdfLayout::changedNumbers("-5", QString::fromUtf8("−5")).isEmpty());
    }
    void scannedCellCleaningPreservesKnownBorders() {
        QImage source(220, 120, QImage::Format_RGB32); source.fill(Qt::white);
        { QPainter p(&source); p.setPen(QPen(QColor(100, 100, 100), 2)); p.drawRect(QRectF(20, 20, 180, 80));
          p.setPen(Qt::black); p.drawText(QPointF(35, 60), "Cell text"); }
        const QJsonObject cell{{"id", "cell"}, {"role", "cell"}, {"containerRect", QJsonArray{20, 20, 180, 80}},
            {"rect", QJsonArray{21, 21, 178, 78}}, {"region", QJsonArray{23, 23, 174, 74}},
            {"masks", QJsonArray{QJsonArray{21, 21, 178, 78}}}, {"backgroundMode", "panel"}, {"panelColor", "#ffffff"}};
        QJsonArray failures; const auto cleaned = PdfScanLayout::eraseText(source, QSizeF(220, 120), QJsonArray{cell}, &failures);
        QVERIFY(failures.isEmpty());
        for (const auto &edge : {QRect(17, 17, 186, 5), QRect(17, 98, 186, 5), QRect(17, 17, 5, 86), QRect(198, 17, 5, 86)})
            QCOMPARE(cleaned.copy(edge), source.copy(edge));
        QImage blank(174, 74, QImage::Format_RGB32); blank.fill(Qt::white);
        QCOMPARE(cleaned.copy(QRect(23, 23, 174, 74)), blank);
        // The detector's complete text box provides the background samples;
        // the narrower cell interior only constrains changed pixels.
        QImage tinted(180, 80, QImage::Format_RGB32); tinted.fill(QColor(235, 235, 235));
        { QPainter p(&tinted); p.fillRect(QRect(30, 20, 120, 20), Qt::black); }
        auto limited = tinted; QString error;
        QVERIFY2(PdfBackground::erase(limited, tinted, {QRect(25, 15, 130, 30)}, "auto", &error, QRect(45, 20, 90, 20)), qUtf8Printable(error));
        QCOMPARE(limited.pixel(60, 25), qRgb(235, 235, 235));
        QCOMPARE(limited.copy(QRect(25, 15, 20, 30)), tinted.copy(QRect(25, 15, 20, 30)));
        QCOMPARE(limited.copy(QRect(135, 15, 20, 30)), tinted.copy(QRect(135, 15, 20, 30)));
    }
    void nativeMixedEmphasisAndSourceEdits() {
        QTemporaryDir dir; const auto source = dir.filePath("styled.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QPageSize::A4)); writer.setPageMargins(QMarginsF());
            QPainter painter(&writer); QFont font("Times New Roman"); font.setPixelSize(12); font.setBold(true); painter.setFont(font);
            const auto name = QStringLiteral("Dimitrios Christopoulos"); painter.drawText(QPointF(50, 100), name);
            double x = 50 + QFontMetricsF(font).horizontalAdvance(name + " "); font.setBold(false); painter.setFont(font);
            const auto prefix = QStringLiteral("reviewed the book "); painter.drawText(QPointF(x, 100), prefix); x += QFontMetricsF(font).horizontalAdvance(prefix);
            font.setItalic(true); painter.setFont(font); painter.drawText(QPointF(x, 100), "OpenGL Programming.");
            font.setItalic(false); painter.setFont(font); painter.drawText(QPointF(50, 116), "He also writes about computer graphics.");
        }
        auto doc = inspect(source); auto pages = doc["pages"].toArray(); auto page = pages[0].toObject();
        QVERIFY2(page["warning"].toString().isEmpty(), qUtf8Printable(page["warning"].toString()));
        auto blocks = page["blocks"].toArray(); QCOMPARE(blocks.size(), 1); auto b = blocks[0].toObject();
        QVERIFY(!b["bold"].toBool()); QVERIFY(b["serif"].toBool()); QVERIFY(b["styledSource"].toString().contains("<b>")); QVERIFY(b["styledSource"].toString().contains("<i>"));
        b["translation"] = QStringLiteral("<b>迪米特里斯</b>审阅了<i>《OpenGL 编程》</i>一书。他也撰写计算机图形学文章。");
        blocks[0] = b; page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto output = dir.filePath("styled-translated.pdf");
        const auto exported = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}});
        QVERIFY2(exported["saved"].toBool(), qUtf8Printable(exported["error"].toString()));
        const auto text = inspect(output)["pages"].toArray()[0].toObject()["text"].toString(); QVERIFY(text.contains(QStringLiteral("迪米特里斯"))); QVERIFY(!text.contains("<b>")); QVERIFY(!text.contains("<i>"));
        QString id;
        {
            AppController app(dir.path()); PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
            id = pdf.blocks().first().toMap()["id"].toString();
            QVERIFY(pdf.reviseBlock(id, {{"source", "Corrected original paragraph"}, {"translation", QStringLiteral("校对后的译文")}, {"size", 13.0}, {"keep", true}}));
        }
        {
            AppController app(dir.path()); PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
            const auto restored = pdf.blocks().first().toMap(); QCOMPARE(restored["id"].toString(), id); QCOMPARE(restored["source"].toString(), QStringLiteral("Corrected original paragraph"));
            QVERIFY(restored["keep"].toBool()); QCOMPARE(restored["size"].toDouble(), 13.0); QCOMPARE(restored["translation"].toString(), QStringLiteral("校对后的译文"));
        }
    }
    void scanLayoutPreservesPageAndArtwork() {
        QTemporaryDir dir; const auto source = dir.filePath("scan.pdf"); makeScanPdfFixture(source, 1, true);
        auto doc = inspect(source); QVERIFY(!doc.contains("error")); auto pages = doc["pages"].toArray();
        const auto recognition = PdfEngine::execute({{"action", "recognitionImage"}, {"source", source}, {"document", doc}, {"page", 0},
            {"region", QJsonArray{0, 0, 1, 1}}, {"pixels", 1800}, {"detectLayout", true}, {"image", dir.filePath("recognition.png")}});
        QVERIFY2(!recognition.contains("error"), qUtf8Printable(recognition["error"].toString()));
        auto blocks = recognition["blocks"].toArray(); QVERIFY(blocks.size() >= 3);
        if (!qEnvironmentVariable("YIYI_PDF_ARTIFACT_DIR").isEmpty()) {
            QDir().mkpath(qEnvironmentVariable("YIYI_PDF_ARTIFACT_DIR"));
            QFile debug(QDir(qEnvironmentVariable("YIYI_PDF_ARTIFACT_DIR")).filePath("scan-regions.json"));
            if (debug.open(QIODevice::WriteOnly)) debug.write(QJsonDocument(recognition).toJson());
            QFile::copy(source, QDir(qEnvironmentVariable("YIYI_PDF_ARTIFACT_DIR")).filePath("scan-source.pdf"));
        }
        for (int i = 0; i < blocks.size(); ++i) {
            auto b = blocks[i].toObject(); b["source"] = QStringLiteral("Source %1").arg(i);
            b["translation"] = i == 0 ? QStringLiteral("扫描文档") : QStringLiteral("保留原页中的图片与分隔线。");
            if (b["rect"].toArray()[1].toDouble() > 700) b["fixed"] = true;
            blocks[i] = b;
        }
        auto page = pages[0].toObject(); page["scan"] = QJsonObject{{"blocks", blocks}}; pages[0] = page; doc["pages"] = pages;
        const auto original = dir.filePath("original.png"), preview = dir.filePath("preview.png"), output = dir.filePath("translated.pdf");
        const auto rendering = PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", 0}, {"original", original}, {"translated", preview}});
        QVERIFY(!rendering.contains("error")); for (const auto &s : rendering["statuses"].toArray()) QVERIFY2(s.toObject()["fits"].toBool(), qUtf8Printable(s.toObject()["error"].toString()));
        const auto exported = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}});
        QVERIFY2(exported["saved"].toBool(), qUtf8Printable(exported["error"].toString()));
        auto saved = inspect(output); const auto savedPages = saved["pages"].toArray(); QCOMPARE(savedPages.size(), 1);
        QCOMPARE(savedPages[0].toObject()["width"], page["width"]); QCOMPARE(savedPages[0].toObject()["height"], page["height"]);
        QVERIFY(savedPages[0].toObject()["text"].toString().contains(QStringLiteral("扫描文档")));
        const auto reread = dir.filePath("exported.png");
        QVERIFY(!PdfEngine::execute({{"action", "render"}, {"source", output}, {"document", saved}, {"page", 0}, {"original", reread}, {"translated", dir.filePath("other.png")}}).contains("error"));
        QCOMPARE(QImage(preview), QImage(reread)); QVERIFY(QImage(original) != QImage(preview));
        const QImage before(original), after(preview);
        // Color artwork and separators remain at the original pixel locations.
        for (const auto &p : {QPoint(150, 580), QPoint(450, 600), QPoint(300, 450), QPoint(300, 740)}) {
            const auto a = before.pixelColor(p), b = after.pixelColor(p);
            QVERIFY(std::abs(a.red() - b.red()) < 12); QVERIFY(std::abs(a.green() - b.green()) < 12); QVERIFY(std::abs(a.blue() - b.blue()) < 12);
        }
        auto changed = blocks[1].toObject(); changed["translation"] = QStringLiteral("这是一段无法放入指定区域的过长译文。").repeated(300); blocks[1] = changed;
        page["scan"] = QJsonObject{{"blocks", blocks}}; pages[0] = page; doc["pages"] = pages;
        const auto rejected = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", dir.filePath("overflow.pdf")}});
        QVERIFY(rejected.contains("error")); QVERIFY(!QFileInfo::exists(dir.filePath("overflow.pdf")));
        const auto artifacts = qEnvironmentVariable("YIYI_PDF_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) { QDir().mkpath(artifacts); QFile::copy(source, QDir(artifacts).filePath("scan.pdf")); QFile::copy(output, QDir(artifacts).filePath("scan-layout.pdf")); QFile::copy(preview, QDir(artifacts).filePath("scan-layout.png")); }
    }
    void scanRecognitionEditingRecovery_data() {
        QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QStringLiteral("openai"); QTest::newRow("responses") << QStringLiteral("responses");
    }
    void scanRecognitionEditingRecovery() {
        QFETCH(QString, protocol); QTemporaryDir dir; const auto source = dir.filePath("scan.pdf"); makeScanPdfFixture(source);
        AppController app(dir.path()); StreamServer server(protocol);
        QVERIFY(app.saveProvider({{"name", "vision"}, {"protocol", protocol}, {"baseUrl", server.url()}, {"model", "test"}, {"apiKey", "test-secret"}}).isEmpty());
        QString firstId;
        {
            PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
            pdf.recognizePages(1, 1);
            for (const auto &text : {"Scanned document", "Please close the door.", "Keep the original page for comparison."}) {
                QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000); QVERIFY(server.request.contains("data:image/png;base64,"));
                QVERIFY(server.request.contains("Do not translate")); server.sendText(text); server.finish({}); server.ready = false; server.request.clear();
            }
            QTRY_VERIFY(!pdf.busy()); QCOMPARE(pdf.blocks().size(), 3); QVERIFY(pdf.layoutReady()); QCOMPARE(pdf.recognizedPages(), 1);
            firstId = pdf.blocks().first().toMap()["id"].toString();
            QVERIFY(pdf.editBlock(firstId, QStringLiteral("扫描文档")));
            QVERIFY(pdf.reviseBlock(firstId, {{"source", "Edited document"}})); QVERIFY(pdf.translationStale()); QCOMPARE(pdf.completed(), 0);
            for (const auto &failure : {"cancel", "disconnect", "empty"}) {
                pdf.recognizePages(1, 1, 1800, true); QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000);
                if (QByteArray(failure) != "empty") server.sendText("Partial");
                if (QByteArray(failure) == "cancel") pdf.cancel(); else if (QByteArray(failure) == "disconnect") server.interrupt(); else server.finish({});
                QTRY_VERIFY(!pdf.busy()); server.ready = false; server.request.clear();
                QCOMPARE(pdf.blocks().first().toMap()["source"].toString(), QStringLiteral("Edited document"));
                QCOMPARE(pdf.blocks().first().toMap()["translation"].toString(), QStringLiteral("扫描文档")); QVERIFY(pdf.translationStale());
            }
            QVERIFY(pdf.retryBlock(firstId)); QTRY_VERIFY(server.ready); QVERIFY(server.request.contains("Edited document"));
            server.sendText(QStringLiteral("修改后的文档")); server.finish({}); server.ready = false; server.request.clear();
            QTRY_VERIFY(!pdf.busy()); QVERIFY(!pdf.translationStale()); QCOMPARE(pdf.completed(), 1);
            auto region = pdf.blocks().first().toMap()["region"].toList(); region[2] = region[2].toDouble() - 3;
            QVERIFY(pdf.reviseBlock(firstId, {{"region", region}, {"size", 18.0}, {"keep", true}})); QCOMPARE(pdf.completed(), 0);
            QVERIFY(!pdf.reviseBlock(firstId, {{"region", QVariantList{-2, 0, 100, 100}}}));
            QSignalSpy crop(&pdf, &PdfController::regionRecognized); pdf.recognizeRegion(QRectF(.1, .1, .8, .2)); QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000);
            server.sendText("Crop text"); server.finish({}); QTRY_COMPARE(crop.size(), 1); QCOMPARE(pdf.regionText(), QStringLiteral("Crop text"));
            QCOMPARE(pdf.blocks().first().toMap()["source"].toString(), QStringLiteral("Edited document"));
        }
        {
            PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
            QCOMPARE(pdf.blocks().size(), 3); const auto first = pdf.blocks().first().toMap();
            QCOMPARE(first["source"].toString(), QStringLiteral("Edited document")); QCOMPARE(first["translation"].toString(), QStringLiteral("修改后的文档"));
            QVERIFY(first["keep"].toBool()); QCOMPARE(first["size"].toDouble(), 18.0);
            pdf.resetTranslations(); QCOMPARE(pdf.blocks().size(), 3); QCOMPARE(pdf.scanSource().split("\n\n").size(), 3); QVERIFY(pdf.blocks().first().toMap()["translation"].toString().isEmpty());
        }
    }
    void scanBatchCancellationDoesNotSendNextPage() {
        QTemporaryDir dir; const auto source = dir.filePath("scan.pdf"); makeScanPdfFixture(source, 2);
        AppController app(dir.path()); StreamServer server("openai");
        QVERIFY(app.saveProvider({{"name", "vision"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "test"}}).isEmpty());
        PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        connect(&pdf, &PdfController::changed, &pdf, [&] { if (pdf.recognizing() && pdf.recognizedPages() == 1) pdf.cancel(); });
        pdf.recognizePages(1, 2);
        for (const auto &text : {"Title", "Body one", "Body two"}) {
            QTRY_VERIFY_WITH_TIMEOUT(server.ready, 20000); server.sendText(text); server.finish({}); server.ready = false; server.request.clear();
        }
        QTRY_VERIFY(!pdf.busy()); QTest::qWait(150); QCOMPARE(server.requestCount, 3); QCOMPARE(pdf.recognizedPages(), 1); QVERIFY(!app.pdfBusy());
    }
    void legacyScanDraftCannotBeExportedAsLayout() {
        QTemporaryDir dir; const auto source = dir.filePath("scan.pdf"); makeScanPdfFixture(source); auto doc = inspect(source);
        auto pages = doc["pages"].toArray(); auto page = pages.first().toObject(); page["scan"] = QJsonObject{{"source", "Saved draft"}, {"translation", QStringLiteral("已有译文")}};
        pages[0] = page; doc["pages"] = pages;
        const auto result = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", dir.filePath("output.pdf")}});
        QVERIFY(result["error"].toString().contains(QStringLiteral("旧版文字草稿"))); QVERIFY(!QFileInfo::exists(dir.filePath("output.pdf")));
        // Old cache must remain available even after an original-only preview.
        const auto cacheDir = dir.filePath("pdf-tasks"); QDir().mkpath(cacheDir);
        QFile cache(QDir(cacheDir).filePath(doc["hash"].toString() + ".json")); QVERIFY(cache.open(QIODevice::WriteOnly));
        cache.write(QJsonDocument(QJsonObject{{"version", 2}, {"hash", doc["hash"]}, {"scans", QJsonArray{QJsonObject{{"page", 0}, {"scan", page["scan"]}}}}}).toJson()); cache.close();
        AppController app(dir.path()); PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!pdf.rendering(), 20000); QCOMPARE(pdf.scanSource(), QStringLiteral("Saved draft")); QCOMPARE(pdf.scanTranslation(), QStringLiteral("已有译文")); QVERIFY(!pdf.layoutReady());
    }
    void hyperlinksListsAndBackgrounds() {
        QTemporaryDir dir; const auto source = dir.filePath("decorated.pdf"); makeDecoratedPdfFixture(source);
        auto doc = inspect(source); QVERIFY2(!doc.contains("error"), qUtf8Printable(doc["error"].toString()));
        auto pages = doc["pages"].toArray(); auto page = pages[0].toObject();
        QVERIFY2(page["warning"].toString().isEmpty(), qUtf8Printable(page["warning"].toString()));
        auto blocks = page["blocks"].toArray(); QSet<QString> markers; int listItems = 0; bool panel = false, link = false;
        for (int i = 0; i < blocks.size(); ++i) {
            auto block = blocks[i].toObject(); const auto text = block["source"].toString();
            if (block["fixed"].toBool()) { markers.insert(text); continue; }
            if (text.startsWith("These")) {
                ++listItems; QVERIFY(text.contains("terms of the license."));
                QVERIFY2(text.contains("The continuation belongs to the same item."), qUtf8Printable(QJsonDocument(page).toJson()));
                QVERIFY(block["rect"].toArray()[0].toDouble() >= 104);
            }
            if (text.startsWith("Homepage:")) { link = true; QVERIFY(text.contains("https://example.org/project")); }
            if (text.startsWith("This text")) {
                panel = true; const auto r = block["region"].toArray();
                QVERIFY(r[0].toDouble() >= 50); QVERIFY(r[0].toDouble() + r[2].toDouble() <= 540.1);
                QVERIFY(r[1].toDouble() + r[3].toDouble() <= 520.1);
            }
            block["translation"] = text.startsWith("Homepage:") ? QStringLiteral("主页：https://example.org/project") : QStringLiteral("这是保留原位置的中文译文。");
            blocks[i] = block;
        }
        QCOMPARE(markers, (QSet<QString>{QStringLiteral("1."), QStringLiteral("a."), QStringLiteral("ii."), QStringLiteral("•"), QStringLiteral("*")}));
        QCOMPARE(listItems, 5); QVERIFY(panel); QVERIFY(link);
        page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto original = dir.filePath("original.png"), rendered = dir.filePath("translated.png"), output = dir.filePath("translated.pdf");
        const auto result = PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", 0}, {"pixels", 1190}, {"original", original}, {"translated", rendered}});
        QVERIFY2(!result.contains("error"), qUtf8Printable(result["error"].toString()));
        QCOMPARE(result["statuses"].toArray().size(), 9);
        for (const auto &s : result["statuses"].toArray()) QVERIFY2(s.toObject()["fits"].toBool(), qUtf8Printable(s.toObject()["error"].toString()));
        QImage before(original), after(rendered); QVERIFY(!before.isNull()); QVERIFY(before != after);
        for (int i = 0; i < 5; ++i) QCOMPARE(before.copy(90, (150 + i * 55 - 15) * 2, 80, 38), after.copy(90, (150 + i * 55 - 15) * 2, 80, 38));
        QCOMPARE(before.copy(98, 980, 984, 60), after.copy(98, 980, 984, 60));
        QCOMPARE(before.copy(90, 1146, 1010, 10), after.copy(90, 1146, 1010, 10));
        QCOMPARE(after.pixelColor(105, 1000), QColor(245, 245, 245));
        const auto exported = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}});
        QVERIFY2(exported["saved"].toBool(), qUtf8Printable(exported["error"].toString()));
        const auto saved = inspect(output); const auto text = saved["pages"].toArray()[0].toObject()["text"].toString();
        QVERIFY(text.contains(QStringLiteral("中文译文"))); QVERIFY(!text.contains("terms of the license"));
        // Reopen the saved PDF too: isolated list markers can acquire whitespace
        // in PDFium's reading order, but their glyphs must stay at the same place.
        const auto reopened = dir.filePath("reopened.png"), reopenedCopy = dir.filePath("reopened-copy.png");
        const auto reopenedResult = PdfEngine::execute({{"action", "render"}, {"source", output}, {"document", saved}, {"page", 0}, {"pixels", 1190}, {"original", reopened}, {"translated", reopenedCopy}});
        QVERIFY2(!reopenedResult.contains("error"), qUtf8Printable(reopenedResult["error"].toString()));
        const QImage exportedImage(reopened); QVERIFY(!exportedImage.isNull());
        for (int i = 0; i < 5; ++i) QCOMPARE(before.copy(90, (150 + i * 55 - 15) * 2, 80, 38), exportedImage.copy(90, (150 + i * 55 - 15) * 2, 80, 38));
    }
    void preserveArtworkAndSearchableText() {
        QTemporaryDir dir; const auto source = dir.filePath("sample.pdf"), output = dir.filePath("translated.pdf");
        makePdfFixture(source); QFile sourceFile(source); QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        const auto sourceBytes = sourceFile.readAll(); sourceFile.close();
        auto doc = inspect(source); QVERIFY2(!doc.contains("error"), qUtf8Printable(doc["error"].toString())); QCOMPARE(doc["pages"].toArray().size(), 2);
        const auto first = doc["pages"].toArray()[0].toObject();
        QVERIFY2(first["warning"].toString().isEmpty(), qUtf8Printable(first["warning"].toString()));
        QCOMPARE(first["blocks"].toArray().size(), 4);
        doc = translated(doc);
        const auto original = dir.filePath("original.png"), rendered = dir.filePath("rendered.png");
        auto result = PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", 0}, {"pixels", 1190}, {"original", original}, {"translated", rendered}});
        QVERIFY2(!result.contains("error"), qUtf8Printable(result["error"].toString()));
        QCOMPARE(result["statuses"].toArray().size(), 3);
        for (const auto &s : result["statuses"].toArray()) QVERIFY2(s.toObject()["fits"].toBool(), qUtf8Printable(s.toObject()["error"].toString()));
        QImage before(original), after(rendered); QVERIFY(!before.isNull()); QVERIFY(before != after);
        const auto titleTop = [](const QImage &image) {
            for (int y = 0; y < 180; ++y) for (int x = 0; x < image.width(); ++x) {
                const auto c = image.pixelColor(x, y);
                if (c.blue() > c.red() * 1.5 && c.blue() > c.green() * 1.2 && c.red() < 100) return y;
            }
            return -1;
        };
        QVERIFY(titleTop(before) >= 0); QVERIFY(qAbs(titleTop(before) - titleTop(after)) <= 2);
        // Untranslated artwork and separators must retain their actual pixels.
        QCOMPARE(before.copy(0, 420, before.width(), 400), after.copy(0, 420, after.width(), 400));
        result = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}});
        QVERIFY2(result["saved"].toBool(), qUtf8Printable(result["error"].toString()));
        const auto saved = inspect(output); QVERIFY2(!saved.contains("error"), qUtf8Printable(saved["error"].toString()));
        QString allText; for (const auto &p : saved["pages"].toArray()) allText += p.toObject()["text"].toString();
        QVERIFY2(allText.contains(QStringLiteral("示例文档")), qUtf8Printable(QJsonDocument(saved).toJson()));
        QVERIFY(allText.contains(QStringLiteral("第二段"))); QVERIFY(!allText.contains("Sample document"));
        allText.remove(QRegularExpression(QStringLiteral("\\s")));
        QVERIFY(allText.contains(QStringLiteral("本文档保留图片、标题与水平分隔线。请将整个段落翻译成中文。")));
        QVERIFY(allText.contains(QStringLiteral("第二段在页面上有独立的位置。译文应保持在分隔线上方。")));
        for (const auto ch : allText.toUcs4()) QVERIFY(ch < 0x2e80 || ch > 0x2fdf);
        QFile file(source); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), sourceBytes);
        const auto artifacts = qEnvironmentVariable("YIYI_PDF_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
            QDir().mkpath(artifacts);
            for (const auto &path : {source, output, original, rendered}) QFile::copy(path, QDir(artifacts).filePath(QFileInfo(path).fileName()));
            QFile manifest(QDir(artifacts).filePath("manifest.json")); if (manifest.open(QIODevice::WriteOnly)) manifest.write(QJsonDocument(doc).toJson());
        }
    }
    void offPageCodeDoesNotExpandTranslationArea() {
        QTemporaryDir dir; const auto source = dir.filePath("off-page-code.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(320, 260), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter painter(&writer); QFont font("Arial"); font.setPixelSize(12); painter.setFont(font);
            painter.drawText(QPointF(30, 50), "A paragraph above the code.");
            font.setFamily("Courier New"); painter.setFont(font);
            painter.drawText(QPointF(30, 220), "const auto value = call_with_a_very_long_identifier_that_extends_past_the_paper();");
        }
        const auto doc = inspect(source); QVERIFY2(!doc.contains("error"), qUtf8Printable(doc["error"].toString()));
        const auto page = doc["pages"].toArray().first().toObject();
        const QRectF bounds(0, 0, page["width"].toDouble(), page["height"].toDouble());
        bool offPage = false, paragraph = false;
        for (const auto &v : page["blocks"].toArray()) {
            const auto b = v.toObject(); offPage |= PdfLayout::rect(b["rect"]).right() > bounds.right() + 20;
            if (!b["source"].toString().contains("paragraph")) continue;
            paragraph = true;
            QVERIFY(bounds.contains(PdfLayout::rect(b["region"])));
            QVERIFY(bounds.contains(PdfLayout::rect(b["containerRect"])));
        }
        QVERIFY(offPage); QVERIFY2(paragraph, qUtf8Printable(QJsonDocument(page).toJson()));
        for (const auto &v : page["layout"].toObject()["regions"].toArray())
            QVERIFY(bounds.contains(PdfLayout::rect(v.toObject()["rect"])));
    }
    void cachedTranslationRegionStaysOnPage_data() {
        QTest::addColumn<QRectF>("region");
        QTest::newRow("right") << QRectF(30, 30, 600, 120);
        QTest::newRow("left") << QRectF(-200, 30, 480, 120);
        QTest::newRow("top") << QRectF(30, -200, 260, 350);
        QTest::newRow("bottom") << QRectF(30, 180, 260, 250);
        QTest::newRow("outside") << QRectF(340, 30, 300, 120);
    }
    void cachedTranslationRegionStaysOnPage() {
        QFETCH(QRectF, region); QTemporaryDir dir;
        const auto source = dir.filePath("source.pdf"), output = dir.filePath("translated.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(320, 220), QPageSize::Point)); writer.setPageMargins(QMarginsF());
            QPainter painter(&writer); QFont font("Arial"); font.setPixelSize(12); painter.setFont(font);
            painter.drawText(QPointF(30, 50), "A paragraph to translate.");
        }
        auto doc = inspect(source); QVERIFY2(!doc.contains("error"), qUtf8Printable(doc["error"].toString()));
        auto pages = doc["pages"].toArray(); auto page = pages.first().toObject(); auto blocks = page["blocks"].toArray(); QCOMPARE(blocks.size(), 1);
        const QRectF bounds(0, 0, page["width"].toDouble(), page["height"].toDouble());
        const auto translation = QStringLiteral("译文中的每一个字符都应留在纸张内部，旧任务的区域超过纸张边界时也应完整显示。");
        auto block = blocks.first().toObject(); block["translation"] = translation; block["region"] = PdfLayout::jsonRect(region); block["centered"] = false;
        blocks[0] = block; page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        const auto checked = PdfEngine::execute({{"action", "checkLayout"}, {"source", source}, {"document", doc}, {"pages", QJsonArray{0}}});
        QVERIFY2(!checked.contains("error"), qUtf8Printable(checked["error"].toString()));
        const auto status = checked["pages"].toArray().first().toObject()["statuses"].toArray().first().toObject();
        const auto result = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}});
        if (!bounds.intersects(region)) {
            QVERIFY(!status["fits"].toBool()); QVERIFY(result.contains("error")); QVERIFY(!QFileInfo::exists(output)); return;
        }
        QVERIFY2(status["fits"].toBool(), qUtf8Printable(status["error"].toString()));
        QVERIFY2(result["saved"].toBool(), qUtf8Printable(result["error"].toString()));
        const auto saved = inspect(output); QVERIFY2(!saved.contains("error"), qUtf8Printable(saved["error"].toString()));
        const auto exportedPage = saved["pages"].toArray().first().toObject();
        auto text = exportedPage["text"].toString(); text.remove(QRegularExpression(QStringLiteral("\\s"))); QVERIFY(text.contains(translation));
        // PDFium can extract invisible off-page text. Check the actual glyph
        // rectangles, not just whether the exported string is searchable.
        QVERIFY(!exportedPage["blocks"].toArray().isEmpty());
        for (const auto &v : exportedPage["blocks"].toArray())
            QVERIFY2(bounds.adjusted(-.5, -.5, .5, .5).contains(PdfLayout::rect(v.toObject()["rect"])), qUtf8Printable(QJsonDocument(v.toObject()).toJson()));
    }
    void overflowDoesNotOverwriteDestination() {
        QTemporaryDir dir; const auto source = dir.filePath("source.pdf"), output = dir.filePath("existing.pdf"); makePdfFixture(source, 1);
        auto doc = translated(inspect(source)); auto pages = doc["pages"].toArray(); QVERIFY(!pages.isEmpty());
        auto page = pages[0].toObject(); auto blocks = page["blocks"].toArray(); QVERIFY(!blocks.isEmpty()); auto b = blocks[0].toObject();
        b["translation"] = QStringLiteral("这是超过标题区域的长译文。").repeated(400); blocks[0] = b; page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        { QFile file(output); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("keep existing file"); }
        const auto result = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}});
        QVERIFY(result.contains("error")); QCOMPARE(result["page"].toInt(-1), 0);
        QFile file(output); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("keep existing file")); file.close();
        const auto original = dir.filePath("original.png"), rendered = dir.filePath("rendered.png");
        const auto preview = PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", 0}, {"pixels", 1190}, {"original", original}, {"translated", rendered}});
        QVERIFY(!preview.contains("error")); QVERIFY(!preview["statuses"].toArray().first().toObject()["fits"].toBool());
        QCOMPARE(QImage(original).copy(0, 0, 1190, 180), QImage(rendered).copy(0, 0, 1190, 180));
        b["translation"] = QStringLiteral("调整后的标题"); blocks[0] = b; page["blocks"] = blocks; pages[0] = page; doc["pages"] = pages;
        QVERIFY(PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", output}})["saved"].toBool());
    }
    void sourceChangesAndOverwriteAreRejected() {
        QTemporaryDir dir; const auto source = dir.filePath("source.pdf"); makePdfFixture(source, 1); const auto doc = translated(inspect(source));
        auto result = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", source}}); QVERIFY(result.contains("error"));
        { QFile file(source); QVERIFY(file.open(QIODevice::Append)); file.write("\n% changed\n"); }
        result = PdfEngine::execute({{"action", "export"}, {"source", source}, {"document", doc}, {"destination", dir.filePath("out.pdf")}}); QVERIFY(result["error"].toString().contains(QStringLiteral("变化")));
    }
    void scanPageRemainsOriginal() {
        QTemporaryDir dir; const auto source = dir.filePath("scan.pdf"); makePdfFixture(source, 1, true); const auto doc = inspect(source);
        QVERIFY(!doc.contains("error")); const auto page = doc["pages"].toArray()[0].toObject();
        QVERIFY(page["blocks"].toArray().isEmpty()); QVERIFY(page["warning"].toString().contains(QStringLiteral("扫描")));
    }
    void layoutCompatibilityAndRegionalFallback_data() {
        QTest::addColumn<QString>("layout");
        for (const auto &layout : {"columns", "staggered-columns", "rotated", "overlap", "gray-image", "gray-ellipse", "crossing-panel"}) QTest::newRow(layout) << QString::fromLatin1(layout);
    }
    void layoutCompatibilityAndRegionalFallback() {
        QFETCH(QString, layout); QTemporaryDir dir; const auto source = dir.filePath("complex.pdf");
        {
            QPdfWriter writer(source); writer.setResolution(72); writer.setPageSize(QPageSize(QPageSize::A4));
            writer.setPageMargins(QMarginsF(0, 0, 0, 0)); QPainter painter(&writer);
            QFont font("Times New Roman"); font.setPixelSize(12); painter.setFont(font);
            if (layout == "rotated") { painter.translate(50, 100); painter.rotate(-10); painter.drawText(QPointF(0, 0), "Angled text should remain intact."); }
            else if (layout == "overlap") { painter.fillRect(QRectF(40, 70, 250, 60), Qt::yellow); painter.drawText(QPointF(50, 100), "Text on top of artwork."); }
            else if (layout == "gray-image" || layout == "gray-ellipse" || layout == "crossing-panel") {
                if (layout == "gray-image") {
                    QImage image(250, 60, QImage::Format_RGB32); image.fill(QColor(245, 245, 245)); painter.drawImage(QPointF(40, 70), image);
                } else if (layout == "gray-ellipse") {
                    painter.setPen(Qt::NoPen); painter.setBrush(QColor(245, 245, 245)); painter.drawEllipse(QRectF(40, 70, 250, 60)); painter.setPen(Qt::black);
                } else painter.fillRect(QRectF(80, 70, 250, 60), QColor(245, 245, 245));
                painter.drawText(QPointF(50, 100), "Text overlapping unsupported artwork.");
            }
            else for (int i = 0; i < 4; ++i) {
                painter.drawText(QPointF(50, 100 + i * 16), "Text in the left column.");
                painter.drawText(QPointF(330, 100 + i * 16 + (layout == "columns" ? 0 : 6)), "Text in the right column.");
            }
        }
        const auto doc = inspect(source); QVERIFY2(!doc.contains("error"), qUtf8Printable(doc["error"].toString()));
        const auto page = doc["pages"].toArray().first().toObject();
        if (layout == "rotated") { QVERIFY(!page["warning"].toString().isEmpty()); QVERIFY(page["blocks"].toArray().isEmpty()); }
        else {
            QVERIFY(page["warning"].toString().isEmpty()); QVERIFY(!page["blocks"].toArray().isEmpty());
            const bool protectedRegion = layout == "gray-ellipse" || layout == "crossing-panel";
            for (const auto &v : page["blocks"].toArray()) QCOMPARE(v.toObject()["protected"].toBool(), protectedRegion);
        }
        const auto original = dir.filePath("original.png"), translated = dir.filePath("translated.png");
        const auto result = PdfEngine::execute({{"action", "render"}, {"source", source}, {"document", doc}, {"page", 0}, {"original", original}, {"translated", translated}});
        QVERIFY(!result.contains("error")); QVERIFY(result["statuses"].toArray().isEmpty()); QCOMPARE(QImage(original), QImage(translated));
    }
    void retryPreservesEditedParagraph_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("failure");
        for (const auto &protocol : {QStringLiteral("openai"), QStringLiteral("responses")})
            for (const auto &failure : {QStringLiteral("cancel"), QStringLiteral("disconnect"), QStringLiteral("empty"), QStringLiteral("invalid-character"), QStringLiteral("control-token"), QStringLiteral("plain-control"), QStringLiteral("timeout-marker"), QStringLiteral("unfinished-marker"), QStringLiteral("embedded-marker"), QStringLiteral("unexpected-script"), QStringLiteral("repeated-symbols")})
                QTest::newRow(qPrintable(protocol + '-' + failure)) << protocol << failure;
    }
    void retryPreservesEditedParagraph() {
        QFETCH(QString, protocol); QFETCH(QString, failure);
        QTemporaryDir dir; const auto source = dir.filePath("retry.pdf"); makePdfFixture(source, 1);
        AppController app(dir.path()); PdfController pdf(&app); StreamServer server(protocol);
        QVERIFY(app.saveProvider({{"name", "retry"}, {"protocol", protocol}, {"baseUrl", server.url()}, {"model", "test"}}).isEmpty());
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        QVERIFY(!pdf.blocks().isEmpty()); const auto id = pdf.blocks().first().toMap()["id"].toString();
        const auto edited = QStringLiteral("已校对的标题"); QVERIFY(pdf.editBlock(id, edited)); QVERIFY(pdf.retryBlock(id)); QTRY_VERIFY(server.ready);
        if (failure == "invalid-character" || failure == "control-token" || failure == "plain-control" || failure == "timeout-marker" || failure == "unfinished-marker" || failure == "embedded-marker" || failure == "unexpected-script" || failure == "repeated-symbols") {
            const char32_t invalid = 0x4e800;
            const auto text = QStringLiteral("标题") + (failure == "invalid-character" ? QString::fromUcs4(&invalid, 1)
                : failure == "unfinished-marker" ? QStringLiteral("\u3011\u3010\u3002")
                : failure == "embedded-marker" ? QStringLiteral("\u3011\u3010 中间的内容")
                : failure == "plain-control" ? QStringLiteral(" The_END ")
                : failure == "timeout-marker" ? QStringLiteral("%timeout")
                : failure == "repeated-symbols" ? QStringLiteral("♀♀♂♀")
                : failure == "unexpected-script" ? QString(QChar(0x0ab5)) : QStringLiteral("<|fim_suffix|>"));
            server.sendText(text); server.finish(text);
        } else {
            if (failure != "empty") server.sendText(QStringLiteral("未完成"));
            if (failure == "cancel") pdf.cancel(); else if (failure == "disconnect") server.interrupt(); else server.finish({});
        }
        QTRY_VERIFY(!pdf.busy()); QCOMPARE(pdf.error().isEmpty(), failure == "cancel"); QVERIFY(!app.pdfBusy());
        QCOMPARE(pdf.blocks().first().toMap()["translation"].toString(), edited); QCOMPARE(pdf.completed(), 1);
        server.ready = false; server.request.clear(); QVERIFY(pdf.retryBlock(id)); QTRY_VERIFY(server.ready);
        server.sendText(QStringLiteral("新的完整标题")); server.finish(QStringLiteral("新的完整标题")); QTRY_VERIFY(!pdf.busy());
        QCOMPARE(pdf.blocks().first().toMap()["translation"].toString(), QStringLiteral("新的完整标题")); QVERIFY(pdf.error().isEmpty());
        QCOMPARE(server.requestCount, 2);
    }
    void translationPreservesSourceScriptsAndPairedLabels() {
        QTemporaryDir dir; const auto source = dir.filePath("scripts.pdf"); makePdfFixture(source, 1);
        AppController app(dir.path()); PdfController pdf(&app); StreamServer server("responses");
        QVERIFY(app.saveProvider({{"name", "scripts"}, {"protocol", "responses"}, {"baseUrl", server.url()}, {"model", "test"}}).isEmpty());
        QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
        const auto id = pdf.blocks().first().toMap()["id"].toString();
        QVERIFY(pdf.reviseBlock(id, {{"source", QStringLiteral("Gujarati letter \u0ab5; symbols ♀♀♂♀")}}));
        QVERIFY(pdf.retryBlock(id)); QTRY_VERIFY(server.ready);
        const auto translation = QStringLiteral("古吉拉特字母 \u0ab5：【甲】【乙】；符号 ♀♀♂♀");
        server.sendText(translation); server.finish(translation); QTRY_VERIFY(!pdf.busy());
        QVERIFY2(pdf.error().isEmpty(), qUtf8Printable(pdf.error()));
        QCOMPARE(pdf.blocks().first().toMap()["translation"].toString(), translation);
    }
    void controllerSnapshotCancelResumeAndCache() {
        QTemporaryDir dir; const auto source = dir.filePath("source.pdf"); makePdfFixture(source, 1);
        AppController app(dir.path()); StreamServer server("openai");
        QVERIFY(app.saveProvider({{"name", "pdf-test"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "pdf-model"}, {"apiKey", "pdf-test-only-secret"}, {"reasoningEffort", "low"}}).isEmpty());
        {
            PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000); QCOMPARE(pdf.total(), 3);
            bool pausedOnce = false;
            connect(&pdf, &PdfController::changed, &pdf, [&] {
                if (!pausedOnce && pdf.completed() == 1 && pdf.busy()) { pausedOnce = true; pdf.cancel(); }
            });
            pdf.start(true); QTRY_VERIFY(server.ready); QVERIFY(app.pdfBusy());
            app.translate(); QCOMPARE(server.requestCount, 1);
            server.sendText(QStringLiteral("示例文档一")); server.finish({}); QTRY_COMPARE(pdf.completed(), 1);
            QVERIFY(pausedOnce); QVERIFY(!pdf.busy()); QVERIFY(!app.pdfBusy());
            QTest::qWait(30); QCOMPARE(server.requestCount, 1);
            const auto first = pdf.blocks().first().toMap(); QVERIFY(pdf.editBlock(first["id"].toString(), QStringLiteral("修改后的标题")));
            pdf.setPage(8); QCOMPARE(pdf.page(), 0);
            QVERIFY(!pdf.loadFile(QUrl::fromLocalFile(dir.filePath("wrong.txt")))); QVERIFY(pdf.loaded());
            const QDir cache(dir.filePath("pdf-tasks")); const auto entries = cache.entryList({"*.sqlite"}, QDir::Files); QCOMPARE(entries.size(), 1);
            PdfTaskStore saved; QVERIFY(saved.open(cache.filePath(entries.first()), true));
            QVERIFY(!QJsonDocument(saved.metadata()).toJson().contains("pdf-test-only-secret"));
            QCOMPARE(saved.page(0)["blocks"].toArray()[0].toObject()["translation"].toString(), QStringLiteral("修改后的标题"));
        }
        {
            PdfController pdf(&app); QVERIFY(pdf.loadFile(QUrl::fromLocalFile(source))); QTRY_VERIFY_WITH_TIMEOUT(pdf.loaded(), 20000);
            QCOMPARE(pdf.completed(), 1); QCOMPARE(pdf.blocks().first().toMap()["translation"].toString(), QStringLiteral("修改后的标题"));
            QVERIFY(pdf.taskDescription().contains("pdf-model"));
            app.setTargetLanguage(QStringLiteral("英语")); server.ready = false; server.request.clear();
            pdf.start(); QTRY_VERIFY(server.ready);
            const auto body = QJsonDocument::fromJson(server.request.mid(server.request.indexOf("\r\n\r\n") + 4)).object();
            QCOMPARE(body["model"].toString(), QStringLiteral("pdf-model")); QCOMPARE(body["reasoning_effort"].toString(), QStringLiteral("low"));
            const auto messages = body["messages"].toArray(); QVERIFY(messages.first().toObject()["content"].toString().contains(QStringLiteral("Simplified Chinese")));
            QVERIFY(messages.last().toObject()["content"].toString().startsWith("This"));
            pdf.cancel(); QCOMPARE(pdf.completed(), 1);
            auto draft = app.providerDraft(app.selectedProviderId()); draft["model"] = "changed-model"; QVERIFY(app.saveProvider(draft).isEmpty());
            pdf.start(); QVERIFY(!pdf.busy()); QVERIFY(pdf.error().contains(QStringLiteral("已改变"))); QCOMPARE(pdf.completed(), 1);
            pdf.resetTranslations(); QCOMPARE(pdf.completed(), 0); QVERIFY(pdf.taskDescription().isEmpty());
        }
    }
};
QTEST_MAIN(PdfTests)
#include "tst_pdf.moc"
