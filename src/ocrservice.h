#pragma once
#include <QObject>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

class OcrService : public QObject {
    Q_OBJECT
public:
    explicit OcrService(QObject *parent = nullptr);
    ~OcrService() override;
    bool busy() const { return m_busy; }
    QString text() const { return m_text; }
    QString error() const { return m_error; }
    void recognize(const QImage &image);
    void cancel();
    void clear();
    QString note() const { return m_note; }
signals:
    void changed();
    void finished();
private:
    void finishProcess(int code, QProcess::ExitStatus status);
    void fail(const QString &error);
    QProcess m_process;
    QTimer m_timeout;
    std::unique_ptr<QTemporaryDir> m_temp;
    QString m_text, m_error;
    bool m_busy = false;
    QString m_note;
};
