/*
 * Tests for src/core/query_log.c — pipeline observability logging. Same
 * throwaway-DB pattern as test_sqlite_store.c: verify writes by reading
 * the rows back directly, not just trusting the insert calls succeeded.
 */

#include "query_log.h"
#include "sqlite_store.h"
#include "test_utils.h"

#include <stdio.h>

#define TEST_DB_PATH "build/test_query_log.db"

static SqliteStore *open_fresh_store(void) {
    remove(TEST_DB_PATH);
    SqliteStore *store = sqlite_store_open(TEST_DB_PATH);
    if (store != NULL) {
        query_log_init_schema(store);
    }
    return store;
}

static void test_init_schema_is_idempotent(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    /* IF NOT EXISTS -- calling it again on an already-initialized
     * connection must not fail or disturb existing tables. */
    TEST_ASSERT(query_log_init_schema(store) == 0, "expected a repeat init to succeed");

    sqlite_store_close(store);
}

static void test_insert_query_returns_increasing_ids(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 first = query_log_insert_query(store, "What is hypertension?");
    sqlite3_int64 second = query_log_insert_query(store, "What causes it?");
    TEST_ASSERT(first == 1, "expected first query id 1, got %lld", (long long)first);
    TEST_ASSERT(second == 2, "expected second query id 2, got %lld", (long long)second);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT question_text, total_latency_ms, succeeded FROM queries WHERE id = ?;";
    TEST_ASSERT(sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK,
                "expected verification query to prepare");
    sqlite3_bind_int64(stmt, 1, first);
    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected a queries row to exist");
    TEST_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "What is hypertension?");
    TEST_ASSERT(sqlite3_column_type(stmt, 1) == SQLITE_NULL,
                "expected total_latency_ms to start NULL, before query_log_finish_query runs");
    TEST_ASSERT(sqlite3_column_type(stmt, 2) == SQLITE_NULL,
                "expected succeeded to start NULL, before query_log_finish_query runs");
    sqlite3_finalize(stmt);

    sqlite_store_close(store);
}

static void test_finish_query_updates_latency_and_status(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 query_id = query_log_insert_query(store, "What is hypertension?");
    TEST_ASSERT(query_log_finish_query(store, query_id, 1234, 1) == 0,
                "expected query_log_finish_query to succeed");

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT total_latency_ms, succeeded FROM queries WHERE id = ?;";
    TEST_ASSERT(sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK,
                "expected verification query to prepare");
    sqlite3_bind_int64(stmt, 1, query_id);
    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected a queries row to exist");
    TEST_ASSERT(sqlite3_column_int(stmt, 0) == 1234, "expected total_latency_ms 1234, got %d",
                sqlite3_column_int(stmt, 0));
    TEST_ASSERT(sqlite3_column_int(stmt, 1) == 1, "expected succeeded 1, got %d",
                sqlite3_column_int(stmt, 1));
    sqlite3_finalize(stmt);

    sqlite_store_close(store);
}

static void test_insert_query_formulation_run_stores_all_fields(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 query_id = query_log_insert_query(store, "What is hypertension?");
    int result = query_log_insert_query_formulation_run(
        store, query_id, 2, "prompt text", "[\"hypertension\", \"treatment\"]", 0,
        "hypertension treatment", 42);
    TEST_ASSERT(result == 0, "expected query_log_insert_query_formulation_run to succeed");

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT surviving_term_count, prompt_text, llm_response_text, "
                       "used_fallback, selected_terms, latency_ms "
                       "FROM query_formulation_runs WHERE query_id = ?;";
    TEST_ASSERT(sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK,
                "expected verification query to prepare");
    sqlite3_bind_int64(stmt, 1, query_id);
    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected a query_formulation_runs row to exist");
    TEST_ASSERT(sqlite3_column_int(stmt, 0) == 2, "expected surviving_term_count 2, got %d",
                sqlite3_column_int(stmt, 0));
    TEST_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "prompt text");
    TEST_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), "[\"hypertension\", \"treatment\"]");
    TEST_ASSERT(sqlite3_column_int(stmt, 3) == 0, "expected used_fallback 0, got %d",
                sqlite3_column_int(stmt, 3));
    TEST_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 4), "hypertension treatment");
    TEST_ASSERT(sqlite3_column_int(stmt, 5) == 42, "expected latency_ms 42, got %d",
                sqlite3_column_int(stmt, 5));
    sqlite3_finalize(stmt);

    sqlite_store_close(store);
}

static void test_insert_query_formulation_run_allows_null_prompt_and_response(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    /* An all-stopwords query never builds a prompt or calls the LLM --
     * prompt_text/llm_response_text must be storable as NULL, not crash. */
    sqlite3_int64 query_id = query_log_insert_query(store, "what is the for");
    int result = query_log_insert_query_formulation_run(store, query_id, 0, NULL, NULL, 0, "", 5);
    TEST_ASSERT(result == 0, "expected NULL prompt/response to be accepted");

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT prompt_text, llm_response_text FROM query_formulation_runs WHERE query_id = ?;";
    TEST_ASSERT(sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK,
                "expected verification query to prepare");
    sqlite3_bind_int64(stmt, 1, query_id);
    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected a query_formulation_runs row to exist");
    TEST_ASSERT(sqlite3_column_type(stmt, 0) == SQLITE_NULL, "expected prompt_text to be NULL");
    TEST_ASSERT(sqlite3_column_type(stmt, 1) == SQLITE_NULL, "expected llm_response_text to be NULL");
    sqlite3_finalize(stmt);

    sqlite_store_close(store);
}

static void test_search_run_and_results_round_trip(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 query_id = query_log_insert_query(store, "What is hypertension?");
    sqlite3_int64 search_run_id = query_log_insert_search_run(store, query_id, 5, 2, 17);
    TEST_ASSERT(search_run_id != -1, "expected query_log_insert_search_run to succeed");

    TEST_ASSERT(query_log_insert_search_result(store, search_run_id, 1, 100, 2.901) == 0,
                "expected first search result insert to succeed");
    TEST_ASSERT(query_log_insert_search_result(store, search_run_id, 2, 101, 0.859) == 0,
                "expected second search result insert to succeed");

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT passage_id, score FROM search_results "
                       "WHERE search_run_id = ? ORDER BY rank ASC;";
    TEST_ASSERT(sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK,
                "expected verification query to prepare");
    sqlite3_bind_int64(stmt, 1, search_run_id);

    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected first search_results row");
    TEST_ASSERT(sqlite3_column_int64(stmt, 0) == 100, "expected passage_id 100, got %lld",
                (long long)sqlite3_column_int64(stmt, 0));
    TEST_ASSERT(sqlite3_column_double(stmt, 1) == 2.901, "expected score 2.901, got %f",
                sqlite3_column_double(stmt, 1));

    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected second search_results row");
    TEST_ASSERT(sqlite3_column_int64(stmt, 0) == 101, "expected passage_id 101, got %lld",
                (long long)sqlite3_column_int64(stmt, 0));

    sqlite3_finalize(stmt);
    sqlite_store_close(store);
}

static void test_insert_generation_run_stores_all_fields(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 query_id = query_log_insert_query(store, "What is hypertension?");
    int result = query_log_insert_generation_run(store, query_id, "openai/gpt-4o-mini", 2, 0,
                                                  "generation prompt", "hypertension is...", 1, 731);
    TEST_ASSERT(result == 0, "expected query_log_insert_generation_run to succeed");

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT model, passages_included, passages_skipped, prompt_text, "
                       "answer_text, succeeded, latency_ms "
                       "FROM generation_runs WHERE query_id = ?;";
    TEST_ASSERT(sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK,
                "expected verification query to prepare");
    sqlite3_bind_int64(stmt, 1, query_id);
    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected a generation_runs row to exist");
    TEST_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "openai/gpt-4o-mini");
    TEST_ASSERT(sqlite3_column_int(stmt, 1) == 2, "expected passages_included 2, got %d",
                sqlite3_column_int(stmt, 1));
    TEST_ASSERT(sqlite3_column_int(stmt, 2) == 0, "expected passages_skipped 0, got %d",
                sqlite3_column_int(stmt, 2));
    TEST_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), "generation prompt");
    TEST_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 4), "hypertension is...");
    TEST_ASSERT(sqlite3_column_int(stmt, 5) == 1, "expected succeeded 1, got %d",
                sqlite3_column_int(stmt, 5));
    TEST_ASSERT(sqlite3_column_int(stmt, 6) == 731, "expected latency_ms 731, got %d",
                sqlite3_column_int(stmt, 6));
    sqlite3_finalize(stmt);

    sqlite_store_close(store);
}

static void test_insert_generation_run_allows_null_prompt_and_answer(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    /* No fallback in generation.c -- a failed API call means answer_text
     * (and possibly prompt_text, if it never even built) are NULL. */
    sqlite3_int64 query_id = query_log_insert_query(store, "What is hypertension?");
    int result = query_log_insert_generation_run(store, query_id, "openai/gpt-4o-mini", 0, 0, NULL,
                                                  NULL, 0, 12);
    TEST_ASSERT(result == 0, "expected NULL prompt/answer to be accepted");

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT prompt_text, answer_text FROM generation_runs WHERE query_id = ?;";
    TEST_ASSERT(sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK,
                "expected verification query to prepare");
    sqlite3_bind_int64(stmt, 1, query_id);
    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected a generation_runs row to exist");
    TEST_ASSERT(sqlite3_column_type(stmt, 0) == SQLITE_NULL, "expected prompt_text to be NULL");
    TEST_ASSERT(sqlite3_column_type(stmt, 1) == SQLITE_NULL, "expected answer_text to be NULL");
    sqlite3_finalize(stmt);

    sqlite_store_close(store);
}

int main(void) {
    test_init_schema_is_idempotent();
    test_insert_query_returns_increasing_ids();
    test_finish_query_updates_latency_and_status();
    test_insert_query_formulation_run_stores_all_fields();
    test_insert_query_formulation_run_allows_null_prompt_and_response();
    test_search_run_and_results_round_trip();
    test_insert_generation_run_stores_all_fields();
    test_insert_generation_run_allows_null_prompt_and_answer();
    remove(TEST_DB_PATH);
    return test_summary();
}
