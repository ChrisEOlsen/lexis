#include "PdfExtractor.h"

#include <poppler-document.h>
#include <poppler-page.h>

#include <memory>

QString extractPdfText(const QString &path, QString *errorOut) {
    std::unique_ptr<poppler::document> doc(poppler::document::load_from_file(path.toStdString()));
    if (doc == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("Could not open as a PDF file.");
        }
        return QString();
    }
    if (doc->is_locked()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("This PDF is password-protected.");
        }
        return QString();
    }

    QString result;
    int pageCount = doc->pages();
    for (int i = 0; i < pageCount; i++) {
        std::unique_ptr<poppler::page> page(doc->create_page(i));
        if (page == nullptr) {
            continue;
        }
        // raw_order_layout, not physical_layout -- physical_layout's
        // geometric column-detection heuristic was tried first on the
        // assumption it would handle multi-column pages better, but on a
        // real two-column PDF (a DMV driver's manual) it did the
        // opposite: it spliced a clause from the neighboring column into
        // the middle of an unrelated sentence ("Minimum age is" ...
        // [wrong column's text]... "16."), confirmed by extracting the
        // same page under all three of poppler-cpp's text_layout_enum
        // values and diffing the output. raw_order_layout (the PDF's own
        // content-stream/drawing order) got it right, matching
        // pdftotext's own default behavior on the same page.
        poppler::ustring text = page->text(poppler::rectf(), poppler::page::raw_order_layout);
        poppler::byte_array utf8 = text.to_utf8();
        QString pageText = QString::fromUtf8(utf8.data(), static_cast<int>(utf8.size())).trimmed();
        if (pageText.isEmpty()) {
            continue;
        }
        if (!result.isEmpty()) {
            result.append(QLatin1Char('\n'));
        }
        result.append(pageText);
    }
    return result;
}

int pdfPageCount(const QString &path) {
    std::unique_ptr<poppler::document> doc(poppler::document::load_from_file(path.toStdString()));
    if (doc == nullptr) {
        return -1;
    }
    return doc->pages();
}
