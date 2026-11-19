#pragma once
#include <QPainter>
#include <QPdfWriter>
#include <QPageSize>
#include <QLinearGradient>
#include <QFile>
#include <cmath>

inline void makeSharedTextPdfFixture(const QString &path, const QByteArray &text = "Name                    42", const QByteArray &font = "Helvetica") {
    const QByteArray content = "BT /F1 12 Tf 1 0 0 1 30 320 Tm (" + text + ") Tj ET";
    const QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /" + font + " >>",
        "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "\nendstream"};
    QByteArray bytes = "%PDF-1.7\n"; QList<int> offsets{0};
    for (int i = 0; i < objects.size(); ++i) { offsets.append(bytes.size()); bytes += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n"; }
    const int xref = bytes.size(); bytes += "xref\n0 " + QByteArray::number(offsets.size()) + "\n0000000000 65535 f \n";
    for (int i = 1; i < offsets.size(); ++i) bytes += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
    bytes += "trailer\n<< /Size " + QByteArray::number(offsets.size()) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    QFile file(path); if (file.open(QIODevice::WriteOnly)) file.write(bytes);
}

inline void makeShadingPdfFixture(const QString &path) {
    const QByteArray content = "q 40 120 200 140 re W n /Gradient sh Q BT /F1 14 Tf 1 0 0 1 40 335 Tm (Gradient artwork) Tj ET";
    const QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << /Font << /F1 4 0 R >> /Shading << /Gradient 6 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "\nendstream",
        "<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [40 260 240 120] /Function 7 0 R /Extend [true true] >>",
        "<< /FunctionType 2 /Domain [0 1] /C0 [1 1 1] /C1 [0.14 0.44 0.75] /N 1 >>"};
    QByteArray bytes = "%PDF-1.7\n"; QList<int> offsets{0};
    for (int i = 0; i < objects.size(); ++i) { offsets.append(bytes.size()); bytes += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n"; }
    const int xref = bytes.size(); bytes += "xref\n0 " + QByteArray::number(offsets.size()) + "\n0000000000 65535 f \n";
    for (int i = 1; i < offsets.size(); ++i) bytes += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
    bytes += "trailer\n<< /Size " + QByteArray::number(offsets.size()) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    QFile file(path); if (file.open(QIODevice::WriteOnly)) file.write(bytes);
}

inline void makeNestedPdfFixture(const QString &path) {
    QList<QByteArray> objects;
    objects << "<< /Type /Catalog /Pages 2 0 R >>" << "<< /Type /Pages /Count 2 /Kids [3 0 R 4 0 R] >>";
    for (int i = 0; i < 2; ++i) objects << QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << /XObject << /Form 5 0 R >> >> /Contents ") + QByteArray::number(6 + i) + " 0 R >>";
    auto stream = [](const QByteArray &head, const QByteArray &body) { return "<< " + head + " /Length " + QByteArray::number(body.size()) + " >>\nstream\n" + body + "\nendstream"; };
    objects << stream("/Type /XObject /Subtype /Form /BBox [0 0 260 300] /Resources << /ColorSpace << /CS0 [/CalRGB << /WhitePoint [0.9505 1 1.089] >>] >> /XObject << /Nested 9 0 R >> >>", "0.9 0.7 0 rg 20 20 30 30 re f /CS0 cs 1 1 1 scn 25 25 20 20 re f q 1 0 0 1 10 20 cm /Nested Do Q");
    objects << stream({}, "q 1 0 0 1 15 30 cm /Form Do Q") << stream({}, "q 1 0 0 1 25 40 cm /Form Do Q");
    objects << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
    objects << stream("/Type /XObject /Subtype /Form /BBox [0 0 250 280] /Resources << /Font << /F1 8 0 R >> >>", "0 0 0 rg BT /F1 1 Tf 12 0 0 12 20 230 Tm (Nested sample text) Tj ET");
    QByteArray bytes = "%PDF-1.7\n"; QList<int> offsets{0};
    for (int i = 0; i < objects.size(); ++i) { offsets.append(bytes.size()); bytes += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n"; }
    const int xref = bytes.size(); bytes += "xref\n0 " + QByteArray::number(offsets.size()) + "\n0000000000 65535 f \n";
    for (int i = 1; i < offsets.size(); ++i) bytes += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
    bytes += "trailer\n<< /Size " + QByteArray::number(offsets.size()) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    QFile file(path); file.open(QIODevice::WriteOnly); file.write(bytes);
}

inline void makeSplitContentPdfFixture(const QString &path) {
    // A content array is one logical stream. Text operands, graphics state,
    // and an unfinished path can span physical streams.
    const QList<QByteArray> streams{
        "BT /F1 16 Tf 1 0 0 1 30 350 Tm [(First title)] ",
        "TJ ET q 1 0 0 1 60 90 cm 0.2 0.4 0.6 RG 2 w 0 0 m 30 30 l ",
        "60 0 l ",
        "90 30 l S Q BT /F1 12 Tf 1 0 0 1 30 300 Tm (Second paragraph) Tj ET"};
    QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << /Font << /F1 4 0 R >> >> /Contents [5 0 R 6 0 R 7 0 R 8 0 R] >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
    for (const auto &stream : streams) objects << "<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream + "\nendstream";
    QByteArray bytes = "%PDF-1.7\n"; QList<int> offsets{0};
    for (int i = 0; i < objects.size(); ++i) { offsets.append(bytes.size()); bytes += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n"; }
    const int xref = bytes.size(); bytes += "xref\n0 " + QByteArray::number(offsets.size()) + "\n0000000000 65535 f \n";
    for (int i = 1; i < offsets.size(); ++i) bytes += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
    bytes += "trailer\n<< /Size " + QByteArray::number(offsets.size()) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    QFile file(path); if (file.open(QIODevice::WriteOnly)) file.write(bytes);
}

inline void makePatternPdfFixture(const QString &path, bool nested) {
    const QByteArray artwork = "q /Pattern cs /P scn 40 100 160 120 re f Q";
    const QByteArray content = "BT /F1 16 Tf 1 0 0 1 30 350 Tm (Pattern illustration) Tj ET " + (nested ? QByteArray("/Form Do") : artwork);
    const auto stream = [](const QByteArray &head, const QByteArray &body) { return "<< " + head + " /Length " + QByteArray::number(body.size()) + " >>\nstream\n" + body + "\nendstream"; };
    const QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << /Font << /F1 4 0 R >> "
            + QByteArray(nested ? "/XObject << /Form 6 0 R >>" : "/Pattern << /P 7 0 R >>") + " >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>", stream({}, content),
        stream("/Type /XObject /Subtype /Form /BBox [0 0 300 400] /Resources << /Pattern << /P 7 0 R >> >>", artwork),
        stream("/Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 8 8] /XStep 8 /YStep 8 /Resources << >>", "0.2 0.4 0.6 RG .5 w 0 0 m 8 8 l S")};
    QByteArray bytes = "%PDF-1.7\n"; QList<int> offsets{0};
    for (int i = 0; i < objects.size(); ++i) { offsets.append(bytes.size()); bytes += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n"; }
    const int xref = bytes.size(); bytes += "xref\n0 " + QByteArray::number(offsets.size()) + "\n0000000000 65535 f \n";
    for (int i = 1; i < offsets.size(); ++i) bytes += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
    bytes += "trailer\n<< /Size " + QByteArray::number(offsets.size()) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    QFile file(path); if (file.open(QIODevice::WriteOnly)) file.write(bytes);
}

struct BackgroundFixture {
    QImage clean, withText;
    QList<QRect> lines;
    QString source;
};
inline BackgroundFixture makeBackgroundFixture(const QString &kind, bool lightText = true) {
    BackgroundFixture result; result.clean = QImage(1200, 720, QImage::Format_RGB32);
    if (kind == "photo") {
        const QImage photo(qEnvironmentVariable("YIYI_PDF_PHOTO_FILE"));
        result.clean = photo.copy(0, photo.height() * 2 / 5, photo.width() / 2, photo.height() / 2)
            .scaled(1200, 720, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
    } else if (kind == "texture") {
        for (int y = 0; y < result.clean.height(); ++y) for (int x = 0; x < result.clean.width(); ++x) {
            const int noise = int((quint32(x * 7919 + y * 104729) * 2654435761u) >> 26) - 32;
            const int base = (lightText ? 70 : 175) + noise + qRound(22 * std::sin(x * .031) * std::cos(y * .049));
            result.clean.setPixelColor(x, y, QColor(qBound(0, base, 255), qBound(0, base + 20, 255), qBound(0, base + 40, 255)));
        }
    } else {
        QPainter background(&result.clean);
        if (kind == "gradient") { QLinearGradient gradient(0, 0, 1200, 720); gradient.setColorAt(0, QColor("#28526f")); gradient.setColorAt(1, QColor("#327644")); background.fillRect(result.clean.rect(), gradient); }
        else background.fillRect(result.clean.rect(), kind == "orange" ? QColor("#ff9818") : lightText ? QColor("#294767") : QColor("#dfd5bd"));
        background.end();
    }
    { QPainter artwork(&result.clean); artwork.setPen(Qt::NoPen); artwork.setBrush(QColor("#d9ab4e")); artwork.drawEllipse(QRectF(970, 480, 120, 120)); }
    result.withText = result.clean.copy(); QPainter painter(&result.withText); painter.setRenderHint(QPainter::TextAntialiasing);
    QFont font("Arial"); font.setPixelSize(38); font.setBold(true); painter.setFont(font); painter.setPen(lightText ? Qt::white : Qt::black);
    const QStringList text{"Read the words on this picture.", "Keep the original background."};
    for (int i = 0; i < text.size(); ++i) { const QPoint origin(90, 220 + i * 65); painter.drawText(origin, text[i]); result.lines.append(QFontMetrics(font).boundingRect(text[i]).translated(origin).adjusted(-5, -5, 5, 5)); }
    painter.end(); result.source = text.join(' '); return result;
}
inline void saveBackgroundPdf(const QString &path, const BackgroundFixture &fixture) {
    QPdfWriter pdf(path); pdf.setResolution(72); pdf.setPageSize(QPageSize(QSizeF(600, 360), QPageSize::Point)); pdf.setPageMargins(QMarginsF());
    QPainter painter(&pdf); painter.drawImage(QRectF(0, 0, 600, 360), fixture.withText);
}

inline void drawStructuredPage(QPainter &painter, int columns = 2, bool merged = false, bool background = true, bool verticalMerge = false) {
    painter.fillRect(QRectF(0, 0, 595, 842), Qt::white);
    QFont font("Times New Roman"); font.setPixelSize(18); font.setBold(true); painter.setFont(font); painter.setPen(Qt::black);
    painter.drawText(QPointF(50, 48), "A report with columns and a structured table");
    font.setPixelSize(11); font.setBold(false); painter.setFont(font);
    const double width = (495 - (columns - 1) * 26.0) / columns;
    for (int c = 0; c < columns; ++c) {
        const double x = 50 + c * (width + 26);
        painter.drawText(QPointF(x, 95), QStringLiteral("Column %1 starts here.").arg(c + 1));
        painter.drawText(QPointF(x, 111), "Read this paragraph first.");
        painter.drawText(QPointF(x, 127), "Keep words in this column.");
        painter.drawText(QPointF(x, 160), "A new paragraph follows.");
        painter.drawText(QPointF(x, 176), "Images remain in place.");
    }
    painter.setPen(QPen(QColor("#315f46"), 1)); painter.drawLine(QPointF(50, 210), QPointF(545, 210));
    painter.setPen(Qt::black); font.setPixelSize(14); font.setBold(true); painter.setFont(font);
    painter.drawText(QPointF(50, 245), "Electrical measurements and component information");
    const double xs[]{50, 215, 380, 545}, ys[]{270, 300, 340, 380, 420};
    painter.setPen(QPen(Qt::black, 1));
    for (double y : ys) painter.drawLine(QPointF(verticalMerge && y == 380 ? 215 : 50, y), QPointF(545, y));
    for (int c = 0; c < 4; ++c) painter.drawLine(QPointF(xs[c], merged && c == 2 ? 300 : 270), QPointF(xs[c], 420));
    font.setPixelSize(11); painter.setFont(font);
    painter.drawText(QPointF(57, 290), "Component"); painter.drawText(QPointF(222, 290), merged ? "Measurements" : "Current");
    if (!merged) painter.drawText(QPointF(387, 290), "Voltage");
    painter.drawText(QPointF(57, 325), "Part name"); painter.drawText(QPointF(222, 325), "Current rating"); painter.drawText(QPointF(387, 325), "Supply voltage");
    font.setBold(false); painter.setFont(font);
    painter.drawText(QPointF(57, 365), "Controller"); painter.drawText(QPointF(222, 365), "12 mA"); painter.drawText(QPointF(387, 365), "5 V");
    if (!verticalMerge) painter.drawText(QPointF(57, 405), "Sensor");
    painter.drawText(QPointF(222, 405), "2 mA"); painter.drawText(QPointF(387, 405), "3.3 V");
    if (background) {
        QImage image(990, 240, QImage::Format_RGB32); QPainter p(&image);
        QLinearGradient gradient(0, 0, image.width(), image.height()); gradient.setColorAt(0, QColor("#17486c")); gradient.setColorAt(1, QColor("#427a47")); p.fillRect(image.rect(), gradient);
        p.setPen(Qt::NoPen); p.setBrush(QColor("#e7bc58")); p.drawEllipse(QRectF(800, 30, 140, 140)); p.end();
        painter.drawImage(QRectF(50, 475, 495, 120), image);
        painter.setPen(Qt::white); font.setPixelSize(14); painter.setFont(font);
        painter.drawText(QPointF(65, 515), "Independent text over an image.");
        painter.drawText(QPointF(65, 535), "The picture must keep its colors.");
    }
    font.setPixelSize(12); painter.setFont(font); painter.setPen(Qt::black);
    painter.drawText(QPointF(50, 660), "Ordinary text after the table must remain translatable.");
}

inline void makeStructuredPdfFixture(const QString &path, int columns = 2, bool raster = false, bool merged = false, bool background = true, bool verticalMerge = false) {
    QPdfWriter writer(path); writer.setResolution(72); writer.setPageSize(QPageSize(QPageSize::A4)); writer.setPageMargins(QMarginsF());
    QPainter painter(&writer);
    if (raster) {
        QImage page(1488, 2105, QImage::Format_RGB32); QPainter ink(&page); ink.scale(1488.0 / 595, 2105.0 / 842);
        drawStructuredPage(ink, columns, merged, background, verticalMerge); ink.end(); painter.drawImage(QRectF(0, 0, 595, 842), page);
    } else drawStructuredPage(painter, columns, merged, background, verticalMerge);
}

inline void makeBorderlessPdfFixture(const QString &path, bool raster = false) {
    auto draw = [](QPainter &painter) {
        painter.fillRect(QRectF(0, 0, 595, 842), Qt::white); painter.setPen(Qt::black);
        QFont font("Times New Roman"); font.setPixelSize(12); font.setBold(true); painter.setFont(font);
        const QList<QStringList> rows{{"Part", "Current", "Voltage"}, {"Controller", "12 mA", "5 V"}, {"Sensor", "2 mA", "3.3 V"}, {"Switch", "0.5 A", "12 V"}};
        for (int r = 0; r < rows.size(); ++r) {
            font.setBold(r == 0); painter.setFont(font);
            for (int c = 0; c < 3; ++c) painter.drawText(QPointF(50 + c * 180, 95 + r * 32), rows[r][c]);
        }
        painter.drawText(QPointF(50, 300), "An ordinary paragraph below the borderless table.");
    };
    QPdfWriter pdf(path); pdf.setResolution(72); pdf.setPageSize(QPageSize(QPageSize::A4)); pdf.setPageMargins(QMarginsF()); QPainter painter(&pdf);
    if (raster) { QImage image(1488, 2105, QImage::Format_RGB32); QPainter ink(&image); ink.scale(1488.0 / 595, 2105.0 / 842); draw(ink); ink.end(); painter.drawImage(QRectF(0, 0, 595, 842), image); }
    else draw(painter);
}

inline void makeContinuedTableFixture(const QString &path, int pageCount = 2) {
    QPdfWriter pdf(path); pdf.setResolution(72); pdf.setPageSize(QPageSize(QPageSize::A4)); pdf.setPageMargins(QMarginsF()); QPainter painter(&pdf);
    for (int page = 0; page < pageCount; ++page) {
        if (page) pdf.newPage(); painter.fillRect(QRectF(0, 0, 595, 842), Qt::white); painter.setPen(QPen(Qt::black, 1));
        const double top = page ? 50 : 500;
        const double rowHeight = page && page + 1 < pageCount ? 150 : 60;
        for (int c = 0; c <= 3; ++c) painter.drawLine(QPointF(50 + c * 160, top), QPointF(50 + c * 160, top + 5 * rowHeight));
        for (int r = 0; r <= 5; ++r) painter.drawLine(QPointF(50, top + r * rowHeight), QPointF(530, top + r * rowHeight));
        QFont font("Times New Roman"); font.setPixelSize(12);
        for (int r = 0; r < 5; ++r) {
            const bool header = !page && r == 0; font.setBold(header); painter.setFont(font);
            const QStringList values = header ? QStringList{"Part", "Current", "Voltage"} : QStringList{page ? "Amplifier" : "Controller", "12 mA", "5 V"};
            for (int c = 0; c < 3; ++c) painter.drawText(QPointF(58 + c * 160, top + r * rowHeight + 30), values[c]);
        }
    }
}

inline void makePdfFixture(const QString &path, int pages = 2, bool imageOnly = false, int imageOnlyPage = -1) {
    QPdfWriter pdf(path); pdf.setResolution(72); pdf.setPageSize(QPageSize(QPageSize::A4));
    pdf.setPageMargins(QMarginsF(0, 0, 0, 0));
    QPainter painter(&pdf);
    for (int page = 0; page < pages; ++page) {
        if (page) pdf.newPage();
        painter.fillRect(QRectF(0, 0, 595, 842), Qt::white);
        if (!imageOnly && page != imageOnlyPage) {
            QFont title("Times New Roman"); title.setPixelSize(22); title.setBold(true); painter.setFont(title); painter.setPen(QColor("#2255aa"));
            painter.drawText(QRectF(50, 35, 495, 40), Qt::AlignCenter, QStringLiteral("Sample document %1").arg(page + 1));
            QFont body("Times New Roman"); body.setPixelSize(12); painter.setFont(body); painter.setPen(Qt::black);
            painter.drawText(QPointF(50, 110), "This document preserves images, headings and horizontal lines.");
            painter.drawText(QPointF(50, 125), "Please translate the complete paragraph into Chinese.");
            painter.drawText(QPointF(50, 175), "The second paragraph has its own position on the page.");
            painter.drawText(QPointF(50, 190), "Its translation must stay above the separator.");
            painter.drawText(QPointF(285, 800), QString::number(page + 1));
        }
        painter.setPen(QPen(QColor("#435a4a"), 1.5)); painter.drawLine(QPointF(50, 215), QPointF(545, 215));
        QImage image(240, 160, QImage::Format_RGB32); image.fill(QColor("#c9e3d3"));
        QPainter graphic(&image); graphic.setPen(Qt::NoPen); graphic.setBrush(QColor("#315f46")); graphic.drawEllipse(30, 25, 105, 105);
        graphic.setBrush(QColor("#eeb866")); graphic.drawRect(135, 65, 70, 65); graphic.end();
        painter.drawImage(QRectF(50, 245, 180, 120), image);
        painter.setPen(QPen(QColor("#435a4a"), 1.5)); painter.drawLine(QPointF(50, 400), QPointF(545, 400));
    }
    painter.end();
}

inline void makeScanPdfFixture(const QString &path, int pages = 1, bool pageNumber = false) {
    QImage image(1200, 1698, QImage::Format_RGB32); image.fill(Qt::white);
    QPainter ink(&image); ink.setPen(Qt::black);
    QFont font("Times New Roman"); font.setPixelSize(48); ink.setFont(font);
    ink.drawText(QPointF(100, 170), "Scanned document"); font.setPixelSize(32); ink.setFont(font);
    ink.drawText(QPointF(100, 280), "Please close the door.");
    ink.drawText(QPointF(100, 340), "Keep the original page for comparison.");
    ink.setPen(QPen(QColor("#435a4a"), 3)); ink.drawLine(100, 450, 1080, 450);
    ink.fillRect(QRect(100, 550, 250, 100), QColor("#d7a340"));
    ink.setBrush(QColor("#315f46")); ink.drawEllipse(QRect(400, 550, 100, 100));
    ink.drawLine(100, 740, 1080, 740); ink.end();
    QPdfWriter writer(path); writer.setResolution(72); writer.setPageSize(QPageSize(QPageSize::A4)); writer.setPageMargins(QMarginsF());
    QPainter painter(&writer);
    for (int i = 0; i < pages; ++i) {
        if (i) writer.newPage();
        painter.drawImage(QRectF(0, 0, 595, 842), image);
        if (pageNumber) painter.drawText(QPointF(290, 815), QString::number(i + 1));
    }
}

inline void makeDecoratedPdfFixture(const QString &path) {
    QPdfWriter pdf(path); pdf.setResolution(72); pdf.setPageSize(QPageSize(QPageSize::A4));
    pdf.setPageMargins(QMarginsF(0, 0, 0, 0)); QPainter painter(&pdf);
    QFont font("Times New Roman"); font.setPixelSize(12); painter.setFont(font);
    painter.drawText(QPointF(50, 50), "License notice");
    const QString label = "Homepage: ", link = "https://example.org/project";
    painter.drawText(QPointF(50, 95), label);
    const auto linkX = 50 + QFontMetricsF(font).horizontalAdvance(label);
    painter.setPen(QColor("#0563c1")); painter.drawText(QPointF(linkX, 95), link);
    // A real hyperlink underline crosses the glyph descenders (g, p, j).
    painter.fillRect(QRectF(linkX, 95.6, QFontMetricsF(font).horizontalAdvance(link), .65), QColor("#0563c1"));
    painter.setPen(Qt::black);
    const QStringList markers{QStringLiteral("1."), QStringLiteral("a."), QStringLiteral("ii."), QStringLiteral("•"), QStringLiteral("*")};
    for (int i = 0; i < markers.size(); ++i) {
        const double baseline = 150 + i * 55;
        painter.drawText(QPointF(50, baseline), markers[i]);
        painter.drawText(QPointF(105, baseline), "These are the terms of the license.");
        painter.drawText(QPointF(105, baseline + 16), "The continuation belongs to the same item.");
    }
    painter.fillRect(QRectF(50, 425, 490, 95), QColor(245, 245, 245));
    painter.fillRect(QRectF(60, 437, 470, 18), QColor(245, 245, 245));
    painter.fillRect(QRectF(60, 453, 470, 18), QColor(245, 245, 245));
    painter.drawText(QPointF(65, 450), "This text is inside a light gray background.");
    painter.drawText(QPointF(65, 466), "Keep the background behind the translated text.");
    painter.drawText(QPointF(50, 550), "This paragraph is outside the gray panel.");
    painter.setPen(QPen(QColor("#435a4a"), 1.5)); painter.drawLine(QPointF(50, 575), QPointF(545, 575));
}
