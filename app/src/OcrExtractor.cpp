#include "OcrExtractor.h"

#include <cstdlib>

#include <allheaders.h>
#include <tesseract/baseapi.h>

#ifndef LEXIS_TESSDATA_PREFIX
#error "LEXIS_TESSDATA_PREFIX must be defined by the build -- see CMakeLists.txt"
#endif

QString extractTextFromImage(const QString &path, QString *errorOut) {
    Pix *image = pixRead(path.toUtf8().constData());
    if (image == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("Could not read this file as an image.");
        }
        return QString();
    }

    tesseract::TessBaseAPI api;
    // Runtime override for the installed app bundle (set by main.cpp);
    // the compile-time Homebrew path serves the dev build.
    const char *tessdata = getenv("LEXIS_TESSDATA_DIR");
    if (api.Init(tessdata != nullptr ? tessdata : LEXIS_TESSDATA_PREFIX, "eng") != 0) {
        pixDestroy(&image);
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("Could not initialize the OCR engine.");
        }
        return QString();
    }

    api.SetImage(image);
    char *rawText = api.GetUTF8Text();
    QString result = rawText != nullptr ? QString::fromUtf8(rawText).trimmed() : QString();
    delete[] rawText; // GetUTF8Text()'s own documented ownership convention

    api.End();
    pixDestroy(&image);

    return result;
}
