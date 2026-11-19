#pragma once
#include <QByteArray>
#include <QDomDocument>
#include <QHash>
#include <QImage>
#include <QVariantList>

struct OfficeSegment {
    QString id, part, source, location, group;
    int paragraph = 0, page = 0;
    QList<int> nodes;
    bool mixedStyle = false;
    double width = 0, height = 0, fontSize = 18;
};

// OOXML package editing never invokes Office or sends XML to a model.
class OfficeDocument {
public:
    static OfficeDocument load(const QString &path);
    bool valid() const { return error.isEmpty() && !kind.isEmpty(); }
    bool save(const QString &path, const QHash<QString, QString> &translations,
              bool fitText, double minimumFont, QString *failure) const;
    QVariantList issues(const QHash<QString, QString> &translations, bool fitText, double minimumFont) const;
    QImage image(const QString &id, QString *failure) const;
    QString kind, path, hash, error;
    QList<OfficeSegment> segments;
    QVariantList notices;
    QVariantList images;
    int pages = 0, imageCount = 0;
private:
    QByteArray m_archive;
    QHash<QString, QDomDocument> m_parts;
    QHash<QString, double> fitScales(const QHash<QString, QString> &translations, bool fitText, double minimumFont) const;
};
