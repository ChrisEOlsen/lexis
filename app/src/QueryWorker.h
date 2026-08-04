// Runs one chat query on a background thread: query formulation (which
// itself calls the local model once) -> BM25 search -> generation
// (which calls the local model again) -> fetches each result passage's
// document name/chunk id for source citations. Mirrors main.c's
// run_query() pipeline exactly, using the simpler convenience wrappers
// (query_formulation_formulate_query()/generation_generate_answer())
// rather than run_query()'s manual step-by-step decomposition -- that
// decomposition exists only to feed query_log.c's testing-mode
// instrumentation, which this app doesn't use.
//
// Both LLM calls are the slow part (see SPEED.md) -- this entire
// pipeline must run off the UI thread, same discipline as IngestWorker.
// Opens its own PgStore connection, scoped to `corpusId` via
// pg_store_use_corpus() -- doesn't touch LexisEngine's connection, same
// reasoning as IngestWorker's connections.
//
// Relies on the local model already being loaded (see ModelLoader) --
// local_llm_client_init() sets up a single, global, process-wide
// llama.cpp context, not anything owned per-QueryWorker. The caller
// (AppController) is responsible for never running a QueryWorker
// concurrently with a ModelLoader or another QueryWorker, since
// local_llm_chat_completion() has no concurrency support of its own.

#ifndef LEXIS_APP_QUERYWORKER_H
#define LEXIS_APP_QUERYWORKER_H

#include <QString>
#include <QThread>
#include <QVariantList>

extern "C" {
#include "lemmatizer.h"
#include "stopwords.h"
#include "wordnet.h"
}

class QueryWorker : public QThread {
    Q_OBJECT

public:
    QueryWorker(QString conninfo, qint64 corpusId, QString question, const StopwordSet *stopwords,
                const WordNetTable *wordnet, const Lemmatizer *lemmatizer, QObject *parent = nullptr);

signals:
    // Named queryFinished, not finished -- same QThread::finished()
    // shadowing reason as IngestWorker::ingestFinished. `sources` is a
    // QVariantList of QVariantMaps ({"documentName", "chunkId", "score"})
    // rather than a custom C++ struct/model type -- already a registered
    // Qt meta-type (safe across the cross-thread queued signal this is
    // delivered over with no extra qRegisterMetaType() needed) and
    // directly consumable from QML as a plain array of objects with no
    // C++-side exposure of its own required.
    void queryFinished(bool ok, QString answer, QVariantList sources);

protected:
    void run() override;

private:
    QString m_conninfo;
    qint64 m_corpusId;
    QString m_question;
    const StopwordSet *m_stopwords;
    const WordNetTable *m_wordnet;
    const Lemmatizer *m_lemmatizer;
};

#endif // LEXIS_APP_QUERYWORKER_H
