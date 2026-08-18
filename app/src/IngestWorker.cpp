#include "IngestWorker.h"
#include "DocxExtractor.h"
#include "OcrExtractor.h"
#include "PdfExtractor.h"

extern "C" {
#include "bulk_ingest.h"
#include "csv_parse.h"
}

#include <QFile>
#include <QFileInfo>
#include <QTextStream>

IngestWorker::IngestWorker(QString conninfo, qint64 corpusId, QStringList filePaths, const StopwordSet *stopwords,
                            const WordNetTable *wordnet, const Lemmatizer *lemmatizer, QObject *parent)
    : QThread(parent), m_conninfo(std::move(conninfo)), m_corpusId(corpusId), m_filePaths(std::move(filePaths)),
      m_stopwords(stopwords), m_wordnet(wordnet), m_lemmatizer(lemmatizer) {
}

void IngestWorker::requestCancel() {
    m_cancelRequested.storeRelease(1);
    // Reaches into whichever bulk_ingest phase is running right now;
    // harmless if the run is still in extraction.
    bulk_ingest_request_cancel();
}

void IngestWorker::run() {
    QVector<QPair<QString, QString>> newDocuments;
    QStringList skipped;
    QStringList malformed;
    QStringList noTextFound;

    // A stale flag from a previously-cancelled run must not abort this
    // one -- see bulk_ingest.h.
    bulk_ingest_clear_cancel();

    int filesDone = 0;
    for (const QString &path : m_filePaths) {
        if (m_cancelRequested.loadAcquire()) {
            emit ingestFinished(true, true, 0, {}, {}, {});
            return;
        }
        emit ingestProgress(filesDone++, m_filePaths.size(), -1);
        QFileInfo info(path);
        QString suffix = info.suffix().toLower();

        if (suffix == QStringLiteral("txt")) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                skipped.append(info.fileName());
                continue;
            }
            QTextStream stream(&file);
            newDocuments.append(qMakePair(info.fileName(), stream.readAll()));
        } else if (suffix == QStringLiteral("csv")) {
            // One CSV file can produce many documents -- one per data
            // row (see APP_SPEC.md's "CSV" section: v1's default
            // document mapping is one row = one document, every column
            // concatenated). csv_parse_file() fails the WHOLE file on
            // any malformed row rather than a partial parse.
            TokenList *rows = csv_parse_file(path.toUtf8().constData());
            if (rows == nullptr) {
                malformed.append(info.fileName());
                continue;
            }
            for (size_t i = 0; i < rows->count; i++) {
                QString rowName = QStringLiteral("%1#row%2").arg(info.fileName()).arg(i + 1);
                newDocuments.append(qMakePair(rowName, QString::fromUtf8(rows->terms[i])));
            }
            token_list_free(rows);
        } else if (suffix == QStringLiteral("docx")) {
            QString error;
            QString text = extractDocxText(path, &error);
            if (text.isEmpty()) {
                malformed.append(info.fileName());
                continue;
            }
            newDocuments.append(qMakePair(info.fileName(), text));
        } else if (suffix == QStringLiteral("pdf")) {
            QString error;
            QString text = extractPdfText(path, &error);
            if (!error.isEmpty()) {
                malformed.append(info.fileName());
                continue;
            }
            if (text.isEmpty()) {
                // No text layer at all -- almost certainly a scanned
                // PDF. Rendering pages and OCRing them isn't
                // implemented yet (see APP_SPEC.md's "Images and
                // scanned PDFs" section) -- reported distinctly so it
                // isn't confused with a genuine parse failure.
                noTextFound.append(info.fileName());
                continue;
            }
            newDocuments.append(qMakePair(info.fileName(), text));
        } else if (suffix == QStringLiteral("png") || suffix == QStringLiteral("jpg") ||
                   suffix == QStringLiteral("jpeg") || suffix == QStringLiteral("tiff") ||
                   suffix == QStringLiteral("tif") || suffix == QStringLiteral("bmp")) {
            QString error;
            QString text = extractTextFromImage(path, &error);
            if (!error.isEmpty()) {
                malformed.append(info.fileName());
                continue;
            }
            if (text.isEmpty()) {
                noTextFound.append(info.fileName());
                continue;
            }
            newDocuments.append(qMakePair(info.fileName(), text));
        } else {
            skipped.append(info.fileName());
        }
    }

    if (m_cancelRequested.loadAcquire()) {
        emit ingestFinished(true, true, 0, {}, {}, {});
        return;
    }
    if (newDocuments.isEmpty()) {
        emit ingestFinished(true, false, 0, skipped, malformed, noTextFound);
        return;
    }

    // Mirrors main.c's LEXIS_CHUNK_SIZE/LEXIS_CHUNK_OVERLAP/
    // LEXIS_INGEST_THREADS exactly -- no config UI for these yet, and
    // matching the CLI's own ingest behavior matters more right now
    // than tuning them differently here.
    constexpr size_t kChunkSize = 200;
    constexpr size_t kChunkOverlap = 40;
    constexpr int kThreadCount = 6;

    // bulk_ingest_rebuild_corpus() wants const char*const* arrays --
    // the QByteArray vectors own the UTF-8 bytes those pointers point
    // into for the duration of this call.
    QVector<QByteArray> nameBytes;
    QVector<QByteArray> textBytes;
    nameBytes.reserve(newDocuments.size());
    textBytes.reserve(newDocuments.size());
    for (const auto &doc : newDocuments) {
        nameBytes.append(doc.first.toUtf8());
        textBytes.append(doc.second.toUtf8());
    }

    QVector<const char *> names;
    QVector<const char *> texts;
    names.reserve(nameBytes.size());
    texts.reserve(textBytes.size());
    qint64 totalTextBytes = 0;
    for (int i = 0; i < nameBytes.size(); i++) {
        names.append(nameBytes[i].constData());
        texts.append(textBytes[i].constData());
        totalTextBytes += textBytes[i].size();
    }

    // Rebuild-duration estimate for the progress display: ~6 bytes per
    // English word, a 160-word stride per passage (chunk 200, overlap
    // 40), and the measured ~3500 passages/sec ingest rate on this
    // machine class. Order-of-magnitude honest, not precise.
    const qint64 estimatedPassages = totalTextBytes / (6 * 160);
    const qint64 indexEtaMs = qMax<qint64>(1000, estimatedPassages * 1000 / 3500);
    emit ingestProgress(m_filePaths.size(), m_filePaths.size(), indexEtaMs);

    long total = bulk_ingest_rebuild_corpus(m_conninfo.toUtf8().constData(), m_corpusId, names.constData(),
                                             texts.constData(), static_cast<size_t>(names.size()), m_stopwords,
                                             m_wordnet, m_lemmatizer, kChunkSize, kChunkOverlap, kThreadCount);

    if (total == BULK_INGEST_CANCELLED) {
        emit ingestFinished(true, true, 0, {}, {}, {});
        return;
    }
    emit ingestFinished(total >= 0, false, total >= 0 ? static_cast<qint64>(total) : 0, skipped, malformed,
                         noTextFound);
}
