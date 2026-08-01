/*
 * Tests for src/core/eval.c -- the retrieval-quality evaluation harness.
 * Uses the real docker-compose Postgres instance (lexis_test database) --
 * `docker compose up -d` must be running for these to pass.
 *
 * local_llm_client_init() is never called in this test binary, so
 * query_formulation_formulate_query() always falls back to its plain
 * stopword-filtered terms (see test_query_formulation.c) -- deterministic
 * and network/model-free, exactly what a unit test wants here.
 */

#include "eval.h"
#include "ingest.h"
#include "pg_store.h"
#include "stopwords.h"
#include "wordnet.h"
#include "lemmatizer.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CONNINFO "host=127.0.0.1 port=5433 dbname=lexis_test user=lexis password=lexis_dev_only"
#define STOPWORD_FILE "data/stopwords/english.txt"
#define WORDNET_DIR "data/wordnet"
#define TEST_QUERIES_PATH "build/test_eval_queries.tsv"
#define TEST_QRELS_PATH "build/test_eval_qrels.tsv"

static void write_file(const char *path, const char *contents) {
    FILE *fp = fopen(path, "wb");
    fwrite(contents, 1, strlen(contents), fp);
    fclose(fp);
}

static PgStore *open_fresh_store(void) {
    PgStore *store = pg_store_open(TEST_CONNINFO);
    if (store == NULL) {
        return NULL;
    }
    PGresult *res = PQexec(store->conn, "TRUNCATE postings, terms, passages RESTART IDENTITY CASCADE;");
    PQclear(res);
    return store;
}

/* A long enough passage (> LEXIS's usual small chunk_size used here) to
 * split into 3 chunks sharing one document_name -- reproduces the exact
 * multi-chunk-document scenario that once let Recall@K exceed 1.0 (a
 * relevant pid counted once per matching chunk instead of once overall). */
static const char *MULTI_CHUNK_TEXT =
    "hypertension treatment options one two three four five six seven eight nine ten "
    "hypertension treatment options eleven twelve thirteen fourteen fifteen sixteen "
    "hypertension treatment options seventeen eighteen nineteen twenty twentyone twentytwo";

int main(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    {
        /* Regression test for the recall-can-exceed-1.0 bug: a document
         * split into multiple chunks must still count as at most one hit
         * against a qrels row that names it once. */
        PgStore *store = open_fresh_store();
        TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is docker compose up?");

        long passages = ingest_document_from_text(store, stopwords, wordnet, lemmatizer, MULTI_CHUNK_TEXT,
                                                    "multi_chunk_doc", 10, 0, NULL);
        TEST_ASSERT(passages > 1, "expected the fixture text to split into multiple chunks, got %ld",
                    passages);

        write_file(TEST_QUERIES_PATH, "q1\thypertension treatment\n");
        write_file(TEST_QRELS_PATH, "query-id\tcorpus-id\tscore\nq1\tmulti_chunk_doc\t1\n");

        EvalMetrics metrics =
            eval_run(store, stopwords, wordnet, lemmatizer, TEST_QUERIES_PATH, TEST_QRELS_PATH);
        TEST_ASSERT(metrics.queries_evaluated == 1, "expected 1 query evaluated, got %ld",
                    metrics.queries_evaluated);
        TEST_ASSERT(metrics.recall_at_10 <= 1.0 + 1e-9, "expected Recall@10 <= 1.0, got %f",
                    metrics.recall_at_10);
        TEST_ASSERT(metrics.recall_at_100 <= 1.0 + 1e-9, "expected Recall@100 <= 1.0, got %f",
                    metrics.recall_at_100);
        TEST_ASSERT(metrics.recall_at_10 == 1.0, "expected the multi-chunk doc to still score a full "
                                                  "hit (1.0), got %f",
                    metrics.recall_at_10);
        TEST_ASSERT(metrics.mrr_at_10 == 1.0, "expected MRR@10 == 1.0 (the only relevant doc ranked "
                                               "first), got %f",
                    metrics.mrr_at_10);

        pg_store_close(store);
    }

    {
        /* A relevant pid that's never actually retrieved -- both metrics
         * should be exactly 0 for that query, not just "not 1.0". */
        PgStore *store = open_fresh_store();
        TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

        long passages = ingest_document_from_text(store, stopwords, wordnet, lemmatizer,
                                                    "cardiac arrest response plan", "unrelated_doc", 100, 0,
                                                    NULL);
        TEST_ASSERT(passages == 1, "expected setup ingest to succeed");

        write_file(TEST_QUERIES_PATH, "q1\tcardiac arrest\n");
        write_file(TEST_QRELS_PATH, "query-id\tcorpus-id\tscore\nq1\tsome_other_doc_never_indexed\t1\n");

        EvalMetrics metrics =
            eval_run(store, stopwords, wordnet, lemmatizer, TEST_QUERIES_PATH, TEST_QRELS_PATH);
        TEST_ASSERT(metrics.queries_evaluated == 1, "expected 1 query evaluated, got %ld",
                    metrics.queries_evaluated);
        TEST_ASSERT(metrics.mrr_at_10 == 0.0, "expected MRR@10 == 0 for a miss, got %f", metrics.mrr_at_10);
        TEST_ASSERT(metrics.recall_at_10 == 0.0, "expected Recall@10 == 0 for a miss, got %f",
                    metrics.recall_at_10);

        pg_store_close(store);
    }

    {
        /* A query with no matching qrels row at all is skipped, not
         * counted as evaluated and not treated as a failure. */
        PgStore *store = open_fresh_store();
        TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

        write_file(TEST_QUERIES_PATH, "q1\tsome question\nq2\tanother question\n");
        write_file(TEST_QRELS_PATH, "query-id\tcorpus-id\tscore\nq1\tsome_doc\t1\n");

        EvalMetrics metrics =
            eval_run(store, stopwords, wordnet, lemmatizer, TEST_QUERIES_PATH, TEST_QRELS_PATH);
        TEST_ASSERT(metrics.queries_evaluated == 1, "expected 1 query evaluated (q1), got %ld",
                    metrics.queries_evaluated);
        TEST_ASSERT(metrics.queries_skipped == 1, "expected 1 query skipped (q2, no qrels), got %ld",
                    metrics.queries_skipped);

        pg_store_close(store);
    }

    {
        /* A qrels row with score 0 doesn't count as relevant -- matches
         * qrels' own convention for a judged-but-not-relevant pair. */
        PgStore *store = open_fresh_store();
        TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

        write_file(TEST_QUERIES_PATH, "q1\tsome question\n");
        write_file(TEST_QRELS_PATH, "query-id\tcorpus-id\tscore\nq1\tsome_doc\t0\n");

        EvalMetrics metrics =
            eval_run(store, stopwords, wordnet, lemmatizer, TEST_QUERIES_PATH, TEST_QRELS_PATH);
        TEST_ASSERT(metrics.queries_evaluated == 0, "expected the query to be skipped (only a score=0 "
                                                      "row, nothing relevant), got %ld evaluated",
                    metrics.queries_evaluated);
        TEST_ASSERT(metrics.queries_skipped == 1, "expected 1 query skipped, got %ld",
                    metrics.queries_skipped);

        pg_store_close(store);
    }

    {
        EvalMetrics metrics =
            eval_run(NULL, stopwords, wordnet, lemmatizer, "build/does_not_exist_queries.tsv", TEST_QRELS_PATH);
        TEST_ASSERT(metrics.queries_evaluated == -1, "expected -1 for a missing queries file, got %ld",
                    metrics.queries_evaluated);
    }

    {
        write_file(TEST_QUERIES_PATH, "q1\tsome question\n");
        EvalMetrics metrics = eval_run(NULL, stopwords, wordnet, lemmatizer, TEST_QUERIES_PATH,
                                        "build/does_not_exist_qrels.tsv");
        TEST_ASSERT(metrics.queries_evaluated == -1, "expected -1 for a missing qrels file, got %ld",
                    metrics.queries_evaluated);
    }

    remove(TEST_QUERIES_PATH);
    remove(TEST_QRELS_PATH);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    return test_summary();
}
