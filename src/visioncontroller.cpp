#include "visioncontroller.h"
#include "visionprompt.h"
#include <QClipboard>
#include <QDir>
#include <QGuiApplication>

VisionController::VisionController(AppController *app, QObject *parent)
    : QObject(parent), m_app(app), m_settings(QDir(app->dataDirectory()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat)
{
    m_providerId = m_settings.value(QStringLiteral("vision/provider"), app->selectedProviderId()).toString();
    connect(app, &AppController::providersChanged, this, [this] {
        if (m_providerId.isEmpty() && !m_busy) setProviderId(m_app->selectedProviderId());
        emit changed();
    });
    m_timer.setInterval(250);
    connect(&m_timer, &QTimer::timeout, this, &VisionController::changed);
    connect(&m_client, &AiClient::delta, this, [this](const QString &text) { if (m_busy) { m_pendingText += text; emit changed(); } });
    connect(&m_client, &AiClient::completed, this, [this](const QString &text) {
        if (!m_busy) return;
        if (text.trimmed().isEmpty()) { fail(QStringLiteral("没有识别到文字，请框选清晰的文字区域后重试。")); return; }
        m_text = text; m_info = m_pendingInfo; m_complete = true; stop(); emit changed();
    });
    connect(&m_client, &AiClient::failed, this, [this](const QString &error) {
        if (!m_busy) return;
        fail(error);
    });
}
VisionController::~VisionController() { cancel(); }
void VisionController::setProviderId(const QString &id)
{
    if (m_busy || m_providerId == id) return;
    m_providerId = id; m_settings.setValue(QStringLiteral("vision/provider"), id); emit changed();
}
void VisionController::setImage(const QImage &image)
{
    cancel(); m_image = image; m_text.clear(); m_error.clear(); m_info.clear(); m_complete = false; m_elapsed = -1; emit changed();
}
void VisionController::setResultText(const QString &text)
{
    if (m_busy || m_text == text) return;
    m_text = text; emit changed();
}
QString VisionController::status() const
{
    if (m_busy) return QStringLiteral("%1 · %2 秒").arg(m_pendingText.isEmpty() ? QStringLiteral("等待模型识图") : QStringLiteral("正在识别原文")).arg(m_clock.elapsed() / 1000.0, 0, 'f', 1);
    if (m_elapsed >= 0) return QStringLiteral("%1 · %2 秒").arg(m_complete ? QStringLiteral("已完成，请对照原图校对") : QStringLiteral("未完成")).arg(m_elapsed / 1000.0, 0, 'f', 1);
    return QStringLiteral("点击后发送当前截图");
}
void VisionController::recognize()
{
    if (m_busy) return;
    m_error.clear(); m_complete = false; m_elapsed = -1;
    if (m_image.isNull()) m_error = QStringLiteral("请先截取图片。");
    else if (m_app->busy() || m_app->documentBusy() || m_app->testBusy() || m_app->pdfBusy()) m_error = QStringLiteral("请等待当前翻译或连接测试结束，再识图。");
    QString providerError;
    const auto provider = m_app->providerById(m_providerId, &providerError);
    if (providerError.isEmpty()) providerError = AiClient::validate(provider);
    if (m_error.isEmpty()) m_error = providerError;
    if (!m_error.isEmpty()) {
        if (!m_text.isEmpty()) m_error += QStringLiteral(" 已保留上次原文及修改。");
        emit changed(); return;
    }
    // Keep the last edited source until a complete recognition replaces it.
    // A cancelled, truncated or failed stream must never erase that draft.
    m_pendingInfo = provider.name + QStringLiteral(" · ") + provider.model;
    m_pendingText.clear(); m_busy = true; m_clock.start(); m_timer.start();
    m_app->setVisionBusy(true); emit changed();
    m_client.startImage(provider, visionTranscriptionPrompt(), QStringLiteral("Read the original text in this image."), m_image);
}
void VisionController::stop()
{
    m_timer.stop(); if (m_busy) m_elapsed = m_clock.elapsed();
    m_busy = false; m_app->setVisionBusy(false);
    m_pendingText.clear(); m_pendingInfo.clear();
}
void VisionController::fail(const QString &error)
{
    m_error = error;
    if (!m_text.isEmpty()) m_error += QStringLiteral(" 已保留上次原文及修改。");
    m_complete = false; stop(); emit changed();
}
void VisionController::cancel()
{
    m_client.cancel();
    if (!m_busy) return;
    fail(QStringLiteral("识别已取消，未完成的内容已丢弃。"));
}
void VisionController::copyResult()
{
    if (!m_busy && !m_text.trimmed().isEmpty()) QGuiApplication::clipboard()->setText(m_text);
}
