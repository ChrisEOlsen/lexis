/*
 * Implementation of pipeline observability logging.
 * See include/query_log.h for the module's role.
 */

/* See tokenizer.c for why this must come before any #include (strdup is a
 * POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "query_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Schema uses BIGINT GENERATED ALWAYS AS IDENTITY (see pg_store.c) in
 * place of SQLite's INTEGER PRIMARY KEY rowid-aliasing trick; REAL becomes
 * DOUBLE PRECISION -- otherwise identical to the SQLite version's shape. */
#define LEXIS_QUERY_LOG_SCHEMA_SQL                                                            \
    "CREATE TABLE IF NOT EXISTS queries ("                                                    \
    "    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"                                 \
    "    question_text TEXT NOT NULL,"                                                        \
    "    created_at BIGINT NOT NULL,"                                                         \
    "    total_latency_ms BIGINT,"                                                             \
    "    succeeded INTEGER"                                                                   \
    ");"                                                                                       \
    "CREATE TABLE IF NOT EXISTS query_formulation_runs ("                                     \
    "    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"                                 \
    "    query_id BIGINT NOT NULL REFERENCES queries(id),"                                    \
    "    surviving_term_count INTEGER NOT NULL,"                                              \
    "    prompt_text TEXT,"                                                                   \
    "    llm_response_text TEXT,"                                                             \
    "    used_fallback INTEGER NOT NULL,"                                                     \
    "    selected_terms TEXT NOT NULL,"                                                       \
    "    latency_ms BIGINT NOT NULL"                                                          \
    ");"                                                                                       \
    "CREATE TABLE IF NOT EXISTS search_runs ("                                                \
    "    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"                                 \
    "    query_id BIGINT NOT NULL REFERENCES queries(id),"                                    \
    "    top_k INTEGER NOT NULL,"                                                             \
    "    result_count INTEGER NOT NULL,"                                                      \
    "    latency_ms BIGINT NOT NULL"                                                          \
    ");"                                                                                       \
    "CREATE TABLE IF NOT EXISTS search_results ("                                             \
    "    search_run_id BIGINT NOT NULL REFERENCES search_runs(id),"                           \
    "    rank INTEGER NOT NULL,"                                                              \
    "    passage_id BIGINT NOT NULL REFERENCES passages(id),"                                 \
    "    score DOUBLE PRECISION NOT NULL,"                                                    \
    "    PRIMARY KEY (search_run_id, rank)"                                                   \
    ");"                                                                                       \
    "CREATE TABLE IF NOT EXISTS generation_runs ("                                            \
    "    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"                                 \
    "    query_id BIGINT NOT NULL REFERENCES queries(id),"                                    \
    "    model TEXT NOT NULL,"                                                                \
    "    passages_included INTEGER NOT NULL,"                                                 \
    "    passages_skipped INTEGER NOT NULL,"                                                  \
    "    prompt_text TEXT,"                                                                   \
    "    answer_text TEXT,"                                                                   \
    "    succeeded INTEGER NOT NULL,"                                                         \
    "    latency_ms BIGINT NOT NULL"                                                          \
    ");"

int query_log_init_schema(PgStore *store) {
    PGresult *res = PQexec(store->conn, LEXIS_QUERY_LOG_SCHEMA_SQL);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "query_log_init_schema: schema creation failed: %s\n",
                PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }
    PQclear(res);
    return 0;
}

int64_t query_log_insert_query(PgStore *store, const char *question_text) {
    char created_at_str[32];
    snprintf(created_at_str, sizeof(created_at_str), "%lld", (long long)time(NULL));
    const char *params[2] = {question_text, created_at_str};

    static const char *sql =
        "INSERT INTO queries (question_text, created_at) VALUES ($1, $2) RETURNING id;";

    PGresult *res = PQexecParams(store->conn, sql, 2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "query_log_insert_query: insert failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }

    int64_t query_id = atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    return query_id;
}

int query_log_finish_query(PgStore *store, int64_t query_id, long total_latency_ms, int succeeded) {
    char latency_str[32];
    char succeeded_str[8];
    char query_id_str[32];
    snprintf(latency_str, sizeof(latency_str), "%ld", total_latency_ms);
    snprintf(succeeded_str, sizeof(succeeded_str), "%d", succeeded);
    snprintf(query_id_str, sizeof(query_id_str), "%lld", (long long)query_id);
    const char *params[3] = {latency_str, succeeded_str, query_id_str};

    static const char *sql =
        "UPDATE queries SET total_latency_ms = $1, succeeded = $2 WHERE id = $3;";

    PGresult *res = PQexecParams(store->conn, sql, 3, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        fprintf(stderr, "query_log_finish_query: update failed: %s\n", PQerrorMessage(store->conn));
    }
    PQclear(res);
    return ok ? 0 : -1;
}

int query_log_insert_query_formulation_run(PgStore *store, int64_t query_id,
                                            int surviving_term_count, const char *prompt_text,
                                            const char *llm_response_text, int used_fallback,
                                            const char *selected_terms, long latency_ms) {
    char query_id_str[32], surviving_str[16], used_fallback_str[8], latency_str[32];
    snprintf(query_id_str, sizeof(query_id_str), "%lld", (long long)query_id);
    snprintf(surviving_str, sizeof(surviving_str), "%d", surviving_term_count);
    snprintf(used_fallback_str, sizeof(used_fallback_str), "%d", used_fallback);
    snprintf(latency_str, sizeof(latency_str), "%ld", latency_ms);

    const char *params[7] = {query_id_str,   surviving_str, prompt_text,     llm_response_text,
                              used_fallback_str, selected_terms, latency_str};

    static const char *sql =
        "INSERT INTO query_formulation_runs "
        "(query_id, surviving_term_count, prompt_text, llm_response_text, used_fallback, "
        " selected_terms, latency_ms) VALUES ($1, $2, $3, $4, $5, $6, $7);";

    PGresult *res = PQexecParams(store->conn, sql, 7, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        fprintf(stderr, "query_log_insert_query_formulation_run: insert failed: %s\n",
                PQerrorMessage(store->conn));
    }
    PQclear(res);
    return ok ? 0 : -1;
}

int64_t query_log_insert_search_run(PgStore *store, int64_t query_id, int top_k, int result_count,
                                     long latency_ms) {
    char query_id_str[32], top_k_str[16], result_count_str[16], latency_str[32];
    snprintf(query_id_str, sizeof(query_id_str), "%lld", (long long)query_id);
    snprintf(top_k_str, sizeof(top_k_str), "%d", top_k);
    snprintf(result_count_str, sizeof(result_count_str), "%d", result_count);
    snprintf(latency_str, sizeof(latency_str), "%ld", latency_ms);
    const char *params[4] = {query_id_str, top_k_str, result_count_str, latency_str};

    static const char *sql = "INSERT INTO search_runs (query_id, top_k, result_count, latency_ms) "
                              "VALUES ($1, $2, $3, $4) RETURNING id;";

    PGresult *res = PQexecParams(store->conn, sql, 4, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "query_log_insert_search_run: insert failed: %s\n",
                PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }

    int64_t search_run_id = atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    return search_run_id;
}

int query_log_insert_search_result(PgStore *store, int64_t search_run_id, int rank,
                                    int64_t passage_id, double score) {
    char search_run_id_str[32], rank_str[16], passage_id_str[32], score_str[64];
    snprintf(search_run_id_str, sizeof(search_run_id_str), "%lld", (long long)search_run_id);
    snprintf(rank_str, sizeof(rank_str), "%d", rank);
    snprintf(passage_id_str, sizeof(passage_id_str), "%lld", (long long)passage_id);
    snprintf(score_str, sizeof(score_str), "%.17g", score);
    const char *params[4] = {search_run_id_str, rank_str, passage_id_str, score_str};

    static const char *sql = "INSERT INTO search_results (search_run_id, rank, passage_id, score) "
                              "VALUES ($1, $2, $3, $4);";

    PGresult *res = PQexecParams(store->conn, sql, 4, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        fprintf(stderr, "query_log_insert_search_result: insert failed: %s\n",
                PQerrorMessage(store->conn));
    }
    PQclear(res);
    return ok ? 0 : -1;
}

int query_log_insert_generation_run(PgStore *store, int64_t query_id, const char *model,
                                     int passages_included, int passages_skipped,
                                     const char *prompt_text, const char *answer_text,
                                     int succeeded, long latency_ms) {
    char query_id_str[32], included_str[16], skipped_str[16], succeeded_str[8], latency_str[32];
    snprintf(query_id_str, sizeof(query_id_str), "%lld", (long long)query_id);
    snprintf(included_str, sizeof(included_str), "%d", passages_included);
    snprintf(skipped_str, sizeof(skipped_str), "%d", passages_skipped);
    snprintf(succeeded_str, sizeof(succeeded_str), "%d", succeeded);
    snprintf(latency_str, sizeof(latency_str), "%ld", latency_ms);

    const char *params[8] = {query_id_str, model,        included_str,  skipped_str,
                              prompt_text,   answer_text, succeeded_str, latency_str};

    static const char *sql =
        "INSERT INTO generation_runs "
        "(query_id, model, passages_included, passages_skipped, prompt_text, answer_text, "
        " succeeded, latency_ms) VALUES ($1, $2, $3, $4, $5, $6, $7, $8);";

    PGresult *res = PQexecParams(store->conn, sql, 8, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        fprintf(stderr, "query_log_insert_generation_run: insert failed: %s\n",
                PQerrorMessage(store->conn));
    }
    PQclear(res);
    return ok ? 0 : -1;
}
