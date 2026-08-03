// Runs bulk_ingest_rebuild_corpus() on a background thread -- that call
// is blocking and potentially slow (a real rebuild re-ingests a whole
// group's documents, see ../../APP_SPEC.md), so it can't run on the UI
// thread without freezing it. Opens its own database connections
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

#include <QPair>
#include <QString>
#include <QThread>
#include <QVector>

extern "C" {
#include "lemmatizer.h"
#include "stopwords.h"
#include "wordnet.h"
}

class IngestWorker : public QThread {
    Q_OBJECT

public:
    IngestWorker(QString conninfo, qint64 corpusId, QVector<QPair<QString, QString>> newDocuments,
                 const StopwordSet *stopwords, const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                 QObject *parent = nullptr);

signals:
    // Named ingestFinished, not finished -- QThread already has its own
    // finished() signal (no args, emitted when run() returns); reusing
    // that name here would shadow it. totalPassages is meaningless when
    // ok is false.
    void ingestFinished(bool ok, qint64 totalPassages);

protected:
    void run() override;

private:
    QString m_conninfo;
    qint64 m_corpusId;
    QVector<QPair<QString, QString>> m_newDocuments;
    const StopwordSet *m_stopwords;
    const WordNetTable *m_wordnet;
    const Lemmatizer *m_lemmatizer;
};

#endif // LEXIS_APP_INGESTWORKER_H
