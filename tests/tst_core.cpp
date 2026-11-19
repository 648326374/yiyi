#include "aiclient.h"
#include "appcontroller.h"
#include "secretstore.h"
#include "storage.h"
#include "streamserver.h"
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <memory>

class MockServer : public QObject {
public:
    QTcpServer server;
    QByteArray request, body, type = "text/event-stream";
    int status = 200;
    int requestCount = 0;
    int fragmentSize = 3;
    int fragmentInterval = 1;
    bool hang = false;
    explicit MockServer(QObject *parent = nullptr) : QObject(parent) {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            ++requestCount;
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                request += socket->readAll();
                const auto end = request.indexOf("\r\n\r\n");
                if (end < 0 || socket->property("sent").toBool()) return;
                const auto headers = request.left(end).split('\n');
                int length = 0;
                for (const auto &line : headers) if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                if (request.size() < end + 4 + length) return;
                socket->setProperty("sent", true);
                if (hang) return;
                socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n");
                auto offset = std::make_shared<int>(0);
                auto *timer = new QTimer(socket);
                QPointer<QTcpSocket> guard(socket);
                connect(timer, &QTimer::timeout, socket, [this, guard, timer, offset] {
                    if (!guard) { timer->stop(); return; }
                    // Three-byte fragments deliberately split UTF-8 characters and SSE delimiters.
                    guard->write(body.mid(*offset, fragmentSize));
                    *offset += fragmentSize;
                    if (*offset >= body.size()) { timer->stop(); guard->disconnectFromHost(); }
                });
                timer->start(fragmentInterval);
            });
        });
    }
    AiProvider provider(const QString &protocol = QStringLiteral("openai")) const {
        return {"test", "Test model", protocol, QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()), "test-model", "private-test-secret"};
    }
    void openai(const QString &text) {
        body = "data: " + QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"delta", QJsonObject{{"content", text}}}}}}}).toJson(QJsonDocument::Compact)
            + "\r\n\r\ndata: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\r\n\r\ndata: [DONE]\r\n\r\n";
    }
    static QJsonObject responseObject(const QString &text) {
        return {{"id", "resp_test"}, {"object", "response"}, {"status", "completed"}, {"error", QJsonValue::Null},
                {"output", QJsonArray{
                    QJsonObject{{"type", "reasoning"}, {"summary", QJsonArray{QJsonObject{{"type", "summary_text"}, {"text", "Not part of the translation"}}}}},
                    QJsonObject{{"type", "message"}, {"role", "assistant"}, {"content", QJsonArray{QJsonObject{{"type", "output_text"}, {"text", text}, {"annotations", QJsonArray{}}}}}}
                }}};
    }
    static QByteArray event(QJsonObject object, bool omitType = false) {
        const auto type = object.value("type").toString().toUtf8();
        if (omitType) object.remove("type");
        return "event: " + type + "\r\ndata: " + QJsonDocument(object).toJson(QJsonDocument::Compact) + "\r\n\r\n";
    }
    void responses(const QString &text) {
        body = event({{"type", "response.created"}, {"response", QJsonObject{{"id", "resp_test"}, {"status", "in_progress"}}}})
            + event({{"type", "response.reasoning_summary_text.delta"}, {"delta", "Not part of the translation"}})
            + event({{"type", "response.output_text.delta"}, {"delta", text}})
            + event({{"type", "response.output_text.done"}, {"text", text}})
            + event({{"type", "response.output_item.done"}, {"item", responseObject(text).value("output").toArray().last()}})
            + event({{"type", "response.completed"}, {"response", responseObject(text)}});
    }
};

class CoreTests : public QObject {
    Q_OBJECT
private slots:
    void generationTimeoutPolicy() {
        StreamServer server("openai"); AiClient client;
        const AiProvider provider{"test", "Timeout test", "openai", server.url(), "gpt-6-luna", {}, "medium"};
        QCOMPARE(client.inactivityTimeout(), 600000);
        client.start(provider, "system", "input"); QTRY_VERIFY(server.ready); QCOMPARE(client.activeTimeout(), 600000);
        client.setInactivityTimeout(1); QCOMPARE(client.inactivityTimeout(), 600000); // Active requests keep their policy.
        client.cancel(); server.ready = false; server.request.clear();
        client.setInactivityTimeout(1200000); client.start(provider, "system", "input", true);
        QTRY_VERIFY(server.ready); QCOMPARE(client.activeTimeout(), 90000); client.cancel();
    }
    void inactivityTimeout_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("partial");
        for (const auto &protocol : {QStringLiteral("openai"), QStringLiteral("responses")})
            for (const auto partial : {false, true}) QTest::newRow(qPrintable(protocol + (partial ? "-partial" : "-silent"))) << protocol << partial;
    }
    void inactivityTimeout() {
        QFETCH(QString, protocol); QFETCH(bool, partial); StreamServer server(protocol); AiClient client;
        client.setInactivityTimeout(350); QSignalSpy completed(&client, &AiClient::completed), failed(&client, &AiClient::failed);
        const AiProvider provider{"test", "Timeout test", protocol, server.url(), "gpt-6-luna", {}, "medium"};
        client.start(provider, "system", "input"); QTRY_VERIFY(server.ready);
        if (partial) server.sendText("Incomplete");
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 2000); QVERIFY(completed.isEmpty());
        QVERIFY(failed.first().first().toString().startsWith(partial ? QStringLiteral("接收响应超时") : QStringLiteral("等待服务响应超时")));
        QVERIFY(client.requestStatus().isEmpty()); QTest::qWait(400); QCOMPARE(failed.size(), 1);
    }
    void streamActivityExtendsWait_data() {
        QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QStringLiteral("openai"); QTest::newRow("responses") << QStringLiteral("responses");
    }
    void streamActivityExtendsWait() {
        QFETCH(QString, protocol); StreamServer server(protocol); AiClient client; client.setInactivityTimeout(450);
        QSignalSpy completed(&client, &AiClient::completed), failed(&client, &AiClient::failed), delta(&client, &AiClient::delta);
        client.start({"test", "Timeout test", protocol, server.url(), "gpt-6-luna", {}, "medium"}, "system", "input"); QTRY_VERIFY(server.ready);
        for (int i = 0; i < 4; ++i) {
            server.socket->write(": keep-alive\n\n"); server.socket->flush(); QTest::qWait(200); QVERIFY(failed.isEmpty());
        }
        const auto reasoning = protocol == "responses" ? QJsonObject{{"type", "response.reasoning_summary_text.delta"}, {"delta", "Internal reasoning"}}
            : QJsonObject{{"choices", QJsonArray{QJsonObject{{"delta", QJsonObject{{"reasoning_content", "Internal reasoning"}}}}}}};
        server.socket->write("data: " + QJsonDocument(reasoning).toJson(QJsonDocument::Compact) + "\n\n"); server.socket->flush(); QTest::qWait(200);
        QVERIFY(failed.isEmpty()); QVERIFY(delta.isEmpty()); QVERIFY(client.requestStatus().contains(QStringLiteral("等待正文")));
        server.sendText("Complete translation"); server.finish("Complete translation"); QTRY_COMPARE(completed.size(), 1); QVERIFY(failed.isEmpty());
        QCOMPARE(completed.first().first().toString(), QStringLiteral("Complete translation"));
    }
    void completionWithoutDisconnect_data() { streamActivityExtendsWait_data(); }
    void completionWithoutDisconnect() {
        QFETCH(QString, protocol); StreamServer server(protocol); AiClient client; client.setInactivityTimeout(350);
        QSignalSpy completed(&client, &AiClient::completed), failed(&client, &AiClient::failed);
        client.start({"test", "Timeout test", protocol, server.url(), "gpt-6-luna", {}, "medium"}, "system", "input"); QTRY_VERIFY(server.ready);
        server.sendText("Complete translation"); server.finish("Complete translation", false);
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 1000); QTest::qWait(400); QCOMPARE(completed.size(), 1); QVERIFY(failed.isEmpty());
    }
    void cancelClearsTimeoutBeforeRestart() {
        StreamServer server("openai"); AiClient client; client.setInactivityTimeout(350);
        const AiProvider provider{"test", "Timeout test", "openai", server.url(), "gpt-6-luna", {}, "medium"};
        QSignalSpy completed(&client, &AiClient::completed), failed(&client, &AiClient::failed);
        client.start(provider, "system", "input"); QTRY_VERIFY(server.ready); client.cancel();
        QTest::qWait(400); QVERIFY(failed.isEmpty()); QVERIFY(completed.isEmpty());
        server.ready = false; server.request.clear(); client.start(provider, "system", "input"); QTRY_VERIFY(server.ready);
        server.sendText("New request"); server.finish("New request"); QTRY_COMPARE(completed.size(), 1); QTest::qWait(400);
        QVERIFY(failed.isEmpty()); QCOMPARE(completed.first().first().toString(), QStringLiteral("New request"));
    }
    void serverTimeoutIsDistinct_data() {
        QTest::addColumn<int>("status"); for (int status : {408, 504, 524}) QTest::newRow(qPrintable(QString::number(status))) << status;
    }
    void serverTimeoutIsDistinct() {
        QFETCH(int, status); MockServer server; server.status = status; server.type = "application/json";
        server.body = "{\"error\":{\"message\":\"Upstream timed out private-test-secret\"}}";
        AiClient client; QSignalSpy failed(&client, &AiClient::failed); client.start(server.provider(), "system", "input"); QTRY_COMPARE(failed.size(), 1);
        const auto message = failed.first().first().toString(); QVERIFY(message.contains(QStringLiteral("服务端或网关")));
        QVERIFY(message.contains(QString::number(status))); QVERIFY(!message.contains("private-test-secret"));
    }
    void imageProtocols_data() {
        QTest::addColumn<QString>("protocol");
        for (auto value : {"openai", "responses", "ollama", "anthropic", "gemini"}) QTest::newRow(value) << QString::fromLatin1(value);
    }
    void imageProtocols() {
        QFETCH(QString, protocol);
        MockServer mock; mock.type = "application/json"; mock.fragmentSize = 4096;
        if (protocol == "responses") mock.body = QJsonDocument(MockServer::responseObject("OK")).toJson();
        else if (protocol == "ollama") mock.body = "{\"message\":{\"content\":\"OK\"},\"done\":true}\n";
        else if (protocol == "anthropic") mock.body = "{\"content\":[{\"type\":\"text\",\"text\":\"OK\"}]}";
        else if (protocol == "gemini") mock.body = "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"OK\"}]},\"finishReason\":\"STOP\"}]}";
        else mock.body = "{\"choices\":[{\"message\":{\"content\":\"OK\"},\"finish_reason\":\"stop\"}]}";
        QImage image(40, 24, QImage::Format_RGB32); image.fill(QColor(30, 80, 140)); image.setPixelColor(3, 4, Qt::red);
        auto provider = mock.provider(protocol); if (AiClient::supportsReasoningEffort(protocol)) provider.reasoningEffort = "medium";
        AiClient client; QSignalSpy done(&client, &AiClient::completed), failed(&client, &AiClient::failed);
        client.startImage(provider, "System", "Read this", image);
        QTRY_COMPARE(done.size() + failed.size(), 1); QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qUtf8Printable(failed.first().first().toString()));
        const auto body = QJsonDocument::fromJson(mock.request.mid(mock.request.indexOf("\r\n\r\n") + 4)).object();
        QString encoded;
        if (protocol == "responses") {
            const auto parts = body["input"].toArray().first().toObject()["content"].toArray();
            QCOMPARE(parts[0].toObject()["type"].toString(), QStringLiteral("input_text")); QCOMPARE(parts[1].toObject()["type"].toString(), QStringLiteral("input_image"));
            encoded = parts[1].toObject()["image_url"].toString().section(',', 1); QCOMPARE(body["reasoning"].toObject()["effort"].toString(), QStringLiteral("medium")); QVERIFY(!body["store"].toBool());
        } else if (protocol == "gemini") {
            const auto part = body["contents"].toArray()[0].toObject()["parts"].toArray()[1].toObject()["inlineData"].toObject();
            QCOMPARE(part["mimeType"].toString(), QStringLiteral("image/png")); encoded = part["data"].toString();
        } else {
            const auto user = body["messages"].toArray().last().toObject();
            if (protocol == "ollama") { QCOMPARE(user["content"].toString(), QStringLiteral("Read this")); encoded = user["images"].toArray()[0].toString(); }
            else if (protocol == "anthropic") {
                const auto source = user["content"].toArray()[0].toObject()["source"].toObject();
                QCOMPARE(source["media_type"].toString(), QStringLiteral("image/png")); encoded = source["data"].toString();
            } else {
                encoded = user["content"].toArray()[1].toObject()["image_url"].toObject()["url"].toString().section(',', 1);
                QCOMPARE(body["reasoning_effort"].toString(), QStringLiteral("medium"));
            }
        }
        const auto decoded = QImage::fromData(QByteArray::fromBase64(encoded.toLatin1()), "PNG");
        QCOMPARE(decoded.size(), image.size()); QCOMPARE(decoded.pixelColor(3, 4), QColor(Qt::red)); QCOMPARE(decoded.pixelColor(8, 8), image.pixelColor(8, 8));
        QCOMPARE(mock.requestCount, 1);
    }
    void imageValidationAndRedaction() {
        MockServer mock; AiClient client; QSignalSpy failed(&client, &AiClient::failed);
        client.startImage(mock.provider(), "", "", {}); QCOMPARE(failed.size(), 1);
        QImage large(4097, 1, QImage::Format_RGB32); client.startImage(mock.provider(), "", "", large); QCOMPARE(failed.size(), 2); QCOMPARE(mock.requestCount, 0);
        mock.status = 400; mock.type = "application/json"; mock.fragmentSize = 8192;
        const QString imageData = QStringLiteral("data:image/png;base64,") + QString(200, 'A');
        mock.body = QJsonDocument(QJsonObject{{"error", QJsonObject{{"message", "bad image " + imageData + " private-test-secret"}}}}).toJson();
        QImage image(10,10,QImage::Format_RGB32); image.fill(Qt::white); client.startImage(mock.provider(), "", "", image);
        QTRY_COMPARE(failed.size(), 3); const auto message = failed.last().first().toString(); QVERIFY(!message.contains(imageData)); QVERIFY(!message.contains("private-test-secret")); QVERIFY(!message.contains(QString(80, 'A')));
    }
    void endpointNormalization() {
        AiProvider p{"", "test", "openai", "http://localhost:1234", "model", ""};
        QCOMPARE(AiClient::endpoint(p).path(), QStringLiteral("/v1/chat/completions"));
        p.baseUrl += QStringLiteral("/v1/");
        QCOMPARE(AiClient::endpoint(p).path(), QStringLiteral("/v1/chat/completions"));
        p.baseUrl = QStringLiteral("https://host/compatible-mode/v1");
        QCOMPARE(AiClient::endpoint(p).path(), QStringLiteral("/compatible-mode/v1/chat/completions"));
        p.baseUrl = QStringLiteral("https://host/v1/messages/"); p.protocol = QStringLiteral("anthropic");
        QCOMPARE(AiClient::endpoint(p).path(), QStringLiteral("/v1/messages"));
        p.baseUrl = QStringLiteral("http://localhost:11434/api"); p.protocol = QStringLiteral("ollama");
        QCOMPARE(AiClient::endpoint(p).path(), QStringLiteral("/api/chat"));
        p.baseUrl = QStringLiteral("https://host/v1beta"); p.protocol = QStringLiteral("gemini");
        QCOMPARE(AiClient::endpoint(p).path(), QStringLiteral("/v1beta/models/model:streamGenerateContent"));
        QCOMPARE(AiClient::endpoint(p).query(), QStringLiteral("alt=sse"));
        p.baseUrl = QStringLiteral("https://host/v1?key=secret");
        QVERIFY(!AiClient::validate(p).isEmpty());
    }

    void responsesEndpoints_data() {
        QTest::addColumn<QString>("base"); QTest::addColumn<QString>("expected");
        QTest::newRow("host") << QStringLiteral("http://localhost:3000") << QStringLiteral("/v1/responses");
        QTest::newRow("v1") << QStringLiteral("http://localhost:3000/v1") << QStringLiteral("/v1/responses");
        QTest::newRow("v1 trailing slash") << QStringLiteral("http://localhost:3000/v1/") << QStringLiteral("/v1/responses");
        QTest::newRow("full endpoint") << QStringLiteral("http://localhost:3000/v1/responses/") << QStringLiteral("/v1/responses");
        QTest::newRow("custom prefix") << QStringLiteral("https://host/proxy/api/v1") << QStringLiteral("/proxy/api/v1/responses");
        QTest::newRow("switch from full chat URL") << QStringLiteral("https://host/v1/chat/completions") << QStringLiteral("/v1/responses");
        QTest::newRow("root response endpoint") << QStringLiteral("https://host/responses") << QStringLiteral("/responses");
    }

    void responsesEndpoints() {
        QFETCH(QString, base); QFETCH(QString, expected);
        AiProvider provider{"test", "Responses", "responses", base, "gpt-6-luna", ""};
        QVERIFY(AiClient::validate(provider).isEmpty());
        QCOMPARE(AiClient::endpoint(provider).path(), expected);
        provider.protocol = QStringLiteral("openai");
        provider.baseUrl = QStringLiteral("https://host/v1/responses");
        QCOMPARE(AiClient::endpoint(provider).path(), QStringLiteral("/v1/chat/completions"));
    }

    void fragmentedStreaming_data() {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("path");
        const auto text = QStringLiteral("你好 🌿");
        MockServer sample; sample.openai(text);
        QTest::newRow("OpenAI SSE") << QStringLiteral("openai") << sample.body << QByteArray("/v1/chat/completions");
        sample.responses(text);
        QTest::newRow("Responses SSE") << QStringLiteral("responses") << sample.body << QByteArray("/v1/responses");
        QTest::newRow("Ollama NDJSON") << QStringLiteral("ollama") << QByteArray("{\"message\":{\"content\":\"") + text.toUtf8() + "\"},\"done\":false}\n{\"message\":{\"content\":\"\"},\"done\":true}\n" << QByteArray("/api/chat");
        QTest::newRow("Anthropic SSE") << QStringLiteral("anthropic") << QByteArray("event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"") + text.toUtf8() + "\"}}\n\nevent: message_stop\ndata: {\"type\":\"message_stop\"}\n\n" << QByteArray("/v1/messages");
        QTest::newRow("Gemini SSE") << QStringLiteral("gemini") << QByteArray("data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"") + text.toUtf8() + "\"}]},\"finishReason\":\"STOP\"}]}\n\n" << QByteArray("/v1beta/models/test-model:streamGenerateContent?alt=sse");
    }

    void fragmentedStreaming() {
        QFETCH(QString, protocol); QFETCH(QByteArray, body); QFETCH(QByteArray, path);
        MockServer mock; mock.body = body;
        if (protocol == QLatin1String("ollama")) mock.type = "application/x-ndjson";
        AiClient client;
        QSignalSpy completed(&client, &AiClient::completed), failed(&client, &AiClient::failed), delta(&client, &AiClient::delta);
        client.start(mock.provider(protocol), QStringLiteral("System instruction"), QStringLiteral("Input"));
        QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty() || !failed.isEmpty(), 10000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(completed.size(), 1);
        QCOMPARE(completed.first().first().toString(), QStringLiteral("你好 🌿"));
        QCOMPARE(delta.size(), 1);
        QVERIFY(mock.request.startsWith("POST " + path + " HTTP/1.1"));
        const auto headers = mock.request.left(mock.request.indexOf("\r\n\r\n")).toLower();
        QVERIFY(headers.contains(protocol == QLatin1String("anthropic") ? "x-api-key: private-test-secret" : protocol == QLatin1String("gemini") ? "x-goog-api-key: private-test-secret" : "authorization: bearer private-test-secret"));
        const auto payload = QJsonDocument::fromJson(mock.request.mid(mock.request.indexOf("\r\n\r\n") + 4)).object();
        QVERIFY(!payload.isEmpty());
        if (protocol == QLatin1String("gemini")) QCOMPARE(payload.value("contents").toArray().first().toObject().value("parts").toArray().first().toObject().value("text").toString(), QStringLiteral("Input"));
        else QCOMPARE(payload.value("model").toString(), QStringLiteral("test-model"));
        if (protocol == QLatin1String("responses")) {
            QCOMPARE(payload.value("instructions").toString(), QStringLiteral("System instruction"));
            QVERIFY(payload.value("stream").toBool());
            QVERIFY(payload.contains("store")); QVERIFY(!payload.value("store").toBool());
            QVERIFY(!payload.contains("messages"));
            const auto message = payload.value("input").toArray().first().toObject();
            QCOMPARE(message.value("role").toString(), QStringLiteral("user"));
            const auto part = message.value("content").toArray().first().toObject();
            QCOMPARE(part.value("type").toString(), QStringLiteral("input_text"));
            QCOMPARE(part.value("text").toString(), QStringLiteral("Input"));
        }
    }

    void reasoningEffortIsOptIn_data() {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<QString>("effort");
        for (const auto protocol : {"openai", "responses"})
            for (const auto effort : {"", "none", "low", "medium", "high", "xhigh", "max"})
                QTest::newRow(qPrintable(QStringLiteral("%1 / %2").arg(QLatin1String(protocol), *effort ? QLatin1String(effort) : QLatin1String("server default"))))
                    << QString::fromLatin1(protocol) << QString::fromLatin1(effort);
        for (const auto protocol : {"ollama", "anthropic", "gemini"})
            QTest::newRow(qPrintable(QStringLiteral("%1 ignores OpenAI effort").arg(QLatin1String(protocol))))
                << QString::fromLatin1(protocol) << QStringLiteral("low");
    }

    void reasoningEffortIsOptIn() {
        QFETCH(QString, protocol); QFETCH(QString, effort);
        MockServer mock;
        mock.type = "application/json";
        if (protocol == QLatin1String("responses")) mock.body = QJsonDocument(MockServer::responseObject("OK")).toJson(QJsonDocument::Compact);
        else if (protocol == QLatin1String("ollama")) mock.body = "{\"message\":{\"content\":\"OK\"},\"done\":true}\n";
        else if (protocol == QLatin1String("anthropic")) mock.body = "{\"content\":[{\"type\":\"text\",\"text\":\"OK\"}]}";
        else if (protocol == QLatin1String("gemini")) mock.body = "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"OK\"}]},\"finishReason\":\"STOP\"}]}";
        else mock.body = "{\"choices\":[{\"message\":{\"content\":\"OK\"},\"finish_reason\":\"stop\"}]}";
        auto provider = mock.provider(protocol);
        provider.model = QStringLiteral("gpt-6-luna"); // A familiar name must not enable reasoning settings automatically.
        provider.reasoningEffort = effort;
        AiClient client; QSignalSpy completed(&client, &AiClient::completed), failed(&client, &AiClient::failed);
        client.start(provider, "system", "input");
        QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty() || !failed.isEmpty(), 5000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(completed.size(), 1); QCOMPARE(mock.requestCount, 1);
        const auto payload = QJsonDocument::fromJson(mock.request.mid(mock.request.indexOf("\r\n\r\n") + 4)).object();
        if (protocol == QLatin1String("responses") && !effort.isEmpty()) {
            QCOMPARE(payload.value("reasoning").toObject(), (QJsonObject{{"effort", effort}}));
            QVERIFY(!payload.contains("reasoning_effort"));
        } else if (protocol == QLatin1String("openai") && !effort.isEmpty()) {
            QCOMPARE(payload.value("reasoning_effort").toString(), effort);
            QVERIFY(!payload.contains("reasoning"));
        } else {
            QVERIFY(!payload.contains("reasoning"));
            QVERIFY(!payload.contains("reasoning_effort"));
        }
    }

    void invalidReasoningDoesNotSendRequest() {
        MockServer mock;
        auto provider = mock.provider(); provider.reasoningEffort = QStringLiteral("automatic");
        AiClient client; QSignalSpy failed(&client, &AiClient::failed);
        client.start(provider, "system", "input");
        QCOMPARE(failed.size(), 1);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("思考等级")));
        QCOMPARE(mock.requestCount, 0);
    }

    void reasoningRejectionIsActionable_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("effort");
        QTest::addColumn<int>("status"); QTest::addColumn<QByteArray>("body"); QTest::addColumn<bool>("hint");
        QTest::newRow("Responses parameter rejected") << QStringLiteral("responses") << QStringLiteral("low") << 400
            << QByteArray("{\"error\":{\"param\":\"reasoning.effort\",\"message\":\"Unsupported value private-test-secret\"}}") << true;
        QTest::newRow("local chat none rejected") << QStringLiteral("openai") << QStringLiteral("none") << 422
            << QByteArray("{\"error\":\"Unknown field reasoning_effort private-test-secret\"}") << true;
        QTest::newRow("Responses stream error") << QStringLiteral("responses") << QStringLiteral("low") << 200
            << MockServer::event({{"type", "error"}, {"param", "reasoning"}, {"message", "Unsupported parameter private-test-secret"}}) << true;
        QTest::newRow("Responses failed event") << QStringLiteral("responses") << QStringLiteral("high") << 200
            << MockServer::event({{"type", "response.failed"}, {"response", QJsonObject{{"status", "failed"}, {"error", QJsonObject{{"param", "reasoning.effort"}, {"message", "Unsupported value private-test-secret"}}}}}}) << true;
        QTest::newRow("model error is unrelated") << QStringLiteral("openai") << QStringLiteral("low") << 400
            << QByteArray("{\"error\":{\"param\":\"model\",\"message\":\"Invalid model private-test-secret\"}}") << false;
        QTest::newRow("default is already omitted") << QStringLiteral("responses") << QString() << 400
            << QByteArray("{\"error\":{\"param\":\"reasoning\",\"message\":\"Unsupported parameter private-test-secret\"}}") << false;
    }

    void reasoningRejectionIsActionable() {
        QFETCH(QString, protocol); QFETCH(QString, effort); QFETCH(int, status); QFETCH(QByteArray, body); QFETCH(bool, hint);
        MockServer mock; mock.status = status; mock.body = body;
        if (status != 200) mock.type = "application/json";
        auto provider = mock.provider(protocol); provider.reasoningEffort = effort;
        AiClient client; QSignalSpy failed(&client, &AiClient::failed), completed(&client, &AiClient::completed);
        client.start(provider, "system", "input", true);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000);
        QVERIFY(completed.isEmpty());
        const auto message = failed.first().first().toString();
        QCOMPARE(message.contains(QStringLiteral("跟随服务")), hint);
        QVERIFY(!message.contains(QStringLiteral("private-test-secret")));
        QTest::qWait(100);
        QCOMPARE(mock.requestCount, 1); // Never silently change the setting and resubmit.
    }

    void reasoningPersistenceAndLegacyDefaults() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString legacyId = QStringLiteral("legacy-local");
        QString error;
        const auto secret = SecretStore::protect(QStringLiteral("legacy-secret"), &error); QVERIFY(error.isEmpty());
        {
            QSettings settings(directory.filePath("settings.ini"), QSettings::IniFormat);
            const QVariantList legacy{QVariantMap{{"id", legacyId}, {"name", "Legacy local"}, {"protocol", "openai"}, {"baseUrl", "http://localhost:1234/v1"}, {"model", "local-model"}, {"secret", secret}}};
            settings.setValue("providers", QJsonDocument::fromVariant(legacy).toJson(QJsonDocument::Compact));
            settings.setValue("selectedProvider", legacyId);
        }
        {
            AppController app(directory.path());
            auto draft = app.providerDraft(legacyId);
            QVERIFY(draft.value("reasoningEffort").toString().isEmpty());
            QVERIFY(draft.value("hasKey").toBool());
            draft["reasoningEffort"] = "low";
            QCOMPARE(app.saveProvider(draft), QString());
            draft = app.providerDraft(legacyId); draft["protocol"] = "responses";
            QCOMPARE(app.saveProvider(draft), QString());
            QCOMPARE(app.providerDraft(legacyId).value("reasoningEffort").toString(), QStringLiteral("low"));
            QVERIFY(app.providerDraft(legacyId).value("hasKey").toBool());
            draft = {{"name", "Other local"}, {"protocol", "openai"}, {"baseUrl", "http://localhost:1234/v1"}, {"model", "other-model"}};
            QCOMPARE(app.saveProvider(draft), QString());
        }
        {
            AppController app(directory.path());
            QCOMPARE(app.providers().size(), 2);
            QCOMPARE(app.providerDraft(legacyId).value("reasoningEffort").toString(), QStringLiteral("low"));
            QVERIFY(app.providers().last().toMap().value("reasoningEffort").toString().isEmpty());
            auto draft = app.providerDraft(legacyId); draft["reasoningEffort"] = "none";
            QCOMPARE(app.saveProvider(draft), QString());
            QCOMPARE(app.providerDraft(legacyId).value("reasoningEffort").toString(), QStringLiteral("none"));
            draft["reasoningEffort"] = "";
            QCOMPARE(app.saveProvider(draft), QString());
            QVERIFY(!app.providerDraft(legacyId).contains("reasoningEffort"));
            draft["reasoningEffort"] = "low"; draft["protocol"] = "ollama";
            QCOMPARE(app.saveProvider(draft), QString());
            QVERIFY(!app.providerDraft(legacyId).contains("reasoningEffort"));
            QVERIFY(app.providerDraft(legacyId).value("hasKey").toBool());
        }
    }

    void responsesFinalOutput_data() {
        QTest::addColumn<QByteArray>("body"); QTest::addColumn<QByteArray>("contentType");
        const auto response = MockServer::responseObject(QStringLiteral("完整结果 🌿"));
        const QJsonObject completed{{"type", "response.completed"}, {"response", response}};
        QTest::newRow("plain JSON including null error") << QJsonDocument(response).toJson(QJsonDocument::Compact) << QByteArray("application/json");
        QTest::newRow("completed event without deltas") << MockServer::event(completed) << QByteArray("text/event-stream");
        QTest::newRow("SSE event-name fallback") << MockServer::event(completed, true) << QByteArray("text/event-stream");
    }

    void responsesFinalOutput() {
        QFETCH(QByteArray, body); QFETCH(QByteArray, contentType);
        MockServer mock; mock.body = body; mock.type = contentType;
        AiClient client; QSignalSpy completed(&client, &AiClient::completed), failed(&client, &AiClient::failed), delta(&client, &AiClient::delta);
        client.start(mock.provider(QStringLiteral("responses")), "system", "input");
        QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty() || !failed.isEmpty(), 10000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(completed.size(), 1); QCOMPARE(delta.size(), 1);
        QCOMPARE(completed.first().first().toString(), QStringLiteral("完整结果 🌿"));
    }

    void responsesFailures_data() {
        QTest::addColumn<QByteArray>("body"); QTest::addColumn<QByteArray>("contentType");
        QTest::newRow("failed event") << MockServer::event({{"type", "response.failed"}, {"response", QJsonObject{{"status", "failed"}, {"error", QJsonObject{{"message", "Rejected private-test-secret"}}}}}}) << QByteArray("text/event-stream");
        QTest::newRow("output limit") << MockServer::event({{"type", "response.incomplete"}, {"response", QJsonObject{{"status", "incomplete"}, {"incomplete_details", QJsonObject{{"reason", "max_output_tokens"}}}}}}) << QByteArray("text/event-stream");
        QTest::newRow("top-level error event") << MockServer::event({{"type", "error"}, {"message", "Failed private-test-secret"}}) << QByteArray("text/event-stream");
        QTest::newRow("DONE without response.completed") << MockServer::event({{"type", "response.output_text.delta"}, {"delta", "Partial"}}) + "data: [DONE]\n\n" << QByteArray("text/event-stream");
        QTest::newRow("nonterminal JSON") << QByteArray("{\"object\":\"response\",\"status\":\"in_progress\",\"error\":null,\"output\":[]}") << QByteArray("application/json");
        auto refusal = MockServer::responseObject({});
        refusal["output"] = QJsonArray{QJsonObject{{"type", "message"}, {"role", "assistant"}, {"content", QJsonArray{QJsonObject{{"type", "refusal"}, {"refusal", "Cannot comply"}}}}}};
        QTest::newRow("refusal") << MockServer::event({{"type", "response.completed"}, {"response", refusal}}) << QByteArray("text/event-stream");
    }

    void responsesFailures() {
        QFETCH(QByteArray, body); QFETCH(QByteArray, contentType);
        MockServer mock; mock.body = body; mock.type = contentType;
        AiClient client; QSignalSpy failed(&client, &AiClient::failed), completed(&client, &AiClient::completed);
        client.start(mock.provider(QStringLiteral("responses")), "system", "input");
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
        QVERIFY(completed.isEmpty());
        QVERIFY(!failed.first().first().toString().contains(QStringLiteral("private-test-secret")));
    }

    void responsesConnectionProbe() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        MockServer mock; mock.responses(QStringLiteral("OK"));
        AppController app(directory.path());
        app.testProvider({{"name", "Responses test"}, {"protocol", "responses"}, {"baseUrl", mock.provider().baseUrl}, {"model", "gpt-6-luna"}, {"apiKey", "private-test-secret"}});
        QTRY_VERIFY_WITH_TIMEOUT(!app.testBusy(), 10000);
        QVERIFY2(app.testSuccess(), qPrintable(app.testMessage()));
        QVERIFY(mock.request.startsWith("POST /v1/responses HTTP/1.1"));
        const auto payload = QJsonDocument::fromJson(mock.request.mid(mock.request.indexOf("\r\n\r\n") + 4)).object();
        QCOMPARE(payload.value("model").toString(), QStringLiteral("gpt-6-luna"));
        QVERIFY(!payload.value("instructions").toString().isEmpty());
        QVERIFY(app.providers().isEmpty()); QVERIFY(app.history().isEmpty());
    }

    void failedResponseIsNotStored() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        MockServer mock;
        mock.body = MockServer::event({{"type", "response.output_text.delta"}, {"delta", "Partial"}})
            + MockServer::event({{"type", "response.incomplete"}, {"response", QJsonObject{{"status", "incomplete"}, {"incomplete_details", QJsonObject{{"reason", "max_output_tokens"}}}}}});
        AppController app(directory.path());
        QCOMPARE(app.saveProvider({{"name", "Responses test"}, {"protocol", "responses"}, {"baseUrl", mock.provider().baseUrl}, {"model", "test-model"}}), QString());
        app.setMode(QStringLiteral("translation")); app.setProperty("inputText", QStringLiteral("Translate this sentence.")); app.translate();
        QTRY_VERIFY_WITH_TIMEOUT(!app.busy(), 10000);
        QVERIFY(!app.error().isEmpty()); QVERIFY(app.history().isEmpty()); QVERIFY(!app.canSave());
    }

    void unsupportedChatSuggestsResponses() {
        MockServer mock; mock.status = 500; mock.type = "application/json";
        mock.body = "{\"error\":{\"message\":\"codex channel: /v1/chat/completions endpoint not supported\"}}";
        AiClient client; QSignalSpy failed(&client, &AiClient::failed);
        client.start(mock.provider(), "system", "input");
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("OpenAI Responses")));
    }

    void httpErrorRedactsKey() {
        MockServer mock; mock.status = 401; mock.type = "application/json"; mock.body = "{\"error\":{\"message\":\"Bad key private-test-secret\"}}";
        AiClient client; QSignalSpy failed(&client, &AiClient::failed), completed(&client, &AiClient::completed);
        client.start(mock.provider(), "system", "input");
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
        QVERIFY(completed.isEmpty());
        const auto error = failed.first().first().toString();
        QVERIFY(error.contains(QStringLiteral("401"))); QVERIFY(!error.contains(QStringLiteral("private-test-secret")));
    }

    void cancellationDoesNotComplete_data() {
        QTest::addColumn<QString>("protocol");
        QTest::newRow("Chat Completions") << QStringLiteral("openai");
        QTest::newRow("Responses") << QStringLiteral("responses");
    }

    void cancellationDoesNotComplete() {
        QFETCH(QString, protocol);
        MockServer mock; mock.hang = true;
        AiClient client; QSignalSpy failed(&client, &AiClient::failed), completed(&client, &AiClient::completed);
        client.start(mock.provider(protocol), "system", "input");
        QTRY_VERIFY_WITH_TIMEOUT(!mock.request.isEmpty(), 3000);
        client.cancel(); QTest::qWait(100);
        QVERIFY(failed.isEmpty()); QVERIFY(completed.isEmpty());
    }

    void nonStreamingFallback() {
        MockServer mock; mock.type = "application/json";
        mock.body = "{\"choices\":[{\"message\":{\"content\":\"Hello\"},\"finish_reason\":\"stop\"}]}";
        AiClient client; QSignalSpy completed(&client, &AiClient::completed);
        client.start(mock.provider(), "system", "input");
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10000);
        QCOMPARE(completed.first().first().toString(), QStringLiteral("Hello"));
    }

    void nullErrorIsNotAFailure_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("type");
        const QByteArray chat = "{\"error\":null,\"choices\":[{\"message\":{\"content\":\"Hello\"},\"finish_reason\":\"stop\"}]}";
        QTest::newRow("Chat JSON") << QStringLiteral("openai") << chat << QByteArray("application/json");
        QTest::newRow("Chat SSE") << QStringLiteral("openai") << QByteArray("data: ") + chat + "\n\ndata: [DONE]\n\n" << QByteArray("text/event-stream");
        QTest::newRow("Ollama") << QStringLiteral("ollama")
            << QByteArray("{\"error\":null,\"message\":{\"content\":\"Hello\"},\"done\":true}\n") << QByteArray("application/x-ndjson");
        QTest::newRow("Anthropic") << QStringLiteral("anthropic")
            << QByteArray("{\"error\":null,\"content\":[{\"type\":\"text\",\"text\":\"Hello\"}],\"stop_reason\":\"end_turn\"}") << QByteArray("application/json");
        QTest::newRow("Gemini") << QStringLiteral("gemini")
            << QByteArray("{\"error\":null,\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hello\"}]},\"finishReason\":\"STOP\"}]}") << QByteArray("application/json");
    }

    void nullErrorIsNotAFailure() {
        QFETCH(QString, protocol); QFETCH(QByteArray, body); QFETCH(QByteArray, type);
        MockServer mock; mock.body = body; mock.type = type; mock.fragmentSize = 4096;
        AiClient client; QSignalSpy completed(&client, &AiClient::completed), failed(&client, &AiClient::failed);
        client.start(mock.provider(protocol), "system", "input");
        QTRY_VERIFY(!completed.isEmpty() || !failed.isEmpty());
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(completed.size(), 1); QCOMPARE(completed.first().first().toString(), QStringLiteral("Hello"));
        QCOMPARE(mock.requestCount, 1);
    }

    void responseSizeLimit_data() {
        QTest::addColumn<bool>("stream"); QTest::addColumn<bool>("oversized");
        for (bool stream : {false, true})
            for (bool oversized : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1/%2").arg(stream ? "SSE" : "JSON", oversized ? "over limit" : "at limit")))
                    << stream << oversized;
    }

    void responseSizeLimit() {
        QFETCH(bool, stream); QFETCH(bool, oversized);
        MockServer mock; mock.fragmentSize = 65536; mock.fragmentInterval = 0;
        QByteArray prefix = "{\"choices\":[{\"message\":{\"content\":\"";
        QByteArray suffix = "\"},\"finish_reason\":\"stop\"}]}";
        if (stream) { prefix.prepend("data: "); suffix += "\n\ndata: [DONE]\n\n"; }
        else mock.type = "application/json";
        // SSE framing may be consumed in earlier TCP fragments. Make the text
        // itself exceed the retained-data limit so the assertion is independent
        // of socket coalescing; a raw-wire +1 boundary is not an output limit.
        const int bodySize = 2 * 1024 * 1024 + (oversized ? (stream ? prefix.size() + suffix.size() + 1 : 1) : 0);
        const auto text = QByteArray(bodySize - prefix.size() - suffix.size(), 'x');
        mock.body = prefix + text + suffix;
        AiClient client; QSignalSpy completed(&client, &AiClient::completed), failed(&client, &AiClient::failed);
        client.start(mock.provider(), "system", "input");
        QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty() || !failed.isEmpty(), 10000);
        if (oversized) {
            QVERIFY(completed.isEmpty()); QCOMPARE(failed.size(), 1);
            QVERIFY(failed.first().first().toString().contains(QStringLiteral("过长")));
        } else {
            QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
            QCOMPARE(completed.size(), 1); QCOMPARE(completed.first().first().toString().size(), text.size());
        }
        QCOMPARE(mock.requestCount, 1);
    }

    void geminiAbnormalFinishIsNotStored_data() {
        QTest::addColumn<QString>("reason"); QTest::addColumn<bool>("stream");
        for (const auto reason : {"OTHER", "BLOCKLIST", "PROHIBITED_CONTENT", "SPII", "MALFORMED_FUNCTION_CALL", "SAFETY", "RECITATION", "MAX_TOKENS"})
            for (bool stream : {false, true})
                QTest::newRow(qPrintable(QString::fromLatin1(reason) + (stream ? "/SSE" : "/JSON"))) << QString::fromLatin1(reason) << stream;
    }

    void geminiAbnormalFinishIsNotStored() {
        QFETCH(QString, reason); QFETCH(bool, stream);
        MockServer mock; mock.fragmentSize = 4096;
        const QJsonObject candidate{{"content", QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", "Partial answer"}}}}}}, {"finishReason", reason}};
        const auto body = QJsonDocument(QJsonObject{{"candidates", QJsonArray{candidate}}}).toJson(QJsonDocument::Compact);
        mock.body = stream ? "data: " + body + "\n\n" : body;
        if (!stream) mock.type = "application/json";
        QTemporaryDir directory; QVERIFY(directory.isValid()); AppController app(directory.path());
        QCOMPARE(app.saveProvider({{"name", "Gemini test"}, {"protocol", "gemini"}, {"baseUrl", mock.provider().baseUrl},
            {"model", "test-model"}, {"apiKey", "private-test-secret"}}), QString());
        app.setMode(QStringLiteral("translation")); app.setProperty("inputText", QStringLiteral("Test input")); app.translate();
        QTRY_VERIFY(!app.busy());
        QVERIFY(!app.error().isEmpty()); QVERIFY(app.history().isEmpty()); QVERIFY(!app.canSave());
        QCOMPARE(mock.requestCount, 1);
    }

    void consumedStreamMetadataDoesNotCountAsOutput() {
        MockServer mock; mock.fragmentSize = 65536; mock.fragmentInterval = 0;
        mock.openai(QStringLiteral("Hello"));
        // Streaming metadata is discarded. Its cumulative wire size must not
        // reject a small valid answer or reduce the available translation size.
        mock.body.prepend(QByteArray(": keep-alive\n\n").repeated(170000));
        AiClient client; QSignalSpy completed(&client, &AiClient::completed), failed(&client, &AiClient::failed);
        client.start(mock.provider(), "system", "input");
        QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty() || !failed.isEmpty(), 10000);
        QVERIFY(failed.isEmpty()); QCOMPARE(completed.size(), 1);
        QCOMPARE(completed.first().first().toString(), QStringLiteral("Hello"));
    }

    void interruptedStreamIsNotAccepted() {
        MockServer mock;
        mock.body = "data: {\"choices\":[{\"delta\":{\"content\":\"Incomplete\"}}]}\n\n";
        AiClient client; QSignalSpy failed(&client, &AiClient::failed), completed(&client, &AiClient::completed);
        client.start(mock.provider(), "system", "input");
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
        QVERIFY(completed.isEmpty());
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("不完整")));
    }

    void malformedStreamIsRejected() {
        MockServer mock; mock.body = "data: {not json}\n\ndata: [DONE]\n\n";
        AiClient client; QSignalSpy failed(&client, &AiClient::failed), completed(&client, &AiClient::completed);
        client.start(mock.provider(), "system", "input");
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
        QVERIFY(completed.isEmpty());
    }

    void dictionaryResultIsStoredAsStructuredData_data() {
        QTest::addColumn<QString>("protocol");
        QTest::newRow("Chat Completions") << QStringLiteral("openai");
        QTest::newRow("Responses") << QStringLiteral("responses");
    }

    void dictionaryResultIsStoredAsStructuredData() {
        QFETCH(QString, protocol);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        MockServer mock;
        const auto entry = QStringLiteral("{\"word\":\"book\",\"definitions\":[{\"meaning\":\"书\"}],\"examples\":[{\"source\":\"A book\",\"translation\":\"一本书\"}]}");
        if (protocol == QLatin1String("responses")) mock.responses(entry);
        else mock.openai(entry);
        AppController app(directory.path());
        QVariantMap draft{{"name", "Dictionary test"}, {"protocol", protocol}, {"baseUrl", mock.provider().baseUrl}, {"model", "test-model"}, {"reasoningEffort", "low"}, {"dictionaryMode", "detailed"}};
        QCOMPARE(app.saveProvider(draft), QString());
        app.setMode(QStringLiteral("dictionary")); app.setProperty("inputText", QStringLiteral("book")); app.translate();
        QTRY_VERIFY_WITH_TIMEOUT(!app.busy(), 10000);
        QVERIFY2(app.error().isEmpty(), qPrintable(app.error()));
        QCOMPARE(app.dictionary().value("word").toString(), QStringLiteral("book"));
        QVERIFY(!app.briefResult());
        const auto payload = QJsonDocument::fromJson(mock.request.mid(mock.request.indexOf("\r\n\r\n") + 4)).object();
        QCOMPARE(protocol == QLatin1String("responses") ? payload.value("reasoning").toObject().value("effort").toString()
                                                      : payload.value("reasoning_effort").toString(), QStringLiteral("low"));
        QCOMPARE(app.history().size(), 1);
        QCOMPARE(app.history().first().toMap().value("dictionary").toMap().value("definitions").toList().size(), 1);
        QVERIFY(app.resultText().contains(QStringLiteral("一本书")));
        const auto id = app.history().first().toMap().value("id").toLongLong();
        app.clear(); app.openRecord(id);
        QCOMPARE(app.resultKind(), QStringLiteral("dictionary"));
        QCOMPARE(app.dictionary().value("word").toString(), QStringLiteral("book"));
    }

    void dictionaryPreviewRequiresCompleteItems() {
        const QJsonObject first{{"meaning", QStringLiteral("关系；联系")}, {"detail", QStringLiteral("括号 } ]、引号 \"、路径 \\ 和 😃")}};
        const QJsonObject second{{"meaning", QStringLiteral("恋爱关系")}};
        const QJsonObject example{{"source", "They have a good relationship."}, {"translation", QStringLiteral("他们关系很好。")}};
        const QJsonObject document{{"word", "relationship"}, {"partOfSpeech", "n."},
            {"definitions", QJsonArray{first, second}}, {"examples", QJsonArray{example}}, {"note", QStringLiteral("简短提示")}};
        auto text = QString::fromUtf8(QJsonDocument(document).toJson(QJsonDocument::Compact));
        text.replace(QStringLiteral("😃"), QStringLiteral("\\uD83D\\uDE03"));
        const auto complete = AppController::parseDictionary(text);
        QVERIFY(!complete.isEmpty());
        const auto expectedDefinitions = complete.value("definitions").toList();
        bool sawPreview = false;
        for (qsizetype i = 0; i < text.size(); ++i) {
            const auto preview = AppController::parseDictionaryPreview(text.left(i), QStringLiteral("relationship"));
            if (preview.isEmpty()) continue;
            sawPreview = true;
            const auto definitions = preview.value("definitions").toList();
            QVERIFY(!definitions.isEmpty()); QVERIFY(definitions.size() <= expectedDefinitions.size());
            for (int j = 0; j < definitions.size(); ++j) QCOMPARE(definitions[j], expectedDefinitions[j]);
            const auto examples = preview.value("examples").toList();
            if (!examples.isEmpty()) QCOMPARE(examples, complete.value("examples").toList());
        }
        QVERIFY(sawPreview);
        QCOMPARE(AppController::parseDictionaryPreview(text), complete);
        QCOMPARE(AppController::parseDictionaryPreview(QStringLiteral("```json\n") + text + QStringLiteral("\n```")), complete);
        QVERIFY(AppController::parseDictionaryPreview(QStringLiteral("{\"word\":\"x\",\"definitions\":[{\"meaning\":\"unfinished")).isEmpty());
        QVERIFY(AppController::parseDictionaryPreview(QStringLiteral("{\"word\":\"x\",\"definitions\":[{\"meaning\" \"invalid\"}]")).isEmpty());
    }

    void detailedDictionaryStreamsBeforeCompletion_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("outcome");
        for (const auto protocol : {"openai", "responses"})
            for (const auto outcome : {"complete", "cancel", "interrupt"})
                QTest::newRow(qPrintable(QString::fromLatin1(protocol) + QLatin1Char('/') + QLatin1String(outcome)))
                    << QString::fromLatin1(protocol) << QString::fromLatin1(outcome);
    }

    void detailedDictionaryStreamsBeforeCompletion() {
        QFETCH(QString, protocol); QFETCH(QString, outcome);
        StreamServer mock(protocol); QVERIFY(mock.server.isListening());
        QTemporaryDir directory; QVERIFY(directory.isValid()); AppController app(directory.path());
        QCOMPARE(app.saveProvider({{"name", "Stream test"}, {"protocol", protocol}, {"baseUrl", mock.url()},
            {"model", "gpt-6.1-sol"}, {"reasoningEffort", "medium"}, {"dictionaryMode", "detailed"}}), QString());
        app.setProperty("inputText", QStringLiteral("relationship")); app.translate();
        QVERIFY(app.generationStatus().contains(QStringLiteral("等待模型响应")));
        QVERIFY(app.requestTiming().isEmpty()); QVERIFY(app.dictionary().isEmpty());
        QTRY_VERIFY(mock.ready);
        const auto payload = QJsonDocument::fromJson(mock.request.mid(mock.request.indexOf("\r\n\r\n") + 4)).object();
        QVERIFY(payload.value("stream").toBool());
        QCOMPARE(protocol == QLatin1String("responses") ? payload.value("reasoning").toObject().value("effort").toString()
            : payload.value("reasoning_effort").toString(), QStringLiteral("medium"));
        const auto prefix = QStringLiteral("{\"word\":\"relationship\",\"partOfSpeech\":\"n.\",\"definitions\":[{\"meaning\":\"关系；联系\"}");
        const auto suffix = QStringLiteral("],\"examples\":[{\"source\":\"They have a good relationship.\",\"translation\":\"他们关系很好。\"}]}");
        mock.sendText(prefix);
        QTRY_VERIFY(!app.dictionary().isEmpty());
        QVERIFY(app.busy()); QVERIFY(app.generationStatus().contains(QStringLiteral("补全词条")));
        QCOMPARE(app.dictionary().value("definitions").toList().first().toMap().value("meaning").toString(), QStringLiteral("关系；联系"));
        QVERIFY(app.resultText().isEmpty()); QVERIFY(app.history().isEmpty()); QVERIFY(!app.canSave());
        QVERIFY(app.requestTiming().isEmpty());
        app.setProperty("inputText", QStringLiteral("changed after request")); app.setTargetLanguage(QStringLiteral("英语"));
        auto draft = app.providerDraft(app.selectedProviderId()); draft["dictionaryMode"] = "brief";
        QCOMPARE(app.saveProvider(draft), QString());
        if (outcome == QLatin1String("complete")) { mock.sendText(suffix); mock.finish(prefix + suffix); }
        else if (outcome == QLatin1String("cancel")) app.cancel();
        else mock.interrupt();
        QTRY_VERIFY(!app.busy());
        QVERIFY(app.generationStatus().isEmpty()); QVERIFY(app.requestTiming().contains(QStringLiteral("首段响应")));
        QCOMPARE(mock.requestCount, 1);
        const auto timing = app.requestTiming(); QTest::qWait(150); QCOMPARE(app.requestTiming(), timing);
        if (outcome == QLatin1String("complete")) {
            QVERIFY2(app.error().isEmpty(), qPrintable(app.error()));
            QCOMPARE(app.dictionary(), AppController::parseDictionary(prefix + suffix));
            QVERIFY(app.resultText().contains(QStringLiteral("他们关系很好。"))); QVERIFY(app.canSave());
            QCOMPARE(app.history().size(), 1);
            const auto record = app.history().first().toMap();
            QCOMPARE(record.value("input").toString(), QStringLiteral("relationship"));
            QCOMPARE(record.value("target").toString(), QStringLiteral("简体中文"));
            app.openRecord(record.value("id").toLongLong()); QVERIFY(app.requestTiming().isEmpty());
        } else {
            QVERIFY(!app.error().isEmpty()); QVERIFY(app.dictionary().isEmpty()); QVERIFY(app.resultText().isEmpty());
            QVERIFY(app.history().isEmpty()); QVERIFY(!app.canSave());
        }
        app.clear(); QVERIFY(app.requestTiming().isEmpty()); QVERIFY(app.generationStatus().isEmpty());
    }

    void briefDictionaryUsesPlainText_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("mode");
        for (const auto protocol : {"openai", "responses"})
            for (const auto mode : {"auto", "dictionary"})
                QTest::newRow(qPrintable(QStringLiteral("%1 / %2").arg(QLatin1String(protocol), QLatin1String(mode))))
                    << QString::fromLatin1(protocol) << QString::fromLatin1(mode);
    }

    void briefDictionaryUsesPlainText() {
        QFETCH(QString, protocol); QFETCH(QString, mode);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        MockServer mock;
        const auto answer = QStringLiteral("看; 注视；\r\n观察；");
        if (protocol == QLatin1String("responses")) mock.responses(answer); else mock.openai(answer);
        // Existing profiles have no dictionaryMode. They should get the brief
        // request without needing to reopen and save a provider first.
        {
            QSettings settings(directory.filePath("settings.ini"), QSettings::IniFormat);
            const QVariantList legacy{QVariantMap{{"id", "legacy"}, {"name", "Local"}, {"protocol", protocol},
                {"baseUrl", mock.provider().baseUrl}, {"model", "small-local-model"}}};
            settings.setValue("providers", QJsonDocument::fromVariant(legacy).toJson(QJsonDocument::Compact));
            settings.setValue("selectedProvider", "legacy");
        }
        qint64 id = -1;
        {
            AppController app(directory.path());
            app.setMode(mode); app.setProperty("inputText", QStringLiteral("look")); app.translate();
            // Editing the input/provider during generation must not rename the
            // word or change how the in-flight answer is interpreted.
            app.setProperty("inputText", QStringLiteral("Changed after request"));
            app.setTargetLanguage(QStringLiteral("英语"));
            auto draft = app.providerDraft(app.selectedProviderId()); draft["dictionaryMode"] = "detailed";
            QCOMPARE(app.saveProvider(draft), QString());
            QTRY_VERIFY_WITH_TIMEOUT(!app.busy(), 10000);
            QVERIFY2(app.error().isEmpty(), qPrintable(app.error()));
            QCOMPARE(mock.requestCount, 1);
            QCOMPARE(app.resultKind(), QStringLiteral("dictionary"));
            QCOMPARE(app.dictionary().value("word").toString(), QStringLiteral("look"));
            const auto definitions = app.dictionary().value("definitions").toList();
            QCOMPARE(definitions.size(), 3);
            QCOMPARE(definitions[0].toMap().value("meaning").toString(), QStringLiteral("看"));
            QCOMPARE(definitions[2].toMap().value("meaning").toString(), QStringLiteral("观察"));
            QVERIFY(!app.dictionary().contains("phonetic")); QVERIFY(!app.dictionary().contains("examples"));
            QVERIFY(app.briefResult());
            QCOMPARE(app.resultText(), QStringLiteral("看\n\n注视\n\n观察"));
            const auto payload = QJsonDocument::fromJson(mock.request.mid(mock.request.indexOf("\r\n\r\n") + 4)).object();
            const auto prompt = protocol == QLatin1String("responses") ? payload.value("instructions").toString()
                : payload.value("messages").toArray().first().toObject().value("content").toString();
            QVERIFY(prompt.contains(QStringLiteral("翻译成Simplified Chinese")));
            QVERIFY(!prompt.contains(QLatin1String("definitions")));
            QCOMPARE(app.history().size(), 1);
            QCOMPARE(app.history().first().toMap().value("input").toString(), QStringLiteral("look"));
            QCOMPARE(app.history().first().toMap().value("target").toString(), QStringLiteral("简体中文"));
            app.toggleFavorite(); QVERIFY(app.favorite());
            id = app.history().first().toMap().value("id").toLongLong();
        }
        {
            AppController app(directory.path()); app.openRecord(id);
            QVERIFY(app.favorite()); QCOMPARE(app.resultKind(), QStringLiteral("dictionary"));
            QCOMPARE(app.dictionary().value("word").toString(), QStringLiteral("look"));
            QCOMPARE(app.dictionary().value("definitions").toList().size(), 3);
            QCOMPARE(app.targetLanguage(), QStringLiteral("简体中文"));
            QCOMPARE(app.providerDraft(app.selectedProviderId()).value("dictionaryMode").toString(), QStringLiteral("detailed"));
            QVERIFY(app.briefResult());
            QCOMPARE(app.resultText(), QStringLiteral("看\n\n注视\n\n观察"));
            // Open a 0.1.6 record: the old output included the source term.
            Storage storage(directory.path());
            auto legacy = storage.get(id);
            auto entry = legacy.value("dictionary").toMap(); entry.remove("format");
            legacy["dictionary"] = entry;
            legacy["output"] = QStringLiteral("look\n\n看\n\n注视\n\n观察");
            const auto legacyId = storage.add(legacy); QVERIFY(legacyId > 0);
            app.openRecord(legacyId);
            QVERIFY(app.briefResult());
            QCOMPARE(app.resultText(), QStringLiteral("看\n\n注视\n\n观察"));
        }
    }

    void briefDictionaryWordClasses() {
        auto entry = AppController::parseBriefDictionary(QStringLiteral(
            "look\n词性：v. 动词\n释义：看；注视\n音标：/lʊk/\n例句：Look at me.\n译文：看着我。"), QStringLiteral("look"));
        QCOMPARE(entry.value("word").toString(), QStringLiteral("look"));
        QCOMPARE(entry.value("format").toString(), QStringLiteral("brief"));
        auto definitions = entry.value("definitions").toList();
        QCOMPARE(definitions.size(), 2);
        QCOMPARE(definitions.first().toMap().value("partOfSpeech").toString(), QStringLiteral("v."));
        QCOMPARE(definitions.last().toMap().value("meaning").toString(), QStringLiteral("注视"));
        QVERIFY(!entry.contains("phonetic")); QVERIFY(!entry.contains("examples"));

        entry = AppController::parseBriefDictionary(QStringLiteral("n. 书籍\nv. 预订"), QStringLiteral("book"));
        definitions = entry.value("definitions").toList();
        QCOMPARE(definitions.size(), 2);
        QCOMPARE(definitions.first().toMap().value("partOfSpeech").toString(), QStringLiteral("n."));
        QCOMPARE(definitions.last().toMap().value("partOfSpeech").toString(), QStringLiteral("v."));
        QCOMPARE(definitions.last().toMap().value("meaning").toString(), QStringLiteral("预订"));

        entry = AppController::parseBriefDictionary(QStringLiteral("词性：副词\n释义：迅速地"), QStringLiteral("quickly"));
        QCOMPARE(entry.value("definitions").toList().first().toMap().value("partOfSpeech").toString(), QStringLiteral("adv."));
        entry = AppController::parseBriefDictionary(QStringLiteral("POS: noun\nMEANING: book"), QStringLiteral("书"));
        QCOMPARE(entry.value("definitions").toList().first().toMap().value("partOfSpeech").toString(), QStringLiteral("n."));

        entry = AppController::parseBriefDictionary(QStringLiteral("词性：light.\n释义：光"), QStringLiteral("light"));
        QVERIFY(!entry.value("definitions").toList().first().toMap().contains("partOfSpeech"));
        entry = AppController::parseBriefDictionary(QStringLiteral("POS: n./v./adj./adv.等实际词性\nTRANSLATION: book"), QStringLiteral("书"));
        QVERIFY(!entry.value("definitions").toList().first().toMap().contains("partOfSpeech"));
        entry = AppController::parseBriefDictionary(QStringLiteral("看"), QStringLiteral("look"));
        QCOMPARE(entry.value("definitions").toList().first().toMap().value("meaning").toString(), QStringLiteral("看"));
        QVERIFY(!entry.value("definitions").toList().first().toMap().contains("partOfSpeech"));
        QVERIFY(AppController::parseBriefDictionary(QStringLiteral("词性：v.\n音标：/lʊk/\n例句：Look at me."), QStringLiteral("look")).isEmpty());
    }

    void briefDictionaryEnglishTarget_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("term");
        QTest::addColumn<QString>("answer"); QTest::addColumn<QString>("expected");
        for (const auto protocol : {"openai", "responses"}) {
            const auto row = [&](const char *name, const QString &term, const QString &answer, const QString &expected) {
                QTest::newRow(qPrintable(QString::fromLatin1(protocol) + QLatin1Char('/') + QLatin1String(name)))
                    << QString::fromLatin1(protocol) << term << answer << expected;
            };
            row("labeled", QStringLiteral("有趣的"), QStringLiteral("POS: adj.\nTRANSLATION: interesting; intriguing"),
                QStringLiteral("adj. interesting\n\nadj. intriguing"));
            row("echoed source", QStringLiteral("有趣的"), QStringLiteral("词性：adj.\n有趣的: interesting"), QStringLiteral("adj. interesting"));
            row("plain", QStringLiteral("有趣的"), QStringLiteral("interesting"), QStringLiteral("interesting"));
            row("accented Latin", QStringLiteral("咖啡馆"), QStringLiteral("POS: n.\nTRANSLATION: café"), QStringLiteral("n. café"));
            row("Chinese only", QStringLiteral("漂亮的"), QStringLiteral("词性：adj.\n释义：美丽、漂亮"), QString());
            row("mixed lines", QStringLiteral("有趣的"), QStringLiteral("有趣的: funny\n幽默的/滑稽的"), QString());
            row("mixed inline", QStringLiteral("有趣的"), QStringLiteral("POS: adj.\nTRANSLATION: funny（有趣的）"), QString());
            row("no meaning", QStringLiteral("有趣的"), QStringLiteral("POS: adj.\nNOTE: no translation"), QString());
        }
    }

    void briefDictionaryEnglishTarget() {
        QFETCH(QString, protocol); QFETCH(QString, term); QFETCH(QString, answer); QFETCH(QString, expected);
        QTemporaryDir directory; QVERIFY(directory.isValid()); MockServer mock;
        if (protocol == QLatin1String("responses")) mock.responses(answer); else mock.openai(answer);
        AppController app(directory.path());
        QCOMPARE(app.saveProvider({{"name", "Local"}, {"protocol", protocol}, {"baseUrl", mock.provider().baseUrl}, {"model", "small"}}), QString());
        app.setSourceLanguage(QStringLiteral("简体中文")); app.setTargetLanguage(QStringLiteral("英语"));
        app.setProperty("inputText", term); app.translate();
        // Validation, prompt and history must use the original direction.
        app.setTargetLanguage(QStringLiteral("简体中文"));
        QTRY_VERIFY_WITH_TIMEOUT(!app.busy(), 10000);
        QCOMPARE(mock.requestCount, 1);
        const auto payload = QJsonDocument::fromJson(mock.request.mid(mock.request.indexOf("\r\n\r\n") + 4)).object();
        const auto prompt = protocol == QLatin1String("responses") ? payload.value("instructions").toString()
            : payload.value("messages").toArray().first().toObject().value("content").toString();
        QVERIFY(prompt.contains(QStringLiteral("翻译成English")));
        QVERIFY(prompt.contains(QStringLiteral("Source language: Simplified Chinese")));
        QCOMPARE(app.resultText(), expected);
        if (expected.isEmpty()) {
            QVERIFY(app.error().contains(QStringLiteral("英文译义")));
            QVERIFY(app.dictionary().isEmpty()); QVERIFY(app.history().isEmpty()); QVERIFY(!app.canSave());
        } else {
            QVERIFY2(app.error().isEmpty(), qPrintable(app.error()));
            QVERIFY(app.briefResult()); QCOMPARE(app.history().size(), 1);
            const auto record = app.history().first().toMap();
            QCOMPARE(record.value("input").toString(), term);
            QCOMPARE(record.value("target").toString(), QStringLiteral("英语"));
            app.toggleFavorite(); app.clear(); app.openRecord(record.value("id").toLongLong());
            QCOMPARE(app.targetLanguage(), QStringLiteral("英语"));
            QCOMPARE(app.resultText(), expected); QVERIFY(app.favorite());
        }
    }

    void briefDictionaryEchoesAndDuplicates() {
        auto entry = AppController::parseBriefDictionary(QStringLiteral("POS: adj.\nTRANSLATION: 有趣的：interesting; interesting"), QStringLiteral("有趣的"));
        auto definitions = entry.value("definitions").toList();
        QCOMPARE(definitions.size(), 1);
        QCOMPARE(definitions.first().toMap().value("meaning").toString(), QStringLiteral("interesting"));
        QCOMPARE(definitions.first().toMap().value("partOfSpeech").toString(), QStringLiteral("adj."));
        // Do not merge distinct casing or different word classes, or strip
        // arbitrary prefixes that merely contain the requested term.
        entry = AppController::parseBriefDictionary(QStringLiteral("US; us\nn. study\nv. study\n好看：good-looking"), QStringLiteral("看"));
        definitions = entry.value("definitions").toList();
        QCOMPARE(definitions.size(), 5);
        QCOMPARE(definitions.last().toMap().value("meaning").toString(), QStringLiteral("好看：good-looking"));
        entry = AppController::parseBriefDictionary(QStringLiteral("C++: 一种编程语言"), QStringLiteral("C++"));
        QCOMPARE(entry.value("definitions").toList().first().toMap().value("meaning").toString(), QStringLiteral("一种编程语言"));
    }

    void untranslatedBriefDictionaryIsNotSaved() {
        QTemporaryDir directory; MockServer mock;
        mock.openai(QStringLiteral("词性：n.\n释义：light"));
        AppController app(directory.path());
        QCOMPARE(app.saveProvider({{"name", "Local"}, {"protocol", "openai"}, {"baseUrl", mock.provider().baseUrl}, {"model", "small"}}), QString());
        app.setProperty("inputText", QStringLiteral("light")); app.translate();
        QTRY_VERIFY_WITH_TIMEOUT(!app.busy(), 5000);
        QVERIFY(app.error().contains(QStringLiteral("没有返回中文释义")));
        QVERIFY(app.dictionary().isEmpty()); QVERIFY(app.history().isEmpty()); QVERIFY(!app.canSave());
        QCOMPARE(mock.requestCount, 1);
    }

    void dictionaryModesAreSavedPerProvider() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QString firstId, secondId;
        {
            AppController app(directory.path());
            QVariantMap draft{{"name", "Brief"}, {"protocol", "openai"}, {"baseUrl", "http://localhost:8080/v1"}, {"model", "small"}};
            QCOMPARE(app.saveProvider(draft), QString()); firstId = app.selectedProviderId();
            QCOMPARE(app.providerDraft(firstId).value("dictionaryMode").toString(), QStringLiteral("brief"));
            draft["dictionaryMode"] = "detailed"; draft["name"] = "Detailed";
            QCOMPARE(app.saveProvider(draft), QString()); secondId = app.providers().last().toMap().value("id").toString();
            draft = app.providerDraft(firstId); draft["dictionaryMode"] = "invalid";
            QVERIFY(!app.saveProvider(draft).isEmpty());
            QCOMPARE(app.providerDraft(firstId).value("dictionaryMode").toString(), QStringLiteral("brief"));
        }
        {
            AppController app(directory.path());
            QCOMPARE(app.providerDraft(firstId).value("dictionaryMode").toString(), QStringLiteral("brief"));
            QCOMPARE(app.providerDraft(secondId).value("dictionaryMode").toString(), QStringLiteral("detailed"));
        }
    }

    void unfinishedBriefDictionaryIsNotSaved_data() {
        QTest::addColumn<bool>("cancel");
        QTest::newRow("cancel") << true;
        QTest::newRow("interrupted stream") << false;
    }

    void unfinishedBriefDictionaryIsNotSaved() {
        QFETCH(bool, cancel);
        QTemporaryDir directory; MockServer mock;
        mock.hang = cancel;
        mock.body = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n";
        AppController app(directory.path());
        QCOMPARE(app.saveProvider({{"name", "Local"}, {"protocol", "openai"}, {"baseUrl", mock.provider().baseUrl}, {"model", "small"}}), QString());
        app.setProperty("inputText", QStringLiteral("look")); app.translate();
        QTRY_COMPARE(mock.requestCount, 1);
        if (cancel) app.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!app.busy(), 5000);
        QVERIFY(!app.error().isEmpty()); QVERIFY(app.history().isEmpty()); QVERIFY(!app.canSave());
        QCOMPARE(mock.requestCount, 1);
    }

    void dictionaryFormattingVariations_data() {
        QTest::addColumn<QString>("protocol"); QTest::addColumn<QString>("dictionaryMode");
        QTest::addColumn<QString>("answer"); QTest::addColumn<QString>("expectedMeaning");
        QFile fixture(QFINDTESTDATA("fixtures/dictionary/hello-malformed-note.txt"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const auto malformedNote = QString::fromUtf8(fixture.readAll());
        for (const auto protocol : {"openai", "responses"}) {
            for (const auto mode : {"brief", "detailed"}) {
                const auto row = [&](const char *name, const QString &answer, const QString &meaning) {
                    QTest::newRow(qPrintable(QStringLiteral("%1/%2/%3").arg(QLatin1String(protocol), QLatin1String(mode), QLatin1String(name))))
                        << QString::fromLatin1(protocol) << QString::fromLatin1(mode) << answer << meaning;
                };
                row("malformed optional note", malformedNote, QStringLiteral("你好；喂"));
                row("malformed optional example", QStringLiteral("{\"word\":\"hello\",\"definitions\":[{\"meaning\":\"你好\"}],\"examples\":[{\"source\":\"Say \"hello\".\"}]}"), QStringLiteral("你好"));
                row("missing headword", QStringLiteral("{\"definitions\":[{\"meaning\":\"你好\"}]}"), QStringLiteral("你好"));
                row("string meanings", QStringLiteral("{\"word\":\"hello\",\"definitions\":[\"你好\",\"喂\"]}"), QStringLiteral("你好"));
                row("plain word class", QStringLiteral("interj. 你好；喂"), QStringLiteral("你好"));
                row("plain translation", QStringLiteral("你好"), QStringLiteral("你好"));
                row("unfinished meaning", QStringLiteral("{\"word\":\"hello\",\"definitions\":[{\"meaning\":\"你好"), QString());
                row("no meanings", QStringLiteral("{\"word\":\"hello\",\"note\":\"没有释义\"}"), QString());
                row("invalid structure", QStringLiteral("{\"word\":\"hello\",\"definitions\":[{\"meaning\" \"你好\"}]}"), QString());
                row("wrong language", QStringLiteral("{\"word\":\"hello\",\"definitions\":[{\"meaning\":\"greeting\"}]}"), QString());
            }
        }
    }

    void dictionaryFormattingVariations() {
        QFETCH(QString, protocol); QFETCH(QString, dictionaryMode); QFETCH(QString, answer); QFETCH(QString, expectedMeaning);
        QTemporaryDir directory; QVERIFY(directory.isValid()); StreamServer mock(protocol);
        AppController app(directory.path());
        QCOMPARE(app.saveProvider({{"name", "Local"}, {"protocol", protocol}, {"baseUrl", mock.url()},
            {"model", "small-local-model"}, {"dictionaryMode", dictionaryMode}}), QString());
        app.setProperty("inputText", QStringLiteral("hello")); app.translate(); QTRY_VERIFY(mock.ready);
        // Use the request snapshot when the user changes settings mid-flight.
        app.setProperty("inputText", QStringLiteral("edited")); app.setTargetLanguage(QStringLiteral("英语"));
        mock.sendText(answer); mock.finish(answer); QTRY_VERIFY(!app.busy());
        QCOMPARE(mock.requestCount, 1); // Recover locally, without a second AI call.
        QCOMPARE(app.resultKind(), QStringLiteral("dictionary"));
        QVERIFY(!app.resultText().contains(QLatin1String("\"definitions\"")));
        if (expectedMeaning.isEmpty()) {
            QVERIFY(!app.error().isEmpty()); QVERIFY(app.resultText().isEmpty());
            QVERIFY(app.dictionary().isEmpty()); QVERIFY(app.history().isEmpty()); QVERIFY(!app.canSave());
            return;
        }
        QVERIFY2(app.error().isEmpty(), qPrintable(app.error()));
        QCOMPARE(app.dictionary().value("word").toString(), QStringLiteral("hello"));
        QCOMPARE(app.dictionary().value("definitions").toList().first().toMap().value("meaning").toString(), expectedMeaning);
        QVERIFY(app.resultText().contains(expectedMeaning)); QCOMPARE(app.history().size(), 1);
        if (dictionaryMode == QLatin1String("brief")) {
            QVERIFY(app.briefResult()); QVERIFY(!app.dictionary().contains("examples"));
            if (answer.contains(QLatin1String("interjection")) || answer.startsWith(QLatin1String("interj.")))
                QCOMPARE(app.dictionary().value("definitions").toList().first().toMap().value("partOfSpeech").toString(), QStringLiteral("interj."));
        }
        const auto record = app.history().first().toMap();
        QCOMPARE(record.value("input").toString(), QStringLiteral("hello"));
        QCOMPARE(record.value("target").toString(), QStringLiteral("简体中文"));
        const auto result = app.resultText(); app.toggleFavorite(); app.clear(); app.openRecord(record.value("id").toLongLong());
        QCOMPARE(app.resultText(), result); QCOMPARE(app.resultKind(), QStringLiteral("dictionary")); QVERIFY(app.favorite());
    }

    void dictionaryValidation() {
        QVERIFY(AppController::looksLikeWord(QStringLiteral("happy accident")));
        QVERIFY(AppController::looksLikeWord(QStringLiteral("机缘巧合")));
        QVERIFY(!AppController::looksLikeWord(QStringLiteral("This is a full sentence.")));
        QVERIFY(!AppController::looksLikeWord(QStringLiteral("第一行\n第二行")));
        const auto entry = AppController::parseDictionary(QStringLiteral("```json\n{\"word\":\"book\",\"definitions\":[{\"meaning\":\"书\"}],\"examples\":[null,42,{\"source\":\"A book\"}],\"synonyms\":[3,\"volume\"]}\n```"));
        QCOMPARE(entry.value("word").toString(), QStringLiteral("book"));
        QCOMPARE(entry.value("examples").toList().size(), 1);
        QCOMPARE(entry.value("synonyms").toStringList(), QStringList{QStringLiteral("volume")});
        QVERIFY(AppController::parseDictionary(QStringLiteral("{\"word\":\"x\",\"definitions\":[3]}")).isEmpty());
        QVERIFY(AppController::parseDictionary(QStringLiteral("not json")).isEmpty());
    }

    void credentialProtection() {
#ifdef Q_OS_WIN
        QString error;
        const auto encrypted = SecretStore::protect(QStringLiteral("sensitive-key-你好"), &error);
        QVERIFY(error.isEmpty()); QVERIFY(!encrypted.contains(QStringLiteral("sensitive-key")));
        QCOMPARE(SecretStore::reveal(encrypted, &error), QStringLiteral("sensitive-key-你好"));
        QVERIFY(error.isEmpty());
        QVERIFY(SecretStore::reveal(QStringLiteral("invalid"), &error).isEmpty());
        QVERIFY(!error.isEmpty());
#else
        QSKIP("Windows DPAPI only");
#endif
    }

    void controllerHistoryFavoritesAndPersistence() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        MockServer mock; mock.openai(QStringLiteral("完整的翻译结果"));
        QString providerId;
        qint64 recordId = -1;
        {
            AppController app(directory.path());
            QVERIFY(app.example()); QVERIFY(app.history().isEmpty()); QVERIFY(!app.canSave());
            QVariantMap draft{{"name", "Local test"}, {"protocol", "openai"}, {"baseUrl", mock.provider().baseUrl}, {"model", "test-model"}, {"apiKey", "private-test-secret"}};
            QCOMPARE(app.saveProvider(draft), QString());
            providerId = app.selectedProviderId(); QVERIFY(!providerId.isEmpty());
            QVERIFY(!app.providers().first().toMap().contains("secret"));
            app.setMode(QStringLiteral("translation"));
            app.setProperty("inputText", QStringLiteral("Original input"));
            app.translate(); QVERIFY(app.busy());
            app.setProperty("inputText", QStringLiteral("Changed after request"));
            QTRY_VERIFY_WITH_TIMEOUT(!app.busy(), 10000);
            QVERIFY2(app.error().isEmpty(), qPrintable(app.error()));
            QCOMPARE(app.resultText(), QStringLiteral("完整的翻译结果"));
            QCOMPARE(app.history().size(), 1);
            QCOMPARE(app.history().first().toMap().value("input").toString(), QStringLiteral("Original input"));
            recordId = app.history().first().toMap().value("id").toLongLong();
            app.toggleFavorite(); QVERIFY(app.favorite()); QCOMPARE(app.favorites().size(), 1);
            auto edit = app.providerDraft(providerId);
            edit["name"] = "Renamed local";
            edit["protocol"] = "responses";
            QCOMPARE(app.saveProvider(edit), QString());
            QVERIFY(app.providerDraft(providerId).value("hasKey").toBool());
        }
        QFile settings(directory.filePath(QStringLiteral("settings.ini"))); QVERIFY(settings.open(QIODevice::ReadOnly));
        QVERIFY(!settings.readAll().contains("private-test-secret")); settings.close();
        {
            AppController app(directory.path());
            QCOMPARE(app.providerName(), QStringLiteral("Renamed local")); QCOMPARE(app.history().size(), 1); QCOMPARE(app.favorites().size(), 1);
            QCOMPARE(app.providerDraft(providerId).value("protocol").toString(), QStringLiteral("responses"));
            app.openRecord(recordId); QVERIFY(!app.example()); QVERIFY(app.favorite());
            QCOMPARE(app.property("inputText").toString(), QStringLiteral("Original input"));
            app.setRecordFavorite(recordId, false); QVERIFY(app.favorites().isEmpty());
            app.deleteRecord(recordId); QVERIFY(app.history().isEmpty()); QVERIFY(!app.canSave());
            auto edit = app.providerDraft(providerId); edit["clearKey"] = true;
            QCOMPARE(app.saveProvider(edit), QString()); QVERIFY(!app.providerDraft(providerId).value("hasKey").toBool());
            app.removeProvider(providerId); QVERIFY(app.providers().isEmpty()); QVERIFY(app.selectedProviderId().isEmpty());
        }
    }

    void externalSelectionIsValidatedAndDoesNotInterrupt() {
        QTemporaryDir directory; AppController app(directory.path());
        StreamServer server(QStringLiteral("openai"));
        QCOMPARE(app.saveProvider({{"name", "Selection test"}, {"protocol", "openai"}, {"baseUrl", server.url()}, {"model", "test"}}), QString());
        app.setMode(QStringLiteral("translation"));
        app.setProperty("inputText", QStringLiteral("previous input"));
        app.lookupSelection(QStringLiteral("   "));
        app.lookupSelection(QString(24001, QLatin1Char('x')));
        QCOMPARE(server.requestCount, 0);
        QCOMPARE(app.property("inputText").toString(), QStringLiteral("previous input"));
        app.lookupSelection(QStringLiteral("  book  "));
        QTRY_VERIFY(server.ready);
        QVERIFY(app.busy()); QCOMPARE(app.mode(), QStringLiteral("auto"));
        QCOMPARE(app.resultKind(), QStringLiteral("dictionary"));
        app.lookupSelection(QStringLiteral("another selection"));
        QCOMPARE(server.requestCount, 1);
        QCOMPARE(app.property("inputText").toString(), QStringLiteral("book"));
        server.sendText(QStringLiteral("POS: n.\nTRANSLATION: 书")); server.finish({});
        QTRY_VERIFY(!app.busy()); QVERIFY(app.error().isEmpty());
        QCOMPARE(app.history().size(), 1);
        QCOMPARE(app.history().first().toMap().value("input").toString(), QStringLiteral("book"));
    }
};

QTEST_GUILESS_MAIN(CoreTests)
#include "tst_core.moc"
