#pragma once
#include "selectioncapture.h"
#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QMap>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <memory>

class QWindow;
class QSystemTrayIcon;
class QMenu;
class QAction;

class DesktopIntegration : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool trayAvailable READ trayAvailable NOTIFY statusChanged)
    Q_PROPERTY(bool closeToTray READ closeToTray WRITE setCloseToTray NOTIFY preferencesChanged)
    Q_PROPERTY(bool shortcutsEnabled READ shortcutsEnabled NOTIFY preferencesChanged)
    Q_PROPERTY(QString showShortcut READ showShortcut NOTIFY preferencesChanged)
    Q_PROPERTY(QString selectionShortcut READ selectionShortcut NOTIFY preferencesChanged)
    Q_PROPERTY(QString shortcutError READ shortcutError NOTIFY statusChanged)
    Q_PROPERTY(bool selectionEnabled READ selectionEnabled WRITE setSelectionEnabled NOTIFY preferencesChanged)
    Q_PROPERTY(QString selectionError READ selectionError NOTIFY statusChanged)
    Q_PROPERTY(bool recordingShortcut MEMBER m_recordingShortcut)
    Q_PROPERTY(bool selectionVisible READ selectionVisible NOTIFY selectionChanged)
    Q_PROPERTY(int bubbleX READ bubbleX NOTIFY selectionChanged)
    Q_PROPERTY(int bubbleY READ bubbleY NOTIFY selectionChanged)
public:
    explicit DesktopIntegration(const QString &directory, bool nativeEnabled = true, QObject *parent = nullptr);
    ~DesktopIntegration() override;
    void setWindow(QWindow *window);
    void start();
    bool available() const;
    bool trayAvailable() const;
    bool closeToTray() const { return m_closeToTray; }
    bool shortcutsEnabled() const { return m_shortcutsEnabled; }
    QString showShortcut() const { return m_showShortcut; }
    QString selectionShortcut() const { return m_selectionShortcut; }
    QString shortcutError() const { return m_shortcutError; }
    bool selectionEnabled() const { return m_selectionEnabled; }
    QString selectionError() const { return m_selectionError; }
    bool selectionVisible() const { return !m_selection.isEmpty(); }
    int bubbleX() const { return m_bubblePosition.x(); }
    int bubbleY() const { return m_bubblePosition.y(); }
    void setCloseToTray(bool enabled);
    void setSelectionEnabled(bool enabled);
    Q_INVOKABLE bool applyShortcuts(bool enabled, const QString &show, const QString &selection);
    Q_INVOKABLE QString shortcutFromKey(int key, int modifiers) const;
    Q_INVOKABLE bool handleClose();
    Q_INVOKABLE void showWindow();
    Q_INVOKABLE void quit();
    Q_INVOKABLE void captureSelection();
    Q_INVOKABLE void acceptSelection();
    Q_INVOKABLE void dismissSelection();
    void nativeMouseEvent(int event, const QPoint &point);
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
    static QString normalizedShortcut(const QString &text);
signals:
    void preferencesChanged();
    void statusChanged();
    void shortcutRecorded(const QString &sequence);
    void selectionChanged();
    void lookupRequested(const QString &text);
    void workspaceRequested();
    void settingsRequested();
    void screenshotRequested();
    void toast(const QString &message);
private:
    bool registerShortcuts(bool enabled, const QString &show, const QString &selection);
    void updateSelectionHook();
    void deliverSelection(const QString &text, bool automatic, quintptr source);
    void save(const QString &key, const QVariant &value);
    QSettings m_settings;
    QPointer<QWindow> m_window;
    std::unique_ptr<QWindow> m_hotkeyWindow;
    std::unique_ptr<QSystemTrayIcon> m_tray;
    std::unique_ptr<QMenu> m_menu;
    QAction *m_selectionAction = nullptr;
    SelectionCapture m_capture;
    QTimer m_selectionDelay, m_bubbleTimer, m_foregroundTimer;
    bool m_nativeEnabled = true, m_started = false, m_exiting = false;
    bool m_closeToTray = true, m_shortcutsEnabled = true, m_selectionEnabled = true;
    bool m_recordingShortcut = false, m_wasMaximized = false, m_closeNoticeShown = false;
    QString m_showShortcut, m_selectionShortcut, m_shortcutError, m_selectionError, m_selection;
    QPoint m_bubblePosition, m_pressPoint, m_releasePoint, m_lastPressPoint;
    quintptr m_hook = 0, m_source = 0, m_bubbleSource = 0;
    qint64 m_lastPressTime = 0;
    bool m_pressed = false, m_doubleClick = false;
    QMap<int, QString> m_hotkeys;
    int m_showId = 0, m_selectionId = 0;
};
