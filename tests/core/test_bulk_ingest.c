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

    long total = bulk_ingest_tsv(TEST_CONNINFO, NULL, stopwords, wordnet, lemmatizer, TEST_TSV_PATH, 100, 0, 4);
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

static void test_bulk_ingest_captures_one_original_document_per_multi_chunk_row(void) {
    PgStore *reset_store = open_fresh_store();
    TEST_ASSERT(reset_store != NULL, "expected pg_store_open to succeed");
    PGresult *truncate_documents = PQexec(reset_store->conn, "TRUNCATE documents;");
    PQclear(truncate_documents);
    pg_store_close(reset_store);

    /* chunk_size 3 (words), no overlap, against an 9-word single-row
     * document -- guaranteed to split into multiple passages/chunks, the
     * exact case where "one documents row per source document, not per
     * chunk" actually matters. */
    write_tsv("400\tone two three four five six seven eight nine\n");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    long total = bulk_ingest_tsv(TEST_CONNINFO, NULL, stopwords, wordnet, lemmatizer, TEST_TSV_PATH, 3, 0, 1);
    TEST_ASSERT(total == 3, "expected 3 passages (9 words / chunk_size 3), got %ld", total);

    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    const char *params[1] = {"400"};
    PGresult *res =
        PQexecParams(store->conn, "SELECT text FROM documents WHERE document_name = $1;", 1, NULL, params, NULL, NULL, 0);
    TEST_ASSERT(PQntuples(res) == 1, "expected exactly one documents row despite 3 passages, got %d", PQntuples(res));
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 0), "one two three four five six seven eight nine");
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

    long total = bulk_ingest_tsv(TEST_CONNINFO, NULL, stopwords, wordnet, lemmatizer,
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

    long total = bulk_ingest_tsv(TEST_CONNINFO, NULL, stopwords, wordnet, lemmatizer, TEST_TSV_PATH, 100, 0, 1);
    TEST_ASSERT(total == -1, "expected -1 -- a malformed row fails the whole COPY, got %ld", total);

    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

static void test_bulk_ingest_targets_specified_corpus(void) {
    PgStore *reset_store = open_fresh_store();
    TEST_ASSERT(reset_store != NULL, "expected pg_store_open to succeed");

    /* A prior run in this same database could have left rows in the
     * legacy public schema (other tests in this file all target it) --
     * truncate so "public stayed untouched" below is a real signal, not
     * a leftover coincidence. */
    PGresult *truncate_res = PQexec(reset_store->conn, "TRUNCATE postings, terms, passages RESTART IDENTITY CASCADE;");
    PQclear(truncate_res);

    char *schema_name = NULL;
    int64_t corpus_id = pg_store_create_corpus(reset_store, "Bulk Ingest Test Corpus", &schema_name);
    TEST_ASSERT(corpus_id > 0, "expected corpus creation to succeed");
    pg_store_close(reset_store);

    write_tsv("300\tisolated corpus text one\n"
              "301\tisolated corpus text two\n");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    long total = bulk_ingest_tsv(TEST_CONNINFO, schema_name, stopwords, wordnet, lemmatizer, TEST_TSV_PATH, 100, 0, 4);
    TEST_ASSERT(total == 2, "expected 2 passages ingested into the target corpus, got %ld", total);

    PgStore *verify_store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(verify_store != NULL, "expected pg_store_open to succeed");

    char count_sql[128];
    snprintf(count_sql, sizeof(count_sql), "SELECT COUNT(*) FROM %s.passages;", schema_name);
    PGresult *corpus_res = PQexec(verify_store->conn, count_sql);
    TEST_ASSERT(PQresultStatus(corpus_res) == PGRES_TUPLES_OK, "expected corpus schema count query to succeed");
    TEST_ASSERT_STR_EQ(PQgetvalue(corpus_res, 0, 0), "2");
    PQclear(corpus_res);

    /* The whole point of this test: the legacy public schema, which
     * every OTHER bulk_ingest test in this file writes to, must stay
     * completely untouched by a run that targeted a specific corpus. */
    PGresult *public_res = PQexec(verify_store->conn, "SELECT COUNT(*) FROM public.passages;");
    TEST_ASSERT(PQresultStatus(public_res) == PGRES_TUPLES_OK, "expected public schema count query to succeed");
    TEST_ASSERT_STR_EQ(PQgetvalue(public_res, 0, 0), "0");
    PQclear(public_res);

    pg_store_close(verify_store);
    free(schema_name);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

static void test_bulk_ingest_rebuild_corpus_adds_new_documents_and_preserves_existing(void) {
    PgStore *setup_store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(setup_store != NULL, "expected pg_store_open to succeed");

    char *schema_name = NULL;
    int64_t corpus_id = pg_store_create_corpus(setup_store, "Rebuild Test Group", &schema_name);
    TEST_ASSERT(corpus_id > 0, "expected corpus creation to succeed");
    pg_store_close(setup_store);

    write_tsv("500\tfirst document text\n501\tsecond document text\n");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    long initial_total = bulk_ingest_tsv(TEST_CONNINFO, schema_name, stopwords, wordnet, lemmatizer, TEST_TSV_PATH,
                                          100, 0, 1);
    TEST_ASSERT(initial_total == 2, "expected 2 passages from the initial ingest, got %ld", initial_total);

    const char *new_names[1] = {"502"};
    const char *new_texts[1] = {"third document text"};
    long rebuilt_total = bulk_ingest_rebuild_corpus(TEST_CONNINFO, corpus_id, new_names, new_texts, 1, stopwords,
                                                     wordnet, lemmatizer, 100, 0, 1);
    TEST_ASSERT(rebuilt_total == 3, "expected 3 passages after rebuild (2 existing + 1 new), got %ld", rebuilt_total);

    PgStore *verify_store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(verify_store != NULL, "expected pg_store_open to succeed");

    /* This corpus_id must still resolve in the registry after the
     * rebuild, to the exact same id -- the swap replaces what's
     * physically behind the schema name, never the registry identity
     * itself (see pg_store_swap_corpus_schema()). Other tests in this
     * file create their own corpora and never reset the registry between
     * tests (unlike test_pg_store.c), so this checks for the specific
     * corpus this test created, not the registry's total count. */
    size_t corpus_count = 0;
    PgStoreCorpus *corpora = pg_store_list_corpora(verify_store, &corpus_count);
    int found = 0;
    for (size_t i = 0; i < corpus_count; i++) {
        if (corpora[i].id == corpus_id) {
            found = 1;
            break;
        }
    }
    TEST_ASSERT(found, "expected corpus_id %lld to still be in the registry after rebuild", (long long)corpus_id);
    pg_store_corpora_free(corpora, corpus_count);

    TEST_ASSERT(pg_store_use_corpus(verify_store, corpus_id) == 0, "expected use_corpus to succeed after rebuild");

    size_t doc_count = 0;
    PgStoreDocument *docs = pg_store_get_all_documents(verify_store, &doc_count);
    TEST_ASSERT(doc_count == 3, "expected 3 documents after rebuild, got %zu", doc_count);
    TEST_ASSERT_STR_EQ(docs[0].document_name, "500");
    TEST_ASSERT_STR_EQ(docs[1].document_name, "501");
    TEST_ASSERT_STR_EQ(docs[2].document_name, "502");
    TEST_ASSERT_STR_EQ(docs[2].text, "third document text");
    pg_store_documents_free(docs, doc_count);

    PGresult *passage_res = PQexec(verify_store->conn, "SELECT COUNT(*) FROM passages WHERE document_name = '502';");
    TEST_ASSERT(PQresultStatus(passage_res) == PGRES_TUPLES_OK, "expected passage count query to succeed");
    TEST_ASSERT_STR_EQ(PQgetvalue(passage_res, 0, 0), "1");
    PQclear(passage_res);

    pg_store_close(verify_store);
    free(schema_name);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

static void test_bulk_ingest_rebuild_corpus_new_document_replaces_existing_same_name(void) {
    PgStore *setup_store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(setup_store != NULL, "expected pg_store_open to succeed");

    char *schema_name = NULL;
    int64_t corpus_id = pg_store_create_corpus(setup_store, "Replace Test Group", &schema_name);
    TEST_ASSERT(corpus_id > 0, "expected corpus creation to succeed");
    pg_store_close(setup_store);

    write_tsv("600\told content\n");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    long initial_total =
        bulk_ingest_tsv(TEST_CONNINFO, schema_name, stopwords, wordnet, lemmatizer, TEST_TSV_PATH, 100, 0, 1);
    TEST_ASSERT(initial_total == 1, "expected 1 passage from the initial ingest, got %ld", initial_total);

    const char *new_names[1] = {"600"};
    const char *new_texts[1] = {"new content"};
    long rebuilt_total = bulk_ingest_rebuild_corpus(TEST_CONNINFO, corpus_id, new_names, new_texts, 1, stopwords,
                                                     wordnet, lemmatizer, 100, 0, 1);
    TEST_ASSERT(rebuilt_total == 1, "expected still exactly 1 passage (replaced, not added), got %ld", rebuilt_total);

    PgStore *verify_store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(verify_store != NULL, "expected pg_store_open to succeed");
    TEST_ASSERT(pg_store_use_corpus(verify_store, corpus_id) == 0, "expected use_corpus to succeed after rebuild");

    size_t doc_count = 0;
    PgStoreDocument *docs = pg_store_get_all_documents(verify_store, &doc_count);
    TEST_ASSERT(doc_count == 1, "expected exactly 1 document (replaced, not duplicated), got %zu", doc_count);
    TEST_ASSERT_STR_EQ(docs[0].document_name, "600");
    TEST_ASSERT_STR_EQ(docs[0].text, "new content");
    pg_store_documents_free(docs, doc_count);

    const char *params[1] = {"600"};
    PGresult *passage_res = PQexecParams(verify_store->conn, "SELECT text FROM passages WHERE document_name = $1;", 1,
                                          NULL, params, NULL, NULL, 0);
    TEST_ASSERT(PQntuples(passage_res) == 1, "expected exactly 1 passage for document 600");
    TEST_ASSERT_STR_EQ(PQgetvalue(passage_res, 0, 0), "new content");
    PQclear(passage_res);

    pg_store_close(verify_store);
    free(schema_name);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

static void test_bulk_ingest_rebuild_corpus_fails_for_nonexistent_corpus(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    const char *new_names[1] = {"700"};
    const char *new_texts[1] = {"some text"};
    long total = bulk_ingest_rebuild_corpus(TEST_CONNINFO, 999999, new_names, new_texts, 1, stopwords, wordnet,
                                             lemmatizer, 100, 0, 1);
    TEST_ASSERT(total == -1, "expected rebuild on a nonexistent corpus id to fail");

    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

int main(void) {
    test_bulk_ingest_ingests_every_row_exactly_once();
    test_bulk_ingest_captures_one_original_document_per_multi_chunk_row();
    test_bulk_ingest_missing_file_returns_negative_one();
    test_bulk_ingest_fails_atomically_on_a_malformed_row();
    test_bulk_ingest_targets_specified_corpus();
    test_bulk_ingest_rebuild_corpus_adds_new_documents_and_preserves_existing();
    test_bulk_ingest_rebuild_corpus_new_document_replaces_existing_same_name();
    test_bulk_ingest_rebuild_corpus_fails_for_nonexistent_corpus();
    remove(TEST_TSV_PATH);
    return test_summary();
}
