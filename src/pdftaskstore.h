#pragma once
#include <QCache>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>

// One database per source fingerprint. Page payloads never have to be read or
// rewritten as a single document. Connections belong to their creating thread.
class PdfTaskStore {
public:
    explicit PdfTaskStore(int cacheKiB = 8192);
    ~PdfTaskStore();
    Q_DISABLE_COPY(PdfTaskStore)
    bool open(const QString &path, bool readOnly = false);
    void close();
    bool isOpen() const { return m_db.isOpen(); }
    QString error() const { return m_error; }
    QString path() const { return m_path; }
    QJsonObject metadata() const;
    bool setMetadata(const QJsonObject &metadata);
    QJsonObject page(int number) const;
    bool putPage(int number, const QJsonObject &page);
    QJsonArray summaries() const;
    bool contains(int number) const;
    bool begin();
    bool commit();
    void rollback();
    static QJsonObject summary(int number, const QJsonObject &page);
    static QJsonArray blockIssues(const QJsonObject &block);
private:
    bool fail(const QString &detail) const;
    QSqlDatabase m_db;
    QString m_connection, m_path;
    mutable QString m_error;
    mutable QCache<int, QJsonObject> m_pages;
    bool m_transaction = false;
};
