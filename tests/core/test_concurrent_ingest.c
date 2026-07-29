/*
 * Tests for src/core/concurrent_ingest.c — genuinely concurrent ingestion
 * via multiple Postgres connections. Uses the real docker-compose Postgres
 * instance (lexis_test database) -- `docker compose up -d` must be
 * running for these to pass.
 */

#include "concurrent_ingest.h"
#include "pg_store.h"
#include "stopwords.h"
#include "wordnet.h"
#include "lemmatizer.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_CONNINFO "host=127.0.0.1 port=5433 dbname=lexis_test user=lexis password=lexis_dev_only"
#define STOPWORD_FILE "data/stopwords/english.txt"
#define WORDNET_DIR "data/wordnet"
#define TEST_CORPUS_DIR "build/test_concurrent_ingest_corpus"

static void write_file_at(const char *path, const char *contents) {
    FILE *fp = fopen(path, "wb");
    fwrite(contents, 1, strlen(contents), fp);
    fclose(fp);
}

static void reset_corpus_dir(void) {
    remove(TEST_CORPUS_DIR "/doc1.txt");
    remove(TEST_CORPUS_DIR "/doc2.txt");
    remove(TEST_CORPUS_DIR "/doc3.txt");
    remove(TEST_CORPUS_DIR "/doc4.txt");
    remove(TEST_CORPUS_DIR "/doc5.txt");
    remove(TEST_CORPUS_DIR "/doc6.txt");
    rmdir(TEST_CORPUS_DIR);
    mkdir(TEST_CORPUS_DIR, 0755);
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

static void test_concurrent_ingest_ingests_every_file_exactly_once(void) {
    PgStore *reset_store = open_fresh_store();
    TEST_ASSERT(reset_store != NULL, "expected pg_store_open to succeed -- is docker compose up?");
    pg_store_close(reset_store);

    reset_corpus_dir();
    /* Six small documents sharing common vocabulary ("hypertension"),
     * split across more worker threads than documents in one call and
     * fewer in another -- exercises both the work-stealing queue and
     * the concurrent term-dedup path (multiple threads racing to be the
     * first to insert "hypertension"). */
    write_file_at(TEST_CORPUS_DIR "/doc1.txt", "hypertension treatment options");
    write_file_at(TEST_CORPUS_DIR "/doc2.txt", "hypertension diagnosis criteria");
    write_file_at(TEST_CORPUS_DIR "/doc3.txt", "hypertension risk factors");
    write_file_at(TEST_CORPUS_DIR "/doc4.txt", "cardiac arrest response plan");
    write_file_at(TEST_CORPUS_DIR "/doc5.txt", "cardiac rehabilitation program");
    write_file_at(TEST_CORPUS_DIR "/doc6.txt", "hypertension medication dosage");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    long total = concurrent_ingest_corpus(TEST_CONNINFO, stopwords, wordnet, lemmatizer,
                                           TEST_CORPUS_DIR, 100, 0, 4);
    TEST_ASSERT(total == 6, "expected 6 total passages across all 6 files, got %ld", total);

    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    PGresult *res = PQexec(store->conn, "SELECT COUNT(*) FROM passages;");
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 6, "expected 6 passages in the index, got %s",
                PQgetvalue(res, 0, 0));
    PQclear(res);

    /* "hypertension" appears in 4 different documents, almost certainly
     * ingested by different worker threads racing on the same new term --
     * must still collapse to exactly one terms row. */
    res = PQexec(store->conn, "SELECT COUNT(*) FROM terms WHERE term = 'hypertension';");
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 1,
                "expected \"hypertension\" deduped to exactly 1 term row despite concurrent "
                "writers, got %s",
                PQgetvalue(res, 0, 0));
    PQclear(res);

    /* And that one term row should have exactly 4 postings -- one per
     * document that actually contains it. */
    res = PQexec(store->conn,
                 "SELECT COUNT(*) FROM postings WHERE term_id = "
                 "(SELECT id FROM terms WHERE term = 'hypertension');");
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 4,
                "expected 4 postings for \"hypertension\" (one per containing document), got %s",
                PQgetvalue(res, 0, 0));
    PQclear(res);

    pg_store_close(store);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

static void test_concurrent_ingest_missing_directory_returns_negative_one(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    long total = concurrent_ingest_corpus(TEST_CONNINFO, stopwords, wordnet, lemmatizer,
                                           "build/does_not_exist_corpus_dir", 100, 0, 4);
    TEST_ASSERT(total == -1, "expected -1 for a missing directory, got %ld", total);

    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

static void test_concurrent_ingest_single_thread_matches_sequential_behavior(void) {
    PgStore *reset_store = open_fresh_store();
    TEST_ASSERT(reset_store != NULL, "expected pg_store_open to succeed");
    pg_store_close(reset_store);

    reset_corpus_dir();
    write_file_at(TEST_CORPUS_DIR "/doc1.txt", "hypertension treatment options");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    /* thread_count == 1 should behave identically to a plain sequential
     * ingest -- no threading-specific edge case with exactly one worker. */
    long total = concurrent_ingest_corpus(TEST_CONNINFO, stopwords, wordnet, lemmatizer,
                                           TEST_CORPUS_DIR, 100, 0, 1);
    TEST_ASSERT(total == 1, "expected 1 passage with a single worker thread, got %ld", total);

    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

int main(void) {
    test_concurrent_ingest_ingests_every_file_exactly_once();
    test_concurrent_ingest_missing_directory_returns_negative_one();
    test_concurrent_ingest_single_thread_matches_sequential_behavior();
    reset_corpus_dir();
    rmdir(TEST_CORPUS_DIR);
    return test_summary();
}
