// Extract-then-rebuild ingest on a background thread (both halves would freeze the UI).
// Uses its own DB connections; language data is caller-owned, read-only, shared.

#ifndef LEXIS_APP_INGESTWORKER_H
#define LEXIS_APP_INGESTWORKER_H

#include <QAtomicInt>
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

    // Cooperative cancel from the UI thread; the group is left unchanged.
    void requestCancel();

signals:
    // Named ingestFinished, not finished (QThread::finished would be shadowed).
    // cancelled implies ok (nothing failed); the corpus is unchanged in that case.
    void ingestFinished(bool ok, bool cancelled, qint64 totalPassages, QStringList skipped,
                         QStringList malformed, QStringList noTextFound);

    // Per-file progress during extraction; one final emission carries the rebuild's
    // time estimate (the rebuild has no progress hooks, so the UI animates through it).
    void ingestProgress(int filesDone, int filesTotal, qint64 indexEtaMs);

protected:
    void run() override;

private:
    QAtomicInt m_cancelRequested;
    QString m_conninfo;
    qint64 m_corpusId;
    QStringList m_filePaths;
    const StopwordSet *m_stopwords;
    const WordNetTable *m_wordnet;
    const Lemmatizer *m_lemmatizer;
};

#endif // LEXIS_APP_INGESTWORKER_H
