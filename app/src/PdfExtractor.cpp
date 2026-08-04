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
        // physical_layout, not the default raw content-stream order --
        // multi-column pages otherwise come out with columns
        // interleaved instead of read in the order a person actually
        // reads them.
        poppler::ustring text = page->text(poppler::rectf(), poppler::page::physical_layout);
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
