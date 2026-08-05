// Every question is first routed by tool_router_choose_tool() (a single
// LLM call, prefilled to skip thinking -- see local_llm_client.h's own
// doc comment) into one of two paths:
//
// SEARCH: today's existing pipeline, unchanged --
// query_formulation_contextualize_question() (a plain LLM call that
// resolves the question against conversation history into a standalone
// search query, e.g. "what about that instead?" -> "what is the minimum
// age for a Junior Operator Class MJ license?") -> BM25 search ->
// generation_generate_answer_with_history(). Good for specific,
// narrow questions.
//
// READ: skips reformulation and BM25 entirely -- answers directly from
// the full text of every document in the group instead of five scattered
// passages. Good for broad questions ("what is this document about?")
// BM25 retrieval structurally can't answer well.
//
// query_formulation_contextualize_question() is NOT the same thing as
// the WordNet-driven query_formulation_formulate_query() (still used by
// eval.c's --use-llm-expansion mode, not dead code) -- that one does
// synonym expansion, this one resolves follow-up references. See
// SPEED.md for why the WordNet expansion step was cut from the
// interactive chat pipeline in the first place (unrelated to why
// contextualization exists now).
#include "QueryWorker.h"

extern "C" {
#include "bm25.h"
#include "generation.h"
#include "pg_store.h"
#include "query_formulation.h"
#include "tool_router.h"
}

#include <QJsonArray>
#include <QJsonDocument>

#include <cstdlib>
#include <vector>

namespace {
// Mirrors main.c's LEXIS_TOP_K exactly -- no config UI for this yet,
// and matching the CLI's own retrieval behavior matters more right now
// than tuning it differently here.
constexpr size_t kTopK = 5;

// Empty string (not "[]") when there's nothing to cite -- matches
// pg_store_append_chat_message()'s NULL-means-no-sources convention, so
// a user message and a sourceless assistant answer are stored the same
// way.
QString sourcesToJson(const QVariantList &sources) {
    if (sources.isEmpty()) {
        return QString();
    }
    return QString::fromUtf8(QJsonDocument(QJsonArray::fromVariantList(sources)).toJson(QJsonDocument::Compact));
}

// SEARCH path -- today's pipeline, extracted unchanged from before the
// tool router existed. Returns false (ok=false) only on a real failure;
// a "nothing to search for"/"no matching passages" outcome is a real,
// friendly answer string, not a failure -- see QueryWorker.h's own
// comment on why `answer` is never empty on success.
bool runSearchPipeline(PgStore *store, const char *questionCstr, const std::vector<LocalLlmTurn> &turns,
                        const StopwordSet *stopwords, const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                        QString *answerOut, QVariantList *sourcesOut) {
    char *reformulated = query_formulation_contextualize_question(questionCstr, turns.data(), turns.size());
    if (reformulated == nullptr) {
        return false;
    }

    // Tokenize/stopword-filter/lemmatize the *reformulated* text, not
    // the raw question -- a follow-up like "what about that instead?"
    // has almost nothing for BM25 to work with until reformulation has
    // resolved it into something concrete.
    TokenList *terms = query_formulation_terms_only(reformulated, stopwords, wordnet, lemmatizer);
    free(reformulated);
    if (terms == nullptr) {
        return false;
    }
    if (terms->count == 0) {
        token_list_free(terms);
        *answerOut = QObject::tr("I don't have enough to search for in that question -- could you rephrase it?");
        return true;
    }

    QVector<const char *> queryTerms;
    queryTerms.reserve(static_cast<int>(terms->count));
    for (size_t i = 0; i < terms->count; i++) {
        queryTerms.append(terms->terms[i]);
    }

    BM25Params params = {BM25_DEFAULT_K1, BM25_DEFAULT_B};
    BM25CorpusStats stats = bm25_corpus_stats(store);
    BM25ResultSet *results = (stats.total_passages >= 0)
                                  ? bm25_search(store, queryTerms.data(), terms->count, kTopK, stats, params)
                                  : nullptr;
    token_list_free(terms);

    if (results == nullptr) {
        return false;
    }
    if (results->count == 0) {
        bm25_result_set_free(results);
        *answerOut = QObject::tr("No matching passages found in this group for that question.");
        return true;
    }

    // Source citations, gathered the same way run_query() does --
    // fetched before generation so a passage that fails to load (rare,
    // but possible if the DB changed underneath a stale result set)
    // simply doesn't appear as a source, rather than aborting the query.
    QVariantList sources;
    for (size_t i = 0; i < results->count; i++) {
        PgStorePassage *passage = pg_store_get_passage(store, results->items[i].passage_id);
        if (passage == nullptr) {
            continue;
        }
        QVariantMap source;
        source[QStringLiteral("documentName")] = QString::fromUtf8(passage->document_name);
        source[QStringLiteral("chunkId")] = passage->chunk_id;
        source[QStringLiteral("score")] = results->items[i].score;
        sources.append(source);
        pg_store_passage_free(passage);
    }

    // The *original* question, not the reformulated search query -- the
    // reformulation only ever existed to help retrieval, not to replace
    // what the user actually asked (see generation.h's own doc comment).
    char *answer = generation_generate_answer_with_history(questionCstr, store, results, turns.data(), turns.size());
    bm25_result_set_free(results);
    if (answer == nullptr) {
        return false;
    }

    *answerOut = QString::fromUtf8(answer);
    free(answer);
    *sourcesOut = sources;
    return true;
}

// READ path -- answers from every document in the group instead of BM25
// passages. Sources are the group's current document names, not
// necessarily exactly what generation_generate_answer_from_documents()
// ended up including after its own windowing -- see NOTES.md/the "Tool-
// routed chat" plan for why that's an accepted simplification here
// rather than threading an "actually included" list back out.
bool runReadPipeline(PgStore *store, const char *questionCstr, const std::vector<LocalLlmTurn> &turns,
                      QString *answerOut, QVariantList *sourcesOut) {
    char *answer = generation_generate_answer_from_documents(questionCstr, store, turns.data(), turns.size());
    if (answer == nullptr) {
        return false;
    }
    *answerOut = QString::fromUtf8(answer);
    free(answer);

    size_t doc_count = 0;
    PgStoreDocument *docs = pg_store_get_all_documents(store, &doc_count);
    QVariantList sources;
    for (size_t i = 0; i < doc_count; i++) {
        QVariantMap source;
        source[QStringLiteral("documentName")] = QString::fromUtf8(docs[i].document_name);
        sources.append(source);
    }
    pg_store_documents_free(docs, doc_count);
    *sourcesOut = sources;
    return true;
}
} // namespace

QueryWorker::QueryWorker(QString conninfo, qint64 corpusId, qint64 sessionId, QString question,
                          const StopwordSet *stopwords, const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                          QObject *parent)
    : QThread(parent), m_conninfo(std::move(conninfo)), m_corpusId(corpusId), m_sessionId(sessionId),
      m_question(std::move(question)), m_stopwords(stopwords), m_wordnet(wordnet), m_lemmatizer(lemmatizer) {
}

void QueryWorker::run() {
    PgStore *store = pg_store_open(m_conninfo.toUtf8().constData());
    if (store == nullptr) {
        emit queryFinished(false, QString(), QVariantList());
        return;
    }
    if (pg_store_use_corpus(store, m_corpusId) != 0) {
        pg_store_close(store);
        emit queryFinished(false, QString(), QVariantList());
        return;
    }

    QByteArray questionUtf8 = m_question.toUtf8();
    const char *questionCstr = questionUtf8.constData();

    // Load history BEFORE persisting the new question -- otherwise this
    // fetch would see its own not-yet-answered question as the most
    // recent "user" turn. A fetch failure degrades to "no history"
    // rather than aborting the query -- conversational context is an
    // enhancement here, not a precondition for answering at all.
    size_t history_count = 0;
    PgStoreChatMessage *history_rows = pg_store_get_chat_messages(store, m_sessionId, &history_count);
    if (history_rows == nullptr) {
        history_count = 0;
    }

    std::vector<LocalLlmTurn> turns(history_count);
    for (size_t i = 0; i < history_count; i++) {
        turns[i].role = history_rows[i].is_user ? "user" : "assistant";
        turns[i].content = history_rows[i].text;
    }

    pg_store_append_chat_message(store, m_sessionId, 1, questionCstr, nullptr);

    ToolChoice tool = tool_router_choose_tool(questionCstr, turns.data(), turns.size());

    QString answerText;
    QVariantList sources;
    bool ok = (tool == TOOL_READ_DOCUMENTS)
                  ? runReadPipeline(store, questionCstr, turns, &answerText, &sources)
                  : runSearchPipeline(store, questionCstr, turns, m_stopwords, m_wordnet, m_lemmatizer, &answerText,
                                      &sources);

    pg_store_chat_messages_free(history_rows, history_count);

    if (!ok) {
        pg_store_close(store);
        emit queryFinished(false, QString(), QVariantList());
        return;
    }

    QString sourcesJson = sourcesToJson(sources);
    pg_store_append_chat_message(store, m_sessionId, 0, answerText.toUtf8().constData(),
                                  sourcesJson.isEmpty() ? nullptr : sourcesJson.toUtf8().constData());
    pg_store_close(store);

    emit queryFinished(true, answerText, sources);
}
