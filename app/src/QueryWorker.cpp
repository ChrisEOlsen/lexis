#include "QueryWorker.h"

extern "C" {
#include "bm25.h"
#include "generation.h"
#include "pg_store.h"
#include "query_formulation.h"
}

#include <cstdlib>

namespace {
// Mirrors main.c's LEXIS_TOP_K exactly -- no config UI for this yet,
// and matching the CLI's own retrieval behavior matters more right now
// than tuning it differently here.
constexpr size_t kTopK = 5;
} // namespace

QueryWorker::QueryWorker(QString conninfo, qint64 corpusId, QString question, const StopwordSet *stopwords,
                          const WordNetTable *wordnet, const Lemmatizer *lemmatizer, QObject *parent)
    : QThread(parent), m_conninfo(std::move(conninfo)), m_corpusId(corpusId), m_question(std::move(question)),
      m_stopwords(stopwords), m_wordnet(wordnet), m_lemmatizer(lemmatizer) {
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

    // Query formulation: tokenize/stopword-filter/lemmatize, gather
    // WordNet candidates, ask the local model which to include -- falls
    // back to the plain lemmatized terms internally if that call fails,
    // so this itself only fails on allocation failure.
    TokenList *terms = query_formulation_formulate_query(questionCstr, m_stopwords, m_wordnet, m_lemmatizer);
    if (terms == nullptr) {
        pg_store_close(store);
        emit queryFinished(false, QString(), QVariantList());
        return;
    }
    if (terms->count == 0) {
        token_list_free(terms);
        pg_store_close(store);
        emit queryFinished(true, QString(), QVariantList()); // a real, valid "nothing to search for" outcome
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
        pg_store_close(store);
        emit queryFinished(false, QString(), QVariantList());
        return;
    }

    if (results->count == 0) {
        bm25_result_set_free(results);
        pg_store_close(store);
        emit queryFinished(true, QString(), QVariantList()); // valid outcome: no matching passages
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

    char *answer = generation_generate_answer(questionCstr, store, results);
    bm25_result_set_free(results);
    pg_store_close(store);

    if (answer == nullptr) {
        emit queryFinished(false, QString(), QVariantList());
        return;
    }

    QString answerText = QString::fromUtf8(answer);
    free(answer);
    emit queryFinished(true, answerText, sources);
}
