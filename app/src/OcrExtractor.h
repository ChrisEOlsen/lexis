// Image text extraction via Tesseract OCR (see dev/APP_SPEC.md for why Tesseract).

#ifndef LEXIS_APP_OCREXTRACTOR_H
#define LEXIS_APP_OCREXTRACTOR_H

#include <QString>

// OCR on any Leptonica-readable image; empty text is still success (no text found).
// *errorOut (if non-null) is the failure signal: unreadable file or engine init failure.
QString extractTextFromImage(const QString &path, QString *errorOut = nullptr);

#endif // LEXIS_APP_OCREXTRACTOR_H
