// Runs the full "extract each dropped file's text, then
// bulk_ingest_rebuild_corpus()" sequence on a background thread. Both
// halves belong here, not just the database rebuild -- format
// extraction (especially OCR, which is inherently slow, see
// OcrExtractor.h) is just as capable of freezing the UI as the database
// call is, so neither can run on the UI thread. Dispatches each dropped
// file to csv_parse_file()/extractDocxText()/extractPdfText()/
// extractTextFromImage() by extension -- see ../../APP_SPEC.md's
// "Document ingestion" section for the per-format contract.
//
// bulk_ingest_rebuild_corpus() opens its own database connections
// internally (see bulk_ingest.c) -- doesn't touch LexisEngine's
// connection at all, so there's no cross-thread PgStore/PGconn sharing
// to worry about.
//
// stopwords/wordnet/lemmatizer are owned by the caller (MainWindow),
// loaded once for the app's whole lifetime and reused across every
// ingest rather than reloaded per drop -- read-only after loading, safe
// to share across threads the same way bulk_ingest.c's own Phase 2
// worker pool already shares one copy across concurrent threads.

#ifndef LEXIS_APP_INGESTWORKER_H
#define LEXIS_APP_INGESTWORKER_H

#include <QString>
#include <QStringList>
#include <QThread>

extern "C" {
#include "lemmatizer.h"
#include "stopwords.h"
#include "wordnet.h"
}

class IngestWorker : public QThread {
    Q_OBJECT

public:
    IngestWorker(QString conninfo, qint64 corpusId, QStringList filePaths, const StopwordSet *stopwords,
                 const WordNetTable *wordnet, const Lemmatizer *lemmatizer, QObject *parent = nullptr);

signals:
    // Named ingestFinished, not finished -- QThread already has its own
    // finished() signal (no args, emitted when run() returns); reusing
    // that name here would shadow it. totalPassages is meaningless when
    // ok is false. skipped/malformed/noTextFound categorize every input
    // file that didn't turn into a document -- see run()'s own comment
    // for what lands in which list.
    void ingestFinished(bool ok, qint64 totalPassages, QStringList skipped, QStringList malformed,
                         QStringList noTextFound);

    // Progress for the two phases of an ingest. During extraction,
    // filesDone/filesTotal advance and indexEtaMs is -1. When the
    // database rebuild starts, one final emission carries indexEtaMs: a
    // rough estimate of how long the rebuild will take (from the text
    // volume and the measured passages/sec rate) -- rough because the
    // rebuild is one C call with no progress hooks, so the UI animates
    // through the estimate rather than tracking real progress.
    void ingestProgress(int filesDone, int filesTotal, qint64 indexEtaMs);

protected:
    void run() override;

private:
    QString m_conninfo;
    qint64 m_corpusId;
    QStringList m_filePaths;
    const StopwordSet *m_stopwords;
    const WordNetTable *m_wordnet;
    const Lemmatizer *m_lemmatizer;
};

#endif // LEXIS_APP_INGESTWORKER_H
