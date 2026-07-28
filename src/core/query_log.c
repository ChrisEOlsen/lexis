/*
 * Implementation of pipeline observability logging.
 * See include/query_log.h for the module's role.
 */

/* See tokenizer.c for why this must come before any #include (strdup is a
 * POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "query_log.h"

#include <stdio.h>
#include <time.h>

#define LEXIS_QUERY_LOG_SCHEMA_SQL                                       \
    "CREATE TABLE IF NOT EXISTS queries ("                              \
    "    id INTEGER PRIMARY KEY,"                                       \
    "    question_text TEXT NOT NULL,"                                  \
    "    created_at INTEGER NOT NULL,"                                  \
    "    total_latency_ms INTEGER,"                                     \
    "    succeeded INTEGER"                                             \
    ");"                                                                \
    "CREATE TABLE IF NOT EXISTS query_formulation_runs ("                \
    "    id INTEGER PRIMARY KEY,"                                       \
    "    query_id INTEGER NOT NULL REFERENCES queries(id),"             \
    "    surviving_term_count INTEGER NOT NULL,"                        \
    "    prompt_text TEXT,"                                             \
    "    llm_response_text TEXT,"                                       \
    "    used_fallback INTEGER NOT NULL,"                               \
    "    selected_terms TEXT NOT NULL,"                                 \
    "    latency_ms INTEGER NOT NULL"                                   \
    ");"                                                                \
    "CREATE TABLE IF NOT EXISTS search_runs ("                          \
    "    id INTEGER PRIMARY KEY,"                                       \
    "    query_id INTEGER NOT NULL REFERENCES queries(id),"             \
    "    top_k INTEGER NOT NULL,"                                       \
    "    result_count INTEGER NOT NULL,"                                \
    "    latency_ms INTEGER NOT NULL"                                   \
    ");"                                                                \
    "CREATE TABLE IF NOT EXISTS search_results ("                       \
    "    search_run_id INTEGER NOT NULL REFERENCES search_runs(id),"    \
    "    rank INTEGER NOT NULL,"                                        \
    "    passage_id INTEGER NOT NULL REFERENCES passages(id),"          \
    "    score REAL NOT NULL,"                                          \
    "    PRIMARY KEY (search_run_id, rank)"                             \
    ");"                                                                \
    "CREATE TABLE IF NOT EXISTS generation_runs ("                      \
    "    id INTEGER PRIMARY KEY,"                                       \
    "    query_id INTEGER NOT NULL REFERENCES queries(id),"             \
    "    model TEXT NOT NULL,"                                          \
    "    passages_included INTEGER NOT NULL,"                           \
    "    passages_skipped INTEGER NOT NULL,"                            \
    "    prompt_text TEXT,"                                             \
    "    answer_text TEXT,"                                             \
    "    succeeded INTEGER NOT NULL,"                                   \
    "    latency_ms INTEGER NOT NULL"                                   \
    ");"

int query_log_init_schema(SqliteStore *store) {
    char *errmsg = NULL;
    if (sqlite3_exec(store->db, LEXIS_QUERY_LOG_SCHEMA_SQL, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "query_log_init_schema: schema creation failed: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

sqlite3_int64 query_log_insert_query(SqliteStore *store, const char *question_text) {
    static const char *sql =
        "INSERT INTO queries (question_text, created_at) VALUES (?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "query_log_insert_query: prepare failed: %s\n", sqlite3_errmsg(store->db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, question_text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));

    sqlite3_int64 query_id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        query_id = sqlite3_last_insert_rowid(store->db);
    } else {
        fprintf(stderr, "query_log_insert_query: insert failed: %s\n", sqlite3_errmsg(store->db));
    }

    sqlite3_finalize(stmt);
    return query_id;
}

int query_log_finish_query(SqliteStore *store, sqlite3_int64 query_id, long total_latency_ms,
                            int succeeded) {
    static const char *sql =
        "UPDATE queries SET total_latency_ms = ?, succeeded = ? WHERE id = ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "query_log_finish_query: prepare failed: %s\n", sqlite3_errmsg(store->db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)total_latency_ms);
    sqlite3_bind_int(stmt, 2, succeeded);
    sqlite3_bind_int64(stmt, 3, query_id);

    int result = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        result = 0;
    } else {
        fprintf(stderr, "query_log_finish_query: update failed: %s\n", sqlite3_errmsg(store->db));
    }

    sqlite3_finalize(stmt);
    return result;
}

int query_log_insert_query_formulation_run(SqliteStore *store, sqlite3_int64 query_id,
                                            int surviving_term_count, const char *prompt_text,
                                            const char *llm_response_text, int used_fallback,
                                            const char *selected_terms, long latency_ms) {
    static const char *sql =
        "INSERT INTO query_formulation_runs "
        "(query_id, surviving_term_count, prompt_text, llm_response_text, used_fallback, "
        " selected_terms, latency_ms) VALUES (?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "query_log_insert_query_formulation_run: prepare failed: %s\n",
                sqlite3_errmsg(store->db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, query_id);
    sqlite3_bind_int(stmt, 2, surviving_term_count);
    sqlite3_bind_text(stmt, 3, prompt_text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, llm_response_text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, used_fallback);
    sqlite3_bind_text(stmt, 6, selected_terms, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)latency_ms);

    int result = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        result = 0;
    } else {
        fprintf(stderr, "query_log_insert_query_formulation_run: insert failed: %s\n",
                sqlite3_errmsg(store->db));
    }

    sqlite3_finalize(stmt);
    return result;
}

sqlite3_int64 query_log_insert_search_run(SqliteStore *store, sqlite3_int64 query_id, int top_k,
                                           int result_count, long latency_ms) {
    static const char *sql =
        "INSERT INTO search_runs (query_id, top_k, result_count, latency_ms) VALUES (?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "query_log_insert_search_run: prepare failed: %s\n",
                sqlite3_errmsg(store->db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, query_id);
    sqlite3_bind_int(stmt, 2, top_k);
    sqlite3_bind_int(stmt, 3, result_count);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)latency_ms);

    sqlite3_int64 search_run_id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        search_run_id = sqlite3_last_insert_rowid(store->db);
    } else {
        fprintf(stderr, "query_log_insert_search_run: insert failed: %s\n",
                sqlite3_errmsg(store->db));
    }

    sqlite3_finalize(stmt);
    return search_run_id;
}

int query_log_insert_search_result(SqliteStore *store, sqlite3_int64 search_run_id, int rank,
                                    sqlite3_int64 passage_id, double score) {
    static const char *sql =
        "INSERT INTO search_results (search_run_id, rank, passage_id, score) VALUES (?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "query_log_insert_search_result: prepare failed: %s\n",
                sqlite3_errmsg(store->db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, search_run_id);
    sqlite3_bind_int(stmt, 2, rank);
    sqlite3_bind_int64(stmt, 3, passage_id);
    sqlite3_bind_double(stmt, 4, score);

    int result = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        result = 0;
    } else {
        fprintf(stderr, "query_log_insert_search_result: insert failed: %s\n",
                sqlite3_errmsg(store->db));
    }

    sqlite3_finalize(stmt);
    return result;
}

int query_log_insert_generation_run(SqliteStore *store, sqlite3_int64 query_id, const char *model,
                                     int passages_included, int passages_skipped,
                                     const char *prompt_text, const char *answer_text,
                                     int succeeded, long latency_ms) {
    static const char *sql =
        "INSERT INTO generation_runs "
        "(query_id, model, passages_included, passages_skipped, prompt_text, answer_text, "
        " succeeded, latency_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "query_log_insert_generation_run: prepare failed: %s\n",
                sqlite3_errmsg(store->db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, query_id);
    sqlite3_bind_text(stmt, 2, model, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, passages_included);
    sqlite3_bind_int(stmt, 4, passages_skipped);
    sqlite3_bind_text(stmt, 5, prompt_text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, answer_text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, succeeded);
    sqlite3_bind_int64(stmt, 8, (sqlite3_int64)latency_ms);

    int result = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        result = 0;
    } else {
        fprintf(stderr, "query_log_insert_generation_run: insert failed: %s\n",
                sqlite3_errmsg(store->db));
    }

    sqlite3_finalize(stmt);
    return result;
}
