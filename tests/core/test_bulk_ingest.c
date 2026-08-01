/*
 * Tests for src/core/bulk_ingest.c -- the three-phase, deferred-term-
 * resolution TSV ingestion pipeline. Uses the real native Postgres
 * instance (lexis_test database, port 5434) -- `make pg-start` must be
 * running for these to pass.
 */

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
#define TEST_TSV_PATH "build/test_bulk_ingest.tsv"

static void write_tsv(const char *contents) {
    FILE *fp = fopen(TEST_TSV_PATH, "wb");
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

static void test_bulk_ingest_ingests_every_row_exactly_once(void) {
    PgStore *reset_store = open_fresh_store();
    TEST_ASSERT(reset_store != NULL, "expected pg_store_open to succeed -- is native Postgres running (make pg-start)?");
    pg_store_close(reset_store);

    /* Six rows sharing common vocabulary ("hypertension"), TSV-formatted
     * as "<pid><TAB><text>" -- mirrors real MS MARCO passage rows. More
     * worker threads than rows exercises both the shared-cursor
     * work-stealing and the concurrent term-dedup path (multiple threads
     * racing to be the first to insert "hypertension"). */
    write_tsv("100\thypertension treatment options\n"
              "101\thypertension diagnosis criteria\n"
              "102\thypertension risk factors\n"
              "103\tcardiac arrest response plan\n"
              "104\tcardiac rehabilitation program\n"
              "105\thypertension medication dosage\n");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    long total = bulk_ingest_tsv(TEST_CONNINFO, stopwords, wordnet, lemmatizer, TEST_TSV_PATH, 100, 0, 4);
    TEST_ASSERT(total == 6, "expected 6 total passages across all 6 rows, got %ld", total);

    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    PGresult *res = PQexec(store->conn, "SELECT COUNT(*) FROM passages;");
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 6, "expected 6 passages in the index, got %s",
                PQgetvalue(res, 0, 0));
    PQclear(res);

    /* document_name must be the row's own pid (not a filename) so
     * results can be mapped back to the original corpus ID later. */
    res = PQexec(store->conn, "SELECT COUNT(*) FROM passages WHERE document_name = '103';");
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 1, "expected pid 103's row to be stored as document_name, got %s",
                PQgetvalue(res, 0, 0));
    PQclear(res);

    /* "hypertension" appears in 4 different rows, almost certainly
     * ingested by different worker threads racing on the same new term
     * -- must still collapse to exactly one terms row. */
    res = PQexec(store->conn, "SELECT COUNT(*) FROM terms WHERE term = 'hypertension';");
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 1,
                "expected \"hypertension\" deduped to exactly 1 term row despite concurrent "
                "writers, got %s",
                PQgetvalue(res, 0, 0));
    PQclear(res);

    pg_store_close(store);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

static void test_bulk_ingest_missing_file_returns_negative_one(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    long total = bulk_ingest_tsv(TEST_CONNINFO, stopwords, wordnet, lemmatizer,
                                  "build/does_not_exist.tsv", 100, 0, 4);
    TEST_ASSERT(total == -1, "expected -1 for a missing TSV file, got %ld", total);

    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

static void test_bulk_ingest_fails_atomically_on_a_malformed_row(void) {
    PgStore *reset_store = open_fresh_store();
    TEST_ASSERT(reset_store != NULL, "expected pg_store_open to succeed");
    pg_store_close(reset_store);

    /* Phase 1 loads the whole file via a single COPY (see
     * bulk_ingest.h) -- unlike the old row-by-row streaming pipeline,
     * a single malformed row (here: a missing tab, so the wrong column
     * count) fails the WHOLE load atomically rather than being skipped.
     * That's an intentional trade-off, not a regression: real MS MARCO
     * corpus.tsv is machine-generated and verified clean at full scale
     * (see SPEED.md), and silently dropping rows on a bulk load is the
     * wrong default for a pipeline whose whole point is trusting the
     * source file's format. */
    write_tsv("200\tvalid row one\n"
              "this line has no tab in it\n"
              "201\tvalid row two\n");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    long total = bulk_ingest_tsv(TEST_CONNINFO, stopwords, wordnet, lemmatizer, TEST_TSV_PATH, 100, 0, 1);
    TEST_ASSERT(total == -1, "expected -1 -- a malformed row fails the whole COPY, got %ld", total);

    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

int main(void) {
    test_bulk_ingest_ingests_every_row_exactly_once();
    test_bulk_ingest_missing_file_returns_negative_one();
    test_bulk_ingest_fails_atomically_on_a_malformed_row();
    remove(TEST_TSV_PATH);
    return test_summary();
}
