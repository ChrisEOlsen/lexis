/*
 * Tests for src/core/query_log.c — pipeline observability logging. Same
 * throwaway-DB pattern as test_pg_store.c: verify writes by reading the
 * rows back directly, not just trusting the insert calls succeeded.
 */

#include "query_log.h"
#include "pg_store.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>

#define TEST_CONNINFO "host=127.0.0.1 port=5434 dbname=lexis_test user=lexis password=lexis_dev_only"

static PgStore *open_fresh_store(void) {
    PgStore *store = pg_store_open(TEST_CONNINFO);
    if (store == NULL) {
        return NULL;
    }
    if (query_log_init_schema(store) != 0) {
        pg_store_close(store);
        return NULL;
    }
    /* Dependency order: search_results/query_formulation_runs/search_runs/
     * generation_runs all reference queries; search_results also
     * references passages (see test_search_run_and_results_round_trip). */
    PGresult *res = PQexec(store->conn,
                            "TRUNCATE search_results, query_formulation_runs, search_runs, "
                            "generation_runs, queries, postings, terms, passages "
                            "RESTART IDENTITY CASCADE;");
    PQclear(res);
    return store;
}

static void test_init_schema_is_idempotent(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed -- is native Postgres running (make pg-start)?");

    /* IF NOT EXISTS -- calling it again on an already-initialized
     * connection must not fail or disturb existing tables. */
    TEST_ASSERT(query_log_init_schema(store) == 0, "expected a repeat init to succeed");

    pg_store_close(store);
}

static void test_insert_query_returns_increasing_ids(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t first = query_log_insert_query(store, "What is hypertension?");
    int64_t second = query_log_insert_query(store, "What causes it?");
    TEST_ASSERT(first == 1, "expected first query id 1, got %lld", (long long)first);
    TEST_ASSERT(second == 2, "expected second query id 2, got %lld", (long long)second);

    char first_str[32];
    snprintf(first_str, sizeof(first_str), "%lld", (long long)first);
    const char *params[1] = {first_str};
    PGresult *res = PQexecParams(
        store->conn, "SELECT question_text, total_latency_ms, succeeded FROM queries WHERE id = $1;",
        1, NULL, params, NULL, NULL, 0);
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(PQntuples(res) == 1, "expected a queries row to exist");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 0), "What is hypertension?");
    TEST_ASSERT(PQgetisnull(res, 0, 1),
                "expected total_latency_ms to start NULL, before query_log_finish_query runs");
    TEST_ASSERT(PQgetisnull(res, 0, 2),
                "expected succeeded to start NULL, before query_log_finish_query runs");
    PQclear(res);

    pg_store_close(store);
}

static void test_finish_query_updates_latency_and_status(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t query_id = query_log_insert_query(store, "What is hypertension?");
    TEST_ASSERT(query_log_finish_query(store, query_id, 1234, 1) == 0,
                "expected query_log_finish_query to succeed");

    char query_id_str[32];
    snprintf(query_id_str, sizeof(query_id_str), "%lld", (long long)query_id);
    const char *params[1] = {query_id_str};
    PGresult *res = PQexecParams(
        store->conn, "SELECT total_latency_ms, succeeded FROM queries WHERE id = $1;", 1, NULL,
        params, NULL, NULL, 0);
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(PQntuples(res) == 1, "expected a queries row to exist");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 1234, "expected total_latency_ms 1234, got %s",
                PQgetvalue(res, 0, 0));
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 1)) == 1, "expected succeeded 1, got %s",
                PQgetvalue(res, 0, 1));
    PQclear(res);

    pg_store_close(store);
}

static void test_insert_query_formulation_run_stores_all_fields(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t query_id = query_log_insert_query(store, "What is hypertension?");
    int result = query_log_insert_query_formulation_run(
        store, query_id, 2, "prompt text", "[\"hypertension\", \"treatment\"]", 0,
        "hypertension treatment", 42);
    TEST_ASSERT(result == 0, "expected query_log_insert_query_formulation_run to succeed");

    char query_id_str[32];
    snprintf(query_id_str, sizeof(query_id_str), "%lld", (long long)query_id);
    const char *params[1] = {query_id_str};
    PGresult *res =
        PQexecParams(store->conn,
                      "SELECT surviving_term_count, prompt_text, llm_response_text, "
                      "used_fallback, selected_terms, latency_ms "
                      "FROM query_formulation_runs WHERE query_id = $1;",
                      1, NULL, params, NULL, NULL, 0);
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(PQntuples(res) == 1, "expected a query_formulation_runs row to exist");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 0)) == 2, "expected surviving_term_count 2, got %s",
                PQgetvalue(res, 0, 0));
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 1), "prompt text");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 2), "[\"hypertension\", \"treatment\"]");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 3)) == 0, "expected used_fallback 0, got %s",
                PQgetvalue(res, 0, 3));
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 4), "hypertension treatment");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 5)) == 42, "expected latency_ms 42, got %s",
                PQgetvalue(res, 0, 5));
    PQclear(res);

    pg_store_close(store);
}

static void test_insert_query_formulation_run_allows_null_prompt_and_response(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    /* An all-stopwords query never builds a prompt or calls the LLM --
     * prompt_text/llm_response_text must be storable as NULL, not crash. */
    int64_t query_id = query_log_insert_query(store, "what is the for");
    int result = query_log_insert_query_formulation_run(store, query_id, 0, NULL, NULL, 0, "", 5);
    TEST_ASSERT(result == 0, "expected NULL prompt/response to be accepted");

    char query_id_str[32];
    snprintf(query_id_str, sizeof(query_id_str), "%lld", (long long)query_id);
    const char *params[1] = {query_id_str};
    PGresult *res = PQexecParams(
        store->conn,
        "SELECT prompt_text, llm_response_text FROM query_formulation_runs WHERE query_id = $1;", 1,
        NULL, params, NULL, NULL, 0);
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(PQntuples(res) == 1, "expected a query_formulation_runs row to exist");
    TEST_ASSERT(PQgetisnull(res, 0, 0), "expected prompt_text to be NULL");
    TEST_ASSERT(PQgetisnull(res, 0, 1), "expected llm_response_text to be NULL");
    PQclear(res);

    pg_store_close(store);
}

static void test_search_run_and_results_round_trip(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    /* search_results.passage_id has a real foreign key to passages(id) --
     * unlike the SQLite version (foreign keys off by default, never
     * enabled in this codebase), Postgres always enforces it, so the
     * referenced passages must actually exist. */
    int64_t passage_a = pg_store_insert_passage(store, "doc1.txt", 0, "hypertension treatment", 2);
    int64_t passage_b = pg_store_insert_passage(store, "doc1.txt", 1, "hypertension diagnosis", 2);
    TEST_ASSERT(passage_a != -1 && passage_b != -1, "expected setup passages to insert");

    int64_t query_id = query_log_insert_query(store, "What is hypertension?");
    int64_t search_run_id = query_log_insert_search_run(store, query_id, 5, 2, 17);
    TEST_ASSERT(search_run_id != -1, "expected query_log_insert_search_run to succeed");

    TEST_ASSERT(query_log_insert_search_result(store, search_run_id, 1, passage_a, 2.901) == 0,
                "expected first search result insert to succeed");
    TEST_ASSERT(query_log_insert_search_result(store, search_run_id, 2, passage_b, 0.859) == 0,
                "expected second search result insert to succeed");

    char search_run_id_str[32];
    snprintf(search_run_id_str, sizeof(search_run_id_str), "%lld", (long long)search_run_id);
    const char *params[1] = {search_run_id_str};
    PGresult *res = PQexecParams(
        store->conn,
        "SELECT passage_id, score FROM search_results WHERE search_run_id = $1 ORDER BY rank ASC;",
        1, NULL, params, NULL, NULL, 0);
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(PQntuples(res) == 2, "expected 2 search_results rows, got %d", PQntuples(res));

    TEST_ASSERT(atoll(PQgetvalue(res, 0, 0)) == passage_a, "expected passage_id %lld, got %s",
                (long long)passage_a, PQgetvalue(res, 0, 0));
    TEST_ASSERT(atof(PQgetvalue(res, 0, 1)) == 2.901, "expected score 2.901, got %s",
                PQgetvalue(res, 0, 1));

    TEST_ASSERT(atoll(PQgetvalue(res, 1, 0)) == passage_b, "expected passage_id %lld, got %s",
                (long long)passage_b, PQgetvalue(res, 1, 0));

    PQclear(res);
    pg_store_close(store);
}

static void test_insert_generation_run_stores_all_fields(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    int64_t query_id = query_log_insert_query(store, "What is hypertension?");
    int result = query_log_insert_generation_run(store, query_id, "openai/gpt-4o-mini", 2, 0,
                                                  "generation prompt", "hypertension is...", 1, 731);
    TEST_ASSERT(result == 0, "expected query_log_insert_generation_run to succeed");

    char query_id_str[32];
    snprintf(query_id_str, sizeof(query_id_str), "%lld", (long long)query_id);
    const char *params[1] = {query_id_str};
    PGresult *res = PQexecParams(store->conn,
                                  "SELECT model, passages_included, passages_skipped, prompt_text, "
                                  "answer_text, succeeded, latency_ms "
                                  "FROM generation_runs WHERE query_id = $1;",
                                  1, NULL, params, NULL, NULL, 0);
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(PQntuples(res) == 1, "expected a generation_runs row to exist");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 0), "openai/gpt-4o-mini");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 1)) == 2, "expected passages_included 2, got %s",
                PQgetvalue(res, 0, 1));
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 2)) == 0, "expected passages_skipped 0, got %s",
                PQgetvalue(res, 0, 2));
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 3), "generation prompt");
    TEST_ASSERT_STR_EQ(PQgetvalue(res, 0, 4), "hypertension is...");
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 5)) == 1, "expected succeeded 1, got %s",
                PQgetvalue(res, 0, 5));
    TEST_ASSERT(atoi(PQgetvalue(res, 0, 6)) == 731, "expected latency_ms 731, got %s",
                PQgetvalue(res, 0, 6));
    PQclear(res);

    pg_store_close(store);
}

static void test_insert_generation_run_allows_null_prompt_and_answer(void) {
    PgStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected pg_store_open to succeed");

    /* No fallback in generation.c -- a failed API call means answer_text
     * (and possibly prompt_text, if it never even built) are NULL. */
    int64_t query_id = query_log_insert_query(store, "What is hypertension?");
    int result = query_log_insert_generation_run(store, query_id, "openai/gpt-4o-mini", 0, 0, NULL,
                                                  NULL, 0, 12);
    TEST_ASSERT(result == 0, "expected NULL prompt/answer to be accepted");

    char query_id_str[32];
    snprintf(query_id_str, sizeof(query_id_str), "%lld", (long long)query_id);
    const char *params[1] = {query_id_str};
    PGresult *res = PQexecParams(
        store->conn, "SELECT prompt_text, answer_text FROM generation_runs WHERE query_id = $1;", 1,
        NULL, params, NULL, NULL, 0);
    TEST_ASSERT(PQresultStatus(res) == PGRES_TUPLES_OK, "expected verification query to succeed");
    TEST_ASSERT(PQntuples(res) == 1, "expected a generation_runs row to exist");
    TEST_ASSERT(PQgetisnull(res, 0, 0), "expected prompt_text to be NULL");
    TEST_ASSERT(PQgetisnull(res, 0, 1), "expected answer_text to be NULL");
    PQclear(res);

    pg_store_close(store);
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
    return test_summary();
}
