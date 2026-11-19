#pragma once
#include "aiclient.h"
#include "pdftaskstore.h"
#include <QJsonArray>
#include <QProcess>
#include <QTemporaryDir>
#include <QRectF>
#include <QSettings>
#include <memory>

class AppController;
class PdfController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool processing READ processing NOTIFY changed)
    Q_PROPERTY(bool rendering READ rendering NOTIFY changed)
    Q_PROPERTY(bool analyzing READ analyzing NOTIFY changed)
    Q_PROPERTY(int analyzedPages READ analyzedPages NOTIFY changed)
    Q_PROPERTY(bool analysisComplete READ analysisComplete NOTIFY changed)
    Q_PROPERTY(bool pageReady READ pageReady NOTIFY changed)
    Q_PROPERTY(bool resetting READ resetting NOTIFY changed)
    Q_PROPERTY(bool resetPending READ resetPending NOTIFY changed)
    Q_PROPERTY(QString operationStatus READ operationStatus NOTIFY changed)
    Q_PROPERTY(double operationProgress READ operationProgress NOTIFY changed)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY changed)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int completed READ completed NOTIFY changed)
    Q_PROPERTY(int pageTotal READ pageTotal NOTIFY changed)
    Q_PROPERTY(int pageCompleted READ pageCompleted NOTIFY changed)
    Q_PROPERTY(QString pageStatus READ pageStatus NOTIFY changed)
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString warning READ warning NOTIFY changed)
    Q_PROPERTY(QString pageNotice READ pageNotice NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString taskDescription READ taskDescription NOTIFY changed)
    Q_PROPERTY(QString taskModel READ taskModel NOTIFY changed)
    Q_PROPERTY(QString taskProviderId READ taskProviderId NOTIFY changed)
    Q_PROPERTY(QString taskTarget READ taskTarget NOTIFY changed)
    Q_PROPERTY(int requestTimeoutMinutes READ requestTimeoutMinutes WRITE setRequestTimeoutMinutes NOTIFY changed)
    Q_PROPERTY(bool taskPending READ taskPending NOTIFY changed)
    Q_PROPERTY(QString taskScope READ taskScope NOTIFY changed)
    Q_PROPERTY(QString activityStatus READ activityStatus NOTIFY changed)
    Q_PROPERTY(double activityProgress READ activityProgress NOTIFY changed)
    Q_PROPERTY(int pendingPages READ pendingPages NOTIFY changed)
    Q_PROPERTY(int reviewPages READ reviewPages NOTIFY changed)
    Q_PROPERTY(bool issueIndexing READ issueIndexing NOTIFY changed)
    Q_PROPERTY(QVariantList pageIssues READ pageIssues NOTIFY changed)
    Q_PROPERTY(QVariantList blocks READ blocks NOTIFY changed)
    Q_PROPERTY(QVariantList layoutRegions READ layoutRegions NOTIFY changed)
    Q_PROPERTY(bool canUndoLayout READ canUndoLayout NOTIFY changed)
    Q_PROPERTY(QUrl originalPreview READ originalPreview NOTIFY changed)
    Q_PROPERTY(QUrl translatedPreview READ translatedPreview NOTIFY changed)
    Q_PROPERTY(QUrl suggestedOutput READ suggestedOutput NOTIFY changed)
    Q_PROPERTY(double pageRatio READ pageRatio NOTIFY changed)
    Q_PROPERTY(double pageWidth READ pageWidth NOTIFY changed)
    Q_PROPERTY(int unsupportedPages READ unsupportedPages NOTIFY changed)
    Q_PROPERTY(int preservedRegions READ preservedRegions NOTIFY changed)
    Q_PROPERTY(int pagePreservedRegions READ pagePreservedRegions NOTIFY changed)
    Q_PROPERTY(bool recognizing READ recognizing NOTIFY changed)
    Q_PROPERTY(bool scanMode READ scanMode NOTIFY changed)
    Q_PROPERTY(bool layoutReady READ layoutReady NOTIFY changed)
    Q_PROPERTY(QString recognitionDraft READ recognitionDraft NOTIFY changed)
    Q_PROPERTY(QString scanSource READ scanSource WRITE setScanSource NOTIFY changed)
    Q_PROPERTY(QString scanTranslation READ scanTranslation WRITE setScanTranslation NOTIFY changed)
    Q_PROPERTY(bool translationStale READ translationStale NOTIFY changed)
    Q_PROPERTY(QString visionProviderId READ visionProviderId WRITE setVisionProviderId NOTIFY changed)
    Q_PROPERTY(QString regionText READ regionText NOTIFY regionRecognized)
    Q_PROPERTY(QString recognitionStatus READ recognitionStatus NOTIFY changed)
    Q_PROPERTY(int scanPages READ scanPages NOTIFY changed)
    Q_PROPERTY(int recognizedPages READ recognizedPages NOTIFY changed)
    Q_PROPERTY(int translatedPages READ translatedPages NOTIFY changed)
public:
    explicit PdfController(AppController *app, QObject *parent = nullptr);
    ~PdfController() override;
    bool loaded() const { return m_ready; }
    bool busy() const { return m_busy || m_recognizing || m_workflowActive; }
    bool recognizing() const { return m_recognizing; }
    bool scanMode() const { return isScanPage(m_page); }
    bool layoutReady() const { return !rawBlocks(m_page).isEmpty(); }
    QString recognitionDraft() const;
    QString scanSource() const;
    QString scanTranslation() const;
    bool translationStale() const;
    void setScanSource(const QString &text);
    void setScanTranslation(const QString &text);
    QString visionProviderId() const { return m_visionProviderId; }
    void setVisionProviderId(const QString &id);
    QString regionText() const { return m_regionText; }
    QString recognitionStatus() const;
    int scanPages() const;
    int recognizedPages() const;
    int translatedPages() const;
    bool processing() const { return m_opening || m_exportRequested || resetPending() || m_action == "info" || m_action == "export"; }
    bool resetting() const { return m_resetting; }
    bool resetPending() const { return m_resetPage >= 0; }
    bool rendering() const { return m_action == "render" || m_renderPending; }
    bool analyzing() const { return m_analyzing; }
    int analyzedPages() const { return m_counts.analyzed; }
    bool analysisComplete() const { return loaded() && m_counts.analyzed == pageCount(); }
    bool pageReady() const { return isPageReady(m_page); }
    QString operationStatus() const;
    double operationProgress() const;
    int page() const { return m_page; }
    void setPage(int page);
    int pageCount() const { return m_document["pageCount"].toInt(); }
    int total() const;
    int completed() const;
    int pageTotal() const;
    int pageCompleted() const;
    QString pageStatus() const;
    QString fileName() const;
    QString error() const { return m_error; }
    QString warning() const;
    QString pageNotice() const;
    QString status() const;
    QString taskDescription() const;
    QString taskModel() const;
    QString taskProviderId() const;
    QString taskTarget() const;
    int requestTimeoutMinutes() const { return m_client.inactivityTimeout() / 60000; }
    void setRequestTimeoutMinutes(int minutes);
    bool taskPending() const { return m_workflow["kind"] == "translate" || m_workflow["kind"] == "retranslate"; }
    QString taskScope() const;
    QString activityStatus() const;
    double activityProgress() const;
    int pendingPages() const;
    int reviewPages() const;
    bool issueIndexing() const { return m_issueIndexCursor >= 0; }
    QVariantList pageIssues() const;
    Q_INVOKABLE QVariantList issuePages() const;
    Q_INVOKABLE void translate(bool currentPageOnly = false, int pixels = 1800);
    Q_INVOKABLE bool retranslatePage();
    Q_INVOKABLE void resumeTask();
    Q_INVOKABLE void discardTask();
    QVariantList blocks() const;
    QVariantList layoutRegions() const;
    bool canUndoLayout() const { return m_layoutUndo.contains(m_page); }
    QUrl originalPreview() const { return m_original; }
    QUrl translatedPreview() const { return m_translated; }
    QUrl suggestedOutput() const;
    double pageRatio() const;
    double pageWidth() const { return pageObject(m_page)["width"].toDouble(595); }
    int unsupportedPages() const;
    int preservedRegions() const;
    int pagePreservedRegions() const;
    Q_INVOKABLE bool loadFile(const QUrl &url);
    Q_INVOKABLE void start(bool currentPageOnly = false);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void pauseAnalysis();
    Q_INVOKABLE void resumeAnalysis();
    Q_INVOKABLE void resetTranslations();
    Q_INVOKABLE bool editBlock(const QString &id, const QString &translation);
    Q_INVOKABLE bool retryBlock(const QString &id);
    Q_INVOKABLE bool reviseBlock(const QString &id, const QVariantMap &changes);
    Q_INVOKABLE bool reviseRegion(const QString &id, const QVariantMap &changes);
    Q_INVOKABLE bool moveBlock(const QString &id, int position);
    Q_INVOKABLE bool mergeBlocks(const QStringList &ids);
    Q_INVOKABLE bool splitBlock(const QString &id, bool vertical, double position, const QString &firstSource, const QString &secondSource);
    Q_INVOKABLE bool undoLayout();
    Q_INVOKABLE bool exportFile(const QUrl &url, bool preserveFailed = false);
    Q_INVOKABLE void recognizePages(int first, int last, int pixels = 1800, bool force = false);
    Q_INVOKABLE void recognizeRegion(const QRectF &region, int pixels = 1800);
    Q_INVOKABLE void copyText(const QString &text);
signals:
    void changed();
    void toast(const QString &message);
    void regionRecognized();
    void exported(const QUrl &file, int preservedBlocks, int preservedPages, const QUrl &report, const QString &warning);
private:
    bool isScanPage(int page) const;
    QJsonArray effectiveBlocks(int page) const;
    QJsonArray rawBlocks(int page) const;
    QJsonArray regionsForPage(int page) const;
    bool commitStructure(const QJsonArray &blocks, const QJsonArray &regions);
    bool structureError(const QString &message);
    void updateScan(int page, const QJsonObject &scan);
    bool prepareRecognition(bool workflow = false);
    void sendNextRecognition();
    void recognitionCompleted(const QString &text);
    void sendRecognitionBlock();
    void stopRecognition(const QString &error = {});
    QJsonObject pageObject(int page) const;
    void setPageObject(int page, const QJsonObject &value, bool linkHeaders = false);
    void setPageSummary(int page, const QJsonObject &value);
    void applyPageSummary(int page, const QJsonObject &summary);
    bool isPageReady(int page) const;
    bool acceptPages(const QJsonArray &pages);
    void scheduleWork();
    void pump();
    void resetBatch();
    void workerProgress();
    bool runWorker(const QString &action, QJsonObject request);
    void processFinished(int code, QProcess::ExitStatus status);
    void workerFailed(const QString &error);
    void stopWorker();
    void requestRender();
    void updateStatuses(int page, const QJsonArray &statuses, bool persist = true);
    void updateBlock(int page, int block, const QJsonObject &value);
    bool prepareSnapshot();
    void beginTranslation(int first, int last);
    void pauseTask();
    bool beginWorkflowRecognition(int page);
    bool stageTranslation(const QString &id, const QString &translation);
    bool commitRetranslation();
    QString retranslationSignature(int page) const;
    void indexIssuesBatch();
    void sendNext();
    void sendPart();
    void rejectTranslation(const QString &error);
    QString deferredTranslationError();
    void stopTranslation(const QString &error = {});
    bool saveCache();
    bool restoreCache();
    QString cachePath() const;
    AppController *m_app;
    QSettings m_settings;
    AiClient m_client;
    QProcess m_worker;
    QTimer m_timeout;
    QTimer m_cacheTimer;
    QTimer m_workTimer;
    std::unique_ptr<QTemporaryDir> m_operationDirectory;
    QTemporaryDir m_previewDirectory;
    QJsonObject m_document, m_snapshot;
    QJsonObject m_workflow;
    bool m_workflowActive = false;
    int m_issueIndexCursor = -1;
    PdfTaskStore m_store;
    PdfTaskStore m_undoStore;
    QVector<QJsonObject> m_pageSummaries;
    QVector<quint64> m_pageRevisions;
    QHash<int, QJsonObject> m_dirtyPages;
    struct Counts { int total = 0, completed = 0, unsupported = 0, preserved = 0, scans = 0, recognized = 0, translated = 0, analyzed = 0; } m_counts;
    QSet<int> m_layoutUndo;
    AiProvider m_provider;
    QString m_path, m_loadingPath, m_error, m_action, m_exportPath;
    QUrl m_original, m_translated;
    QList<QPair<int, int>> m_queue;
    QStringList m_parts;
    QStringList m_partSeparators;
    QString m_pendingTranslation;
    QString m_translationSuffix;
    QString m_retryBlockKey;
    int m_translationRetry = 0, m_consecutiveRejectedBlocks = 0;
    bool m_translationRetryPending = false;
    int m_page = 0, m_renderPage = 0, m_queueIndex = 0, m_partIndex = 0;
    int m_analysisCursor = 0, m_inspectFirst = -1, m_inspectCount = 0;
    int m_translationPage = 0, m_translationLast = -1;
    int m_resetPage = -1;
    bool m_resetting = false;
    bool m_ready = false, m_opening = false, m_analyzing = false, m_exportRequested = false;
    bool m_analysisBeforeExport = false, m_waitingTranslation = false, m_waitingRecognition = false;
    bool m_preserveExportFailures = false;
    QString m_progressStage;
    qint64 m_progressCompleted = 0, m_progressTotal = 0;
    QByteArray m_progressBuffer;
    quint64 m_revision = 0, m_renderRevision = 0, m_previewNumber = 0;
    quint64 m_taskGeneration = 0;
    bool m_busy = false, m_renderPending = false;
    AiProvider m_visionProvider;
    QString m_visionProviderId, m_regionText;
    QList<int> m_recognitionQueue;
    QRectF m_recognitionRegion;
    int m_recognitionIndex = 0, m_recognitionPixels = 1800;
    bool m_recognizing = false, m_recognitionForce = false;
    QImage m_recognitionImage;
    QJsonArray m_recognitionBlocks;
    QJsonObject m_recognitionLayout;
    int m_recognitionBlock = 0;
};
