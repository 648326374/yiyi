#pragma once
#include "aiclient.h"
#include <QObject>
#include <QUrl>

class AppController;

struct DocumentPart {
    QString text;
    bool translate = false;
    bool continuation = false;
};

class DocumentController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString sourceText READ sourceText NOTIFY changed)
    Q_PROPERTY(QString resultText READ resultText NOTIFY changed)
    Q_PROPERTY(QString preview READ preview NOTIFY changed)
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
    Q_PROPERTY(QString encoding READ encoding NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString taskDescription READ taskDescription NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(int completed READ completed NOTIFY changed)
    Q_PROPERTY(bool hasUnexportedResult READ hasUnexportedResult NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(QUrl suggestedOutput READ suggestedOutput NOTIFY changed)
public:
    explicit DocumentController(AppController *app, QObject *parent = nullptr);
    ~DocumentController() override;
    QString sourceText() const { return m_source; }
    QString resultText() const { return m_result; }
    QString preview() const { return m_preview; }
    QString fileName() const;
    QString encoding() const { return m_encoding; }
    QString error() const { return m_error; }
    QString state() const { return m_state; }
    QString taskDescription() const;
    bool busy() const { return m_busy; }
    int completed() const { return m_completed; }
    bool hasUnexportedResult() const { return m_completed > m_exportedCompleted; }
    int total() const { return m_total; }
    QUrl suggestedOutput() const;
    Q_INVOKABLE bool loadFile(const QUrl &url, const QString &encoding = QStringLiteral("auto"));
    Q_INVOKABLE bool saveFile(const QUrl &url);
    Q_INVOKABLE void start();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void reset();
    static QString decode(const QByteArray &bytes, const QString &encoding, QString *detected, QString *error);
    static QList<DocumentPart> splitText(const QString &text, int limit = 1000);
signals:
    void changed();
    void toast(const QString &message);
private:
    void sendNext();
    void stop(const QString &state, const QString &error = {});
    void reject(const QString &error);
    AppController *m_app;
    AiClient m_client;
    AiProvider m_provider;
    QList<DocumentPart> m_parts;
    QString m_source, m_result, m_preview, m_path, m_encoding, m_error;
    QString m_state = QStringLiteral("empty"), m_sourceLanguage, m_targetLanguage, m_tone, m_newline;
    int m_index = 0, m_completed = 0, m_exportedCompleted = 0, m_total = 0;
    quint64 m_generation = 0;
    bool m_busy = false, m_hasSnapshot = false;
};
