#include "pdftaskstore.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QRegularExpression>

PdfTaskStore::PdfTaskStore(int cacheKiB) : m_pages(qMax(0, cacheKiB)) {}
PdfTaskStore::~PdfTaskStore() { close(); }
bool PdfTaskStore::fail(const QString &detail) const {
    m_error = QStringLiteral("PDF 任务数据读写失败：%1").arg(detail); return false;
}
void PdfTaskStore::close() {
    m_pages.clear();
    if (m_transaction) rollback();
    if (m_db.isValid()) m_db.close();
    m_db = QSqlDatabase();
    if (!m_connection.isEmpty()) QSqlDatabase::removeDatabase(m_connection);
    m_connection.clear(); m_path.clear();
}
bool PdfTaskStore::open(const QString &path, bool readOnly) {
    close(); m_error.clear();
    if (!readOnly && !QDir().mkpath(QFileInfo(path).absolutePath())) return fail(QStringLiteral("无法创建任务目录"));
    m_connection = QStringLiteral("pdf-task-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    m_db.setDatabaseName(path);
    m_db.setConnectOptions(readOnly ? QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000") : QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!m_db.open()) return fail(m_db.lastError().text());
    m_path = QFileInfo(path).absoluteFilePath();
    QSqlQuery query(m_db);
    if (!readOnly) {
        for (const auto &statement : {
                 "PRAGMA journal_mode=WAL", "PRAGMA synchronous=FULL", "PRAGMA wal_autocheckpoint=256",
                 "CREATE TABLE IF NOT EXISTS metadata (id INTEGER PRIMARY KEY CHECK(id=1), data BLOB NOT NULL)",
                 "CREATE TABLE IF NOT EXISTS pages (number INTEGER PRIMARY KEY CHECK(number>=0), data BLOB NOT NULL, summary BLOB NOT NULL)"})
            if (!query.exec(QString::fromLatin1(statement))) return fail(query.lastError().text());
    }
    if (!query.exec(QStringLiteral("SELECT data FROM metadata WHERE id=1"))) return fail(query.lastError().text());
    return true;
}
QJsonObject PdfTaskStore::metadata() const {
    m_error.clear();
    QSqlQuery query(m_db);
    if (!isOpen() || !query.exec(QStringLiteral("SELECT data FROM metadata WHERE id=1"))) { fail(query.lastError().text()); return {}; }
    if (!query.next()) return {};
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(query.value(0).toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) { fail(QStringLiteral("任务信息损坏")); return {}; }
    return json.object();
}
bool PdfTaskStore::setMetadata(const QJsonObject &metadata) {
    m_error.clear();
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT INTO metadata(id,data) VALUES(1,?) ON CONFLICT(id) DO UPDATE SET data=excluded.data"));
    query.addBindValue(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    return (isOpen() && query.exec()) || fail(query.lastError().text());
}
QJsonObject PdfTaskStore::page(int number) const {
    m_error.clear();
    if (const auto *cached = m_pages.object(number)) return *cached;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT data FROM pages WHERE number=?")); query.addBindValue(number);
    if (!isOpen() || !query.exec()) { fail(query.lastError().text()); return {}; }
    if (!query.next()) return {};
    const auto bytes = query.value(0).toByteArray(); QJsonParseError error;
    const auto json = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) { fail(QStringLiteral("第 %1 页任务损坏").arg(number + 1)); return {}; }
    const auto page = json.object();
    const auto cost = qMax<qsizetype>(1, (bytes.size() + 1023) / 1024);
    if (cost <= m_pages.maxCost()) m_pages.insert(number, new QJsonObject(page), int(cost));
    return page;
}
QJsonArray PdfTaskStore::blockIssues(const QJsonObject &block) {
    QJsonArray issues;
    if (block["keep"].toBool()) return issues;
    const auto add = [&](const char *kind, const QString &reason) {
        issues.append(QJsonObject{{"kind", kind}, {"reason", reason}, {"blockId", block["id"]}, {"severity", "review"}});
    };
    static const QRegularExpression unclear(QStringLiteral("[\\[［]\\s*(?:unclear|unreadable|illegible|不清楚|不清晰|无法辨认|模糊)\\s*[\\]］]"), QRegularExpression::CaseInsensitiveOption);
    if (unclear.match(block["source"].toString()).hasMatch() || unclear.match(block["translation"].toString()).hasMatch())
        add("unclear", QStringLiteral("含不清晰文字，请对照原图校对"));
    if (block["stale"].toBool()) add("stale", QStringLiteral("原文已变动，译文需更新"));
    if (!block["translationError"].toString().isEmpty()) add("translation", block["translationError"].toString());
    if (!block["layoutError"].toString().isEmpty()) add("layout", block["layoutError"].toString());
    if (block["backgroundReview"].toBool() && !block["backgroundConfirmed"].toBool()) add("background", QStringLiteral("背景修复效果待确认"));
    return issues;
}
QJsonObject PdfTaskStore::summary(int number, const QJsonObject &page) {
    QJsonObject result{{"number", number}};
    for (const auto &key : {"width", "height", "warning", "notice", "kind", "baseLayoutHash", "analyzed", "analysisError", "extractionVersion"})
        if (page.contains(key)) result[key] = page[key];
    const bool scan = page["kind"] == "scan" || page.contains("scan");
    const auto data = scan ? page["scan"].toObject() : page;
    int total = 0, completed = 0, preserved = 0, review = 0;
    QJsonObject reasons;
    for (const auto &value : data["blocks"].toArray()) {
        const auto b = value.toObject();
        const auto issues = blockIssues(b);
        if (!issues.isEmpty()) ++review;
        for (const auto &issue : issues) { const auto kind = issue.toObject()["kind"].toString(); reasons[kind] = reasons[kind].toInt() + 1; }
        if (b["keep"].toBool() || b["backgroundMode"] == "original") ++preserved;
        if (b["fixed"].toBool() || b["keep"].toBool()) continue;
        ++total;
        if (!b["stale"].toBool() && !b["translation"].toString().trimmed().isEmpty()) ++completed;
    }
    result["scan"] = scan; result["recognized"] = scan && !data["source"].toString().trimmed().isEmpty();
    result["total"] = total; result["completed"] = completed; result["preserved"] = preserved;
    result["hasBlocks"] = !data["blocks"].toArray().isEmpty();
    result["issueVersion"] = 1; result["review"] = review; result["issues"] = reasons;
    return result;
}
bool PdfTaskStore::putPage(int number, const QJsonObject &page) {
    m_error.clear();
    if (number < 0 || page.isEmpty()) return fail(QStringLiteral("页码或页面数据无效"));
    const auto bytes = QJsonDocument(page).toJson(QJsonDocument::Compact);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT INTO pages(number,data,summary) VALUES(?,?,?) ON CONFLICT(number) DO UPDATE SET data=excluded.data,summary=excluded.summary"));
    query.addBindValue(number); query.addBindValue(bytes);
    query.addBindValue(QJsonDocument(summary(number, page)).toJson(QJsonDocument::Compact));
    if (!isOpen() || !query.exec()) return fail(query.lastError().text());
    m_pages.remove(number);
    const auto cost = qMax<qsizetype>(1, (bytes.size() + 1023) / 1024);
    if (cost <= m_pages.maxCost()) m_pages.insert(number, new QJsonObject(page), int(cost));
    return true;
}
QJsonArray PdfTaskStore::summaries() const {
    m_error.clear();
    QJsonArray result; QSqlQuery query(m_db);
    query.setForwardOnly(true);
    if (!isOpen() || !query.exec(QStringLiteral("SELECT summary FROM pages ORDER BY number"))) { fail(query.lastError().text()); return {}; }
    while (query.next()) {
        QJsonParseError error; const auto json = QJsonDocument::fromJson(query.value(0).toByteArray(), &error);
        if (error.error != QJsonParseError::NoError || !json.isObject()) { fail(QStringLiteral("页面目录损坏")); return {}; }
        result.append(json.object());
    }
    return result;
}
bool PdfTaskStore::contains(int number) const {
    m_error.clear();
    QSqlQuery query(m_db); query.prepare(QStringLiteral("SELECT 1 FROM pages WHERE number=?")); query.addBindValue(number);
    if (!isOpen() || !query.exec()) return fail(query.lastError().text());
    return query.next();
}
bool PdfTaskStore::begin() {
    m_error.clear();
    if (m_transaction) return fail(QStringLiteral("重复的任务事务"));
    if (!isOpen() || !m_db.transaction()) return fail(m_db.lastError().text());
    m_transaction = true; return true;
}
bool PdfTaskStore::commit() {
    m_error.clear();
    if (!m_transaction) return fail(QStringLiteral("任务事务未开始"));
    if (!m_db.commit()) { const auto detail = m_db.lastError().text(); rollback(); return fail(detail); }
    m_transaction = false; return true;
}
void PdfTaskStore::rollback() {
    if (m_transaction) m_db.rollback();
    m_transaction = false; m_pages.clear();
}
