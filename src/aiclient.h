#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QJsonObject>
#include <QImage>
#include <QElapsedTimer>

struct AiProvider {
    QString id, name, protocol, baseUrl, model, apiKey;
    QString reasoningEffort; // Empty means omit the parameter, not "none".
};

class AiClient : public QObject {
    Q_OBJECT
public:
    explicit AiClient(QObject *parent = nullptr);
    void start(const AiProvider &provider, const QString &system, const QString &input, bool probe = false);
    void startImage(const AiProvider &provider, const QString &system, const QString &input, const QImage &image, bool probe = false);
    void cancel();
    // Applied to generation requests; connection probes retain a shorter wait.
    void setInactivityTimeout(int milliseconds);
    int inactivityTimeout() const { return m_inactivityTimeout; }
    int activeTimeout() const { return m_timer.interval(); }
    QString requestStatus() const;
    static QUrl endpoint(const AiProvider &provider);
    static QString validate(const AiProvider &provider);
    static bool supportsReasoningEffort(const QString &protocol);
signals:
    void delta(const QString &text);
    void completed(const QString &text);
    void failed(const QString &message);
    void progress();
private:
    void send(const AiProvider &provider, const QString &system, const QString &input, bool probe, const QByteArray &png = {});
    void readAvailable();
    void finishReply(class QNetworkReply *reply, bool transportFinished);
    void consumeLines(bool final = false);
    void consumeEvent(const QByteArray &event);
    void consumeObject(const QJsonObject &object);
    void consumeResponsesObject(const QJsonObject &object);
    void finishResponse(const QJsonObject &response, const QString &status);
    QString safeError(QString error) const;
    QString reasoningErrorHint(const QJsonObject &error) const;
    QNetworkAccessManager m_network;
    QPointer<class QNetworkReply> m_reply;
    QTimer m_timer, m_progressTimer;
    QElapsedTimer m_requestClock;
    int m_inactivityTimeout = 10 * 60 * 1000;
    bool m_receivedData = false;
    AiProvider m_provider;
    QByteArray m_buffer, m_event, m_eventType, m_body;
    QString m_text, m_error;
    bool m_isStream = false;
    bool m_done = false;
};
