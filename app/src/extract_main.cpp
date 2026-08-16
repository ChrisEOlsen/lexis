// Extraction verification tool: runs files through the SAME format
// extractors the app's drag-and-drop ingestion uses (DocxExtractor,
// PdfExtractor, OcrExtractor) and prints what came out -- so "does DOCX
// work? does OCR work?" is answerable from the terminal, with real
// files, without driving the GUI.
//
// usage: lexis_extract <file> [file...]
//        lexis_extract --ingest "<group name>" <file> [file...]
//
// Plain mode prints each file's extracted text. --ingest additionally
// creates a new group and indexes the extracted text through
// bulk_ingest_rebuild_corpus() -- the identical call IngestWorker makes
// -- and prints the new group's id, so the result can be queried with
// lexis_eval or opened in the app.

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include "DocxExtractor.h"
#include "OcrExtractor.h"
#include "PdfExtractor.h"

extern "C" {
#include "bulk_ingest.h"
#include "config.h"
#include "lemmatizer.h"
#include "pg_store.h"
#include "stopwords.h"
#include "wordnet.h"
}

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

// Same suffix dispatch as IngestWorker::run() -- keep in sync.
QString extractByType(const QString &path, QString *kindOut, QString *errorOut) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("docx")) {
        *kindOut = QStringLiteral("docx");
        return extractDocxText(path, errorOut);
    }
    if (suffix == QStringLiteral("pdf")) {
        *kindOut = QStringLiteral("pdf");
        return extractPdfText(path, errorOut);
    }
    if (suffix == QStringLiteral("png") || suffix == QStringLiteral("jpg") ||
        suffix == QStringLiteral("jpeg") || suffix == QStringLiteral("tiff") ||
        suffix == QStringLiteral("tif") || suffix == QStringLiteral("bmp")) {
        *kindOut = QStringLiteral("image/ocr");
        return extractTextFromImage(path, errorOut);
    }
    *kindOut = QStringLiteral("unsupported");
    *errorOut = QStringLiteral("no extractor for .%1").arg(suffix);
    return QString();
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QStringList args = QCoreApplication::arguments();
    args.removeFirst();

    QString groupName;
    if (args.size() >= 2 && args.first() == QStringLiteral("--ingest")) {
        args.removeFirst();
        groupName = args.takeFirst();
    }
    if (args.isEmpty()) {
        fprintf(stderr, "usage: lexis_extract [--ingest \"<group name>\"] <file> [file...]\n");
        return 2;
    }

    std::vector<QByteArray> nameBytes;
    std::vector<QByteArray> textBytes;
    bool allOk = true;

    for (const QString &path : args) {
        QString kind, error;
        const QString text = extractByType(path, &kind, &error);
        printf("=== %s (%s)\n", qPrintable(path), qPrintable(kind));
        if (text.isEmpty()) {
            printf("EXTRACTION FAILED: %s\n\n", qPrintable(error.isEmpty() ? QStringLiteral("no text found") : error));
            allOk = false;
            continue;
        }
        printf("%s\n\n", qPrintable(text));
        nameBytes.push_back(QFileInfo(path).fileName().toUtf8());
        textBytes.push_back(text.toUtf8());
    }

    if (groupName.isEmpty()) {
        return allOk ? 0 : 1;
    }
    if (nameBytes.empty()) {
        fprintf(stderr, "nothing extracted -- not creating a group\n");
        return 1;
    }

    char *conninfo = config_load_db_conninfo(LEXIS_CONFIG_PATH_DEFAULT);
    if (conninfo == nullptr) {
        fprintf(stderr, "no database configured -- set db_conninfo in config/lexis.conf\n");
        return 1;
    }
    StopwordSet *stopwords = stopword_set_load("data/stopwords/english.txt");
    WordNetTable *wordnet = wordnet_table_load("data/wordnet");
    Lemmatizer *lemmatizer = lemmatizer_load("data/wordnet");
    if (stopwords == nullptr || wordnet == nullptr || lemmatizer == nullptr) {
        fprintf(stderr, "language data failed to load -- run from the project root\n");
        return 1;
    }

    PgStore *store = pg_store_open(conninfo);
    if (store == nullptr) {
        fprintf(stderr, "cannot connect to the database\n");
        return 1;
    }
    char *schemaName = nullptr;
    const int64_t corpusId = pg_store_create_corpus(store, groupName.toUtf8().constData(), &schemaName);
    pg_store_close(store);
    free(schemaName);
    if (corpusId <= 0) {
        fprintf(stderr, "could not create group\n");
        return 1;
    }

    std::vector<const char *> names;
    std::vector<const char *> texts;
    for (size_t i = 0; i < nameBytes.size(); i++) {
        names.push_back(nameBytes[i].constData());
        texts.push_back(textBytes[i].constData());
    }
    // Identical call and constants to IngestWorker::run().
    const long total = bulk_ingest_rebuild_corpus(conninfo, corpusId, names.data(), texts.data(),
                                                   names.size(), stopwords, wordnet, lemmatizer,
                                                   200, 40, 6);
    if (total < 0) {
        fprintf(stderr, "ingest failed\n");
        return 1;
    }
    printf("group \"%s\" created: id=%lld, %zu documents, %ld passages\n", qPrintable(groupName),
           (long long)corpusId, names.size(), total);
    return allOk ? 0 : 1;
}
