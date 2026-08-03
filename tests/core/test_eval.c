/*
 * Tests for src/core/eval.c -- the retrieval-quality evaluation harness.
 * Uses the real native Postgres instance (lexis_test database, port
 * 5434) -- `make pg-start` must be running for these to pass.
 *
 * local_llm_client_init() is never called in this test binary, so
 * query_formulation_formulate_query() always falls back to its plain
 * stopword-filtered terms (see test_query_formulation.c) -- deterministic
 * and network/model-free, exactly what a unit test wants here.
 */

#include "eval.h"
#include "bulk_ingest.h"
#include "pg_store.h"
#include "stopwords.h"
#include "wordnet.h"
#include "lemmatizer.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CONNINFO "host=127.0.0.1 port=5434 dbname=lexis_test user=lexis password=lexis_dev_only"
#define STOPWORD_FILE "data/stopwords/english.txt"
#define WORDNET_DIR "data/wordnet"
#define TEST_QUERIES_PATH "build/test_eval_queries.tsv"
#define TEST_QRELS_PATH "build/test_eval_qrels.tsv"
#define TEST_SEED_TSV_PATH "build/test_eval_seed.tsv"

static void write_file(const char *path, const char *contents) {
    FILE *fp = fopen(path, "wb");
    fwrite(contents, 1, strlen(contents), fp);
    fclose(fp);
}

/* Seeds one document into the index via the real ingestion pipeline
 * (bulk_ingest_tsv(), single-threaded) instead of any lower-level
 * document-ingestion primitive -- bulk_ingest.c's three-phase pipeline
 * is the only ingestion path this codebase has, so eval's own test
 * fixtures go through the same path real corpora do. `pid`/`text` must
 * not themselves contain a literal tab, double-quote, or newline (none
 * of this file's fixtures do) -- see pg_store.c's pg_store_copy_
 * documents_raw() for why those specifically would need CSV quoting. */
static long seed_document(const StopwordSet *stopwords, const WordNetTable *wordnet,
                           const Lemmatizer *lemmatizer, const char *pid, const char *text,
                           size_t chunk_size, size_t overlap) {
    char line[4096];
    snprintf(line, sizeof(line), "%s\t%s\n", pid, text);
    write_file(TEST_SEED_TSV_PATH, line);
    return bulk_ingest_tsv(TEST_CONNINFO, NULL, stopwords, wordnet, lemmatizer, TEST_SEED_TSV_PATH, chunk_size,
                            overlap, 1);
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
        TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is native Postgres running (make pg-start)?");

        long passages = seed_document(stopwords, wordnet, lemmatizer, "multi_chunk_doc", MULTI_CHUNK_TEXT, 10, 0);
        TEST_ASSERT(passages > 1, "expected the fixture text to split into multiple chunks, got %ld",
                    passages);

        write_file(TEST_QUERIES_PATH, "q1\thypertension treatment\n");
        write_file(TEST_QRELS_PATH, "query-id\tcorpus-id\tscore\nq1\tmulti_chunk_doc\t1\n");

        EvalMetrics metrics =
            eval_run(store, stopwords, wordnet, lemmatizer, TEST_QUERIES_PATH, TEST_QRELS_PATH, 1);
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

        long passages = seed_document(stopwords, wordnet, lemmatizer, "unrelated_doc",
                                       "cardiac arrest response plan", 100, 0);
        TEST_ASSERT(passages == 1, "expected setup ingest to succeed");

        write_file(TEST_QUERIES_PATH, "q1\tcardiac arrest\n");
        write_file(TEST_QRELS_PATH, "query-id\tcorpus-id\tscore\nq1\tsome_other_doc_never_indexed\t1\n");

        EvalMetrics metrics =
            eval_run(store, stopwords, wordnet, lemmatizer, TEST_QUERIES_PATH, TEST_QRELS_PATH, 1);
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
            eval_run(store, stopwords, wordnet, lemmatizer, TEST_QUERIES_PATH, TEST_QRELS_PATH, 1);
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
            eval_run(store, stopwords, wordnet, lemmatizer, TEST_QUERIES_PATH, TEST_QRELS_PATH, 1);
        TEST_ASSERT(metrics.queries_evaluated == 0, "expected the query to be skipped (only a score=0 "
                                                      "row, nothing relevant), got %ld evaluated",
                    metrics.queries_evaluated);
        TEST_ASSERT(metrics.queries_skipped == 1, "expected 1 query skipped, got %ld",
                    metrics.queries_skipped);

        pg_store_close(store);
    }

    {
        /* use_llm_expansion=0 -- query_formulation_terms_only() instead
         * of query_formulation_formulate_query(), no model call at all.
         * local_llm_client_init() is never called anywhere in this test
         * binary (see this file's header comment), so this exercises the
         * real no-LLM code path standalone, not by coincidence. */
        PgStore *store = open_fresh_store();
        TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

        long passages = seed_document(stopwords, wordnet, lemmatizer, "hypertension_doc",
                                       "hypertension treatment options", 100, 0);
        TEST_ASSERT(passages == 1, "expected setup ingest to succeed");

        write_file(TEST_QUERIES_PATH, "q1\thypertension treatment\n");
        write_file(TEST_QRELS_PATH, "query-id\tcorpus-id\tscore\nq1\thypertension_doc\t1\n");

        EvalMetrics metrics =
            eval_run(store, stopwords, wordnet, lemmatizer, TEST_QUERIES_PATH, TEST_QRELS_PATH, 0);
        TEST_ASSERT(metrics.queries_evaluated == 1, "expected 1 query evaluated, got %ld",
                    metrics.queries_evaluated);
        TEST_ASSERT(metrics.mrr_at_10 == 1.0, "expected plain lemmatized terms alone to still find the "
                                               "exact-match document (MRR@10 == 1.0), got %f",
                    metrics.mrr_at_10);
        TEST_ASSERT(metrics.recall_at_10 == 1.0, "expected Recall@10 == 1.0, got %f", metrics.recall_at_10);

        pg_store_close(store);
    }

    {
        EvalMetrics metrics =
            eval_run(NULL, stopwords, wordnet, lemmatizer, "build/does_not_exist_queries.tsv", TEST_QRELS_PATH, 1);
        TEST_ASSERT(metrics.queries_evaluated == -1, "expected -1 for a missing queries file, got %ld",
                    metrics.queries_evaluated);
    }

    {
        write_file(TEST_QUERIES_PATH, "q1\tsome question\n");
        EvalMetrics metrics = eval_run(NULL, stopwords, wordnet, lemmatizer, TEST_QUERIES_PATH,
                                        "build/does_not_exist_qrels.tsv", 1);
        TEST_ASSERT(metrics.queries_evaluated == -1, "expected -1 for a missing qrels file, got %ld",
                    metrics.queries_evaluated);
    }

    remove(TEST_QUERIES_PATH);
    remove(TEST_QRELS_PATH);
    remove(TEST_SEED_TSV_PATH);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    return test_summary();
}
