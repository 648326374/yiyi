#pragma once
#include <QObject>
#include <QElapsedTimer>
#include <QPoint>
#include <QProcess>
#include <QTimer>
#include <memory>

class QMimeData;

// UI Automation runs in a short-lived child of our own executable. A hung
// accessibility provider must never block the GUI, mouse hook, or shutdown.
class SelectionCapture : public QObject {
    Q_OBJECT
public:
    explicit SelectionCapture(QObject *parent = nullptr);
    ~SelectionCapture() override;
    void request(quintptr sourceWindow, const QPoint &nativePoint, bool automatic);
    void cancel();
    bool busy() const { return m_stage != Idle; }
    static int runProbe(int argc, char **argv);
signals:
    void captured(const QString &text, bool automatic, quintptr sourceWindow);
    void failed(const QString &message, bool automatic);
private:
    enum Stage { Idle, Probing, WaitingForKeys, WaitingForCopy };
    void probeFinished();
    void pollCopy();
    void fail(const QString &message);
    void complete(const QString &text);
    bool sourceIsCurrent() const;
    QProcess m_probe;
    QTimer m_timeout, m_poll;
    QElapsedTimer m_clock;
    Stage m_stage = Idle;
    quintptr m_source = 0;
    quint32 m_sequence = 0, m_sourcePid = 0;
    bool m_automatic = false;
    std::unique_ptr<QMimeData> m_clipboard;
};
