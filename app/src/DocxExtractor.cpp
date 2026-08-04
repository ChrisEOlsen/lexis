#include "DocxExtractor.h"

#include <pugixml.hpp>
#include <zip.h>

namespace {

// Reads the full uncompressed contents of `entryName` from an already-
// open zip archive. Returns an empty QByteArray if the entry doesn't
// exist or can't be read -- callers that require the entry to exist
// (word/document.xml) check for that themselves; callers for optional
// parts (headers/footers/footnotes/endnotes) just get nothing to
// append, which is the correct behavior for a part that's legitimately
// absent from a given document.
QByteArray readZipEntry(zip_t *archive, const char *entryName) {
    zip_int64_t index = zip_name_locate(archive, entryName, 0);
    if (index < 0) {
        return QByteArray();
    }

    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(archive, index, 0, &stat) != 0 || (stat.valid & ZIP_STAT_SIZE) == 0) {
        return QByteArray();
    }

    zip_file_t *file = zip_fopen_index(archive, index, 0);
    if (file == nullptr) {
        return QByteArray();
    }

    QByteArray buffer(static_cast<qsizetype>(stat.size), Qt::Uninitialized);
    zip_int64_t bytesRead = zip_fread(file, buffer.data(), stat.size);
    zip_fclose(file);
    if (bytesRead < 0 || static_cast<zip_uint64_t>(bytesRead) != stat.size) {
        return QByteArray();
    }
    return buffer;
}

// Appends every <w:p> paragraph's <w:t> run text (in document order) to
// `out`, one paragraph per line. Matched by local-name(), not a literal
// "w:p"/"w:t" name, so this doesn't depend on which namespace prefix a
// given file's XML happens to bind to the wordprocessingml namespace
// (virtually always "w" in practice, but not guaranteed by the XML
// namespace spec, and not worth trusting when local-name() matching is
// just as simple). <w:delText> (tracked-changes deletions) has a
// different local name than <w:t> and is excluded automatically as a
// consequence, not via explicit exclusion logic.
void appendParagraphText(const pugi::xml_document &xml, QString *out) {
    pugi::xpath_node_set paragraphs = xml.select_nodes("//*[local-name()='p']");
    for (const pugi::xpath_node &paragraphNode : paragraphs) {
        QString paragraphText;
        pugi::xpath_node_set runs = paragraphNode.node().select_nodes(".//*[local-name()='t']");
        for (const pugi::xpath_node &runNode : runs) {
            paragraphText += QString::fromUtf8(runNode.node().text().get());
        }
        if (paragraphText.isEmpty()) {
            continue;
        }
        if (!out->isEmpty()) {
            out->append(QLatin1Char('\n'));
        }
        out->append(paragraphText);
    }
}

} // namespace

QString extractDocxText(const QString &path, QString *errorOut) {
    int err = 0;
    zip_t *archive = zip_open(path.toUtf8().constData(), ZIP_RDONLY, &err);
    if (archive == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("Could not open as a .docx (zip) archive.");
        }
        return QString();
    }

    QByteArray documentXml = readZipEntry(archive, "word/document.xml");
    if (documentXml.isEmpty()) {
        zip_close(archive);
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("Missing word/document.xml -- not a valid .docx file.");
        }
        return QString();
    }

    pugi::xml_document mainDoc;
    pugi::xml_parse_result parseResult = mainDoc.load_buffer(documentXml.constData(), documentXml.size());
    if (!parseResult) {
        zip_close(archive);
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("word/document.xml is not valid XML -- the file may be corrupted.");
        }
        return QString();
    }

    QString result;
    appendParagraphText(mainDoc, &result);

    // Headers/footers/footnotes/endnotes are optional and their count
    // varies (a document can have a default/first-page/even-page
    // variant of each) -- enumerate every archive entry and match by
    // name pattern rather than guessing specific filenames.
    zip_int64_t entryCount = zip_get_num_entries(archive, 0);
    for (zip_int64_t i = 0; i < entryCount; i++) {
        const char *name = zip_get_name(archive, i, 0);
        if (name == nullptr) {
            continue;
        }
        QString entryName = QString::fromUtf8(name);
        bool isSupplementalPart = entryName.startsWith(QStringLiteral("word/header")) ||
                                   entryName.startsWith(QStringLiteral("word/footer")) ||
                                   entryName == QStringLiteral("word/footnotes.xml") ||
                                   entryName == QStringLiteral("word/endnotes.xml");
        if (!isSupplementalPart) {
            continue;
        }

        QByteArray partXml = readZipEntry(archive, name);
        if (partXml.isEmpty()) {
            continue;
        }
        pugi::xml_document partDoc;
        if (partDoc.load_buffer(partXml.constData(), partXml.size())) {
            appendParagraphText(partDoc, &result);
        }
    }

    zip_close(archive);

    if (result.isEmpty() && errorOut != nullptr) {
        *errorOut = QStringLiteral("No text content found in this .docx file.");
    }
    return result;
}
