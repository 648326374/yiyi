#pragma once
#include <QObject>
#include <QLocalServer>
#include <QLockFile>
#include <memory>

// One foreground app per data directory. Test profiles remain independent.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    enum Result { Primary, ActivatedExisting, Failed };
    explicit SingleInstance(QObject *parent = nullptr);
    ~SingleInstance() override;
    Result acquire(const QString &directory);
    QString error() const { return m_error; }
signals:
    void activated();
private:
    QLocalServer m_server;
    std::unique_ptr<QLockFile> m_lock;
    QString m_error;
};
