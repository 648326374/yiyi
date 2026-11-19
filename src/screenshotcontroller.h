#pragma once
#include "ocrservice.h"
#include <QObject>
#include <QPointer>

class QWidget;
class QWindow;

class ScreenshotController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool capturing READ capturing NOTIFY changed)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString note READ note NOTIFY changed)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY changed)
    Q_PROPERTY(QString preview READ preview NOTIFY imageChanged)
    Q_PROPERTY(bool hasImage READ hasImage NOTIFY imageChanged)
public:
    explicit ScreenshotController(QObject *parent = nullptr);
    ~ScreenshotController() override;
    void setWindow(QWindow *window);
    bool busy() const { return m_capturing || m_ocr.busy(); }
    bool capturing() const { return m_capturing; }
    QString text() const { return m_text; }
    void setText(const QString &text);
    QString note() const { return m_ocr.note(); }
    QString error() const { return m_error.isEmpty() ? m_ocr.error() : m_error; }
    QString mode() const { return m_mode; }
    void setMode(const QString &mode);
    QString preview() const { return m_preview; }
    bool hasImage() const { return !m_image.isNull(); }
    QImage image() const { return m_image; }
    void acceptImage(const QImage &image);
    void reviewImage(const QImage &image);
    Q_INVOKABLE void recognize();
    Q_INVOKABLE void clearImage();
    Q_INVOKABLE void capture();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void copyResult();
    static QImage cropImage(const QImage &image, const QSize &logicalSize, const QRect &selection);
signals:
    void imageChanged();
    void changed();
    void reviewRequested();
    void restoreRequested();
private:
    void showOverlays();
    void closeOverlays();
    void restoreWindow();
    OcrService m_ocr;
    QPointer<QWindow> m_window;
    QList<QPointer<QWidget>> m_overlays;
    QString m_error, m_text;
    QString m_mode = QStringLiteral("local"), m_preview;
    QImage m_image;
    quint64 m_captureGeneration = 0;
    bool m_capturing = false, m_restoreWindow = false;
};
