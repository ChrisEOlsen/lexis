/*
 * Tests for src/core/sqlite_store.c — SqliteStore lifecycle and the
 * passage/term/posting write path. Uses a throwaway DB file under build/,
 * removed before each open so repeated runs don't accumulate stale rows.
 */

#include "sqlite_store.h"
#include "test_utils.h"

#include <stdio.h>

#define TEST_DB_PATH "build/test_sqlite_store.db"

static SqliteStore *open_fresh_store(void) {
    remove(TEST_DB_PATH);
    return sqlite_store_open(TEST_DB_PATH);
}

static void test_open_creates_store(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");
    sqlite_store_close(store);
}

static void test_insert_passage_returns_ids(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 id = sqlite_store_insert_passage(store, "doc1.txt", 0, "hello world", 2);
    TEST_ASSERT(id == 1, "expected first passage id 1, got %lld", (long long)id);

    sqlite3_int64 second_id = sqlite_store_insert_passage(store, "doc1.txt", 1, "second chunk", 2);
    TEST_ASSERT(second_id == 2, "expected second passage id 2, got %lld", (long long)second_id);

    sqlite_store_close(store);
}

static void test_get_or_create_term_dedups(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 first = sqlite_store_get_or_create_term(store, "hypertension");
    sqlite3_int64 second = sqlite_store_get_or_create_term(store, "hypertension");
    TEST_ASSERT(first > 0, "expected a positive term id, got %lld", (long long)first);
    TEST_ASSERT(first == second, "expected same id on repeat lookup, got %lld then %lld",
                (long long)first, (long long)second);

    sqlite3_int64 other = sqlite_store_get_or_create_term(store, "treatment");
    TEST_ASSERT(other != first, "expected a distinct id for a different term");

    sqlite_store_close(store);
}

static void test_full_round_trip(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 passage_id =
        sqlite_store_insert_passage(store, "doc1.txt", 0, "hypertension treatment", 2);
    sqlite3_int64 term_id = sqlite_store_get_or_create_term(store, "hypertension");
    int result = sqlite_store_insert_posting(store, term_id, passage_id, 1);
    TEST_ASSERT(result == 0, "expected sqlite_store_insert_posting to succeed");

    /* Verify directly against the database rather than trusting the write
     * calls alone — confirms the row actually landed with the right shape. */
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT term_frequency FROM postings WHERE term_id = ? AND passage_id = ?;";
    TEST_ASSERT(sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK,
                "expected verification query to prepare");
    sqlite3_bind_int64(stmt, 1, term_id);
    sqlite3_bind_int64(stmt, 2, passage_id);
    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected a posting row to exist");
    TEST_ASSERT(sqlite3_column_int(stmt, 0) == 1, "expected term_frequency 1, got %d",
                sqlite3_column_int(stmt, 0));
    sqlite3_finalize(stmt);

    sqlite_store_close(store);
}

static void test_lookup_term_finds_existing_term(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 created_id = sqlite_store_get_or_create_term(store, "hypertension");
    sqlite3_int64 looked_up_id = sqlite_store_lookup_term(store, "hypertension");
    TEST_ASSERT(looked_up_id == created_id,
                "expected lookup to return the same id %lld, got %lld",
                (long long)created_id, (long long)looked_up_id);

    sqlite_store_close(store);
}

static void test_lookup_term_returns_negative_one_when_unseen(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 id = sqlite_store_lookup_term(store, "nonexistent");
    TEST_ASSERT(id == -1, "expected -1 for a term never inserted, got %lld", (long long)id);

    sqlite_store_close(store);
}

static void test_lookup_term_never_inserts(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite_store_lookup_term(store, "ghost");

    /* A lookup must be read-only -- confirm "ghost" never landed in the
     * terms table as a side effect of merely searching for it. */
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT COUNT(*) FROM terms WHERE term = ?;";
    TEST_ASSERT(sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK,
                "expected verification query to prepare");
    sqlite3_bind_text(stmt, 1, "ghost", -1, SQLITE_TRANSIENT);
    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected a count row");
    TEST_ASSERT(sqlite3_column_int(stmt, 0) == 0,
                "expected lookup_term not to insert a row, found %d",
                sqlite3_column_int(stmt, 0));
    sqlite3_finalize(stmt);

    sqlite_store_close(store);
}

static void test_get_passage_returns_stored_data(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 passage_id =
        sqlite_store_insert_passage(store, "doc1.txt", 3, "hypertension treatment options", 4);

    SqliteStorePassage *passage = sqlite_store_get_passage(store, passage_id);
    TEST_ASSERT(passage != NULL, "expected sqlite_store_get_passage to succeed");
    TEST_ASSERT_STR_EQ(passage->document_name, "doc1.txt");
    TEST_ASSERT(passage->chunk_id == 3, "expected chunk_id 3, got %d", passage->chunk_id);
    TEST_ASSERT_STR_EQ(passage->text, "hypertension treatment options");
    TEST_ASSERT(passage->token_count == 4, "expected token_count 4, got %d", passage->token_count);

    sqlite_store_passage_free(passage);
    sqlite_store_close(store);
}

static void test_get_passage_returns_null_for_nonexistent_id(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    SqliteStorePassage *passage = sqlite_store_get_passage(store, 999999);
    TEST_ASSERT(passage == NULL, "expected NULL for a passage id that doesn't exist");

    sqlite_store_close(store);
}

static void test_passage_free_null_is_safe(void) {
    sqlite_store_passage_free(NULL);
}

static void test_commit_transaction_persists_writes(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    TEST_ASSERT(sqlite_store_begin_transaction(store) == 0, "expected begin to succeed");
    sqlite3_int64 passage_id = sqlite_store_insert_passage(store, "doc1.txt", 0, "hello world", 2);
    TEST_ASSERT(passage_id != -1, "expected insert inside a transaction to succeed");
    TEST_ASSERT(sqlite_store_commit_transaction(store) == 0, "expected commit to succeed");

    SqliteStorePassage *passage = sqlite_store_get_passage(store, passage_id);
    TEST_ASSERT(passage != NULL, "expected the committed passage to be readable");
    sqlite_store_passage_free(passage);

    sqlite_store_close(store);
}

static void test_rollback_transaction_discards_writes(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    TEST_ASSERT(sqlite_store_begin_transaction(store) == 0, "expected begin to succeed");
    sqlite3_int64 passage_id = sqlite_store_insert_passage(store, "doc1.txt", 0, "hello world", 2);
    TEST_ASSERT(passage_id != -1, "expected insert inside a transaction to succeed");
    TEST_ASSERT(sqlite_store_rollback_transaction(store) == 0, "expected rollback to succeed");

    /* The row must not exist after a rollback -- confirm directly rather
     * than trusting the rollback call alone. */
    SqliteStorePassage *passage = sqlite_store_get_passage(store, passage_id);
    TEST_ASSERT(passage == NULL, "expected a rolled-back passage to no longer exist");

    sqlite_store_close(store);
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
    remove(TEST_DB_PATH);
    return test_summary();
}
