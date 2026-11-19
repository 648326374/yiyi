#include "storage.h"
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

Storage::Storage(const QString &directory)
    : m_connection(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    QDir().mkpath(directory);
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    m_db.setDatabaseName(QDir(directory).filePath(QStringLiteral("history.sqlite")));
    if (!m_db.open()) { m_error = m_db.lastError().text(); return; }
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    query.exec(QStringLiteral("PRAGMA busy_timeout=3000"));
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS records ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, input TEXT NOT NULL, output TEXT NOT NULL,"
            "kind TEXT NOT NULL, dictionary TEXT NOT NULL DEFAULT '{}',"
            "source TEXT NOT NULL, target TEXT NOT NULL, provider TEXT NOT NULL, model TEXT NOT NULL,"
            "created TEXT NOT NULL, favorite INTEGER NOT NULL DEFAULT 0)")))
        m_error = query.lastError().text();
}

Storage::~Storage()
{
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connection);
}
bool Storage::isOpen() const { return m_db.isOpen() && m_error.isEmpty(); }
QString Storage::error() const { return m_error; }

qint64 Storage::add(const QVariantMap &record)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT INTO records (input,output,kind,dictionary,source,target,provider,model,created) "
                                 "VALUES (:input,:output,:kind,:dictionary,:source,:target,:provider,:model,:created)"));
    for (const auto &key : {"input", "output", "kind", "source", "target", "provider", "model"})
        query.bindValue(QLatin1Char(':') + QString::fromLatin1(key), record.value(QLatin1String(key)).toString());
    query.bindValue(QStringLiteral(":dictionary"), QString::fromUtf8(QJsonDocument::fromVariant(record.value(QStringLiteral("dictionary"), QVariantMap{})).toJson(QJsonDocument::Compact)));
    query.bindValue(QStringLiteral(":created"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!query.exec()) { m_error = query.lastError().text(); return -1; }
    return query.lastInsertId().toLongLong();
}

static QVariantMap readRecord(const QSqlQuery &query)
{
    QVariantMap record;
    for (const auto &key : {"id", "input", "output", "kind", "source", "target", "provider", "model", "created", "favorite"})
        record[QLatin1String(key)] = query.value(QLatin1String(key));
    record[QStringLiteral("favorite")] = query.value(QStringLiteral("favorite")).toBool();
    record[QStringLiteral("dictionary")] = QJsonDocument::fromJson(query.value(QStringLiteral("dictionary")).toString().toUtf8()).object().toVariantMap();
    record[QStringLiteral("dateLabel")] = QDateTime::fromString(record.value(QStringLiteral("created")).toString(), Qt::ISODateWithMs).toLocalTime().toString(QStringLiteral("MM.dd  HH:mm"));
    return record;
}

QVariantList Storage::list(bool favoritesOnly) const
{
    QSqlQuery query(m_db);
    const QString sql = QStringLiteral("SELECT * FROM records") + (favoritesOnly ? QStringLiteral(" WHERE favorite=1") : QString()) + QStringLiteral(" ORDER BY id DESC");
    if (!query.exec(sql)) { m_error = query.lastError().text(); return {}; }
    QVariantList records;
    while (query.next()) records.append(readRecord(query));
    return records;
}

QVariantMap Storage::get(qint64 id) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM records WHERE id=?"));
    query.addBindValue(id);
    if (query.exec() && query.next()) return readRecord(query);
    return {};
}

bool Storage::setFavorite(qint64 id, bool favorite)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE records SET favorite=? WHERE id=?"));
    query.addBindValue(favorite);
    query.addBindValue(id);
    if (!query.exec()) { m_error = query.lastError().text(); return false; }
    return query.numRowsAffected() > 0;
}

bool Storage::remove(qint64 id)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM records WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec()) { m_error = query.lastError().text(); return false; }
    return query.numRowsAffected() > 0;
}
