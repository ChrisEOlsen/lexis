// PDF text-layer extraction via poppler-cpp (see dev/APP_SPEC.md for why poppler-cpp).
// Reads only the existing text layer; scanned PDFs extract as empty (see OcrExtractor.h).

#ifndef LEXIS_APP_PDFEXTRACTOR_H
#define LEXIS_APP_PDFEXTRACTOR_H

#include <QString>

// Every page's text in order, '\n'-joined. Empty text is still success (scanned PDF);
// *errorOut (if non-null) is the failure signal, set only for unreadable/locked files.
QString extractPdfText(const QString &path, QString *errorOut = nullptr);

// Page count, or -1 if unreadable. Separate so the scanned-PDF fallback can compare
// text volume against page count even when extraction found nothing.
int pdfPageCount(const QString &path);

#endif // LEXIS_APP_PDFEXTRACTOR_H
