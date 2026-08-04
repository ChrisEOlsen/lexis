// Extracts text from an image via Tesseract OCR. See ../../APP_SPEC.md's
// "Images and scanned PDFs" section -- Tesseract was chosen as the one
// piece of the format-extraction story with no real lightweight
// alternative (OCR is inherently not a "small" problem the way DOCX/CSV
// parsing are), but it's still a single, real Homebrew formula with no
// exotic build chain, unlike QtPdf or DocWire.

#ifndef LEXIS_APP_OCREXTRACTOR_H
#define LEXIS_APP_OCREXTRACTOR_H

#include <QString>

// Runs OCR on the image file at `path` (any format Leptonica can read
// -- PNG, JPEG, TIFF, BMP, and more) and returns the recognized text.
//
// *errorOut (if non-null) is the authoritative success/failure signal
// -- left untouched on success, set to a human-readable reason on
// failure (the file can't be read as an image, or the OCR engine
// itself failed to initialize, e.g. missing language data). The
// returned text may legitimately be empty on success -- an image with
// no recognizable text isn't a failure of this function. No implementation-
// level speed/accuracy trade-off has been tuned yet (default Tesseract
// engine mode); see APP_SPEC.md's "Images and scanned PDFs" section for
// why OCR is expected to be the slowest of the four format adapters
// regardless.
QString extractTextFromImage(const QString &path, QString *errorOut = nullptr);

#endif // LEXIS_APP_OCREXTRACTOR_H
