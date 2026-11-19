#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>

// The test controls each response phase, so a preview can be inspected while
// the request is still open without depending on a real model or long sleeps.
class StreamServer : public QObject {
public:
    QTcpServer server;
    QPointer<QTcpSocket> socket;
    QByteArray request;
    QString protocol;
    bool ready = false;
    int requestCount = 0;

    explicit StreamServer(const QString &protocol) : protocol(protocol) {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            auto *connection = server.nextPendingConnection();
            connect(connection, &QTcpSocket::disconnected, connection, &QObject::deleteLater);
            connect(connection, &QTcpSocket::readyRead, this, [this, connection, buffer = QByteArray(), responded = false]() mutable {
                buffer += connection->readAll();
                const auto end = buffer.indexOf("\r\n\r\n");
                if (end < 0 || responded) return;
                int length = 0;
                for (const auto &line : buffer.left(end).split('\n'))
                    if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                if (buffer.size() < end + 4 + length) return;
                // Qt may open an idle connection alongside the actual request.
                // Count complete HTTP requests and respond on their own socket.
                responded = true; ++requestCount; socket = connection; request = buffer;
                connection->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
                ready = true;
            });
        });
    }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort()); }
    void sendText(const QString &text) {
        if (protocol == QLatin1String("responses")) {
            send({{"type", "response.output_text.delta"}, {"delta", text}});
        } else {
            const QJsonObject delta{{"content", text}};
            const QJsonObject choice{{"delta", delta}};
            send({{"choices", QJsonArray{choice}}});
        }
    }
    void finish(const QString &text, bool closeConnection = true) {
        if (protocol == QLatin1String("responses")) {
            const QJsonObject part{{"type", "output_text"}, {"text", text}};
            const QJsonObject message{{"type", "message"}, {"role", "assistant"}, {"content", QJsonArray{part}}};
            const QJsonObject response{{"status", "completed"}, {"output", QJsonArray{message}}};
            send({{"type", "response.completed"}, {"response", response}});
        } else {
            const QJsonObject choice{{"delta", QJsonObject{}}, {"finish_reason", "stop"}};
            send({{"choices", QJsonArray{choice}}});
            socket->write("data: [DONE]\n\n");
        }
        if (closeConnection) socket->disconnectFromHost(); else socket->flush();
    }
    void interrupt() { socket->disconnectFromHost(); }
private:
    void send(const QJsonObject &event) {
        Q_ASSERT(ready && socket);
        socket->write("data: " + QJsonDocument(event).toJson(QJsonDocument::Compact) + "\n\n");
        socket->flush();
    }
};
