// Deliberately calls query_formulation_contextualize_question() (a plain
// LLM call that resolves the question against conversation history into
// a standalone search query, e.g. "what about that instead?" -> "what is
// the minimum age for a Junior Operator Class MJ license?") rather than
// the WordNet-driven query_formulation_formulate_query() -- the latter
// is NOT dead code (eval.c's --use-llm-expansion comparison mode still
// calls it), it's just solving a different problem (synonym expansion)
// than what an interactive multi-turn chat needs (resolving follow-up
// references). The full 6,980-query naked-BM25 eval run (see SPEED.md)
// found the WordNet expansion step wasn't measurably improving retrieval
// quality even with no conversation history in the picture at all --
// that finding motivated cutting it from the chat pipeline in the first
// place, and is unrelated to why contextualization is needed now.
#include "QueryWorker.h"

extern "C" {
#include "bm25.h"
#include "generation.h"
#include "pg_store.h"
#include "query_formulation.h"
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

    // Query reformulation: see this file's top-of-file comment for why
    // this LLM step exists and how it differs from the WordNet-driven
    // one. Only fails on allocation failure.
    char *reformulated = query_formulation_contextualize_question(questionCstr, turns.data(), turns.size());
    if (reformulated == nullptr) {
        pg_store_chat_messages_free(history_rows, history_count);
        pg_store_close(store);
        emit queryFinished(false, QString(), QVariantList());
        return;
    }

    // Tokenize/stopword-filter/lemmatize the *reformulated* text, not
    // the raw question -- a follow-up like "what about that instead?"
    // has almost nothing for BM25 to work with until reformulation has
    // resolved it into something concrete.
    TokenList *terms = query_formulation_terms_only(reformulated, m_stopwords, m_wordnet, m_lemmatizer);
    free(reformulated);
    if (terms == nullptr) {
        pg_store_chat_messages_free(history_rows, history_count);
        pg_store_close(store);
        emit queryFinished(false, QString(), QVariantList());
        return;
    }
    if (terms->count == 0) {
        token_list_free(terms);
        QString answer = tr("I don't have enough to search for in that question -- could you rephrase it?");
        pg_store_append_chat_message(store, m_sessionId, 0, answer.toUtf8().constData(), nullptr);
        pg_store_chat_messages_free(history_rows, history_count);
        pg_store_close(store);
        emit queryFinished(true, answer, QVariantList());
        return;
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
        pg_store_chat_messages_free(history_rows, history_count);
        pg_store_close(store);
        emit queryFinished(false, QString(), QVariantList());
        return;
    }

    if (results->count == 0) {
        bm25_result_set_free(results);
        QString answer = tr("No matching passages found in this group for that question.");
        pg_store_append_chat_message(store, m_sessionId, 0, answer.toUtf8().constData(), nullptr);
        pg_store_chat_messages_free(history_rows, history_count);
        pg_store_close(store);
        emit queryFinished(true, answer, QVariantList());
        return;
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
    pg_store_chat_messages_free(history_rows, history_count);

    if (answer == nullptr) {
        pg_store_close(store);
        emit queryFinished(false, QString(), QVariantList());
        return;
    }

    QString answerText = QString::fromUtf8(answer);
    free(answer);

    QString sourcesJson = sourcesToJson(sources);
    pg_store_append_chat_message(store, m_sessionId, 0, answerText.toUtf8().constData(),
                                  sourcesJson.isEmpty() ? nullptr : sourcesJson.toUtf8().constData());
    pg_store_close(store);

    emit queryFinished(true, answerText, sources);
}
