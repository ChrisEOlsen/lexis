/*
 * Implementation of Postgres-backed index/passage persistence.
 * See include/pg_store.h for the module's role and the SQLite-vs-Postgres
 * rationale.
 */

#define _POSIX_C_SOURCE 200809L

#include "pg_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Schema for the inverted index -- same shape as sqlite_store.c's, with
 * GENERATED ALWAYS AS IDENTITY in place of SQLite's INTEGER PRIMARY KEY
 * rowid-aliasing trick. IF NOT EXISTS makes this safe to run on every
 * open, not just the first. */
#define LEXIS_SCHEMA_SQL                                                 \
    "CREATE TABLE IF NOT EXISTS passages ("                              \
    "    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"            \
    "    document_name TEXT NOT NULL,"                                   \
    "    chunk_id INTEGER NOT NULL,"                                     \
    "    text TEXT NOT NULL,"                                            \
    "    token_count INTEGER NOT NULL"                                   \
    ");"                                                                 \
    "CREATE TABLE IF NOT EXISTS terms ("                                 \
    "    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"            \
    "    term TEXT NOT NULL UNIQUE"                                      \
    ");"                                                                 \
    "CREATE TABLE IF NOT EXISTS postings ("                              \
    "    term_id BIGINT NOT NULL REFERENCES terms(id),"                  \
    "    passage_id BIGINT NOT NULL REFERENCES passages(id),"            \
    "    term_frequency INTEGER NOT NULL,"                               \
    "    PRIMARY KEY (term_id, passage_id)"                              \
    ");"

PgStore *pg_store_open(const char *conninfo) {
    PgStore *store = malloc(sizeof(PgStore));
    if (store == NULL) {
        return NULL;
    }

    store->conn = PQconnectdb(conninfo);
    if (PQstatus(store->conn) != CONNECTION_OK) {
        fprintf(stderr, "pg_store_open: %s\n", PQerrorMessage(store->conn));
        PQfinish(store->conn);
        free(store);
        return NULL;
    }

    PGresult *res = PQexec(store->conn, LEXIS_SCHEMA_SQL);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "pg_store_open: schema creation failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        PQfinish(store->conn);
        free(store);
        return NULL;
    }
    PQclear(res);

    return store;
}

void pg_store_close(PgStore *store) {
    if (store == NULL) {
        return;
    }
    PQfinish(store->conn);
    free(store);
}

int64_t pg_store_insert_passage(PgStore *store, const char *document_name, int chunk_id,
                                 const char *text, int token_count) {
    char chunk_id_str[32];
    char token_count_str[32];
    snprintf(chunk_id_str, sizeof(chunk_id_str), "%d", chunk_id);
    snprintf(token_count_str, sizeof(token_count_str), "%d", token_count);

    const char *params[4] = {document_name, chunk_id_str, text, token_count_str};
    static const char *sql =
        "INSERT INTO passages (document_name, chunk_id, text, token_count) "
        "VALUES ($1, $2, $3, $4) RETURNING id;";

    PGresult *res = PQexecParams(store->conn, sql, 4, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_store_insert_passage: insert failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }

    int64_t passage_id = atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    return passage_id;
}

int64_t pg_store_get_or_create_term(PgStore *store, const char *term) {
    /* Fast path: a plain read, no write-lock contention, for the common
     * case of an already-seen term. */
    {
        const char *params[1] = {term};
        static const char *select_sql = "SELECT id FROM terms WHERE term = $1;";
        PGresult *res = PQexecParams(store->conn, select_sql, 1, NULL, params, NULL, NULL, 0);
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            fprintf(stderr, "pg_store_get_or_create_term: select failed: %s\n",
                    PQerrorMessage(store->conn));
            PQclear(res);
            return -1;
        }
        if (PQntuples(res) > 0) {
            int64_t term_id = atoll(PQgetvalue(res, 0, 0));
            PQclear(res);
            return term_id;
        }
        PQclear(res);
    }

    /* Not found -- insert atomically. DO UPDATE (a no-op self-assignment)
     * rather than DO NOTHING specifically so RETURNING id still yields a
     * row on conflict; this closes the SELECT-then-INSERT race the SQLite
     * version couldn't avoid (see LIMITATIONS.md), safe if two writers hit
     * this at once. */
    const char *params[1] = {term};
    static const char *insert_sql = "INSERT INTO terms (term) VALUES ($1) "
                                     "ON CONFLICT (term) DO UPDATE SET term = EXCLUDED.term "
                                     "RETURNING id;";
    PGresult *res = PQexecParams(store->conn, insert_sql, 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_store_get_or_create_term: insert failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }

    int64_t term_id = atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    return term_id;
}

int pg_store_insert_posting(PgStore *store, int64_t term_id, int64_t passage_id, int term_frequency) {
    char term_id_str[32];
    char passage_id_str[32];
    char frequency_str[32];
    snprintf(term_id_str, sizeof(term_id_str), "%lld", (long long)term_id);
    snprintf(passage_id_str, sizeof(passage_id_str), "%lld", (long long)passage_id);
    snprintf(frequency_str, sizeof(frequency_str), "%d", term_frequency);

    const char *params[3] = {term_id_str, passage_id_str, frequency_str};
    static const char *sql =
        "INSERT INTO postings (term_id, passage_id, term_frequency) VALUES ($1, $2, $3);";

    PGresult *res = PQexecParams(store->conn, sql, 3, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        fprintf(stderr, "pg_store_insert_posting: insert failed: %s\n", PQerrorMessage(store->conn));
    }
    PQclear(res);
    return ok ? 0 : -1;
}

int64_t pg_store_lookup_term(PgStore *store, const char *term) {
    const char *params[1] = {term};
    static const char *sql = "SELECT id FROM terms WHERE term = $1;";

    PGresult *res = PQexecParams(store->conn, sql, 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_store_lookup_term: select failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }

    int64_t term_id = -1;
    if (PQntuples(res) > 0) {
        term_id = atoll(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    return term_id;
}

void pg_store_passage_free(PgStorePassage *passage) {
    if (passage == NULL) {
        return;
    }
    free(passage->document_name);
    free(passage->text);
    free(passage);
}

PgStorePassage *pg_store_get_passage(PgStore *store, int64_t passage_id) {
    char passage_id_str[32];
    snprintf(passage_id_str, sizeof(passage_id_str), "%lld", (long long)passage_id);
    const char *params[1] = {passage_id_str};
    static const char *sql =
        "SELECT document_name, chunk_id, text, token_count FROM passages WHERE id = $1;";

    PGresult *res = PQexecParams(store->conn, sql, 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        return NULL;
    }

    PgStorePassage *passage = malloc(sizeof(PgStorePassage));
    if (passage == NULL) {
        PQclear(res);
        return NULL;
    }

    passage->document_name = strdup(PQgetvalue(res, 0, 0));
    passage->chunk_id = atoi(PQgetvalue(res, 0, 1));
    passage->text = strdup(PQgetvalue(res, 0, 2));
    passage->token_count = atoi(PQgetvalue(res, 0, 3));
    PQclear(res);

    if (passage->document_name == NULL || passage->text == NULL) {
        pg_store_passage_free(passage);
        return NULL;
    }

    return passage;
}

static int exec_simple(PGconn *conn, const char *sql, const char *caller) {
    PGresult *res = PQexec(conn, sql);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        fprintf(stderr, "%s: %s\n", caller, PQerrorMessage(conn));
    }
    PQclear(res);
    return ok ? 0 : -1;
}

int pg_store_begin_transaction(PgStore *store) {
    return exec_simple(store->conn, "BEGIN;", "pg_store_begin_transaction");
}

int pg_store_commit_transaction(PgStore *store) {
    return exec_simple(store->conn, "COMMIT;", "pg_store_commit_transaction");
}

int pg_store_rollback_transaction(PgStore *store) {
    return exec_simple(store->conn, "ROLLBACK;", "pg_store_rollback_transaction");
}
