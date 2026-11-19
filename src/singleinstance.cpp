#include "singleinstance.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalSocket>
#include <QTimer>
#include <QThread>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

SingleInstance::SingleInstance(QObject *parent) : QObject(parent) {
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (auto *socket = m_server.nextPendingConnection()) {
            socket->setParent(this);
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                const auto buffer = socket->property("request").toByteArray() + socket->readAll();
                if (buffer.size() < 8 && QByteArrayLiteral("activate").startsWith(buffer)) {
                    socket->setProperty("request", buffer); return;
                }
                if (buffer == QByteArrayLiteral("activate")) {
                    emit activated(); socket->write("ok"); socket->flush();
                }
                socket->disconnectFromServer();
            });
            QTimer::singleShot(2000, socket, [socket] { socket->abort(); socket->deleteLater(); });
        }
    });
}

SingleInstance::~SingleInstance() { m_server.close(); }

SingleInstance::Result SingleInstance::acquire(const QString &directory) {
    QDir().mkpath(directory);
    const auto path = QFileInfo(directory).canonicalFilePath();
    const auto normalized =
#ifdef Q_OS_WIN
        path.toLower();
#else
        path;
#endif
    const auto name = QStringLiteral("Yiyi-") + QString::fromLatin1(QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
    m_lock = std::make_unique<QLockFile>(QDir(directory).filePath(QStringLiteral("desktop.lock")));
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock()) {
#ifdef Q_OS_WIN
        qint64 pid = 0; QString host, app;
        if (m_lock->getLockInfo(&pid, &host, &app)) AllowSetForegroundWindow(DWORD(pid));
#endif
        QLocalSocket socket;
        QElapsedTimer wait; wait.start();
        do {
            socket.connectToServer(name);
            if (socket.waitForConnected(100)) break;
            socket.abort(); QThread::msleep(50);
        } while (wait.elapsed() < 1200);
        if (socket.state() == QLocalSocket::ConnectedState) {
            socket.write("activate"); socket.flush();
            if (socket.waitForReadyRead(1500) && socket.readAll() == QByteArrayLiteral("ok")) return ActivatedExisting;
        }
        m_error = QStringLiteral("译意正在启动或退出，请稍后重试。若问题持续，请检查数据目录是否可写。");
        return Failed;
    }
    QLocalServer::removeServer(name);
    if (!m_server.listen(name)) { m_error = QStringLiteral("无法创建本地唤起通道：") + m_server.errorString(); return Failed; }
    return Primary;
}
