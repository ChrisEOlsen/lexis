/* Postgres-backed index/passage persistence; see pg_store.h. */

#define _POSIX_C_SOURCE 200809L

#include "pg_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same shape as sqlite_store.c (IDENTITY replaces rowid alias). postings.token_count
 * denormalizes passages.token_count so BM25 avoids a per-row JOIN (see dev/LIMITATIONS.md). */
/* documents holds each source's ORIGINAL un-chunked text (one row per document, keyed
 * by document_name) so rebuild-on-append can re-chunk consistently (see dev/APP_SPEC.md). */
#define LEXIS_SCHEMA_SQL                                                 \
    "CREATE TABLE IF NOT EXISTS documents ("                            \
    "    document_name TEXT PRIMARY KEY,"                                \
    "    text TEXT NOT NULL"                                             \
    ");"                                                                 \
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
    "    token_count INTEGER NOT NULL,"                                  \
    "    PRIMARY KEY (term_id, passage_id)"                              \
    ");"

static int exec_simple(PGconn *conn, const char *sql, const char *caller);

/* public.corpora maps display_name to the server-generated schema_name holding one
 * group's tables. schema_name is never built from user input (see dev/APP_SPEC.md). */
#define LEXIS_CORPORA_REGISTRY_SQL                                      \
    "CREATE TABLE IF NOT EXISTS public.corpora ("                       \
    "    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"           \
    "    display_name TEXT NOT NULL,"                                  \
    "    schema_name TEXT NOT NULL UNIQUE,"                            \
    "    created_at TIMESTAMPTZ NOT NULL DEFAULT now()"                \
    ");"

int pg_store_ensure_corpora_registry(PgStore *store) {
    return exec_simple(store->conn, LEXIS_CORPORA_REGISTRY_SQL, "pg_store_ensure_corpora_registry");
}

/* CREATE SCHEMA + tables, shared by create_corpus() and rebuild's temp schema.
 * schema_name must be trusted/server-generated (safe to interpolate). 0 ok, -1 fail. */
static int create_lexis_tables_in_schema(PGconn *conn, const char *schema_name) {
    char ddl[2048];
    int written = snprintf(
        ddl, sizeof(ddl),
        "CREATE SCHEMA %s;"
        "CREATE TABLE %s.documents ("
        "    document_name TEXT PRIMARY KEY,"
        "    text TEXT NOT NULL"
        ");"
        "CREATE TABLE %s.passages ("
        "    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"
        "    document_name TEXT NOT NULL,"
        "    chunk_id INTEGER NOT NULL,"
        "    text TEXT NOT NULL,"
        "    token_count INTEGER NOT NULL"
        ");"
        "CREATE TABLE %s.terms ("
        "    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"
        "    term TEXT NOT NULL UNIQUE"
        ");"
        "CREATE TABLE %s.postings ("
        "    term_id BIGINT NOT NULL REFERENCES %s.terms(id),"
        "    passage_id BIGINT NOT NULL REFERENCES %s.passages(id),"
        "    term_frequency INTEGER NOT NULL,"
        "    token_count INTEGER NOT NULL,"
        "    PRIMARY KEY (term_id, passage_id)"
        ");",
        schema_name, schema_name, schema_name, schema_name, schema_name, schema_name, schema_name);
    if (written < 0 || (size_t)written >= sizeof(ddl)) {
        fprintf(stderr, "create_lexis_tables_in_schema: schema DDL didn't fit the buffer\n");
        return -1;
    }
    return exec_simple(conn, ddl, "create_lexis_tables_in_schema");
}

int64_t pg_store_create_corpus(PgStore *store, const char *display_name, char **schema_name_out) {
    if (display_name == NULL || display_name[0] == '\0' || schema_name_out == NULL) {
        return -1;
    }
    if (pg_store_ensure_corpora_registry(store) != 0) {
        return -1;
    }
    if (exec_simple(store->conn, "BEGIN;", "pg_store_create_corpus") != 0) {
        return -1;
    }

    /* Genuinely two round trips: a single WITH-CTE UPDATE can't see the row its sibling
     * CTE inserted (same-statement snapshot; verified: UPDATE matched 0 rows). */
    const char *insert_params[1] = {display_name};
    PGresult *insert_res =
        PQexecParams(store->conn, "INSERT INTO public.corpora (display_name, schema_name) VALUES ($1, '') RETURNING id;",
                      1, NULL, insert_params, NULL, NULL, 0);
    if (PQresultStatus(insert_res) != PGRES_TUPLES_OK || PQntuples(insert_res) != 1) {
        fprintf(stderr, "pg_store_create_corpus: registry insert failed: %s\n", PQerrorMessage(store->conn));
        PQclear(insert_res);
        exec_simple(store->conn, "ROLLBACK;", "pg_store_create_corpus");
        return -1;
    }
    int64_t id = strtoll(PQgetvalue(insert_res, 0, 0), NULL, 10);
    PQclear(insert_res);

    char schema_name_buf[32];
    snprintf(schema_name_buf, sizeof(schema_name_buf), "corpus_%lld", (long long)id);
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)id);
    const char *update_params[2] = {schema_name_buf, id_str};
    PGresult *update_res = PQexecParams(store->conn, "UPDATE public.corpora SET schema_name = $1 WHERE id = $2;", 2,
                                         NULL, update_params, NULL, NULL, 0);
    if (PQresultStatus(update_res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "pg_store_create_corpus: registry schema_name update failed: %s\n", PQerrorMessage(store->conn));
        PQclear(update_res);
        exec_simple(store->conn, "ROLLBACK;", "pg_store_create_corpus");
        return -1;
    }
    PQclear(update_res);

    char *schema_name = strdup(schema_name_buf);
    if (schema_name == NULL) {
        exec_simple(store->conn, "ROLLBACK;", "pg_store_create_corpus");
        return -1;
    }

    if (create_lexis_tables_in_schema(store->conn, schema_name) != 0) {
        free(schema_name);
        exec_simple(store->conn, "ROLLBACK;", "pg_store_create_corpus");
        return -1;
    }
    if (exec_simple(store->conn, "COMMIT;", "pg_store_create_corpus") != 0) {
        free(schema_name);
        return -1;
    }

    *schema_name_out = schema_name;
    return id;
}

/* corpus_id -> schema_name lookup shared by use/delete/swap. Returns a malloc'd
 * string the caller frees, or NULL if missing/error. */
static char *lookup_corpus_schema_name(PGconn *conn, int64_t corpus_id) {
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)corpus_id);
    const char *params[1] = {id_str};
    PGresult *res =
        PQexecParams(conn, "SELECT schema_name FROM public.corpora WHERE id = $1;", 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1) {
        PQclear(res);
        return NULL;
    }
    char *schema_name = strdup(PQgetvalue(res, 0, 0));
    PQclear(res);
    return schema_name;
}

int pg_store_use_schema(PgStore *store, const char *schema_name) {
    /* schema_name must be trusted/server-generated (interpolated into SET);
     * never call with user input. */
    char sql[128];
    snprintf(sql, sizeof(sql), "SET search_path TO %s, public;", schema_name);
    return exec_simple(store->conn, sql, "pg_store_use_schema");
}

int pg_store_use_corpus(PgStore *store, int64_t corpus_id) {
    char *schema_name = lookup_corpus_schema_name(store->conn, corpus_id);
    if (schema_name == NULL) {
        fprintf(stderr, "pg_store_use_corpus: no corpus with id %lld\n", (long long)corpus_id);
        return -1;
    }
    int result = pg_store_use_schema(store, schema_name);
    free(schema_name);
    return result;
}

PgStoreCorpus *pg_store_list_corpora(PgStore *store, size_t *count_out) {
    if (pg_store_ensure_corpora_registry(store) != 0) {
        return NULL;
    }

    PGresult *res = PQexec(store->conn, "SELECT id, display_name FROM public.corpora ORDER BY id;");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_store_list_corpora: select failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return NULL;
    }

    int rows = PQntuples(res);
    PgStoreCorpus *corpora = malloc(sizeof(PgStoreCorpus) * (size_t)(rows > 0 ? rows : 1));
    if (corpora == NULL) {
        PQclear(res);
        return NULL;
    }

    for (int r = 0; r < rows; r++) {
        corpora[r].id = strtoll(PQgetvalue(res, r, 0), NULL, 10);
        corpora[r].display_name = strdup(PQgetvalue(res, r, 1));
        if (corpora[r].display_name == NULL) {
            PQclear(res);
            pg_store_corpora_free(corpora, (size_t)r + 1);
            return NULL;
        }
    }
    PQclear(res);

    *count_out = (size_t)rows;
    return corpora;
}

void pg_store_corpora_free(PgStoreCorpus *corpora, size_t count) {
    if (corpora == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(corpora[i].display_name);
    }
    free(corpora);
}

int pg_store_delete_corpus(PgStore *store, int64_t corpus_id) {
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)corpus_id);
    const char *params[1] = {id_str};

    if (exec_simple(store->conn, "BEGIN;", "pg_store_delete_corpus") != 0) {
        return -1;
    }

    char *schema_name = lookup_corpus_schema_name(store->conn, corpus_id);
    if (schema_name == NULL) {
        fprintf(stderr, "pg_store_delete_corpus: no corpus with id %lld\n", (long long)corpus_id);
        exec_simple(store->conn, "ROLLBACK;", "pg_store_delete_corpus");
        return -1;
    }

    /* Our own server-generated value; safe to interpolate. */
    char drop_sql[128];
    snprintf(drop_sql, sizeof(drop_sql), "DROP SCHEMA %s CASCADE;", schema_name);
    if (exec_simple(store->conn, drop_sql, "pg_store_delete_corpus") != 0) {
        free(schema_name);
        exec_simple(store->conn, "ROLLBACK;", "pg_store_delete_corpus");
        return -1;
    }
    free(schema_name);

    PGresult *delete_res =
        PQexecParams(store->conn, "DELETE FROM public.corpora WHERE id = $1;", 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(delete_res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "pg_store_delete_corpus: registry row delete failed: %s\n", PQerrorMessage(store->conn));
        PQclear(delete_res);
        exec_simple(store->conn, "ROLLBACK;", "pg_store_delete_corpus");
        return -1;
    }
    PQclear(delete_res);

    return exec_simple(store->conn, "COMMIT;", "pg_store_delete_corpus");
}

/* Chat tables live in public (see pg_store.h). sources is JSONB for future queries;
 * this module only stores/returns it verbatim. */
#define LEXIS_CHAT_TABLES_SQL                                           \
    "CREATE TABLE IF NOT EXISTS public.chat_sessions ("                 \
    "    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"           \
    "    corpus_id BIGINT NOT NULL REFERENCES public.corpora(id) ON DELETE CASCADE," \
    "    title TEXT NOT NULL,"                                          \
    "    created_at TIMESTAMPTZ NOT NULL DEFAULT now()"                 \
    ");"                                                                \
    "CREATE TABLE IF NOT EXISTS public.chat_messages ("                 \
    "    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"           \
    "    session_id BIGINT NOT NULL REFERENCES public.chat_sessions(id) ON DELETE CASCADE," \
    "    is_user BOOLEAN NOT NULL,"                                     \
    "    text TEXT NOT NULL,"                                           \
    "    sources JSONB,"                                                \
    "    created_at TIMESTAMPTZ NOT NULL DEFAULT now()"                 \
    ");"                                                                \
    "CREATE INDEX IF NOT EXISTS chat_messages_session_id_idx ON public.chat_messages(session_id, id);"

int pg_store_ensure_chat_tables(PgStore *store) {
    return exec_simple(store->conn, LEXIS_CHAT_TABLES_SQL, "pg_store_ensure_chat_tables");
}

int64_t pg_store_create_chat_session(PgStore *store, int64_t corpus_id, const char *title) {
    if (title == NULL || title[0] == '\0') {
        return -1;
    }
    if (pg_store_ensure_chat_tables(store) != 0) {
        return -1;
    }

    char corpus_id_str[32];
    snprintf(corpus_id_str, sizeof(corpus_id_str), "%lld", (long long)corpus_id);
    const char *params[2] = {corpus_id_str, title};
    PGresult *res = PQexecParams(store->conn,
                                  "INSERT INTO public.chat_sessions (corpus_id, title) VALUES ($1, $2) RETURNING id;",
                                  2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1) {
        fprintf(stderr, "pg_store_create_chat_session: insert failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }
    int64_t id = strtoll(PQgetvalue(res, 0, 0), NULL, 10);
    PQclear(res);
    return id;
}

PgStoreChatSession *pg_store_list_chat_sessions(PgStore *store, int64_t corpus_id, size_t *count_out) {
    if (pg_store_ensure_chat_tables(store) != 0) {
        return NULL;
    }

    char corpus_id_str[32];
    snprintf(corpus_id_str, sizeof(corpus_id_str), "%lld", (long long)corpus_id);
    const char *params[1] = {corpus_id_str};
    /* to_char() pins ISO 8601; created_at::text would follow DateStyle instead
     * (unparseable by QDateTime::fromString(..., Qt::ISODate)). */
    PGresult *res = PQexecParams(
        store->conn,
        "SELECT id, title, to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') "
        "FROM public.chat_sessions WHERE corpus_id = $1 ORDER BY id DESC;",
        1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_store_list_chat_sessions: select failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return NULL;
    }

    int rows = PQntuples(res);
    PgStoreChatSession *sessions = malloc(sizeof(PgStoreChatSession) * (size_t)(rows > 0 ? rows : 1));
    if (sessions == NULL) {
        PQclear(res);
        return NULL;
    }

    for (int r = 0; r < rows; r++) {
        sessions[r].id = strtoll(PQgetvalue(res, r, 0), NULL, 10);
        sessions[r].title = strdup(PQgetvalue(res, r, 1));
        sessions[r].created_at = strdup(PQgetvalue(res, r, 2));
        if (sessions[r].title == NULL || sessions[r].created_at == NULL) {
            PQclear(res);
            pg_store_chat_sessions_free(sessions, (size_t)r + 1);
            return NULL;
        }
    }
    PQclear(res);

    *count_out = (size_t)rows;
    return sessions;
}

void pg_store_chat_sessions_free(PgStoreChatSession *sessions, size_t count) {
    if (sessions == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(sessions[i].title);
        free(sessions[i].created_at);
    }
    free(sessions);
}

int pg_store_delete_chat_session(PgStore *store, int64_t session_id) {
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)session_id);
    const char *params[1] = {id_str};

    PGresult *res = PQexecParams(store->conn, "DELETE FROM public.chat_sessions WHERE id = $1;", 1, NULL, params,
                                  NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "pg_store_delete_chat_session: delete failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }
    /* 0 rows = session never existed (same convention as delete_corpus). */
    int affected = atoi(PQcmdTuples(res));
    PQclear(res);
    if (affected == 0) {
        fprintf(stderr, "pg_store_delete_chat_session: no session with id %lld\n", (long long)session_id);
        return -1;
    }
    return 0;
}

int pg_store_append_chat_message(PgStore *store, int64_t session_id, int is_user, const char *text,
                                  const char *sources_json) {
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)session_id);
    const char *is_user_str = is_user ? "true" : "false";
    /* NULL paramValues entry means SQL NULL (libpq convention): user messages get
     * NULL sources, not the string "null". */
    const char *params[4] = {id_str, is_user_str, text, sources_json};
    PGresult *res = PQexecParams(
        store->conn,
        "INSERT INTO public.chat_messages (session_id, is_user, text, sources) VALUES ($1, $2, $3, $4::jsonb);", 4,
        NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "pg_store_append_chat_message: insert failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }
    PQclear(res);
    return 0;
}

PgStoreChatMessage *pg_store_get_chat_messages(PgStore *store, int64_t session_id, size_t *count_out) {
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)session_id);
    const char *params[1] = {id_str};
    PGresult *res = PQexecParams(
        store->conn, "SELECT is_user, text, sources::text FROM public.chat_messages WHERE session_id = $1 ORDER BY id;",
        1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_store_get_chat_messages: select failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return NULL;
    }

    int rows = PQntuples(res);
    PgStoreChatMessage *messages = malloc(sizeof(PgStoreChatMessage) * (size_t)(rows > 0 ? rows : 1));
    if (messages == NULL) {
        PQclear(res);
        return NULL;
    }

    for (int r = 0; r < rows; r++) {
        messages[r].is_user = PQgetvalue(res, r, 0)[0] == 't';
        messages[r].text = strdup(PQgetvalue(res, r, 1));
        messages[r].sources_json = PQgetisnull(res, r, 2) ? NULL : strdup(PQgetvalue(res, r, 2));
        if (messages[r].text == NULL || (!PQgetisnull(res, r, 2) && messages[r].sources_json == NULL)) {
            PQclear(res);
            pg_store_chat_messages_free(messages, (size_t)r + 1);
            return NULL;
        }
    }
    PQclear(res);

    *count_out = (size_t)rows;
    return messages;
}

void pg_store_chat_messages_free(PgStoreChatMessage *messages, size_t count) {
    if (messages == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(messages[i].text);
        free(messages[i].sources_json);
    }
    free(messages);
}

int pg_store_update_last_assistant_message(PgStore *store, int64_t session_id, const char *text,
                                            const char *sources_json) {
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)session_id);
    const char *params[3] = {id_str, text, sources_json};
    /* Only the session's newest row is a candidate, and only if it's an assistant
     * row; 0 rows surfaces as -1 below instead of touching an older answer. */
    PGresult *res = PQexecParams(
        store->conn,
        "UPDATE public.chat_messages SET text = $2, sources = $3::jsonb "
        "WHERE id = (SELECT id FROM public.chat_messages WHERE session_id = $1 ORDER BY id DESC LIMIT 1) "
        "  AND is_user = false;",
        3, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "pg_store_update_last_assistant_message: update failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }
    int affected = atoi(PQcmdTuples(res));
    PQclear(res);
    if (affected == 0) {
        fprintf(stderr, "pg_store_update_last_assistant_message: session %lld has no assistant message to update\n",
                (long long)session_id);
        return -1;
    }
    return 0;
}

PgStoreDocumentStats *pg_store_list_document_stats(PgStore *store, size_t *count_out) {
    PGresult *res = PQexec(store->conn,
                           "SELECT document_name, COUNT(*) AS passage_count, "
                           "COALESCE(SUM(token_count), 0) AS total_tokens "
                           "FROM passages GROUP BY document_name ORDER BY document_name;");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_store_list_document_stats: select failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return NULL;
    }

    int rows = PQntuples(res);
    PgStoreDocumentStats *stats = malloc(sizeof(PgStoreDocumentStats) * (size_t)(rows > 0 ? rows : 1));
    if (stats == NULL) {
        PQclear(res);
        return NULL;
    }

    for (int r = 0; r < rows; r++) {
        stats[r].document_name = strdup(PQgetvalue(res, r, 0));
        stats[r].passage_count = atol(PQgetvalue(res, r, 1));
        stats[r].total_tokens = atol(PQgetvalue(res, r, 2));
        if (stats[r].document_name == NULL) {
            PQclear(res);
            pg_store_document_stats_free(stats, (size_t)r + 1);
            return NULL;
        }
    }
    PQclear(res);

    *count_out = (size_t)rows;
    return stats;
}

void pg_store_document_stats_free(PgStoreDocumentStats *stats, size_t count) {
    if (stats == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(stats[i].document_name);
    }
    free(stats);
}

char *pg_store_get_document_text(PgStore *store, const char *document_name) {
    const char *params[1] = {document_name};
    PGresult *res = PQexecParams(store->conn, "SELECT text FROM documents WHERE document_name = $1;", 1, NULL,
                                 params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_store_get_document_text: select failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return NULL;
    }
    if (PQntuples(res) == 0) {
        PQclear(res);
        return NULL;
    }
    char *text = strdup(PQgetvalue(res, 0, 0));
    PQclear(res);
    return text;
}

PgStoreDocumentPassage *pg_store_get_document_passages(PgStore *store, const char *document_name,
                                                       size_t *count_out) {
    const char *params[1] = {document_name};
    PGresult *res = PQexecParams(store->conn,
                                 "SELECT chunk_id, text, token_count FROM passages "
                                 "WHERE document_name = $1 ORDER BY chunk_id;",
                                 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_store_get_document_passages: select failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return NULL;
    }

    int rows = PQntuples(res);
    PgStoreDocumentPassage *passages = malloc(sizeof(PgStoreDocumentPassage) * (size_t)(rows > 0 ? rows : 1));
    if (passages == NULL) {
        PQclear(res);
        return NULL;
    }

    for (int r = 0; r < rows; r++) {
        passages[r].chunk_id = atoi(PQgetvalue(res, r, 0));
        passages[r].text = strdup(PQgetvalue(res, r, 1));
        passages[r].token_count = atoi(PQgetvalue(res, r, 2));
        if (passages[r].text == NULL) {
            PQclear(res);
            pg_store_document_passages_free(passages, (size_t)r + 1);
            return NULL;
        }
    }
    PQclear(res);

    *count_out = (size_t)rows;
    return passages;
}

void pg_store_document_passages_free(PgStoreDocumentPassage *passages, size_t count) {
    if (passages == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(passages[i].text);
    }
    free(passages);
}

int pg_store_remove_document(PgStore *store, const char *document_name) {
    const char *params[1] = {document_name};

    if (pg_store_begin_transaction(store) != 0) {
        return -1;
    }

    /* Postings first (they reference passages). One transaction: any failure
     * rolls the whole removal back (see pg_store.h). */
    PGresult *res = PQexecParams(
        store->conn,
        "DELETE FROM postings WHERE passage_id IN "
        "(SELECT id FROM passages WHERE document_name = $1);",
        1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "pg_store_remove_document: postings delete failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        pg_store_rollback_transaction(store);
        return -1;
    }
    PQclear(res);

    res = PQexecParams(store->conn, "DELETE FROM passages WHERE document_name = $1;", 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "pg_store_remove_document: passages delete failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        pg_store_rollback_transaction(store);
        return -1;
    }
    int passages_deleted = atoi(PQcmdTuples(res));
    PQclear(res);

    res = PQexecParams(store->conn, "DELETE FROM documents WHERE document_name = $1;", 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "pg_store_remove_document: documents delete failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        pg_store_rollback_transaction(store);
        return -1;
    }
    int documents_deleted = atoi(PQcmdTuples(res));
    PQclear(res);

    if (documents_deleted == 0) {
        /* Unknown name: the deletes above were no-ops; roll back and fail. */
        fprintf(stderr, "pg_store_remove_document: no document named '%s'\n", document_name);
        pg_store_rollback_transaction(store);
        return -1;
    }

    /* Sweep orphaned terms (NOT EXISTS anti-join stays correct however many documents
     * shared a term, incl. leftovers from a previous partial state). */
    res = PQexec(store->conn,
                 "DELETE FROM terms WHERE NOT EXISTS "
                 "(SELECT 1 FROM postings WHERE postings.term_id = terms.id);");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "pg_store_remove_document: terms sweep failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        pg_store_rollback_transaction(store);
        return -1;
    }
    PQclear(res);
    (void)passages_deleted;

    if (pg_store_commit_transaction(store) != 0) {
        return -1;
    }
    return 0;
}

/* One summary per group in public (see pg_store.h); PK on corpus_id makes the
 * write an upsert rather than insert-plus-cleanup. */
#define LEXIS_SUMMARY_TABLE_SQL                                              \
    "CREATE TABLE IF NOT EXISTS public.corpus_summaries ("                   \
    "    corpus_id BIGINT PRIMARY KEY REFERENCES public.corpora(id) ON DELETE CASCADE," \
    "    text TEXT NOT NULL,"                                                \
    "    document_count INT NOT NULL,"                                       \
    "    generated_at TIMESTAMPTZ NOT NULL DEFAULT now()"                    \
    ");"

int pg_store_ensure_summary_table(PgStore *store) {
    return exec_simple(store->conn, LEXIS_SUMMARY_TABLE_SQL, "pg_store_ensure_summary_table");
}

char *pg_store_get_corpus_summary(PgStore *store, int64_t corpus_id, int *document_count_out) {
    if (pg_store_ensure_summary_table(store) != 0) {
        return NULL;
    }

    char corpus_id_str[32];
    snprintf(corpus_id_str, sizeof(corpus_id_str), "%lld", (long long)corpus_id);
    const char *params[1] = {corpus_id_str};
    PGresult *res =
        PQexecParams(store->conn, "SELECT text, document_count FROM public.corpus_summaries WHERE corpus_id = $1;", 1,
                      NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_store_get_corpus_summary: select failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return NULL;
    }
    /* No row yet is normal (first question), not an error. */
    if (PQntuples(res) != 1) {
        PQclear(res);
        return NULL;
    }

    char *text = strdup(PQgetvalue(res, 0, 0));
    if (text != NULL && document_count_out != NULL) {
        *document_count_out = atoi(PQgetvalue(res, 0, 1));
    }
    PQclear(res);
    return text;
}

int pg_store_set_corpus_summary(PgStore *store, int64_t corpus_id, const char *text, int document_count) {
    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    if (pg_store_ensure_summary_table(store) != 0) {
        return -1;
    }

    char corpus_id_str[32];
    char document_count_str[32];
    snprintf(corpus_id_str, sizeof(corpus_id_str), "%lld", (long long)corpus_id);
    snprintf(document_count_str, sizeof(document_count_str), "%d", document_count);
    const char *params[3] = {corpus_id_str, text, document_count_str};

    /* Upsert replaces the stale summary in place; no history accumulates. */
    PGresult *res = PQexecParams(store->conn,
                                  "INSERT INTO public.corpus_summaries (corpus_id, text, document_count) "
                                  "VALUES ($1, $2, $3) "
                                  "ON CONFLICT (corpus_id) DO UPDATE SET text = EXCLUDED.text, "
                                  "document_count = EXCLUDED.document_count, generated_at = now();",
                                  3, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "pg_store_set_corpus_summary: upsert failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }
    PQclear(res);
    return 0;
}

int pg_store_create_bare_schema(PgStore *store, const char *schema_name) {
    return create_lexis_tables_in_schema(store->conn, schema_name);
}

int pg_store_drop_bare_schema(PgStore *store, const char *schema_name) {
    char sql[128];
    snprintf(sql, sizeof(sql), "DROP SCHEMA IF EXISTS %s CASCADE;", schema_name);
    return exec_simple(store->conn, sql, "pg_store_drop_bare_schema");
}

int pg_store_swap_corpus_schema(PgStore *store, int64_t corpus_id, const char *new_schema_name) {
    char *old_schema_name = lookup_corpus_schema_name(store->conn, corpus_id);
    if (old_schema_name == NULL) {
        fprintf(stderr, "pg_store_swap_corpus_schema: no corpus with id %lld\n", (long long)corpus_id);
        return -1;
    }

    if (exec_simple(store->conn, "BEGIN;", "pg_store_swap_corpus_schema") != 0) {
        free(old_schema_name);
        return -1;
    }

    /* Both DDL statements in one transaction (clean swap or untouched live data).
     * Both names are trusted/server-generated (see pg_store_use_schema()). */
    char sql[256];
    int written = snprintf(sql, sizeof(sql), "DROP SCHEMA %s CASCADE; ALTER SCHEMA %s RENAME TO %s;", old_schema_name,
                            new_schema_name, old_schema_name);
    if (written < 0 || (size_t)written >= sizeof(sql)) {
        fprintf(stderr, "pg_store_swap_corpus_schema: swap DDL didn't fit the buffer\n");
        free(old_schema_name);
        exec_simple(store->conn, "ROLLBACK;", "pg_store_swap_corpus_schema");
        return -1;
    }
    free(old_schema_name);

    if (exec_simple(store->conn, sql, "pg_store_swap_corpus_schema") != 0) {
        exec_simple(store->conn, "ROLLBACK;", "pg_store_swap_corpus_schema");
        return -1;
    }
    return exec_simple(store->conn, "COMMIT;", "pg_store_swap_corpus_schema");
}

PgStoreDocument *pg_store_get_all_documents(PgStore *store, size_t *count_out) {
    PGresult *res = PQexec(store->conn, "SELECT document_name, text FROM documents ORDER BY document_name;");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_store_get_all_documents: select failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return NULL;
    }

    int rows = PQntuples(res);
    PgStoreDocument *docs = malloc(sizeof(PgStoreDocument) * (size_t)(rows > 0 ? rows : 1));
    if (docs == NULL) {
        PQclear(res);
        return NULL;
    }

    for (int r = 0; r < rows; r++) {
        docs[r].document_name = strdup(PQgetvalue(res, r, 0));
        docs[r].text = strdup(PQgetvalue(res, r, 1));
        if (docs[r].document_name == NULL || docs[r].text == NULL) {
            PQclear(res);
            pg_store_documents_free(docs, (size_t)r + 1);
            return NULL;
        }
    }
    PQclear(res);

    *count_out = (size_t)rows;
    return docs;
}

void pg_store_documents_free(PgStoreDocument *docs, size_t count) {
    if (docs == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(docs[i].document_name);
        free(docs[i].text);
    }
    free(docs);
}

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

int pg_store_insert_document(PgStore *store, const char *document_name, const char *text) {
    const char *params[2] = {document_name, text};
    /* DO NOTHING: Phase 2 batch retries can legitimately re-insert the same
     * document_name with identical content (see BULK_PHASE2_BATCH_RETRIES). */
    static const char *sql = "INSERT INTO documents (document_name, text) VALUES ($1, $2) "
                              "ON CONFLICT (document_name) DO NOTHING;";

    PGresult *res = PQexecParams(store->conn, sql, 2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "pg_store_insert_document: insert failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }
    PQclear(res);
    return 0;
}

int64_t pg_store_get_or_create_term(PgStore *store, const char *term) {
    /* Fast path: plain read for the common already-seen term, no write lock. */
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

    /* DO NOTHING, not DO UPDATE: the no-op self-assignment still took a row lock and
     * deadlocked under concurrent writers (verified; see dev/SPEED.md). Costs one re-SELECT. */
    const char *params[1] = {term};
    static const char *insert_sql =
        "INSERT INTO terms (term) VALUES ($1) ON CONFLICT (term) DO NOTHING;";
    PGresult *res = PQexecParams(store->conn, insert_sql, 1, NULL, params, NULL, NULL, 0);
    int insert_ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    if (!insert_ok) {
        fprintf(stderr, "pg_store_get_or_create_term: insert failed: %s\n", PQerrorMessage(store->conn));
        return -1;
    }

    /* Either we inserted it or lost the race; either way the row exists now. */
    static const char *reselect_sql = "SELECT id FROM terms WHERE term = $1;";
    res = PQexecParams(store->conn, reselect_sql, 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        fprintf(stderr, "pg_store_get_or_create_term: post-insert select failed: %s\n",
                PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }

    int64_t term_id = atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    return term_id;
}

/* Builds a quoted/escaped {"a","b\"c"} array literal for unnest(). Caller frees;
 * NULL on allocation failure. */
static char *build_text_array_literal(const char *const *items, size_t count) {
    size_t capacity = 3;
    for (size_t i = 0; i < count; i++) {
        /* Worst case: every byte escaped (2x) + quotes + comma. */
        capacity += strlen(items[i]) * 2 + 4;
    }
    char *buffer = malloc(capacity);
    if (buffer == NULL) {
        return NULL;
    }

    char *w = buffer;
    *w++ = '{';
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            *w++ = ',';
        }
        *w++ = '"';
        for (const char *p = items[i]; *p != '\0'; p++) {
            if (*p == '"' || *p == '\\') {
                *w++ = '\\';
            }
            *w++ = *p;
        }
        *w++ = '"';
    }
    *w++ = '}';
    *w = '\0';
    return buffer;
}

/* Same idea as build_text_array_literal(), for plain int64 arrays --
 * numeric formatting needs no escaping, just digits/sign/commas. */
static char *build_int64_array_literal(const int64_t *items, size_t count) {
    size_t capacity = count * 22 + 3; /* int64 max ~20 digits + sign + comma */
    char *buffer = malloc(capacity);
    if (buffer == NULL) {
        return NULL;
    }
    char *w = buffer;
    size_t remaining = capacity;
    *w++ = '{';
    remaining--;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            *w++ = ',';
            remaining--;
        }
        int written = snprintf(w, remaining, "%lld", (long long)items[i]);
        w += written;
        remaining -= (size_t)written;
    }
    *w++ = '}';
    *w = '\0';
    return buffer;
}

/* Same idea, for plain int arrays (term frequencies). */
static char *build_int_array_literal(const int *items, size_t count) {
    size_t capacity = count * 13 + 3; /* int max ~11 digits + sign + comma */
    char *buffer = malloc(capacity);
    if (buffer == NULL) {
        return NULL;
    }
    char *w = buffer;
    size_t remaining = capacity;
    *w++ = '{';
    remaining--;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            *w++ = ',';
            remaining--;
        }
        int written = snprintf(w, remaining, "%d", items[i]);
        w += written;
        remaining -= (size_t)written;
    }
    *w++ = '}';
    *w = '\0';
    return buffer;
}

/* Fills ids[i] for terms matching res rows (id, term). O(rows*count) is fine
 * at chunk scale (dozens of terms). */
static void fill_ids_from_result(PGresult *res, const char *const *terms, size_t count, int64_t *ids) {
    int rows = PQntuples(res);
    for (int r = 0; r < rows; r++) {
        int64_t id = atoll(PQgetvalue(res, r, 0));
        const char *term = PQgetvalue(res, r, 1);
        for (size_t i = 0; i < count; i++) {
            if (ids[i] == -1 && strcmp(terms[i], term) == 0) {
                ids[i] = id;
            }
        }
    }
}

/* Collects still-unresolved distinct terms (borrows pointers; caller frees the array).
 * NULL with *out_count == 0 means "all resolved" or allocation failure. */
static const char **collect_unresolved(const char *const *terms, size_t count, const int64_t *ids,
                                        size_t *out_count) {
    const char **unresolved = malloc(sizeof(char *) * count);
    if (unresolved == NULL) {
        *out_count = 0;
        return NULL;
    }
    size_t unresolved_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (ids[i] != -1) {
            continue;
        }
        int already_queued = 0;
        for (size_t j = 0; j < unresolved_count; j++) {
            if (strcmp(unresolved[j], terms[i]) == 0) {
                already_queued = 1;
                break;
            }
        }
        if (!already_queued) {
            unresolved[unresolved_count++] = terms[i];
        }
    }
    *out_count = unresolved_count;
    return unresolved;
}

int64_t *pg_store_get_or_create_terms(PgStore *store, const char *const *terms, size_t count) {
    int64_t *ids = malloc(sizeof(int64_t) * count);
    if (ids == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        ids[i] = -1; /* sentinel: "not yet resolved" */
    }

    char *all_literal = build_text_array_literal(terms, count);
    if (all_literal == NULL) {
        free(ids);
        return NULL;
    }

    /* Phase 1: one bulk SELECT for every term that already exists. */
    {
        const char *params[1] = {all_literal};
        PGresult *res = PQexecParams(store->conn, "SELECT id, term FROM terms WHERE term = ANY($1::text[]);",
                                      1, NULL, params, NULL, NULL, 0);
        free(all_literal);
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            fprintf(stderr, "pg_store_get_or_create_terms: select failed: %s\n", PQerrorMessage(store->conn));
            PQclear(res);
            free(ids);
            return NULL;
        }
        fill_ids_from_result(res, terms, count, ids);
        PQclear(res);
    }

    /* Phase 2: one bulk INSERT ... ON CONFLICT DO NOTHING for the rest. DO NOTHING
     * avoids concurrent speculative-insertion deadlocks (see get_or_create_term). */
    {
        size_t missing_count = 0;
        const char **missing = collect_unresolved(terms, count, ids, &missing_count);
        if (missing == NULL && missing_count == 0) {
            /* NULL is ambiguous ("nothing missing" vs alloc failure); fatal only if
             * phase 1 didn't actually resolve everything. */
            for (size_t i = 0; i < count; i++) {
                if (ids[i] == -1) {
                    free(ids);
                    return NULL;
                }
            }
        }
        if (missing_count > 0) {
            char *missing_literal = build_text_array_literal(missing, missing_count);
            free(missing);
            if (missing_literal == NULL) {
                free(ids);
                return NULL;
            }
            const char *params[1] = {missing_literal};
            PGresult *res = PQexecParams(
                store->conn,
                "INSERT INTO terms (term) SELECT unnest($1::text[]) "
                "ON CONFLICT (term) DO NOTHING RETURNING id, term;",
                1, NULL, params, NULL, NULL, 0);
            free(missing_literal);
            if (PQresultStatus(res) != PGRES_TUPLES_OK) {
                fprintf(stderr, "pg_store_get_or_create_terms: insert failed: %s\n",
                        PQerrorMessage(store->conn));
                PQclear(res);
                free(ids);
                return NULL;
            }
            fill_ids_from_result(res, terms, count, ids);
            PQclear(res);
        } else {
            free(missing);
        }
    }

    /* Phase 3: re-SELECT stragglers that lost the insert race to a concurrent
     * writer (rare in practice). */
    {
        size_t straggler_count = 0;
        const char **stragglers = collect_unresolved(terms, count, ids, &straggler_count);
        if (straggler_count > 0) {
            char *straggler_literal = build_text_array_literal(stragglers, straggler_count);
            free(stragglers);
            if (straggler_literal == NULL) {
                free(ids);
                return NULL;
            }
            const char *params[1] = {straggler_literal};
            PGresult *res = PQexecParams(store->conn,
                                          "SELECT id, term FROM terms WHERE term = ANY($1::text[]);", 1,
                                          NULL, params, NULL, NULL, 0);
            free(straggler_literal);
            if (PQresultStatus(res) != PGRES_TUPLES_OK) {
                fprintf(stderr, "pg_store_get_or_create_terms: re-select failed: %s\n",
                        PQerrorMessage(store->conn));
                PQclear(res);
                free(ids);
                return NULL;
            }
            fill_ids_from_result(res, terms, count, ids);
            PQclear(res);
        } else {
            free(stragglers);
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (ids[i] == -1) {
            /* Shouldn't happen (nothing deletes terms concurrently); fail loudly. */
            fprintf(stderr, "pg_store_get_or_create_terms: term \"%s\" unresolved after all phases\n",
                    terms[i]);
            free(ids);
            return NULL;
        }
    }

    return ids;
}

int pg_store_insert_posting(PgStore *store, int64_t term_id, int64_t passage_id, int term_frequency,
                             int token_count) {
    char term_id_str[32];
    char passage_id_str[32];
    char frequency_str[32];
    char token_count_str[32];
    snprintf(term_id_str, sizeof(term_id_str), "%lld", (long long)term_id);
    snprintf(passage_id_str, sizeof(passage_id_str), "%lld", (long long)passage_id);
    snprintf(frequency_str, sizeof(frequency_str), "%d", term_frequency);
    snprintf(token_count_str, sizeof(token_count_str), "%d", token_count);

    const char *params[4] = {term_id_str, passage_id_str, frequency_str, token_count_str};
    static const char *sql = "INSERT INTO postings (term_id, passage_id, term_frequency, token_count) "
                              "VALUES ($1, $2, $3, $4);";

    PGresult *res = PQexecParams(store->conn, sql, 4, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        fprintf(stderr, "pg_store_insert_posting: insert failed: %s\n", PQerrorMessage(store->conn));
    }
    PQclear(res);
    return ok ? 0 : -1;
}

int pg_store_insert_postings(PgStore *store, const int64_t *term_ids, int64_t passage_id,
                              const int *term_frequencies, int token_count, size_t count) {
    char *term_ids_literal = build_int64_array_literal(term_ids, count);
    char *freqs_literal = build_int_array_literal(term_frequencies, count);
    char passage_id_str[32];
    char token_count_str[32];
    snprintf(passage_id_str, sizeof(passage_id_str), "%lld", (long long)passage_id);
    snprintf(token_count_str, sizeof(token_count_str), "%d", token_count);
    if (term_ids_literal == NULL || freqs_literal == NULL) {
        free(term_ids_literal);
        free(freqs_literal);
        return -1;
    }

    /* Same-target-list unnest()s zip lock-step, not cross product (verified). $2/$4 are
     * bound once and reused: all postings here share passage_id/token_count. */
    const char *params[4] = {term_ids_literal, passage_id_str, freqs_literal, token_count_str};
    static const char *sql =
        "INSERT INTO postings (term_id, passage_id, term_frequency, token_count) "
        "SELECT unnest($1::bigint[]), $2::bigint, unnest($3::int[]), $4::int;";

    PGresult *res = PQexecParams(store->conn, sql, 4, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        fprintf(stderr, "pg_store_insert_postings: insert failed: %s\n", PQerrorMessage(store->conn));
    }
    PQclear(res);
    free(term_ids_literal);
    free(freqs_literal);
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

char **pg_store_get_document_names(PgStore *store, const int64_t *passage_ids, size_t count) {
    char **names = malloc(sizeof(char *) * count);
    if (names == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        names[i] = NULL;
    }

    char *ids_literal = build_int64_array_literal(passage_ids, count);
    if (ids_literal == NULL) {
        free(names);
        return NULL;
    }

    const char *params[1] = {ids_literal};
    static const char *sql = "SELECT id, document_name FROM passages WHERE id = ANY($1::bigint[]);";
    PGresult *res = PQexecParams(store->conn, sql, 1, NULL, params, NULL, NULL, 0);
    free(ids_literal);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_store_get_document_names: select failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        free(names);
        return NULL;
    }

    int rows = PQntuples(res);
    for (int r = 0; r < rows; r++) {
        int64_t id = atoll(PQgetvalue(res, r, 0));
        const char *document_name = PQgetvalue(res, r, 1);
        for (size_t i = 0; i < count; i++) {
            if (passage_ids[i] == id && names[i] == NULL) {
                names[i] = strdup(document_name);
            }
        }
    }
    PQclear(res);

    return names;
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

int pg_store_disable_synchronous_commit(PgStore *store) {
    return exec_simple(store->conn, "SET synchronous_commit = off;", "pg_store_disable_synchronous_commit");
}

/* UNLOGGED/unconstrained staging (see pg_store.h). row_num is the ordering key Phase 2
 * workers range-partition on (COPY preserves no scan order; pids aren't contiguous). */
#define LEXIS_STAGING_SCHEMA_SQL                                            \
    "CREATE UNLOGGED TABLE IF NOT EXISTS documents_raw ("                   \
    "    row_num BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"          \
    "    pid TEXT NOT NULL,"                                                \
    "    text TEXT NOT NULL"                                                \
    ");"                                                                    \
    "CREATE UNLOGGED TABLE IF NOT EXISTS postings_staged ("                 \
    "    passage_id BIGINT NOT NULL,"                                       \
    "    term TEXT NOT NULL,"                                               \
    "    term_frequency INTEGER NOT NULL,"                                  \
    "    token_count INTEGER NOT NULL"                                      \
    ");"

int pg_store_create_staging_tables(PgStore *store) {
    return exec_simple(store->conn, LEXIS_STAGING_SCHEMA_SQL, "pg_store_create_staging_tables");
}

int pg_store_truncate_staging_tables(PgStore *store) {
    return exec_simple(store->conn,
                        "TRUNCATE documents_raw RESTART IDENTITY; TRUNCATE postings_staged;",
                        "pg_store_truncate_staging_tables");
}

int pg_store_drop_staging_tables(PgStore *store) {
    return exec_simple(store->conn, "DROP TABLE IF EXISTS documents_raw; DROP TABLE IF EXISTS postings_staged;",
                        "pg_store_drop_staging_tables");
}

int64_t pg_store_copy_documents_raw(PgStore *store, const char *tsv_path) {
    FILE *fp = fopen(tsv_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "pg_store_copy_documents_raw: could not open %s\n", tsv_path);
        return -1;
    }

    PGresult *begin_res = PQexec(
        store->conn, "COPY documents_raw (pid, text) FROM STDIN (FORMAT csv, DELIMITER E'\\t');");
    if (PQresultStatus(begin_res) != PGRES_COPY_IN) {
        fprintf(stderr, "pg_store_copy_documents_raw: COPY did not start: %s\n",
                PQerrorMessage(store->conn));
        PQclear(begin_res);
        fclose(fp);
        return -1;
    }
    PQclear(begin_res);

    /* The file is already in COPY's exact wire format: stream it through unparsed.
     * It must be CSV-quoted, not plain TSV (see dev/SPEED.md). */
    char buffer[65536];
    size_t bytes_read;
    int read_failed = 0;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (PQputCopyData(store->conn, buffer, (int)bytes_read) != 1) {
            fprintf(stderr, "pg_store_copy_documents_raw: PQputCopyData failed: %s\n",
                    PQerrorMessage(store->conn));
            read_failed = 1;
            break;
        }
    }
    if (!read_failed && ferror(fp)) {
        fprintf(stderr, "pg_store_copy_documents_raw: error reading %s\n", tsv_path);
        read_failed = 1;
    }
    fclose(fp);

    if (PQputCopyEnd(store->conn, read_failed ? "client-side read failure" : NULL) != 1) {
        fprintf(stderr, "pg_store_copy_documents_raw: PQputCopyEnd failed: %s\n",
                PQerrorMessage(store->conn));
        return -1;
    }

    PGresult *end_res = PQgetResult(store->conn);
    int ok = !read_failed && PQresultStatus(end_res) == PGRES_COMMAND_OK;
    int64_t rows_loaded = ok ? strtoll(PQcmdTuples(end_res), NULL, 10) : -1;
    if (!ok) {
        fprintf(stderr, "pg_store_copy_documents_raw: COPY failed: %s\n", PQerrorMessage(store->conn));
    }
    PQclear(end_res);

    /* Drain PQgetResult to NULL: COPY queues one more empty result after the status
     * one, and the connection can't run another command until drained. */
    PGresult *drain;
    while ((drain = PQgetResult(store->conn)) != NULL) {
        PQclear(drain);
    }

    return rows_loaded;
}

PgStoreRawDocument *pg_store_get_raw_documents_range(PgStore *store, int64_t start_row, int64_t end_row,
                                                      size_t *count_out) {
    char start_str[32];
    char end_str[32];
    snprintf(start_str, sizeof(start_str), "%lld", (long long)start_row);
    snprintf(end_str, sizeof(end_str), "%lld", (long long)end_row);

    const char *params[2] = {start_str, end_str};
    static const char *sql = "SELECT row_num, pid, text FROM documents_raw "
                              "WHERE row_num >= $1::bigint AND row_num < $2::bigint ORDER BY row_num;";

    PGresult *res = PQexecParams(store->conn, sql, 2, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "pg_store_get_raw_documents_range: select failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return NULL;
    }

    int rows = PQntuples(res);
    PgStoreRawDocument *docs = malloc(sizeof(PgStoreRawDocument) * (size_t)(rows > 0 ? rows : 1));
    if (docs == NULL) {
        PQclear(res);
        return NULL;
    }

    for (int r = 0; r < rows; r++) {
        docs[r].row_num = atoll(PQgetvalue(res, r, 0));
        docs[r].pid = strdup(PQgetvalue(res, r, 1));
        docs[r].text = strdup(PQgetvalue(res, r, 2));
        if (docs[r].pid == NULL || docs[r].text == NULL) {
            PQclear(res);
            pg_store_raw_documents_free(docs, (size_t)r + 1);
            return NULL;
        }
    }
    PQclear(res);

    *count_out = (size_t)rows;
    return docs;
}

void pg_store_raw_documents_free(PgStoreRawDocument *docs, size_t count) {
    if (docs == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(docs[i].pid);
        free(docs[i].text);
    }
    free(docs);
}

int pg_store_insert_staged_postings(PgStore *store, int64_t passage_id, const char *const *terms,
                                     const int *term_frequencies, int token_count, size_t count) {
    char *terms_literal = build_text_array_literal(terms, count);
    char *freqs_literal = build_int_array_literal(term_frequencies, count);
    char passage_id_str[32];
    char token_count_str[32];
    snprintf(passage_id_str, sizeof(passage_id_str), "%lld", (long long)passage_id);
    snprintf(token_count_str, sizeof(token_count_str), "%d", token_count);
    if (terms_literal == NULL || freqs_literal == NULL) {
        free(terms_literal);
        free(freqs_literal);
        return -1;
    }

    /* Lock-step unnest() zip as in pg_store_insert_postings(). */
    const char *params[4] = {terms_literal, passage_id_str, freqs_literal, token_count_str};
    static const char *sql =
        "INSERT INTO postings_staged (passage_id, term, term_frequency, token_count) "
        "SELECT $2::bigint, unnest($1::text[]), unnest($3::int[]), $4::int;";

    PGresult *res = PQexecParams(store->conn, sql, 4, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        fprintf(stderr, "pg_store_insert_staged_postings: insert failed: %s\n", PQerrorMessage(store->conn));
    }
    PQclear(res);
    free(terms_literal);
    free(freqs_literal);
    return ok ? 0 : -1;
}

long pg_store_finalize_terms_and_postings(PgStore *store) {
    /* Session-local work_mem bump: only this connection runs the DISTINCT + hash JOIN
     * over hundreds of millions of staged rows. */
    if (exec_simple(store->conn, "SET work_mem = '1GB';", "pg_store_finalize_terms_and_postings") != 0) {
        return -1;
    }

    if (exec_simple(store->conn, "INSERT INTO terms (term) SELECT DISTINCT term FROM postings_staged "
                                  "ON CONFLICT (term) DO NOTHING;",
                     "pg_store_finalize_terms_and_postings") != 0) {
        return -1;
    }

    PGresult *res = PQexec(store->conn,
                            "INSERT INTO postings (term_id, passage_id, term_frequency, token_count) "
                            "SELECT t.id, s.passage_id, s.term_frequency, s.token_count "
                            "FROM postings_staged s JOIN terms t ON t.term = s.term;");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "pg_store_finalize_terms_and_postings: postings insert failed: %s\n",
                PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }
    long postings_written = strtol(PQcmdTuples(res), NULL, 10);
    PQclear(res);

    return postings_written;
}

int pg_store_prepare_bulk_load(PgStore *store) {
    /* Drop constraints before UNLOGGED: Postgres refuses to weaken a table while a
     * still-LOGGED table's FK references it. (passages deliberately left out.) */
    static const char *sql =
        "ALTER TABLE postings DROP CONSTRAINT IF EXISTS postings_pkey;"
        "ALTER TABLE postings DROP CONSTRAINT IF EXISTS postings_term_id_fkey;"
        "ALTER TABLE postings DROP CONSTRAINT IF EXISTS postings_passage_id_fkey;"
        "ALTER TABLE postings SET UNLOGGED;"
        "ALTER TABLE terms SET UNLOGGED;";
    return exec_simple(store->conn, sql, "pg_store_prepare_bulk_load");
}

int pg_store_finish_bulk_load(PgStore *store) {
    /* Rebuild constraints while still UNLOGGED, then SET LOGGED (terms before postings:
     * postings' FK can't point at a still-UNLOGGED terms). See the ordering test. */
    static const char *sql =
        "ALTER TABLE postings ADD PRIMARY KEY (term_id, passage_id);"
        "ALTER TABLE postings ADD CONSTRAINT postings_term_id_fkey "
        "    FOREIGN KEY (term_id) REFERENCES terms(id);"
        "ALTER TABLE postings ADD CONSTRAINT postings_passage_id_fkey "
        "    FOREIGN KEY (passage_id) REFERENCES passages(id);"
        "ALTER TABLE terms SET LOGGED;"
        "ALTER TABLE postings SET LOGGED;";
    return exec_simple(store->conn, sql, "pg_store_finish_bulk_load");
}
