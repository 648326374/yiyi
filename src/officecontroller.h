#pragma once
#include "aiclient.h"
#include "officedocument.h"
#include "documentcontroller.h"
#include <QFutureWatcher>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <memory>

class AppController;
class OfficeController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool processing READ processing NOTIFY changed)
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
    Q_PROPERTY(QString kind READ kind NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY activityChanged)
    Q_PROPERTY(QString taskDescription READ taskDescription NOTIFY changed)
    Q_PROPERTY(QVariantMap taskSettings READ taskSettings NOTIFY changed)
    Q_PROPERTY(QString preview READ preview NOTIFY previewChanged)
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int completed READ completed NOTIFY changed)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(QVariantList segments READ segments NOTIFY changed)
    Q_PROPERTY(QVariantList issues READ issues NOTIFY changed)
    Q_PROPERTY(QVariantList images READ images NOTIFY changed)
    Q_PROPERTY(QUrl suggestedOutput READ suggestedOutput NOTIFY changed)
    Q_PROPERTY(bool fitText READ fitText WRITE setFitText NOTIFY changed)
    Q_PROPERTY(double minimumFont READ minimumFont WRITE setMinimumFont NOTIFY changed)
    Q_PROPERTY(bool hasUnexportedResult READ hasUnexportedResult NOTIFY changed)
    Q_PROPERTY(QString draftId READ draftId NOTIFY draftChanged)
    Q_PROPERTY(QString draftText READ draftText NOTIFY draftChanged)
    Q_PROPERTY(bool draftDirty READ draftDirty NOTIFY draftChanged)
    Q_PROPERTY(bool canResume READ canResume NOTIFY changed)
    Q_PROPERTY(QString rendererPath READ rendererPath WRITE setRendererPath NOTIFY changed)
    Q_PROPERTY(QUrl layoutPreview READ layoutPreview NOTIFY changed)
public:
    explicit OfficeController(AppController *app, QObject *parent = nullptr);
    ~OfficeController() override;
    bool loaded() const { return m_document.valid(); }
    bool busy() const { return m_busy; }
    bool processing() const { return m_processing; }
    QString fileName() const;
    QString kind() const { return m_document.kind; }
    QString error() const { return m_error; }
    QString status() const;
    QString taskDescription() const;
    QVariantMap taskSettings() const;
    QString preview() const { return m_preview; }
    int activeIndex() const { return m_active; }
    int total() const { return m_document.segments.size(); }
    int completed() const { return m_translations.size(); }
    int pageCount() const { return m_document.pages; }
    QVariantList segments() const;
    QVariantList issues() const;
    QVariantList images() const { return m_document.images; }
    Q_INVOKABLE bool reviewImage(const QString &id);
    QUrl suggestedOutput() const;
    bool fitText() const { return m_fitText; }
    void setFitText(bool value);
    double minimumFont() const { return m_minimumFont; }
    void setMinimumFont(double value);
    bool hasUnexportedResult() const { return m_revision != m_exportedRevision || draftDirty(); }
    QString draftId() const { return m_draftId; }
    QString draftText() const { return m_draftText; }
    bool draftDirty() const { return !m_draftId.isEmpty() && m_draftText != m_translations.value(m_draftId); }
    Q_INVOKABLE void stageEdit(const QString &id, const QString &text);
    Q_INVOKABLE void discardDraft();
    bool canResume() const { return loaded() && completed() < total(); }
    QString rendererPath() const { return m_rendererPath; }
    void setRendererPath(const QString &value);
    QUrl layoutPreview() const { return m_layoutPreview; }
    Q_INVOKABLE bool loadFile(const QUrl &url);
    Q_INVOKABLE void start();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool editSegment(const QString &id, const QString &translation);
    Q_INVOKABLE bool keepOriginal(const QString &id);
    Q_INVOKABLE bool retrySegment(const QString &id);
    Q_INVOKABLE bool resetTranslations();
    Q_INVOKABLE bool saveFile(const QUrl &url);
    Q_INVOKABLE bool generateLayoutPreview();
    Q_INVOKABLE void openLayoutPreview();
signals:
    void changed();
    void previewChanged();
    void draftChanged();
    void activityChanged();
    void toast(const QString &message);
    void opened();
    void exported(const QUrl &url);
    void imageReviewRequested(const QImage &image);
private:
    void sendNext();
    void sendChunk();
    void queueNext(bool chunk = false);
    void startRenderer();
    bool prepareProvider();
    bool available();
    bool saveCache();
    void restoreCache();
    void fail(const QString &message);
    void stop();
    QString cachePath() const;
    int segmentIndex(const QString &id) const;
    AppController *m_app;
    AiClient m_client;
    OfficeDocument m_document;
    QHash<QString, QString> m_translations;
    QSet<QString> m_kept;
    QHash<QString, QString> m_failures;
    QList<DocumentPart> m_chunks;
    int m_chunk = 0;
    QString m_partial;
    QFutureWatcher<OfficeDocument> m_loader;
    QFutureWatcher<QString> m_exporter;
    QFutureWatcher<QString> m_previewPreparer;
    QProcess m_renderer;
    QTimer m_rendererTimer;
    QTimer m_draftTimer;
    std::unique_ptr<QTemporaryDir> m_previewDirectory;
    QString m_error, m_preview, m_providerId, m_providerName, m_source, m_target, m_tone, m_rendererPath;
    QString m_draftId, m_draftText;
    AiProvider m_provider;
    bool m_busy = false, m_processing = false, m_fitText = true, m_single = false, m_providerResolved = false;
    int m_active = -1;
    quint64 m_revision = 0, m_exportedRevision = 0;
    quint64 m_generation = 0;
    double m_minimumFont = 12;
    QUrl m_exportDestination, m_layoutPreview;
    mutable QVariantList m_segmentCache, m_issueCache;
    mutable bool m_segmentCacheDirty = true, m_issueCacheDirty = true;
};
