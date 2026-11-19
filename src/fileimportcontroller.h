#pragma once
#include <QObject>
#include <QUrl>
#include <QVariantMap>

class DocumentController;
class PdfController;
class OfficeController;

// File dialogs and external drops share validation and replacement protection.
class FileImportController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString textEncoding READ textEncoding WRITE setTextEncoding NOTIFY encodingChanged)
    Q_PROPERTY(QString textBlockReason READ textBlockReason NOTIFY availabilityChanged)
    Q_PROPERTY(QString pdfBlockReason READ pdfBlockReason NOTIFY availabilityChanged)
    Q_PROPERTY(QString officeBlockReason READ officeBlockReason NOTIFY availabilityChanged)
    Q_PROPERTY(QUrl replacementOutput READ replacementOutput NOTIFY pendingChanged)
    Q_PROPERTY(QString replacementKind READ replacementKind NOTIFY pendingChanged)
    Q_PROPERTY(bool confirmationPending READ confirmationPending NOTIFY pendingChanged)
    Q_PROPERTY(QString pendingFileName READ pendingFileName NOTIFY pendingChanged)
public:
    FileImportController(DocumentController *documents, PdfController *pdf, OfficeController *office = nullptr, QObject *parent = nullptr);
    QString textEncoding() const { return m_textEncoding; }
    void setTextEncoding(const QString &encoding);
    QString textBlockReason() const;
    QString pdfBlockReason() const;
    QString officeBlockReason() const;
    QUrl replacementOutput() const;
    QString replacementKind() const;
    bool confirmationPending() const { return !m_pendingUrl.isEmpty(); }
    QString pendingFileName() const;
    Q_INVOKABLE QVariantMap inspect(const QList<QUrl> &urls) const;
    Q_INVOKABLE bool requestDrop(const QList<QUrl> &urls);
    Q_INVOKABLE bool requestOpen(const QUrl &url);
    Q_INVOKABLE bool confirmReplacement();
    Q_INVOKABLE bool exportAndReplace(const QUrl &destination);
    Q_INVOKABLE void cancelReplacement();
signals:
    void encodingChanged();
    void availabilityChanged();
    void pendingChanged();
    void opened(const QString &kind);
    void toast(const QString &message);
private:
    bool open(const QUrl &url, const QString &encoding);
    QString blockReason(const QString &kind) const;
    DocumentController *m_documents;
    PdfController *m_pdf;
    OfficeController *m_office;
    QString m_textEncoding = QStringLiteral("auto"), m_pendingEncoding;
    QUrl m_pendingUrl;
};
