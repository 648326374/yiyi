#include "aiclient.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QBuffer>
#include <QRegularExpression>

static constexpr qint64 maxResponseSize = 2 * 1024 * 1024;

AiClient::AiClient(QObject *parent) : QObject(parent)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(m_inactivityTimeout);
    m_timer.setTimerType(Qt::PreciseTimer);
    m_progressTimer.setInterval(1000);
    connect(&m_progressTimer, &QTimer::timeout, this, &AiClient::progress);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (!m_reply) return;
        const int seconds = (m_timer.interval() + 999) / 1000;
        const auto duration = seconds % 60 == 0 ? QStringLiteral("%1 分钟").arg(seconds / 60) : QStringLiteral("%1 秒").arg(seconds);
        const auto message = m_receivedData
            ? QStringLiteral("接收响应超时：连续 %1 未收到新的服务数据，未完成结果不会采用。可延长无响应等待时间后重试。").arg(duration)
            : QStringLiteral("等待服务响应超时：%1 内未收到数据。服务可能仍在排队或推理，可延长无响应等待时间，或检查服务连接后重试。").arg(duration);
        cancel();
        emit failed(message);
    });
}

void AiClient::setInactivityTimeout(int milliseconds)
{
    if (m_reply || milliseconds <= 0) return;
    m_inactivityTimeout = milliseconds;
}

QString AiClient::requestStatus() const
{
    if (!m_reply || !m_requestClock.isValid()) return {};
    const auto state = !m_text.isEmpty() ? QStringLiteral("正在接收正文")
        : m_receivedData ? QStringLiteral("服务已响应，等待正文") : QStringLiteral("等待模型响应");
    return QStringLiteral("%1 · %2 秒").arg(state).arg(m_requestClock.elapsed() / 1000);
}

bool AiClient::supportsReasoningEffort(const QString &protocol)
{
    return protocol == QLatin1String("openai") || protocol == QLatin1String("responses");
}

QString AiClient::validate(const AiProvider &p)
{
    if (p.name.trimmed().isEmpty()) return QStringLiteral("请填写服务名称。");
    if (!QStringList{QStringLiteral("openai"), QStringLiteral("responses"), QStringLiteral("ollama"), QStringLiteral("anthropic"), QStringLiteral("gemini")}.contains(p.protocol))
        return QStringLiteral("不支持的接口协议。");
    const QUrl url(p.baseUrl);
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")))
        return QStringLiteral("请输入完整的 http:// 或 https:// 服务地址。");
    if (!url.userInfo().isEmpty() || url.hasFragment() || url.hasQuery())
        return QStringLiteral("服务地址不能包含用户密码、查询参数或片段。API Key 请填入专用字段。");
    if (p.model.trimmed().isEmpty()) return QStringLiteral("请填写模型名称，与服务端的模型 ID 保持一致。");
    if ((p.protocol == QLatin1String("anthropic") || p.protocol == QLatin1String("gemini")) && p.apiKey.isEmpty())
        return QStringLiteral("此接口需要 API Key。");
    if (p.apiKey.contains(QLatin1Char('\n')) || p.apiKey.contains(QLatin1Char('\r')))
        return QStringLiteral("API Key 不能包含换行。");
    if (supportsReasoningEffort(p.protocol) && !p.reasoningEffort.isEmpty()
        && !QStringList{QStringLiteral("none"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("xhigh"), QStringLiteral("max")}.contains(p.reasoningEffort))
        return QStringLiteral("思考等级无效，请重新选择，或改为「跟随服务」。");
    return {};
}

QUrl AiClient::endpoint(const AiProvider &p)
{
    QUrl url(p.baseUrl.trimmed());
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    if (p.protocol == QLatin1String("ollama")) {
        if (!path.endsWith(QLatin1String("/api/chat"))) path += path.endsWith(QLatin1String("/api")) ? QStringLiteral("/chat") : QStringLiteral("/api/chat");
    } else if (p.protocol == QLatin1String("anthropic")) {
        if (!path.endsWith(QLatin1String("/messages"))) path += path.endsWith(QLatin1String("/v1")) ? QStringLiteral("/messages") : QStringLiteral("/v1/messages");
    } else if (p.protocol == QLatin1String("gemini")) {
        if (path.isEmpty()) path = QStringLiteral("/v1beta");
        if (!path.contains(QLatin1String("/models/"))) path += QStringLiteral("/models/") + p.model + QStringLiteral(":streamGenerateContent");
        else if (path.endsWith(QLatin1String(":generateContent"))) path.replace(QStringLiteral(":generateContent"), QStringLiteral(":streamGenerateContent"));
        else if (!path.endsWith(QLatin1String(":streamGenerateContent"))) path += QStringLiteral(":streamGenerateContent");
        QUrlQuery query; query.addQueryItem(QStringLiteral("alt"), QStringLiteral("sse")); url.setQuery(query);
    } else {
        const auto suffix = p.protocol == QLatin1String("responses") ? QStringLiteral("/responses") : QStringLiteral("/chat/completions");
        const auto otherSuffix = p.protocol == QLatin1String("responses") ? QStringLiteral("/chat/completions") : QStringLiteral("/responses");
        if (path.endsWith(otherSuffix)) {
            path.chop(otherSuffix.size());
            path += suffix;
        } else if (!path.endsWith(suffix)) {
            path += (path.isEmpty() ? QStringLiteral("/v1") : QString()) + suffix;
        }
    }
    url.setPath(path);
    return url;
}

void AiClient::start(const AiProvider &provider, const QString &system, const QString &input, bool probe)
{
    send(provider, system, input, probe);
}

void AiClient::startImage(const AiProvider &provider, const QString &system, const QString &input, const QImage &image, bool probe)
{
    cancel();
    // Preserve small text. Oversize captures are rejected, never silently shrunk.
    if (image.isNull() || image.width() > 4096 || image.height() > 4096 || qint64(image.width()) * image.height() > 12000000) {
        emit failed(QStringLiteral("图片过大或无效，请将截图限制在 4096 × 4096 以内，并缩小文字区域。")); return;
    }
    QByteArray png;
    QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG") || png.size() > 8 * 1024 * 1024) {
        emit failed(QStringLiteral("图片编码失败或超过 8 MB，请缩小截图范围。")); return;
    }
    send(provider, system, input, probe, png);
}

void AiClient::send(const AiProvider &provider, const QString &system, const QString &input, bool probe, const QByteArray &png)
{
    cancel();
    const auto validation = validate(provider);
    if (!validation.isEmpty()) { emit failed(validation); return; }
    m_provider = provider;
    m_buffer.clear(); m_event.clear(); m_eventType.clear(); m_body.clear(); m_text.clear(); m_error.clear(); m_isStream = false; m_done = false;
    m_receivedData = false;
    m_timer.setInterval(probe ? 90000 : m_inactivityTimeout);
    QNetworkRequest request(endpoint(provider));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "text/event-stream, application/x-ndjson, application/json");
    request.setRawHeader("User-Agent", "Yiyi/0.7.0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    const auto systemMessage = QJsonObject{{"role", "system"}, {"content", system}};
    const auto userMessage = QJsonObject{{"role", "user"}, {"content", input}};
    // Gemini's output budget can include internal thinking. A tiny probe budget
    // can exhaust before the first visible token even for a valid connection.
    const int maxTokens = probe && provider.protocol != QLatin1String("gemini") ? 64 : 4096;
    QJsonObject body;
    if (provider.protocol == QLatin1String("responses")) {
        if (!provider.apiKey.isEmpty()) request.setRawHeader("Authorization", "Bearer " + provider.apiKey.toUtf8());
        body = {{"model", provider.model}, {"instructions", system},
                {"input", QJsonArray{QJsonObject{{"role", "user"}, {"content", QJsonArray{QJsonObject{{"type", "input_text"}, {"text", input}}}}}}},
                {"stream", true}, {"store", false}};
        if (!provider.reasoningEffort.isEmpty())
            body["reasoning"] = QJsonObject{{"effort", provider.reasoningEffort}};
    } else if (provider.protocol == QLatin1String("anthropic")) {
        request.setRawHeader("x-api-key", provider.apiKey.toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");
        body = {{"model", provider.model}, {"system", system}, {"messages", QJsonArray{userMessage}}, {"stream", true}, {"max_tokens", maxTokens}};
    } else if (provider.protocol == QLatin1String("gemini")) {
        request.setRawHeader("x-goog-api-key", provider.apiKey.toUtf8());
        body = {{"systemInstruction", QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", system}}}}}},
                {"contents", QJsonArray{QJsonObject{{"role", "user"}, {"parts", QJsonArray{QJsonObject{{"text", input}}}}}}},
                {"generationConfig", QJsonObject{{"temperature", 0.3}, {"maxOutputTokens", maxTokens}}}};
    } else {
        if (!provider.apiKey.isEmpty()) request.setRawHeader("Authorization", "Bearer " + provider.apiKey.toUtf8());
        body = {{"model", provider.model}, {"messages", QJsonArray{systemMessage, userMessage}}, {"stream", true}};
        if (provider.protocol == QLatin1String("openai") && !provider.reasoningEffort.isEmpty())
            body["reasoning_effort"] = provider.reasoningEffort;
        if (provider.protocol == QLatin1String("ollama")) {
            body["options"] = QJsonObject{{"temperature", 0.3}, {"num_predict", maxTokens}};
            body["think"] = false;
        }
        // Compatible endpoints differ in their token/temperature parameter support.
        // Reasoning is opt-in; otherwise compatible servers retain their defaults.
    }
    if (!png.isEmpty()) {
        const auto base64 = QString::fromLatin1(png.toBase64());
        const auto dataUrl = QStringLiteral("data:image/png;base64,") + base64;
        QJsonObject imageUser = userMessage;
        if (provider.protocol == QLatin1String("responses")) {
            body["input"] = QJsonArray{QJsonObject{{"role", "user"}, {"content", QJsonArray{
                QJsonObject{{"type", "input_text"}, {"text", input}},
                QJsonObject{{"type", "input_image"}, {"image_url", dataUrl}}}}}};
        } else if (provider.protocol == QLatin1String("gemini")) {
            body["contents"] = QJsonArray{QJsonObject{{"role", "user"}, {"parts", QJsonArray{
                QJsonObject{{"text", input}}, QJsonObject{{"inlineData", QJsonObject{{"mimeType", "image/png"}, {"data", base64}}}}}}}};
        } else {
            if (provider.protocol == QLatin1String("ollama")) imageUser["images"] = QJsonArray{base64};
            else if (provider.protocol == QLatin1String("anthropic")) imageUser["content"] = QJsonArray{
                QJsonObject{{"type", "image"}, {"source", QJsonObject{{"type", "base64"}, {"media_type", "image/png"}, {"data", base64}}}},
                QJsonObject{{"type", "text"}, {"text", input}}};
            else imageUser["content"] = QJsonArray{QJsonObject{{"type", "text"}, {"text", input}},
                QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", dataUrl}}}}};
            body["messages"] = provider.protocol == QLatin1String("anthropic") ? QJsonArray{imageUser} : QJsonArray{systemMessage, imageUser};
        }
    }
    m_requestClock.start();
    m_reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_reply->setReadBufferSize(maxResponseSize + 1);
    auto *reply = m_reply.data();
    connect(reply, &QNetworkReply::readyRead, this, &AiClient::readAvailable);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finishReply(reply, true); });
    m_timer.start(); m_progressTimer.start();
}

void AiClient::finishReply(QNetworkReply *reply, bool transportFinished)
{
    if (m_reply != reply) return;
    if (transportFinished) readAvailable();
    // A delta handler can cancel this request or start another one.
    if (m_reply != reply) return;
    m_timer.stop(); m_progressTimer.stop();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString failure = m_error;
    if (status < 200 || status >= 300) {
        const auto object = QJsonDocument::fromJson(m_body).object();
        QString detail = object.value("error").isObject() ? object.value("error").toObject().value("message").toString() : object.value("error").toString();
        if (status == 401 || status == 403) failure = QStringLiteral("认证失败（%1）。请检查 API Key 和模型访问权限。").arg(status);
        else if (status == 404) failure = QStringLiteral("未找到接口或模型（404）。请检查协议、地址和模型名称。");
        else if (status == 429) failure = QStringLiteral("服务请求受限（429）。请检查额度，或稍后重试。");
        else if (status == 408 || status == 504 || status == 524) failure = QStringLiteral("服务端或网关返回超时（HTTP %1）。请检查服务端的等待限制后重试；延长客户端等待无法改变服务端限制。").arg(status);
        else if (status >= 300 && status < 400) failure = QStringLiteral("服务返回重定向。请直接填写最终 API 地址。");
        else failure = status ? QStringLiteral("服务返回 HTTP %1。").arg(status) : QStringLiteral("无法连接服务。请检查服务是否启动、地址及网络设置。");
        if (!detail.isEmpty()) failure += QLatin1Char('\n') + safeError(detail);
        if (status == 400 || status == 422)
            failure += reasoningErrorHint(object.value("error").isObject() ? object.value("error").toObject() : object);
        if (m_provider.protocol == QLatin1String("openai") && detail.contains(QLatin1String("chat/completions"), Qt::CaseInsensitive)
            && (detail.contains(QLatin1String("not supported"), Qt::CaseInsensitive) || detail.contains(QLatin1String("unsupported"), Qt::CaseInsensitive) || detail.contains(QStringLiteral("不支持"))))
            failure += QStringLiteral("\n此服务可能只支持 Responses，请将接口协议改为「OpenAI Responses」后重试。");
    } else if (reply->error() != QNetworkReply::NoError && failure.isEmpty() && !m_done) {
        failure = QStringLiteral("连接中断，结果尚未完成。请重试。");
    } else if (failure.isEmpty()) {
        if (m_isStream) { if (transportFinished) consumeLines(true); }
        else {
            const auto doc = QJsonDocument::fromJson(m_body);
            if (doc.isObject()) consumeObject(doc.object());
            else m_error = QStringLiteral("服务返回了无法识别的数据，请检查接口协议。");
        }
        failure = m_error;
        if (failure.isEmpty() && m_isStream && !m_done) failure = QStringLiteral("服务提前结束了响应，结果可能不完整。请重试。");
        if (failure.isEmpty() && m_text.trimmed().isEmpty()) failure = QStringLiteral("服务已结束请求，但没有返回可用正文。请检查服务返回详情或重试。");
    }
    m_reply = nullptr;
    reply->disconnect(this);
    if (!reply->isFinished()) reply->abort();
    reply->deleteLater();
    if (failure.isEmpty()) emit completed(m_text.trimmed());
    else emit failed(failure);
}

void AiClient::cancel()
{
    m_timer.stop(); m_progressTimer.stop();
    if (m_reply) {
        auto *reply = m_reply.data();
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

void AiClient::readAvailable()
{
    if (!m_reply || !m_reply->isOpen()) return;
    const auto chunk = m_reply->readAll();
    if (chunk.isEmpty()) return;
    m_receivedData = true;
    m_timer.start();
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300) { if (m_body.size() < 16384) m_body += chunk.left(16384 - m_body.size()); return; }
    const auto type = m_reply->header(QNetworkRequest::ContentTypeHeader).toString();
    m_isStream = type.contains(QLatin1String("event-stream")) || type.contains(QLatin1String("ndjson")) || m_provider.protocol == QLatin1String("ollama");
    // Include the incoming chunk so a final oversized chunk cannot bypass the
    // limit. Count retained output and parser buffers, not discarded SSE framing.
    if (m_text.size() + m_buffer.size() + m_body.size() + m_event.size() + chunk.size() > maxResponseSize) {
        m_error = QStringLiteral("返回内容过长，已停止接收。请缩短原文后重试。");
        m_reply->abort();
        return;
    }
    if (m_isStream) { m_buffer += chunk; consumeLines(); }
    else m_body += chunk;
    // An explicit protocol completion is sufficient even when a gateway leaves
    // the HTTP stream open. Defer until this chunk has been fully validated.
    if (m_reply && m_isStream && m_done) {
        const QPointer<QNetworkReply> reply = m_reply;
        QTimer::singleShot(0, this, [this, reply] { if (reply && m_reply == reply && m_done) finishReply(reply, false); });
    }
}

void AiClient::consumeLines(bool final)
{
    int newline;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        auto line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (m_provider.protocol == QLatin1String("ollama")) {
            if (!line.trimmed().isEmpty()) consumeEvent(line);
        } else if (line.isEmpty()) {
            if (!m_event.isEmpty()) consumeEvent(m_event);
            m_event.clear(); m_eventType.clear();
        } else if (line.startsWith("event:")) {
            m_eventType = line.mid(6).trimmed();
        } else if (line.startsWith("data:")) {
            if (!m_event.isEmpty()) m_event += '\n';
            m_event += line.mid(5).trimmed();
        }
    }
    if (final) {
        if (!m_buffer.trimmed().isEmpty()) {
            if (m_provider.protocol == QLatin1String("ollama")) consumeEvent(m_buffer);
            else if (m_buffer.startsWith("data:")) m_event += m_buffer.mid(5).trimmed();
        }
        if (!m_event.isEmpty()) consumeEvent(m_event);
        m_buffer.clear(); m_event.clear(); m_eventType.clear();
    }
}

void AiClient::consumeEvent(const QByteArray &event)
{
    if (event.trimmed() == "[DONE]") {
        // Responses has an explicit completion event. A transport sentinel alone
        // must not make an interrupted or incomplete response look successful.
        if (m_provider.protocol != QLatin1String("responses")) m_done = true;
        return;
    }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(event, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        m_error = QStringLiteral("流式响应格式不完整，请检查服务的接口协议后重试。"); return;
    }
    auto object = doc.object();
    if (m_provider.protocol == QLatin1String("responses") && !object.contains("type") && !m_eventType.isEmpty())
        object["type"] = QString::fromUtf8(m_eventType);
    consumeObject(object);
}

void AiClient::consumeObject(const QJsonObject &o)
{
    if (m_provider.protocol == QLatin1String("responses")) {
        consumeResponsesObject(o);
        return;
    }
    const auto error = o.value("error");
    if (!error.isUndefined() && !error.isNull()) {
        const auto detail = error.isObject() ? error.toObject().value("message").toString() : error.toString();
        m_error = QStringLiteral("服务错误：") + safeError(detail)
            + reasoningErrorHint(error.isObject() ? error.toObject() : QJsonObject{{"message", detail}});
        return;
    }
    QString token;
    if (m_provider.protocol == QLatin1String("ollama")) {
        if (o.value("done").toBool()) m_done = true;
        token = o.value("message").toObject().value("content").toString();
        if (o.value("done_reason").toString() == QLatin1String("length")) m_error = QStringLiteral("模型输出达到长度上限，请缩短原文后重试。");
    } else if (m_provider.protocol == QLatin1String("anthropic")) {
        if (o.value("type").toString() == QLatin1String("message_stop")) m_done = true;
        if (o.value("type").toString() == QLatin1String("content_block_delta")) token = o.value("delta").toObject().value("text").toString();
        else if (o.value("type").toString() == QLatin1String("content_block_start")) token = o.value("content_block").toObject().value("text").toString();
        else if (o.contains("content")) for (const auto &part : o.value("content").toArray()) token += part.toObject().value("text").toString();
        if (o.value("delta").toObject().value("stop_reason").toString() == QLatin1String("max_tokens") || o.value("stop_reason").toString() == QLatin1String("max_tokens")) m_error = QStringLiteral("模型输出达到长度上限，请缩短原文后重试。");
    } else if (m_provider.protocol == QLatin1String("gemini")) {
        const auto candidates = o.value("candidates").toArray();
        if (!candidates.isEmpty()) {
            const auto candidate = candidates.first().toObject();
            for (const auto &part : candidate.value("content").toObject().value("parts").toArray()) {
                if (!part.toObject().value("thought").toBool()) token += part.toObject().value("text").toString();
            }
            const auto reason = candidate.value("finishReason").toString();
            if (!reason.isEmpty()) m_done = true;
            if (reason == QLatin1String("MAX_TOKENS")) m_error = QStringLiteral("模型输出达到长度上限，请缩短原文后重试。");
            else if (!reason.isEmpty() && reason != QLatin1String("STOP")) m_error = QStringLiteral("模型未能完成这次请求，结果不完整，请调整输入后重试。");
        }
        if (o.value("promptFeedback").toObject().contains("blockReason")) m_error = QStringLiteral("模型未接受此请求，请调整输入后重试。");
    } else {
        const auto choices = o.value("choices").toArray();
        if (!choices.isEmpty()) {
            const auto choice = choices.first().toObject();
            token = choice.value("delta").toObject().value("content").toString();
            if (token.isEmpty()) token = choice.value("message").toObject().value("content").toString();
            const auto reason = choice.value("finish_reason").toString();
            if (!reason.isEmpty()) m_done = true;
            if (reason == QLatin1String("length")) m_error = QStringLiteral("模型输出达到长度上限，请缩短原文后重试。");
            else if (reason == QLatin1String("content_filter")) m_error = QStringLiteral("模型未能完成这次请求，请调整输入后重试。");
        }
    }
    if (!token.isEmpty()) { m_text += token; emit delta(token); }
}

void AiClient::consumeResponsesObject(const QJsonObject &object)
{
    const auto type = object.value("type").toString();
    const auto error = object.value("error");
    if ((!error.isNull() && !error.isUndefined()) || type == QLatin1String("error")) {
        const auto detail = type == QLatin1String("error") ? object.value("message").toString()
            : error.isObject() ? error.toObject().value("message").toString() : error.toString();
        m_error = QStringLiteral("服务错误：") + (detail.isEmpty() ? QStringLiteral("Responses 请求失败。") : safeError(detail))
            + reasoningErrorHint(type == QLatin1String("error") ? object : error.isObject() ? error.toObject() : QJsonObject{{"message", detail}});
        return;
    }
    if (type == QLatin1String("response.output_text.delta")) {
        if (m_done) return;
        const auto token = object.value("delta").toString();
        if (!token.isEmpty()) { m_text += token; emit delta(token); }
    } else if (type == QLatin1String("response.refusal.delta") || type == QLatin1String("response.refusal.done")) {
        m_error = QStringLiteral("模型未接受此请求，请调整输入后重试。");
    } else if (type == QLatin1String("response.completed") || type == QLatin1String("response.failed") || type == QLatin1String("response.incomplete")) {
        const auto response = object.value("response").toObject();
        auto status = type.mid(9);
        // A nested failure takes precedence over an inconsistent event label.
        const auto nestedStatus = response.value("status").toString();
        if (nestedStatus == QLatin1String("failed") || nestedStatus == QLatin1String("incomplete")) status = nestedStatus;
        finishResponse(response, status);
    } else if (object.value("object").toString() == QLatin1String("response") || (type.isEmpty() && object.contains("output"))) {
        finishResponse(object, object.value("status").toString());
    }
    // Text-done and output-item-done events repeat already-streamed text.
    // Reasoning, tool and lifecycle events are intentionally not translation text.
}

void AiClient::finishResponse(const QJsonObject &response, const QString &status)
{
    m_done = true;
    if (status == QLatin1String("failed")) {
        const auto detail = response.value("error").toObject().value("message").toString();
        m_error = QStringLiteral("Responses 请求失败。") + (detail.isEmpty() ? QString() : QLatin1Char('\n') + safeError(detail))
            + reasoningErrorHint(response.value("error").toObject());
        return;
    }
    if (status == QLatin1String("incomplete")) {
        const auto reason = response.value("incomplete_details").toObject().value("reason").toString();
        if (reason == QLatin1String("max_output_tokens")) m_error = QStringLiteral("模型输出达到长度上限，结果不完整，请缩短原文后重试。");
        else if (reason == QLatin1String("content_filter")) m_error = QStringLiteral("模型未能完成这次请求，请调整输入后重试。");
        else m_error = QStringLiteral("Responses 返回的结果不完整，请重试。");
        return;
    }
    if (status != QLatin1String("completed")) {
        m_error = QStringLiteral("Responses 尚未返回完整结果，请重试。");
        return;
    }
    QString text;
    for (const auto &value : response.value("output").toArray()) {
        const auto item = value.toObject();
        if (item.value("type").toString() != QLatin1String("message")) continue;
        const auto role = item.value("role").toString();
        if (!role.isEmpty() && role != QLatin1String("assistant")) continue;
        for (const auto &value : item.value("content").toArray()) {
            const auto part = value.toObject();
            if (part.value("type").toString() == QLatin1String("output_text")) text += part.value("text").toString();
            else if (part.value("type").toString() == QLatin1String("refusal"))
                m_error = QStringLiteral("模型未接受此请求，请调整输入后重试。");
        }
    }
    if (!text.isEmpty()) {
        // The final response is authoritative. Append only the missing suffix so
        // streamed deltas and the complete output do not duplicate each other.
        const auto suffix = text.startsWith(m_text) ? text.mid(m_text.size()) : QString();
        m_text = text;
        if (!suffix.isEmpty()) emit delta(suffix);
    }
}

QString AiClient::safeError(QString error) const
{
    if (!m_provider.apiKey.isEmpty()) error.replace(m_provider.apiKey, QStringLiteral("[已隐藏]"));
    error.replace(QRegularExpression(QStringLiteral("data:image/[^\\s,\"']+[,;][A-Za-z0-9+/=;,:-]*")), QStringLiteral("[图片已隐藏]"));
    error.replace(QRegularExpression(QStringLiteral("[A-Za-z0-9+/=]{80,}")), QStringLiteral("[数据已隐藏]"));
    return error.left(400);
}

QString AiClient::reasoningErrorHint(const QJsonObject &error) const
{
    if (!supportsReasoningEffort(m_provider.protocol) || m_provider.reasoningEffort.isEmpty()) return {};
    const auto param = error.value("param").toString().toLower();
    auto message = error.value("message").toString().toLower();
    if (message.isEmpty()) message = error.value("error").toString().toLower();
    const bool identified = param == QLatin1String("reasoning") || param.startsWith(QLatin1String("reasoning.")) || param == QLatin1String("reasoning_effort");
    const bool mentioned = message.contains(QLatin1String("reasoning")) || message.contains(QStringLiteral("思考等级"));
    const bool rejected = message.contains(QLatin1String("unsupported")) || message.contains(QLatin1String("not support"))
        || message.contains(QLatin1String("not allowed")) || message.contains(QLatin1String("invalid"))
        || message.contains(QLatin1String("unknown")) || message.contains(QLatin1String("unrecognized"))
        || message.contains(QStringLiteral("不支持")) || message.contains(QStringLiteral("无效"));
    if (identified || (mentioned && rejected))
        return QStringLiteral("\n当前模型或服务未接受所选思考设置。请在 AI 服务配置中将「思考等级」改为「跟随服务」后重试；「关闭思考」也需要模型支持。");
    return {};
}
