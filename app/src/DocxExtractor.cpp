#include "DocxExtractor.h"

#include <pugixml.hpp>
#include <zip.h>

namespace {

// Full uncompressed contents of `entryName`. Empty when the entry is missing or
// unreadable (required callers check; optional parts just append nothing).
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

// Appends every paragraph's run text to `out`, one paragraph per line. Matched by
// local-name() (prefix-independent); tracked-changes <w:delText> is excluded implicitly.
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

    // Optional parts vary in count (default/first-page/even-page variants); enumerate
    // entries and match by name pattern rather than guessing filenames.
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
