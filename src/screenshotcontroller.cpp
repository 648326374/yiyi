#include "screenshotcontroller.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <QtMath>
#include <functional>

namespace {
class SelectionOverlay : public QWidget {
public:
    QImage image;
    std::function<void(const QRect &)> accepted;
    std::function<void()> cancelled;
    explicit SelectionOverlay(QScreen *screen, const QImage &frame) : image(frame)
    {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_QuitOnClose, false); setCursor(Qt::CrossCursor); setMouseTracking(true);
        setObjectName(QStringLiteral("screenshotSelectionOverlay"));
        setGeometry(screen->geometry()); setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(QStringLiteral("截图框选：拖动选择区域，Esc 或右键取消"));
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.drawImage(rect(), image); painter.fillRect(rect(), QColor(0, 0, 0, 95));
        if (m_dragging) {
            const QRect selection = QRect(m_start, m_end).normalized().intersected(rect());
            painter.save(); painter.setClipRect(selection); painter.drawImage(rect(), image); painter.restore();
            painter.setPen(QPen(QColor(QStringLiteral("#80d8a4")), 2)); painter.drawRect(selection.adjusted(0, 0, -1, -1));
        }
        const auto message = QStringLiteral("拖动框选文字区域 · 松开完成截图 · Esc / 右键取消");
        painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 11));
        const auto width = qMin(this->width() - 32, painter.fontMetrics().horizontalAdvance(message) + 32);
        const QRect hint((this->width() - width) / 2, 24, width, 44);
        painter.setPen(Qt::NoPen); painter.setBrush(QColor(30, 43, 36, 235)); painter.drawRoundedRect(hint, 10, 10);
        painter.setPen(Qt::white); painter.drawText(hint, Qt::AlignCenter, message);
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::RightButton) { if (cancelled) cancelled(); return; }
        if (event->button() != Qt::LeftButton) return;
        m_start = m_end = bounded(event->position().toPoint()); m_dragging = true; update();
    }
    void mouseMoveEvent(QMouseEvent *event) override { if (m_dragging) { m_end = bounded(event->position().toPoint()); update(); } }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton || !m_dragging) return;
        m_end = bounded(event->position().toPoint());
        const auto selection = QRect(m_start, m_end).normalized().intersected(rect());
        m_dragging = false;
        if (selection.width() >= 4 && selection.height() >= 4) { if (accepted) accepted(selection); }
        else update();
    }
    void keyPressEvent(QKeyEvent *event) override { if (event->key() == Qt::Key_Escape && cancelled) cancelled(); }
private:
    QPoint bounded(const QPoint &point) const { return {qBound(0, point.x(), width() - 1), qBound(0, point.y(), height() - 1)}; }
    QPoint m_start, m_end;
    bool m_dragging = false;
};
}

ScreenshotController::ScreenshotController(QObject *parent) : QObject(parent)
{
    connect(&m_ocr, &OcrService::changed, this, &ScreenshotController::changed);
    connect(&m_ocr, &OcrService::finished, this, [this] {
        if (m_ocr.error().isEmpty()) m_text = m_ocr.text();
        else if (!m_text.isEmpty()) m_error = m_ocr.error() + QStringLiteral(" 已保留上次原文及修改。");
        emit changed();
    });
}
ScreenshotController::~ScreenshotController() { m_restoreWindow = false; closeOverlays(); m_ocr.cancel(); }
void ScreenshotController::setWindow(QWindow *window) { m_window = window; }
void ScreenshotController::setText(const QString &text)
{
    if (busy() || m_text == text) return;
    m_text = text; emit changed();
}
void ScreenshotController::copyResult()
{
    if (!busy() && !m_text.trimmed().isEmpty()) QApplication::clipboard()->setText(m_text);
}
void ScreenshotController::setMode(const QString &mode)
{
    if (m_capturing || m_mode == mode || (mode != QLatin1String("local") && mode != QLatin1String("vision"))) return;
    m_ocr.cancel(); m_mode = mode; emit changed();
}
void ScreenshotController::acceptImage(const QImage &image)
{
    m_text.clear(); m_ocr.clear(); m_image = image; m_error.clear(); m_preview.clear();
    if (!image.isNull()) {
        QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
        image.scaled(1000, 700, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "PNG");
        m_preview = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
    }
    emit imageChanged();
    if (m_mode == QLatin1String("local")) recognize();
    emit changed();
}
void ScreenshotController::reviewImage(const QImage &image)
{
    if (busy() || image.isNull()) return;
    setMode(QStringLiteral("vision")); // Reviewing a document image never starts a request.
    acceptImage(image); emit reviewRequested();
}
void ScreenshotController::recognize()
{
    if (!busy() && hasImage()) { m_error.clear(); m_ocr.recognize(m_image); }
}
void ScreenshotController::clearImage()
{
    cancel(); m_image = {}; m_preview.clear(); m_text.clear(); m_ocr.clear(); m_error.clear(); emit imageChanged(); emit changed();
}
void ScreenshotController::capture()
{
    if (busy()) return;
    m_text.clear(); m_ocr.clear(); m_image = {}; m_preview.clear(); emit imageChanged();
    m_error.clear(); m_capturing = true;
    const auto generation = ++m_captureGeneration;
    m_restoreWindow = m_window && m_window->isVisible();
    if (m_window) m_window->hide();
    emit changed();
    QTimer::singleShot(220, this, [this, generation] { if (m_capturing && generation == m_captureGeneration) showOverlays(); });
}
void ScreenshotController::showOverlays()
{
    const auto screens = QGuiApplication::screens();
    QList<QPair<QScreen *, QImage>> frames;
    for (auto *screen : screens) {
        auto image = screen->grabWindow(0).toImage();
        image.setDevicePixelRatio(1);
        if (!image.isNull()) frames.append({screen, image});
    }
    if (frames.isEmpty()) {
        m_capturing = false; m_error = QStringLiteral("无法截取当前屏幕，请确认桌面可见且未锁定。");
        restoreWindow(); emit changed(); emit reviewRequested(); return;
    }
    SelectionOverlay *active = nullptr;
    for (const auto &frame : frames) {
        auto *overlay = new SelectionOverlay(frame.first, frame.second);
        overlay->accepted = [this, overlay](const QRect &selection) {
            const auto image = cropImage(overlay->image, overlay->size(), selection);
            closeOverlays(); m_capturing = false; restoreWindow();
            acceptImage(image); emit changed(); emit reviewRequested();
        };
        overlay->cancelled = [this] { cancel(); };
        m_overlays.append(overlay); overlay->show();
        if (!active || frame.first->geometry().contains(QCursor::pos())) active = overlay;
    }
    if (active) { active->raise(); active->activateWindow(); active->setFocus(); }
}
void ScreenshotController::closeOverlays()
{
    for (auto &overlay : m_overlays) if (overlay) { overlay->hide(); overlay->deleteLater(); }
    m_overlays.clear();
}
void ScreenshotController::restoreWindow()
{
    if (m_restoreWindow && m_window) { m_window->show(); emit restoreRequested(); }
    m_restoreWindow = false;
}
void ScreenshotController::cancel()
{
    if (m_ocr.busy()) m_error = m_text.isEmpty() ? QStringLiteral("本地识别已取消。") : QStringLiteral("本地识别已取消，已保留上次原文及修改。");
    ++m_captureGeneration;
    closeOverlays(); m_capturing = false; m_ocr.cancel(); restoreWindow(); emit changed();
}
QImage ScreenshotController::cropImage(const QImage &image, const QSize &logicalSize, const QRect &selection)
{
    if (image.isNull() || logicalSize.isEmpty()) return {};
    const auto area = selection.intersected(QRect(QPoint(0, 0), logicalSize));
    if (area.isEmpty()) return {};
    const qreal sx = qreal(image.width()) / logicalSize.width(), sy = qreal(image.height()) / logicalSize.height();
    const int left = qFloor(area.left() * sx), top = qFloor(area.top() * sy);
    const int right = qCeil((area.x() + area.width()) * sx), bottom = qCeil((area.y() + area.height()) * sy);
    return image.copy(QRect(left, top, right - left, bottom - top).intersected(image.rect()));
}
