/*
 * Implementation of SQLite-backed index/passage persistence.
 * See include/sqlite_store.h for the module's role (spec 5.2.1, Stage 2).
 */

/* See tokenizer.c for why this must come before any #include (strdup is a
 * POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "sqlite_store.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Schema for the inverted index (see header comment for the design).
 * IF NOT EXISTS makes this safe to run on every open, not just the first. */
#define LEXIS_SCHEMA_SQL                                            \
    "CREATE TABLE IF NOT EXISTS passages ("                         \
    "    id INTEGER PRIMARY KEY,"                                   \
    "    document_name TEXT NOT NULL,"                              \
    "    chunk_id INTEGER NOT NULL,"                                \
    "    text TEXT NOT NULL,"                                       \
    "    token_count INTEGER NOT NULL"                              \
    ");"                                                            \
    "CREATE TABLE IF NOT EXISTS terms ("                            \
    "    id INTEGER PRIMARY KEY,"                                   \
    "    term TEXT NOT NULL UNIQUE"                                 \
    ");"                                                            \
    "CREATE TABLE IF NOT EXISTS postings ("                         \
    "    term_id INTEGER NOT NULL REFERENCES terms(id),"            \
    "    passage_id INTEGER NOT NULL REFERENCES passages(id),"      \
    "    term_frequency INTEGER NOT NULL,"                          \
    "    PRIMARY KEY (term_id, passage_id)"                         \
    ");"

SqliteStore *sqlite_store_open(const char *path) {
    SqliteStore *store = malloc(sizeof(SqliteStore));
    if (store == NULL) {
        return NULL;
    }

    /* sqlite3_open() sets *ppDb even on failure (to a handle you can pull
     * an error message from), so sqlite3_close() below is always correct
     * to call — never skip it just because the open failed. */
    if (sqlite3_open(path, &store->db) != SQLITE_OK) {
        fprintf(stderr, "sqlite_store_open: %s\n", sqlite3_errmsg(store->db));
        sqlite3_close(store->db);
        free(store);
        return NULL;
    }

    char *errmsg = NULL;
    if (sqlite3_exec(store->db, LEXIS_SCHEMA_SQL, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "sqlite_store_open: schema creation failed: %s\n", errmsg);
        sqlite3_free(errmsg);
        sqlite3_close(store->db);
        free(store);
        return NULL;
    }

    return store;
}

void sqlite_store_close(SqliteStore *store) {
    if (store == NULL) {
        return;
    }

    sqlite3_close(store->db);
    free(store);
}

sqlite3_int64 sqlite_store_insert_passage(SqliteStore *store,
                                           const char *document_name,
                                           int chunk_id, const char *text,
                                           int token_count) {
    static const char *sql =
        "INSERT INTO passages (document_name, chunk_id, text, token_count) "
        "VALUES (?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "sqlite_store_insert_passage: prepare failed: %s\n",
                sqlite3_errmsg(store->db));
        return -1;
    }

    /* Bind indices are 1-based, not 0-based. SQLITE_TRANSIENT tells
     * SQLite to copy each string immediately rather than trusting our
     * pointer to stay valid — safe default when we don't control the
     * caller's buffer lifetime. */
    sqlite3_bind_text(stmt, 1, document_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, chunk_id);
    sqlite3_bind_text(stmt, 3, text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, token_count);

    sqlite3_int64 passage_id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        passage_id = sqlite3_last_insert_rowid(store->db);
    } else {
        fprintf(stderr, "sqlite_store_insert_passage: insert failed: %s\n",
                sqlite3_errmsg(store->db));
    }

    sqlite3_finalize(stmt);
    return passage_id;
}

sqlite3_int64 sqlite_store_get_or_create_term(SqliteStore *store,
                                               const char *term) {
    static const char *select_sql = "SELECT id FROM terms WHERE term = ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, select_sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "sqlite_store_get_or_create_term: select prepare failed: %s\n",
                sqlite3_errmsg(store->db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, term, -1, SQLITE_TRANSIENT);

    int step_result = sqlite3_step(stmt);
    if (step_result == SQLITE_ROW) {
        /* Column indices are 0-based, unlike bind's 1-based indices. */
        sqlite3_int64 term_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return term_id;
    }
    sqlite3_finalize(stmt);

    if (step_result != SQLITE_DONE) {
        fprintf(stderr, "sqlite_store_get_or_create_term: select failed: %s\n",
                sqlite3_errmsg(store->db));
        return -1;
    }

    /* No existing row — this is the first time we've seen this term. */
    static const char *insert_sql = "INSERT INTO terms (term) VALUES (?);";
    if (sqlite3_prepare_v2(store->db, insert_sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "sqlite_store_get_or_create_term: insert prepare failed: %s\n",
                sqlite3_errmsg(store->db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, term, -1, SQLITE_TRANSIENT);

    sqlite3_int64 term_id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        term_id = sqlite3_last_insert_rowid(store->db);
    } else {
        fprintf(stderr, "sqlite_store_get_or_create_term: insert failed: %s\n",
                sqlite3_errmsg(store->db));
    }
    sqlite3_finalize(stmt);
    return term_id;
}

int sqlite_store_insert_posting(SqliteStore *store, sqlite3_int64 term_id,
                                 sqlite3_int64 passage_id,
                                 int term_frequency) {
    static const char *sql =
        "INSERT INTO postings (term_id, passage_id, term_frequency) "
        "VALUES (?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "sqlite_store_insert_posting: prepare failed: %s\n",
                sqlite3_errmsg(store->db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, term_id);
    sqlite3_bind_int64(stmt, 2, passage_id);
    sqlite3_bind_int(stmt, 3, term_frequency);

    int result = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        result = 0;
    } else {
        fprintf(stderr, "sqlite_store_insert_posting: insert failed: %s\n",
                sqlite3_errmsg(store->db));
    }

    sqlite3_finalize(stmt);
    return result;
}

sqlite3_int64 sqlite_store_lookup_term(SqliteStore *store, const char *term)
{
    static const char *select_sql = "SELECT id FROM terms WHERE term = ?";
    sqlite3_stmt *stmt = NULL;
    if(sqlite3_prepare_v2(store->db, select_sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "sqlite_store_lookup_term: prepare failed: %s\n",
                sqlite3_errmsg(store->db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, term, -1, SQLITE_TRANSIENT);

    int result = sqlite3_step(stmt);

    if(result == SQLITE_ROW)
    {
        sqlite3_int64 term_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return term_id;
    }
    else if (result == SQLITE_DONE)
    {
        sqlite3_finalize(stmt);
        return -1;
    } else {
        fprintf(stderr, "sqlite_store_lookup_term: select failed: %s\n",
                sqlite3_errmsg(store->db));
    }

    sqlite3_finalize(stmt);
    return -1;
}

void sqlite_store_passage_free(SqliteStorePassage *passage) {
    if (passage == NULL) {
        return;
    }

    free(passage->document_name);
    free(passage->text);
    free(passage);
}

static int exec_transaction_statement(SqliteStore *store, const char *sql, const char *caller) {
    char *errmsg = NULL;
    if (sqlite3_exec(store->db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "%s: %s\n", caller, errmsg);
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

int sqlite_store_begin_transaction(SqliteStore *store) {
    return exec_transaction_statement(store, "BEGIN;", "sqlite_store_begin_transaction");
}

int sqlite_store_commit_transaction(SqliteStore *store) {
    return exec_transaction_statement(store, "COMMIT;", "sqlite_store_commit_transaction");
}

int sqlite_store_rollback_transaction(SqliteStore *store) {
    return exec_transaction_statement(store, "ROLLBACK;", "sqlite_store_rollback_transaction");
}

SqliteStorePassage *sqlite_store_get_passage(SqliteStore *store, sqlite3_int64 passage_id) {
    static const char *sql =
        "SELECT document_name, chunk_id, text, token_count FROM passages WHERE id = ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "sqlite_store_get_passage: prepare failed: %s\n",
                sqlite3_errmsg(store->db));
        return NULL;
    }
    sqlite3_bind_int64(stmt, 1, passage_id);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    SqliteStorePassage *passage = malloc(sizeof(SqliteStorePassage));
    if (passage == NULL) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    passage->document_name = strdup((const char *)sqlite3_column_text(stmt, 0));
    passage->chunk_id = sqlite3_column_int(stmt, 1);
    passage->text = strdup((const char *)sqlite3_column_text(stmt, 2));
    passage->token_count = sqlite3_column_int(stmt, 3);

    sqlite3_finalize(stmt);

    if (passage->document_name == NULL || passage->text == NULL) {
        sqlite_store_passage_free(passage);
        return NULL;
    }

    return passage;
}
