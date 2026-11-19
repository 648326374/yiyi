#include "ocrservice.h"
#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

OcrService::OcrService(QObject *parent) : QObject(parent)
{
    m_timeout.setSingleShot(true); m_timeout.setInterval(60000);
#ifdef Q_OS_WIN
    m_process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &OcrService::finishProcess);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_busy) fail(QStringLiteral("无法启动 RapidOCR，请使用包含 OCR 组件的完整发行包。"));
    });
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (!m_busy) return;
        cancel(); fail(QStringLiteral("本地识别超过 60 秒，已停止。请缩小截图范围后重试。"));
    });
}
OcrService::~OcrService() { cancel(); }
void OcrService::clear() { cancel(); m_text.clear(); m_error.clear(); m_note.clear(); emit changed(); }

void OcrService::recognize(const QImage &image)
{
    if (m_busy) return;
    m_text.clear(); m_error.clear(); m_note.clear();
    if (image.isNull() || qint64(image.width()) * image.height() > 64000000) { fail(QStringLiteral("截图无效或过大，请缩小范围。")); return; }
    m_temp = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/Yiyi-ocr-XXXXXX"));
    if (!m_temp->isValid() || !image.save(m_temp->filePath(QStringLiteral("input.png")), "PNG")) { fail(QStringLiteral("无法写入识别临时图片，请检查磁盘空间。")); return; }
    const auto worker = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("yiyi_ocr_worker.exe"));
    m_busy = true; emit changed(); m_timeout.start();
    m_process.start(worker, {m_temp->filePath(QStringLiteral("input.png")), m_temp->filePath(QStringLiteral("result.json"))});
}

void OcrService::finishProcess(int code, QProcess::ExitStatus status)
{
    if (!m_busy) return;
    m_timeout.stop();
    m_process.readAllStandardOutput();
    m_process.readAllStandardError();
    QByteArray output;
    if (m_temp) {
        QFile file(m_temp->filePath(QStringLiteral("result.json")));
        output = file.open(QIODevice::ReadOnly) ? file.read(2 * 1024 * 1024) : QByteArray();
    }
    m_temp.reset();
    const auto object = QJsonDocument::fromJson(output).object();
    const auto errorCode = object.value(QStringLiteral("code")).toString();
    if (code != 0 || status != QProcess::NormalExit || object.isEmpty() || !errorCode.isEmpty()) {
        fail(QStringLiteral("RapidOCR 组件未能运行。请使用完整发行包并保留 ocr 目录；也可手动切换 AI 识图。")); return;
    }
    m_text = object.value(QStringLiteral("text")).toString();
    if (m_text.trimmed().isEmpty()) { fail(QStringLiteral("没有识别到文字。请框选清晰的文字区域后重试。")); return; }
    m_note = object.value(QStringLiteral("confidence")).toDouble(1) < 0.85
        ? QStringLiteral("部分文字置信度较低，请对照原图校对。") : QStringLiteral("识别完成，请校对后使用。");
    m_busy = false; emit changed(); emit finished();
}
void OcrService::fail(const QString &error)
{
    m_timeout.stop(); m_busy = false; m_error = error; m_temp.reset(); emit changed(); emit finished();
}
void OcrService::cancel()
{
    const bool wasBusy = m_busy;
    m_busy = false; m_timeout.stop();
    if (m_process.state() != QProcess::NotRunning) { m_process.kill(); m_process.waitForFinished(1500); }
    if (m_process.isOpen()) { m_process.readAllStandardOutput(); m_process.readAllStandardError(); }
    m_temp.reset();
    if (wasBusy) emit changed();
}
