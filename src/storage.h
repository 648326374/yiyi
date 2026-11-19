#pragma once
#include <QSqlDatabase>
#include <QVariantList>
#include <QVariantMap>

class Storage {
public:
    explicit Storage(const QString &directory);
    ~Storage();
    bool isOpen() const;
    QString error() const;
    qint64 add(const QVariantMap &record);
    QVariantList list(bool favoritesOnly = false) const;
    QVariantMap get(qint64 id) const;
    bool setFavorite(qint64 id, bool favorite);
    bool remove(qint64 id);
private:
    QString m_connection;
    QSqlDatabase m_db;
    mutable QString m_error;
};
