/*
 * Tests for src/core/pg_store.c — PgStore lifecycle and the passage/term/
 * posting write path against a real Postgres instance (see
 * docker-compose.yml -- `docker compose up -d` must be running for these
 * to pass). TRUNCATE resets state between tests, since there's no file to
 * delete the way the SQLite version's tests worked.
 */

#include "pg_store.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>

#define TEST_CONNINFO "host=127.0.0.1 port=5433 dbname=lexis_test user=lexis password=lexis_dev_only"

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

static void test_open_creates_store(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is docker compose up?");
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
    int result = pg_store_insert_posting(store, term_id, passage_id, 1);
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

int main(void) {
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
    test_get_or_create_term_survives_concurrent_style_conflict();
    return test_summary();
}
