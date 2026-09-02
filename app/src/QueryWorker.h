// Runs one chat query on a background thread: loads sessionId's message
// history -> persists the user's question -> context-aware query
// reformulation (resolves pronouns/references against history into a
// standalone search query -- see QueryWorker.cpp's own top-of-file
// comment) -> BM25 search -> history-aware generation (the original
// question, not the reformulated one, plus history and the retrieved
// passages) -> persists the assistant's answer -> fetches each result
// passage's document name/chunk id for source citations.
//
// Live progress: queryStage() reports each pipeline step as it starts
// (see QueryStage below), and queryToken() streams the answer's text
// piece by piece as the model writes it -- the answer still arrives in
// full via queryFinished() (the streamed pieces are display plumbing;
// the finished signal's `answer` is the authoritative text, identical to
// what a non-streaming run would have produced).
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
#include "local_llm_client.h"
#include "stopwords.h"
#include "wordnet.h"
}

// Pipeline stages, reported as they begin so the UI can say what is
// happening during the seconds before tokens exist (the whole point:
// a bare "Thinking..." over a multi-second silent gap reads as a hang).
// Every query starts with Routing; which stages follow depends on the
// routed tool (SEARCH: Rewriting -> Searching -> Reading -> Writing;
// SUMMARY: Summarizing -> Writing; CHAT: Writing). Retrying is emitted
// instead of a second Writing when a refusal triggered the deeper
// retry, so the UI can clear the streamed refusal text it just showed.
// `payload` carries a count where one exists (Reading: passages handed
// to the model), -1 otherwise.
enum QueryStage {
    StageRouting = 0,
    StageRewriting,
    StageSearching,
    StageReading,
    StageWriting,
    StageRetrying,
    StageSummarizing,
};

class QueryWorker : public QThread {
    Q_OBJECT

public:
    // forceRetry: run the SEARCH pipeline with the deeper retrieval
    // policy (score floor off, full passage budget) and the reasoning
    // pass forced on from the start -- the same parameters the automatic
    // refusal retry uses (see runSearchPipeline()), exposed as the
    // answer's "Try harder" action. Skips the refusal pre-check (the
    // user just asked for it) and tool routing (the question was already
    // routed to SEARCH).
    //
    // thinkingOverride: forwarded to the generation calls -- -1 follows
    // config/lexis.conf's `thinking` (the historical behavior, and what
    // the eval harness passes), 0/1 force the reasoning pass off/on for
    // every answer-producing call in this query. Exists so the app's
    // Settings toggle applies live without a restart (generation.c
    // caches its config read once per process, so live changes have to
    // arrive as explicit overrides). Ignored when forceRetry is set
    // (a retry always thinks).
    QueryWorker(QString conninfo, qint64 corpusId, qint64 sessionId, QString question,
                const StopwordSet *stopwords, const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                bool forceRetry = false, int thinkingOverride = -1, QObject *parent = nullptr);

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
    //
    // `searchQuery`/`searchTerms` are the SEARCH path's retrieval
    // provenance for the source inspector: the reformulated standalone
    // question (empty when the rewrite came back identical to what the
    // user typed -- showing two identical strings reads as a bug) and
    // the space-joined term union BM25 actually ran. Both empty for
    // CHAT/SUMMARY, which have no lexical query at all.
    void queryFinished(bool ok, QString answer, QVariantList sources, QString tool,
                       QString searchQuery, QString searchTerms);

    // Progress, emitted from the worker thread (delivered queued).
    // queryToken's piece is one increment of the final answer text --
    // concatenated in order they reproduce it exactly; the first token
    // is a UI's signal that the answer has started (create the live
    // message row), and StageRetrying tells it to clear what it showed.
    void queryStage(int stage, int payload);
    void queryToken(QString piece);

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
    bool m_forceRetry;
    int m_thinkingOverride;
};

#endif // LEXIS_APP_QUERYWORKER_H
