#pragma once
#include "appcontroller.h"
#include <QElapsedTimer>
#include <QSettings>

class VisionController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString providerId READ providerId WRITE setProviderId NOTIFY changed)
    Q_PROPERTY(QString resultText READ resultText WRITE setResultText NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString resultInfo READ resultInfo NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool complete READ complete NOTIFY changed)
public:
    explicit VisionController(AppController *app, QObject *parent = nullptr);
    ~VisionController() override;
    QString providerId() const { return m_providerId; }
    void setProviderId(const QString &id);
    QString resultText() const { return m_busy ? m_pendingText : m_text; }
    void setResultText(const QString &text);
    QString error() const { return m_error; }
    QString status() const;
    QString resultInfo() const { return m_busy ? m_pendingInfo : m_info; }
    bool busy() const { return m_busy; }
    bool complete() const { return m_complete; }
    void setImage(const QImage &image);
    Q_INVOKABLE void recognize();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void copyResult();
signals:
    void changed();
private:
    void stop();
    void fail(const QString &error);
    AppController *m_app;
    QSettings m_settings;
    AiClient m_client;
    QImage m_image;
    QTimer m_timer;
    QElapsedTimer m_clock;
    QString m_providerId, m_text, m_error, m_info;
    QString m_pendingText, m_pendingInfo;
    qint64 m_elapsed = -1;
    bool m_busy = false, m_complete = false;
};
