#pragma once
#include "pdfengine.h"
#include <QStringList>
#include <QSet>

namespace PdfMerge {
// Make a bounded editing copy, coalescing physical content streams before
// PDFium rewrites them. Graphics state and path/text operators can span streams.
void copyPages(const QString &source, const QString &destination, int first, int last);
QSet<int> patternPages(const QString &source, int first, int last);
// Replace only page contents/resources, preserving the source page identities,
// catalog, outlines and links. Encoded streams remain backed by the input files.
void write(const QString &source, const QStringList &parts, const QString &destination,
           int pageCount, const PdfEngine::Progress &progress);
}
