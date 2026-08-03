/*
 * Tests for src/core/pg_store.c — PgStore lifecycle and the passage/term/
 * posting write path against a real Postgres instance (the native
 * install, port 5434 -- `make pg-start` must be running for these to
 * pass). TRUNCATE resets state between tests, since there's no file to
 * delete the way the SQLite version's tests worked.
 */

#include "pg_store.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CONNINFO "host=127.0.0.1 port=5434 dbname=lexis_test user=lexis password=lexis_dev_only"

static PgStore *open_fresh_store(void) {
    PgStore *store = pg_store_open(TEST_CONNINFO);
    if (store == NULL) {
        return NULL;
    }
    /* Dependency order: postings references both other tables. */
    PGresult *res = PQexec(store->conn, "TRUNCATE postings, terms, passages RESTART IDENTITY CASCADE;");
    PQclear(res);
    return store;
}

/* Drops every schema a prior test run registered (in case a prior run
 * crashed mid-test and left one behind -- CREATE SCHEMA would otherwise
 * collide with it) and empties the registry itself, so every corpus test
 * starts from a genuinely clean slate regardless of what earlier runs
 * left behind. Tolerates public.corpora not existing yet (first-ever
 * run) -- both queries just no-op in that case. */
static void reset_corpora_registry(PgStore *store) {
    PGresult *res = PQexec(store->conn, "SELECT schema_name FROM public.corpora;");
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(res); i++) {
            char drop_sql[256];
            snprintf(drop_sql, sizeof(drop_sql), "DROP SCHEMA IF EXISTS %s CASCADE;", PQgetvalue(res, i, 0));
            PGresult *drop_res = PQexec(store->conn, drop_sql);
            PQclear(drop_res);
        }
    }
    PQclear(res);
    PGresult *truncate_res = PQexec(store->conn, "TRUNCATE public.corpora RESTART IDENTITY CASCADE;");
    PQclear(truncate_res);
}

static int schema_has_lexis_tables(PgStore *store, const char *schema_name) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM information_schema.tables "
             "WHERE table_schema = '%s' AND table_name IN ('passages', 'terms', 'postings');",
             schema_name);
    PGresult *res = PQexec(store->conn, sql);
    int count = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
        count = atoi(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    return count == 3;
}

static void test_create_corpus_creates_schema_and_registry_row(void) {
    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is native Postgres running (make pg-start)?");
    reset_corpora_registry(store);

    char *schema_name = NULL;
    int64_t id = pg_store_create_corpus(store, "Test Group", &schema_name);
    TEST_ASSERT(id > 0, "expected a positive corpus id, got %lld", (long long)id);
    TEST_ASSERT(schema_name != NULL, "expected schema_name_out to be set on success");

    char expected_schema[64];
    snprintf(expected_schema, sizeof(expected_schema), "corpus_%lld", (long long)id);
    TEST_ASSERT_STR_EQ(schema_name, expected_schema);
    TEST_ASSERT(schema_has_lexis_tables(store, schema_name),
                "expected passages/terms/postings to exist in the new schema");

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)id);
    const char *params[1] = {id_str};
    PGresult *res = PQexecParams(store->conn, "SELECT display_name, schema_name FROM public.corpora WHERE id = $1;",
                                  1, NULL, params, NULL, NULL, 0);
    TEST_ASSERT(PQntuples(res) == 1, "expected exactly one registry row for the new corpus");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 0), "Test Group");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 1), schema_name);
    PQclear(res);

    free(schema_name);
    pg_store_close(store);
}

static void test_create_corpus_isolates_tables_between_corpora(void) {
    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");
    reset_corpora_registry(store);

    char *schema_a = NULL;
    char *schema_b = NULL;
    TEST_ASSERT(pg_store_create_corpus(store, "Group A", &schema_a) > 0, "expected corpus A to be created");
    TEST_ASSERT(pg_store_create_corpus(store, "Group B", &schema_b) > 0, "expected corpus B to be created");

    char insert_sql[256];
    snprintf(insert_sql, sizeof(insert_sql),
             "INSERT INTO %s.passages (document_name, chunk_id, text, token_count) "
             "VALUES ('doc-a', 0, 'only in A', 3);",
             schema_a);
    PGresult *insert_res = PQexec(store->conn, insert_sql);
    TEST_ASSERT(PQresultStatus(insert_res) == PGRES_COMMAND_OK, "expected insert into corpus A's own schema to succeed");
    PQclear(insert_res);

    char count_sql[128];
    snprintf(count_sql, sizeof(count_sql), "SELECT count(*) FROM %s.passages;", schema_b);
    PGresult *count_res = PQexec(store->conn, count_sql);
    TEST_ASSERT(PQresultStatus(count_res) == PGRES_TUPLES_OK, "expected count query on corpus B to succeed");
    TEST_ASSERT_STR_EQ(PQgetvalue(count_res, 0, 0), "0");
    PQclear(count_res);

    free(schema_a);
    free(schema_b);
    pg_store_close(store);
}

static void test_create_corpus_rejects_empty_display_name(void) {
    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");
    reset_corpora_registry(store);

    char *schema_name = NULL;
    int64_t id = pg_store_create_corpus(store, "", &schema_name);
    TEST_ASSERT(id == -1, "expected an empty display_name to be rejected");
    TEST_ASSERT(schema_name == NULL, "expected schema_name_out left untouched on failure");

    pg_store_close(store);
}

static void test_use_corpus_scopes_queries_to_chosen_corpus(void) {
    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");
    reset_corpora_registry(store);

    char *schema_a = NULL;
    char *schema_b = NULL;
    int64_t corpus_a = pg_store_create_corpus(store, "Group A", &schema_a);
    int64_t corpus_b = pg_store_create_corpus(store, "Group B", &schema_b);
    TEST_ASSERT(corpus_a > 0 && corpus_b > 0, "expected both corpora to be created");

    /* Real, unmodified pg_store_insert_passage()/pg_store_get_passage()
     * calls -- the whole point of search_path scoping is that these need
     * zero corpus-awareness of their own to land in the right schema. */
    TEST_ASSERT(pg_store_use_corpus(store, corpus_a) == 0, "expected use_corpus(A) to succeed");
    int64_t id_in_a = pg_store_insert_passage(store, "doc-a", 0, "text in A", 3);
    TEST_ASSERT(id_in_a == 1, "expected the first passage in a fresh corpus to get id 1, got %lld",
                (long long)id_in_a);

    TEST_ASSERT(pg_store_use_corpus(store, corpus_b) == 0, "expected use_corpus(B) to succeed");
    int64_t id_in_b = pg_store_insert_passage(store, "doc-b", 0, "text in B", 3);
    TEST_ASSERT(id_in_b == 1,
                "expected corpus B to have its own independent id sequence (also starting at 1), got %lld",
                (long long)id_in_b);

    /* id 1 exists in both schemas now, with different content -- proves
     * they're genuinely separate tables, not a shared one filtered by
     * search_path (search_path picks which table "passages" even means,
     * it isn't a WHERE-clause-style filter). */
    PgStorePassage *passage_in_b = pg_store_get_passage(store, 1);
    TEST_ASSERT(passage_in_b != NULL, "expected id 1 to exist while scoped to corpus B");
    TEST_ASSERT_STR_EQ(passage_in_b->text, "text in B");
    pg_store_passage_free(passage_in_b);

    TEST_ASSERT(pg_store_use_corpus(store, corpus_a) == 0, "expected switching back to corpus A to succeed");
    PgStorePassage *passage_in_a = pg_store_get_passage(store, 1);
    TEST_ASSERT(passage_in_a != NULL, "expected id 1 to exist while scoped to corpus A");
    TEST_ASSERT_STR_EQ(passage_in_a->text, "text in A");
    pg_store_passage_free(passage_in_a);

    free(schema_a);
    free(schema_b);
    pg_store_close(store);
}

static void test_use_corpus_fails_for_nonexistent_id(void) {
    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");
    reset_corpora_registry(store);

    TEST_ASSERT(pg_store_use_corpus(store, 999999) == -1, "expected use_corpus on a nonexistent id to fail");

    pg_store_close(store);
}

static void test_open_creates_store(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is native Postgres running (make pg-start)?");
    pg_store_close(store);
}

static void test_insert_passage_returns_ids(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t id = pg_store_insert_passage(store, "doc1.txt", 0, "hello world", 2);
    TEST_ASSERT(id == 1, "expected first passage id 1, got %lld", (long long)id);

    int64_t second_id = pg_store_insert_passage(store, "doc1.txt", 1, "second chunk", 2);
    TEST_ASSERT(second_id == 2, "expected second passage id 2, got %lld", (long long)second_id);

    pg_store_close(store);
}

static void test_get_or_create_term_dedups(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t first = pg_store_get_or_create_term(store, "hypertension");
    int64_t second = pg_store_get_or_create_term(store, "hypertension");
    TEST_ASSERT(first > 0, "expected a positive term id, got %lld", (long long)first);
    TEST_ASSERT(first == second, "expected same id on repeat lookup, got %lld then %lld",
                (long long)first, (long long)second);

    int64_t other = pg_store_get_or_create_term(store, "treatment");
    TEST_ASSERT(other != first, "expected a distinct id for a different term");

    pg_store_close(store);
}

static void test_full_round_trip(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t passage_id = pg_store_insert_passage(store, "doc1.txt", 0, "hypertension treatment", 2);
    int64_t term_id = pg_store_get_or_create_term(store, "hypertension");
    int result = pg_store_insert_posting(store, term_id, passage_id, 1, 2);
    TEST_ASSERT(result == 0, "expected pg_store_insert_posting to succeed");

    char term_id_str[32], passage_id_str[32];
    snprintf(term_id_str, sizeof(term_id_str), "%lld", (long long)term_id);
    snprintf(passage_id_str, sizeof(passage_id_str), "%lld", (long long)passage_id);
    const char *params[2] = {term_id_str, passage_id_str};
    PGresult *res = PQexecParams(
        store->conn, "SELECT term_frequency FROM postings WHERE term_id = $1 AND passage_id = $2;", 2,
        NULL, params, NULL, NULL, 0);
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(PQntuples(res) == 1, "expected a posting row to exist");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 1, "expected term_frequency 1, got %s",
                PQgetvalue(res, 0, 0));
    PQclear(res);

    pg_store_close(store);
}

static void test_lookup_term_finds_existing_term(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t created_id = pg_store_get_or_create_term(store, "hypertension");
    int64_t looked_up_id = pg_store_lookup_term(store, "hypertension");
    TEST_ASSERT(looked_up_id == created_id, "expected lookup to return the same id %lld, got %lld",
                (long long)created_id, (long long)looked_up_id);

    pg_store_close(store);
}

static void test_lookup_term_returns_negative_one_when_unseen(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t id = pg_store_lookup_term(store, "nonexistent");
    TEST_ASSERT(id == -1, "expected -1 for a term never inserted, got %lld", (long long)id);

    pg_store_close(store);
}

static void test_lookup_term_never_inserts(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    pg_store_lookup_term(store, "ghost");

    const char *params[1] = {"ghost"};
    PGresult *res =
        PQexecParams(store->conn, "SELECT COUNT(*) FROM terms WHERE term = $1;", 1, NULL, params, NULL,
                     NULL, 0);
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 0,
                "expected lookup_term not to insert a row, found %s", PQgetvalue(res, 0, 0));
    PQclear(res);

    pg_store_close(store);
}

static void test_get_passage_returns_stored_data(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t passage_id =
        pg_store_insert_passage(store, "doc1.txt", 3, "hypertension treatment options", 4);

    PgStorePassage *passage = pg_store_get_passage(store, passage_id);
    TEST_ASSERT(passage != NULL, "expected pg_store_get_passage to succeed");
    TEST_ASSERT_STR_EQ(passage->document_name, "doc1.txt");
    TEST_ASSERT(passage->chunk_id == 3, "expected chunk_id 3, got %d", passage->chunk_id);
    TEST_ASSERT_STR_EQ(passage->text, "hypertension treatment options");
    TEST_ASSERT(passage->token_count == 4, "expected token_count 4, got %d", passage->token_count);

    pg_store_passage_free(passage);
    pg_store_close(store);
}

static void test_get_passage_returns_null_for_nonexistent_id(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    PgStorePassage *passage = pg_store_get_passage(store, 999999);
    TEST_ASSERT(passage == NULL, "expected NULL for a passage id that doesn't exist");

    pg_store_close(store);
}

static void test_passage_free_null_is_safe(void) {
    pg_store_passage_free(NULL);
}

static void test_commit_transaction_persists_writes(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    TEST_ASSERT(pg_store_begin_transaction(store) == 0, "expected begin to succeed");
    int64_t passage_id = pg_store_insert_passage(store, "doc1.txt", 0, "hello world", 2);
    TEST_ASSERT(passage_id != -1, "expected insert inside a transaction to succeed");
    TEST_ASSERT(pg_store_commit_transaction(store) == 0, "expected commit to succeed");

    PgStorePassage *passage = pg_store_get_passage(store, passage_id);
    TEST_ASSERT(passage != NULL, "expected the committed passage to be readable");
    pg_store_passage_free(passage);

    pg_store_close(store);
}

static void test_rollback_transaction_discards_writes(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    TEST_ASSERT(pg_store_begin_transaction(store) == 0, "expected begin to succeed");
    int64_t passage_id = pg_store_insert_passage(store, "doc1.txt", 0, "hello world", 2);
    TEST_ASSERT(passage_id != -1, "expected insert inside a transaction to succeed");
    TEST_ASSERT(pg_store_rollback_transaction(store) == 0, "expected rollback to succeed");

    PgStorePassage *passage = pg_store_get_passage(store, passage_id);
    TEST_ASSERT(passage == NULL, "expected a rolled-back passage to no longer exist");

    pg_store_close(store);
}

static void test_disable_synchronous_commit_succeeds(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    TEST_ASSERT(pg_store_disable_synchronous_commit(store) == 0,
                "expected disabling synchronous_commit to succeed");

    PGresult *res = PQexec(store->conn, "SHOW synchronous_commit;");
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected SHOW to succeed");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 0), "off");
    PQclear(res);

    pg_store_close(store);
}

static void test_get_or_create_term_survives_concurrent_style_conflict(void) {
    /* Simulates what two concurrent writer connections racing on the same
     * new term would produce: an ON CONFLICT upsert from one connection
     * while the term already exists must still return the *original* id,
     * not create a duplicate row or error out -- the exact race the
     * SQLite version's SELECT-then-INSERT couldn't close safely. */
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t first = pg_store_get_or_create_term(store, "hypertension");

    /* Directly exercise the same INSERT ... ON CONFLICT statement
     * get_or_create_term uses internally, as if a second connection raced
     * in after the term already existed. */
    const char *params[1] = {"hypertension"};
    PGresult *res = PQexecParams(store->conn,
                                  "INSERT INTO terms (term) VALUES ($1) "
                                  "ON CONFLICT (term) DO UPDATE SET term = EXCLUDED.term "
                                  "RETURNING id;",
                                  1, NULL, params, NULL, NULL, 0);
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected the conflicting upsert to succeed");
    int64_t conflict_id = atoll(PQgetvalue(res, 0, 0));
    PQclear(res);

    TEST_ASSERT(conflict_id == first, "expected the conflict to resolve to the original id %lld, got %lld",
                (long long)first, (long long)conflict_id);

    const char *count_params[1] = {"hypertension"};
    res = PQexecParams(store->conn, "SELECT COUNT(*) FROM terms WHERE term = $1;", 1, NULL, count_params,
                        NULL, NULL, 0);
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 1, "expected exactly 1 row, not a duplicate, got %s",
                PQgetvalue(res, 0, 0));
    PQclear(res);

    pg_store_close(store);
}

static void test_get_or_create_terms_batch_mixes_new_and_existing(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t pre_existing = pg_store_get_or_create_term(store, "hypertension");
    TEST_ASSERT(pre_existing != -1, "expected setup term to insert");

    const char *terms[3] = {"hypertension", "treatment", "diagnosis"};
    int64_t *ids = pg_store_get_or_create_terms(store, terms, 3);
    TEST_ASSERT(ids != NULL, "expected batch resolve to succeed");
    TEST_ASSERT(ids[0] == pre_existing, "expected the already-existing term to resolve to its real id");
    TEST_ASSERT(ids[1] != -1 && ids[2] != -1, "expected the two new terms to get real ids");
    TEST_ASSERT(ids[1] != ids[2], "expected the two distinct new terms to get distinct ids");
    free(ids);

    PGresult *res = PQexec(store->conn, "SELECT COUNT(*) FROM terms;");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 3, "expected exactly 3 term rows total, got %s",
                PQgetvalue(res, 0, 0));
    PQclear(res);

    pg_store_close(store);
}

static void test_get_or_create_terms_batch_handles_duplicates_in_input(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    /* "treatment" appears twice in the same batch -- must resolve to the
     * same id both times, and must not create two rows. */
    const char *terms[3] = {"treatment", "hypertension", "treatment"};
    int64_t *ids = pg_store_get_or_create_terms(store, terms, 3);
    TEST_ASSERT(ids != NULL, "expected batch resolve to succeed");
    TEST_ASSERT(ids[0] == ids[2], "expected both occurrences of \"treatment\" to resolve to the same id");
    free(ids);

    PGresult *res = PQexec(store->conn, "SELECT COUNT(*) FROM terms WHERE term = 'treatment';");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 1, "expected exactly 1 row for \"treatment\", got %s",
                PQgetvalue(res, 0, 0));
    PQclear(res);

    pg_store_close(store);
}

static void test_get_or_create_terms_batch_handles_special_characters(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    /* Exercises the array-literal escaping path directly -- a comma is
     * the array-element separator, so this term ("1,000", which the
     * tokenizer's internal-connector-punctuation rule can legitimately
     * produce) must round-trip intact, not get split into two elements. */
    const char *terms[2] = {"1,000", "normal"};
    int64_t *ids = pg_store_get_or_create_terms(store, terms, 2);
    TEST_ASSERT(ids != NULL, "expected batch resolve to succeed");
    TEST_ASSERT(ids[0] != -1 && ids[1] != -1, "expected both terms to resolve");
    free(ids);

    PGresult *res = PQexec(store->conn, "SELECT term FROM terms ORDER BY id;");
    TEST_ASSERT(PQntuples(res) == 2, "expected exactly 2 term rows, got %d", PQntuples(res));
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 0), "1,000");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 1, 0), "normal");
    PQclear(res);

    pg_store_close(store);
}

static void test_insert_postings_batch_zips_not_cross_products(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t passage_id = pg_store_insert_passage(store, "doc1.txt", 0, "some text", 3);
    const char *terms[3] = {"alpha", "beta", "gamma"};
    int64_t *term_ids = pg_store_get_or_create_terms(store, terms, 3);
    TEST_ASSERT(term_ids != NULL, "expected batch resolve to succeed");

    int frequencies[3] = {5, 7, 9};
    int result = pg_store_insert_postings(store, term_ids, passage_id, frequencies, 3, 3);
    TEST_ASSERT(result == 0, "expected batch posting insert to succeed");

    /* If unnest() were a cross product instead of zipping element-wise,
     * this would be 9 rows (3x3), not 3. */
    PGresult *res = PQexec(store->conn, "SELECT COUNT(*) FROM postings;");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 3, "expected exactly 3 posting rows, got %s",
                PQgetvalue(res, 0, 0));
    PQclear(res);

    /* And each term_id must be paired with its OWN frequency, not a
     * mismatched one. */
    for (int i = 0; i < 3; i++) {
        char term_id_str[32];
        snprintf(term_id_str, sizeof(term_id_str), "%lld", (long long)term_ids[i]);
        const char *params[1] = {term_id_str};
        res = PQexecParams(store->conn, "SELECT term_frequency FROM postings WHERE term_id = $1;", 1, NULL,
                            params, NULL, NULL, 0);
        TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == frequencies[i],
                    "expected term_ids[%d] paired with frequency %d, got %s", i, frequencies[i],
                    PQgetvalue(res, 0, 0));
        PQclear(res);
    }

    free(term_ids);
    pg_store_close(store);
}

static void test_get_document_names_batch_maps_in_order(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t id_a = pg_store_insert_passage(store, "pid-100", 0, "alpha text", 2);
    int64_t id_b = pg_store_insert_passage(store, "pid-200", 0, "beta text", 2);
    int64_t id_c = pg_store_insert_passage(store, "pid-300", 0, "gamma text", 2);

    /* Deliberately out of insertion order, and repeats id_a -- the
     * result must still line up index-for-index with the input, not
     * insertion or database order. */
    int64_t passage_ids[4] = {id_c, id_a, id_b, id_a};
    char **names = pg_store_get_document_names(store, passage_ids, 4);
    TEST_ASSERT(names != NULL, "expected batch lookup to succeed");
    TEST_ASSERT_STR_EQ(names[0], "pid-300");
    TEST_ASSERT_STR_EQ(names[1], "pid-100");
    TEST_ASSERT_STR_EQ(names[2], "pid-200");
    TEST_ASSERT_STR_EQ(names[3], "pid-100");

    for (int i = 0; i < 4; i++) {
        free(names[i]);
    }
    free(names);
    pg_store_close(store);
}

static void test_get_document_names_batch_null_for_missing_id(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t id_a = pg_store_insert_passage(store, "pid-100", 0, "alpha text", 2);

    int64_t passage_ids[2] = {id_a, 999999};
    char **names = pg_store_get_document_names(store, passage_ids, 2);
    TEST_ASSERT(names != NULL, "expected batch lookup to succeed");
    TEST_ASSERT_STR_EQ(names[0], "pid-100");
    TEST_ASSERT(names[1] == NULL, "expected NULL for a passage id that doesn't exist");

    free(names[0]);
    free(names);
    pg_store_close(store);
}

#define TEST_STAGING_CSV_PATH "build/test_copy_documents_raw.csv"

static void write_staging_csv(const char *contents) {
    FILE *fp = fopen(TEST_STAGING_CSV_PATH, "wb");
    fwrite(contents, 1, strlen(contents), fp);
    fclose(fp);
}

static void test_staging_tables_create_truncate_drop_round_trip(void) {
    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is native Postgres running (make pg-start)?");

    TEST_ASSERT(pg_store_create_staging_tables(store) == 0, "expected staging table creation to succeed");
    /* Idempotent: creating twice must not error (IF NOT EXISTS). */
    TEST_ASSERT(pg_store_create_staging_tables(store) == 0,
                "expected re-creating staging tables to be a no-op");

    TEST_ASSERT(pg_store_truncate_staging_tables(store) == 0,
                "expected truncating staging tables to succeed");

    TEST_ASSERT(pg_store_drop_staging_tables(store) == 0, "expected dropping staging tables to succeed");
    /* Idempotent: dropping twice must not error (IF EXISTS). */
    TEST_ASSERT(pg_store_drop_staging_tables(store) == 0, "expected re-dropping staging tables to be a no-op");

    pg_store_close(store);
}

static void test_copy_documents_raw_loads_every_row(void) {
    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is native Postgres running (make pg-start)?");
    TEST_ASSERT(pg_store_create_staging_tables(store) == 0, "expected staging table creation to succeed");
    TEST_ASSERT(pg_store_truncate_staging_tables(store) == 0, "expected truncate to succeed");

    /* Real MS MARCO passages contain literal, unescaped backslashes
     * (e.g. LaTeX-style "\displaystyle", "\%") and embedded double
     * quotes/commas -- exactly what motivated CSV format over plain TSV
     * in the first place (see SPEED.md). This fixture mirrors those
     * cases directly: row 2 has a raw backslash, row 3 is CSV-quoted
     * (embedded comma + doubled internal quote per RFC4180). */
    write_staging_csv("100\tplain text with no special characters\n"
                       "101\ttext with a literal \\backslash and \\% escape-looking sequence\n"
                       "102\t\"quoted, with a comma and a \"\"doubled\"\" quote\"\n");

    int64_t rows_loaded = pg_store_copy_documents_raw(store, TEST_STAGING_CSV_PATH);
    TEST_ASSERT(rows_loaded == 3, "expected all 3 rows to load");

    PGresult *res = PQexec(store->conn, "SELECT pid, text FROM documents_raw ORDER BY row_num;");
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected select to succeed");
    TEST_ASSERT(PQntuples(res) == 3, "expected 3 rows in documents_raw");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 0), "100");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 1), "plain text with no special characters");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 1, 0), "101");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 1, 1),
                        "text with a literal \\backslash and \\% escape-looking sequence");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 2, 0), "102");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 2, 1), "quoted, with a comma and a \"doubled\" quote");
    PQclear(res);

    pg_store_drop_staging_tables(store);
    pg_store_close(store);
}

static void test_get_raw_documents_range_returns_requested_rows(void) {
    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is native Postgres running (make pg-start)?");
    TEST_ASSERT(pg_store_create_staging_tables(store) == 0, "expected staging table creation to succeed");
    TEST_ASSERT(pg_store_truncate_staging_tables(store) == 0, "expected truncate to succeed");

    write_staging_csv("100\tfirst\n101\tsecond\n102\tthird\n103\tfourth\n104\tfifth\n");
    int64_t rows_loaded = pg_store_copy_documents_raw(store, TEST_STAGING_CSV_PATH);
    TEST_ASSERT(rows_loaded == 5, "expected all 5 rows to load");

    /* [2, 4) -- rows 2 and 3, exclusive of 4 -- exercises the exact
     * half-open range convention Phase 2's worker partitioning relies
     * on. */
    size_t count = 0;
    PgStoreRawDocument *docs = pg_store_get_raw_documents_range(store, 2, 4, &count);
    TEST_ASSERT(docs != NULL, "expected range fetch to succeed");
    TEST_ASSERT(count == 2, "expected exactly 2 rows in [2, 4)");
    TEST_ASSERT(docs[0].row_num == 2, "expected first row_num to be 2");
    TEST_ASSERT_STR_EQ(docs[0].pid, "101");
    TEST_ASSERT_STR_EQ(docs[0].text, "second");
    TEST_ASSERT(docs[1].row_num == 3, "expected second row_num to be 3");
    TEST_ASSERT_STR_EQ(docs[1].pid, "102");
    TEST_ASSERT_STR_EQ(docs[1].text, "third");
    pg_store_raw_documents_free(docs, count);

    /* A range that runs past the end of the table should just come back
     * short, not error -- this is exactly what happens to whichever
     * worker claims the last batch. */
    docs = pg_store_get_raw_documents_range(store, 4, 100, &count);
    TEST_ASSERT(docs != NULL, "expected a past-the-end range fetch to still succeed");
    TEST_ASSERT(count == 2, "expected only 2 rows (row_num 4 and 5) in a range that runs past the table's end");
    pg_store_raw_documents_free(docs, count);

    pg_store_drop_staging_tables(store);
    pg_store_close(store);
}

static void test_insert_staged_postings_batch_writes_raw_term_text(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is native Postgres running (make pg-start)?");
    TEST_ASSERT(pg_store_create_staging_tables(store) == 0, "expected staging table creation to succeed");
    TEST_ASSERT(pg_store_truncate_staging_tables(store) == 0, "expected truncate to succeed");

    int64_t passage_id = pg_store_insert_passage(store, "pid-staged", 0, "some text", 5);
    TEST_ASSERT(passage_id != -1, "expected passage insert to succeed");

    const char *terms[3] = {"alpha", "beta", "alpha"};
    int frequencies[3] = {2, 1, 2};
    TEST_ASSERT(pg_store_insert_staged_postings(store, passage_id, terms, frequencies, 5, 3) == 0,
                "expected staged postings insert to succeed");

    PGresult *res = PQexec(store->conn,
                            "SELECT passage_id, term, term_frequency, token_count FROM postings_staged "
                            "ORDER BY term;");
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected select to succeed");
    TEST_ASSERT(PQntuples(res) == 3, "expected 3 raw staged rows (no dedup at this stage)");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 1), "alpha");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 2), "2");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 3), "5");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 2, 1), "beta");
    PQclear(res);

    pg_store_drop_staging_tables(store);
    pg_store_close(store);
}

static void test_finalize_terms_and_postings_resolves_and_dedups(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is native Postgres running (make pg-start)?");
    TEST_ASSERT(pg_store_create_staging_tables(store) == 0, "expected staging table creation to succeed");
    TEST_ASSERT(pg_store_truncate_staging_tables(store) == 0, "expected truncate to succeed");

    /* A term ("existing") already resolved through the normal path
     * before Phase 3 ever runs -- finalize must fold staged postings
     * into its *existing* id via ON CONFLICT DO NOTHING, not create a
     * second "existing" row. */
    int64_t existing_term_id = pg_store_get_or_create_term(store, "existing");
    TEST_ASSERT(existing_term_id != -1, "expected pre-existing term to be created");

    int64_t passage_a = pg_store_insert_passage(store, "pid-a", 0, "text a", 4);
    int64_t passage_b = pg_store_insert_passage(store, "pid-b", 0, "text b", 3);

    /* "shared" appears in both passages -- must resolve to the SAME
     * terms.id for both postings rows, not one each. */
    const char *terms_a[2] = {"existing", "shared"};
    int freqs_a[2] = {1, 2};
    TEST_ASSERT(pg_store_insert_staged_postings(store, passage_a, terms_a, freqs_a, 4, 2) == 0,
                "expected staged postings insert for passage a to succeed");

    const char *terms_b[2] = {"shared", "unique"};
    int freqs_b[2] = {1, 1};
    TEST_ASSERT(pg_store_insert_staged_postings(store, passage_b, terms_b, freqs_b, 3, 2) == 0,
                "expected staged postings insert for passage b to succeed");

    long postings_written = pg_store_finalize_terms_and_postings(store);
    TEST_ASSERT(postings_written == 4, "expected 4 total postings rows written");

    PGresult *res = PQexec(store->conn, "SELECT count(*) FROM terms WHERE term = 'existing';");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 0), "1");
    PQclear(res);

    int64_t shared_term_id = pg_store_lookup_term(store, "shared");
    TEST_ASSERT(shared_term_id != -1, "expected 'shared' to have been resolved into terms");

    res = PQexec(store->conn, "SELECT count(*) FROM postings WHERE term_id = "
                              "(SELECT id FROM terms WHERE term = 'shared');");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 0), "2");
    PQclear(res);

    char query[256];
    snprintf(query, sizeof(query),
             "SELECT term_frequency, token_count FROM postings WHERE passage_id = %lld "
             "AND term_id = %lld;",
             (long long)passage_a, (long long)existing_term_id);
    res = PQexec(store->conn, query);
    TEST_ASSERT(PQntuples(res) == 1, "expected exactly one posting for (passage_a, existing)");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 0), "1");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 1), "4");
    PQclear(res);

    pg_store_drop_staging_tables(store);
    pg_store_close(store);
}

static int table_persistence_is_unlogged(PgStore *store, const char *table_name) {
    const char *params[1] = {table_name};
    PGresult *res = PQexecParams(store->conn, "SELECT relpersistence FROM pg_class WHERE relname = $1;", 1,
                                  NULL, params, NULL, NULL, 0);
    int is_unlogged = (PQntuples(res) == 1 && strcmp(PQgetvalue(res, 0, 0), "u") == 0);
    PQclear(res);
    return is_unlogged;
}

static int postings_has_pk_and_fks(PgStore *store) {
    PGresult *res = PQexec(store->conn,
                            "SELECT count(*) FROM pg_constraint WHERE conrelid = 'postings'::regclass "
                            "AND contype IN ('p', 'f');");
    int count = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);
    return count == 3; /* 1 primary key + 2 foreign keys */
}

static void test_prepare_bulk_load_defers_constraints_and_durability(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is native Postgres running (make pg-start)?");

    TEST_ASSERT(postings_has_pk_and_fks(store), "expected a fresh schema to have postings' PK + 2 FKs");
    TEST_ASSERT(!table_persistence_is_unlogged(store, "postings"), "expected postings to start LOGGED");
    TEST_ASSERT(!table_persistence_is_unlogged(store, "terms"), "expected terms to start LOGGED");

    TEST_ASSERT(pg_store_prepare_bulk_load(store) == 0, "expected prepare_bulk_load to succeed");

    TEST_ASSERT(!postings_has_pk_and_fks(store), "expected PK + FKs to be dropped after prepare");
    TEST_ASSERT(table_persistence_is_unlogged(store, "postings"), "expected postings to be UNLOGGED");
    TEST_ASSERT(table_persistence_is_unlogged(store, "terms"), "expected terms to be UNLOGGED");

    /* Idempotent: a prior crashed run may have already left things in
     * exactly this state -- calling prepare again must not error. */
    TEST_ASSERT(pg_store_prepare_bulk_load(store) == 0, "expected a second prepare_bulk_load to be a no-op");

    pg_store_close(store);
}

static void test_finish_bulk_load_restores_constraints_and_durability_and_data_survives(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is native Postgres running (make pg-start)?");

    /* Real data inserted BEFORE deferring constraints, exactly like a
     * real bulk-ingest run -- proves the constraint drop/restore cycle
     * doesn't lose or corrupt anything already written. */
    int64_t term_id = pg_store_get_or_create_term(store, "hypertension");
    int64_t passage_id = pg_store_insert_passage(store, "pid-1", 0, "some text", 3);
    TEST_ASSERT(pg_store_insert_posting(store, term_id, passage_id, 2, 3) == 0,
                "expected posting insert to succeed");

    TEST_ASSERT(pg_store_prepare_bulk_load(store) == 0, "expected prepare_bulk_load to succeed");
    TEST_ASSERT(pg_store_finish_bulk_load(store) == 0, "expected finish_bulk_load to succeed");

    TEST_ASSERT(postings_has_pk_and_fks(store), "expected PK + FKs restored after finish");
    TEST_ASSERT(!table_persistence_is_unlogged(store, "postings"), "expected postings restored to LOGGED");
    TEST_ASSERT(!table_persistence_is_unlogged(store, "terms"), "expected terms restored to LOGGED");

    PGresult *res = PQexec(store->conn, "SELECT term_frequency, token_count FROM postings;");
    TEST_ASSERT(PQntuples(res) == 1, "expected the posting inserted before prepare to survive");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 0), "2");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 1), "3");
    PQclear(res);

    pg_store_close(store);
}

int main(void) {
    test_create_corpus_creates_schema_and_registry_row();
    test_create_corpus_isolates_tables_between_corpora();
    test_create_corpus_rejects_empty_display_name();
    test_use_corpus_scopes_queries_to_chosen_corpus();
    test_use_corpus_fails_for_nonexistent_id();
    test_open_creates_store();
    test_insert_passage_returns_ids();
    test_get_or_create_term_dedups();
    test_full_round_trip();
    test_lookup_term_finds_existing_term();
    test_lookup_term_returns_negative_one_when_unseen();
    test_lookup_term_never_inserts();
    test_get_passage_returns_stored_data();
    test_get_passage_returns_null_for_nonexistent_id();
    test_passage_free_null_is_safe();
    test_commit_transaction_persists_writes();
    test_rollback_transaction_discards_writes();
    test_disable_synchronous_commit_succeeds();
    test_get_or_create_term_survives_concurrent_style_conflict();
    test_get_or_create_terms_batch_mixes_new_and_existing();
    test_get_or_create_terms_batch_handles_duplicates_in_input();
    test_get_or_create_terms_batch_handles_special_characters();
    test_insert_postings_batch_zips_not_cross_products();
    test_get_document_names_batch_maps_in_order();
    test_get_document_names_batch_null_for_missing_id();
    test_staging_tables_create_truncate_drop_round_trip();
    test_copy_documents_raw_loads_every_row();
    test_get_raw_documents_range_returns_requested_rows();
    test_insert_staged_postings_batch_writes_raw_term_text();
    test_finalize_terms_and_postings_resolves_and_dedups();
    test_prepare_bulk_load_defers_constraints_and_durability();
    test_finish_bulk_load_restores_constraints_and_durability_and_data_survives();
    return test_summary();
}
