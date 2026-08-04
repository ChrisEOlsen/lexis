// Extracts plain text from a PDF's text layer via poppler-cpp. See
// ../../APP_SPEC.md's "PDF, text layer" section for why poppler-cpp
// specifically -- QtPdf turned out to require building PDFium via
// Chromium's own gn/ninja toolchain (not a normal CMake build, and not
// something Homebrew packages), essentially the same class of problem
// that ruled out DocWire; poppler-cpp is a real Homebrew formula with
// no exotic build chain.
//
// This only reads whatever text layer a PDF already has -- a scanned
// PDF (a rasterized image with no embedded text) will extract as empty
// or near-empty regardless of how good this code is. See
// OcrExtractor.h for the render-and-OCR fallback for that case.

#ifndef LEXIS_APP_PDFEXTRACTOR_H
#define LEXIS_APP_PDFEXTRACTOR_H

#include <QString>

// Extracts every page's text-layer content, in page order, joined with
// '\n' between pages (blank pages contribute nothing, not an empty
// line). Uses poppler's physical_layout mode so multi-column pages come
// out in reading order rather than raw content-stream order.
//
// *errorOut (if non-null) is the authoritative success/failure signal
// -- left untouched on success, set to a human-readable reason on a
// real failure (the file can't be opened as a PDF, or it's password-
// protected). The returned text may legitimately be empty even on
// success -- a scanned PDF has no text layer at all, which isn't a
// failure of this function; see OcrExtractor.h for handling that case.
QString extractPdfText(const QString &path, QString *errorOut = nullptr);

// Number of pages in the PDF at `path`, or -1 if it can't be opened.
// Exposed separately from extractPdfText() because the scanned-PDF
// fallback decision (implausibly little text relative to page count)
// needs the page count even when text extraction itself fails to find
// anything.
int pdfPageCount(const QString &path);

#endif // LEXIS_APP_PDFEXTRACTOR_H
