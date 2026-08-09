// Runs one chat query on a background thread: loads sessionId's message
// history -> persists the user's question -> context-aware query
// reformulation (resolves pronouns/references against history into a
// standalone search query -- see QueryWorker.cpp's own top-of-file
// comment) -> BM25 search -> history-aware generation (the original
// question, not the reformulated one, plus history and the retrieved
// passages) -> persists the assistant's answer -> fetches each result
// passage's document name/chunk id for source citations.
//
// Generation is the slow part (see SPEED.md) -- this entire pipeline
// still runs off the UI thread, same discipline as IngestWorker. Opens
// its own PgStore connection, scoped to `corpusId` via
// pg_store_use_corpus() -- doesn't touch LexisEngine's connection, same
// reasoning as IngestWorker's connections. Chat history persistence
// (public.chat_sessions/chat_messages) goes through this same
// connection directly via pg_store_* calls, not through LexisEngine --
// those tables live in the public schema regardless of search_path, so
// no corpus-scoping conflict with the rest of this connection's queries.
//
// Relies on the local model already being loaded (see ModelLoader) --
// local_llm_client_init() sets up a single, global, process-wide
// llama.cpp context, not anything owned per-QueryWorker. The caller
// (AppController) is responsible for never running a QueryWorker
// concurrently with a ModelLoader or another QueryWorker, since
// local_llm_chat_completion()/local_llm_chat_completion_multi() have no
// concurrency support of their own.

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
    QueryWorker(QString conninfo, qint64 corpusId, qint64 sessionId, QString question,
                const StopwordSet *stopwords, const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                QObject *parent = nullptr);

signals:
    // Named queryFinished, not finished -- same QThread::finished()
    // shadowing reason as IngestWorker::ingestFinished.
    //
    // `tool` is which tool the router picked -- "search", "read" or
    // "chat" -- and is what the UI's source inspector reports. It is
    // carried here rather than inferred from `sources` because the two
    // are not equivalent: a SEARCH that matched nothing and a CHAT that
    // retrieved nothing both arrive with an empty list, and they are not
    // the same event. Empty string when ok is false.
    //
    // `sources` is a QVariantList of QVariantMaps -- SEARCH supplies
    // {"documentName", "chunkId", "score", "text", "tokenCount"}, READ
    // supplies {"documentName"} per document, CHAT supplies nothing --
    // rather than a custom C++ struct/model type -- already a registered
    // Qt meta-type (safe across the cross-thread queued signal this is
    // delivered over with no extra qRegisterMetaType() needed) and
    // directly consumable from QML as a plain array of objects with no
    // C++-side exposure of its own required. `answer` is always the real
    // text to show and persist -- never empty on ok == true (a "nothing
    // to search for" or "no matching passages" outcome is a real answer
    // string here, not a sentinel the caller has to special-case).
    void queryFinished(bool ok, QString answer, QVariantList sources, QString tool);

protected:
    void run() override;

private:
    QString m_conninfo;
    qint64 m_corpusId;
    qint64 m_sessionId;
    QString m_question;
    const StopwordSet *m_stopwords;
    const WordNetTable *m_wordnet;
    const Lemmatizer *m_lemmatizer;
};

#endif // LEXIS_APP_QUERYWORKER_H
