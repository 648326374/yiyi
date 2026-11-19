#pragma once
#include "aiclient.h"
#include "storage.h"
#include <QObject>
#include <QElapsedTimer>
#include <QSettings>
#include <memory>

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString inputText MEMBER m_inputText NOTIFY inputChanged)
    Q_PROPERTY(QString sourceLanguage READ sourceLanguage WRITE setSourceLanguage NOTIFY preferencesChanged)
    Q_PROPERTY(QString targetLanguage READ targetLanguage WRITE setTargetLanguage NOTIFY preferencesChanged)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY preferencesChanged)
    Q_PROPERTY(QString tone READ tone WRITE setTone NOTIFY preferencesChanged)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY preferencesChanged)
    Q_PROPERTY(QString selectedProviderId READ selectedProviderId WRITE selectProvider NOTIFY providersChanged)
    Q_PROPERTY(QVariantList providers READ providers NOTIFY providersChanged)
    Q_PROPERTY(QString providerName READ providerName NOTIFY providersChanged)
    Q_PROPERTY(QString modelName READ modelName NOTIFY providersChanged)
    Q_PROPERTY(QString resultText READ resultText NOTIFY resultChanged)
    Q_PROPERTY(QVariantMap dictionary READ dictionary NOTIFY resultChanged)
    Q_PROPERTY(QString resultKind READ resultKind NOTIFY resultChanged)
    Q_PROPERTY(bool briefResult READ briefResult NOTIFY resultChanged)
    Q_PROPERTY(QString resultProvider READ resultProvider NOTIFY resultChanged)
    Q_PROPERTY(QString resultModel READ resultModel NOTIFY resultChanged)
    Q_PROPERTY(bool example READ example NOTIFY resultChanged)
    Q_PROPERTY(bool favorite READ favorite NOTIFY resultChanged)
    Q_PROPERTY(bool canSave READ canSave NOTIFY resultChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool documentBusy READ documentBusy NOTIFY busyChanged)
    Q_PROPERTY(bool visionBusy READ visionBusy NOTIFY busyChanged)
    Q_PROPERTY(bool pdfBusy READ pdfBusy NOTIFY busyChanged)
    Q_PROPERTY(QString generationStatus READ generationStatus NOTIFY generationChanged)
    Q_PROPERTY(QString requestTiming READ requestTiming NOTIFY generationChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY recordsChanged)
    Q_PROPERTY(QVariantList favorites READ favorites NOTIFY recordsChanged)
    Q_PROPERTY(bool testBusy READ testBusy NOTIFY testChanged)
    Q_PROPERTY(QString testMessage READ testMessage NOTIFY testChanged)
    Q_PROPERTY(bool testSuccess READ testSuccess NOTIFY testChanged)
    Q_PROPERTY(QString dataDirectory READ dataDirectory CONSTANT)
public:
    explicit AppController(const QString &dataDirectory = {}, QObject *parent = nullptr);
    ~AppController() override;
    QString sourceLanguage() const { return m_sourceLanguage; }
    QString targetLanguage() const { return m_targetLanguage; }
    QString mode() const { return m_mode; }
    QString tone() const { return m_tone; }
    QString theme() const { return m_theme; }
    QString selectedProviderId() const { return m_selectedProviderId; }
    QVariantList providers() const;
    QString providerName() const;
    QString modelName() const;
    QString resultText() const { return m_resultText; }
    QVariantMap dictionary() const { return m_dictionary; }
    QString resultKind() const { return m_resultKind; }
    bool briefResult() const;
    QString resultProvider() const { return m_resultProvider; }
    QString resultModel() const { return m_resultModel; }
    bool example() const { return m_example; }
    bool favorite() const { return m_favorite; }
    bool canSave() const { return m_resultId > 0 && !m_busy && !m_example; }
    bool busy() const { return m_busy; }
    bool documentBusy() const { return m_documentBusy; }
    void setDocumentBusy(bool busy);
    bool visionBusy() const { return m_visionBusy; }
    void setVisionBusy(bool busy);
    bool pdfBusy() const { return m_pdfBusy; }
    void setPdfBusy(bool busy);
    AiProvider providerById(const QString &id, QString *error) const;
    AiProvider translationProvider(QString *error) const;
    QString generationStatus() const;
    QString requestTiming() const;
    QString error() const { return m_error; }
    QVariantList history() const { return m_history; }
    QVariantList favorites() const { return m_favorites; }
    bool testBusy() const { return m_testBusy; }
    QString testMessage() const { return m_testMessage; }
    bool testSuccess() const { return m_testSuccess; }
    QString dataDirectory() const { return m_directory; }
    void setSourceLanguage(const QString &value);
    void setTargetLanguage(const QString &value);
    void setMode(const QString &value);
    void setTone(const QString &value);
    void setTheme(const QString &value);
    Q_INVOKABLE void selectProvider(const QString &id);
    Q_INVOKABLE QString saveProvider(const QVariantMap &draft);
    Q_INVOKABLE void removeProvider(const QString &id);
    Q_INVOKABLE QVariantMap providerDraft(const QString &id) const;
    Q_INVOKABLE void testProvider(const QVariantMap &draft);
    Q_INVOKABLE void testVisionProvider(const QVariantMap &draft);
    Q_INVOKABLE void resetTest();
    Q_INVOKABLE void translate();
    Q_INVOKABLE void lookupSelection(const QString &text);
    Q_INVOKABLE bool importText(const QString &text);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clear();
    Q_INVOKABLE void showExample();
    Q_INVOKABLE void swapLanguages();
    Q_INVOKABLE void copyResult();
    Q_INVOKABLE void pasteInput();
    Q_INVOKABLE void toggleFavorite();
    Q_INVOKABLE void setRecordFavorite(qint64 id, bool favorite);
    Q_INVOKABLE void openRecord(qint64 id);
    Q_INVOKABLE void deleteRecord(qint64 id);
    Q_INVOKABLE void clearError();
    static bool looksLikeWord(const QString &text);
    static QVariantMap parseDictionary(const QString &text, const QString &term = {});
    static QVariantMap parseDictionaryPreview(const QString &text, const QString &term = {});
    static QVariantMap parseBriefDictionary(const QString &text, const QString &term);
signals:
    void inputChanged();
    void preferencesChanged();
    void providersChanged();
    void resultChanged();
    void busyChanged();
    void generationChanged();
    void errorChanged();
    void recordsChanged();
    void testChanged();
    void toast(const QString &message);
    void configurationRequested();
    void recordOpened();
private:
    AiProvider providerFromDraft(const QVariantMap &draft, QString *error) const;
    AiProvider currentProvider(QString *error) const;
    QVariantMap rawProvider(const QString &id) const;
    bool persistProviders(const QVariantList &providers, const QString &selected);
    void refreshRecords();
    void finish(const QString &text);
    void stopGeneration();
    void resetGeneration();
    void setError(const QString &message);
    QString displayDictionary(const QVariantMap &dictionary) const;
    QString m_directory;
    std::unique_ptr<QSettings> m_settings;
    std::unique_ptr<Storage> m_storage;
    AiClient m_client, m_probe;
    QTimer m_generationTimer;
    QElapsedTimer m_requestClock;
    qint64 m_firstTextMs = -1, m_totalMs = -1;
    QString m_streamText;
    bool m_previewDirty = false;
    QVariantList m_providers, m_history, m_favorites;
    QString m_inputText, m_sourceLanguage, m_targetLanguage, m_mode, m_tone, m_theme, m_selectedProviderId;
    QString m_resultText, m_resultKind, m_resultProvider, m_resultModel, m_error, m_testMessage;
    QVariantMap m_dictionary, m_request;
    qint64 m_resultId = -1;
    bool m_busy = false, m_example = false, m_favorite = false, m_testBusy = false, m_testSuccess = false;
    bool m_documentBusy = false;
    bool m_visionBusy = false;
    bool m_pdfBusy = false;
    QString m_visionTestCode;
};
