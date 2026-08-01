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
 * open, not just the first.
 *
 * postings.token_count is a deliberate denormalization of
 * passages.token_count -- BM25 needs each matching passage's own length
 * for its length-normalization term (see bm25_term_score()), and at real
 * MS MARCO scale (measured directly, see LIMITATIONS.md) fetching it via
 * a JOIN against passages meant one random-access index lookup *per
 * matching posting row* -- 11-14+ seconds for a term with 100K+ matches,
 * worse for genuinely common words. Storing a copy directly on postings
 * turns that into a single index-only scan with no join at all, at the
 * cost of repeating a 4-byte int across every posting for a given
 * passage (acceptable; see LIMITATIONS.md's existing postings-storage
 * tradeoff discussion). */
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
    "    token_count INTEGER NOT NULL,"                                  \
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

    /* Not found on the fast-path read -- try to insert. DO NOTHING, not
     * DO UPDATE: an earlier version used
     * "ON CONFLICT (term) DO UPDATE SET term = EXCLUDED.term RETURNING id"
     * specifically so RETURNING would yield a row on conflict too -- but
     * DO UPDATE takes a row lock even for that no-op self-assignment, and
     * under real concurrent writers racing on overlapping term sets that
     * caused genuine Postgres deadlocks -- "deadlock detected ... while
     * inserting index tuple ... in relation terms" -- silently dropping
     * whole documents (verified directly, see SPEED.md). DO NOTHING
     * skips the row lock entirely, at the cost of one extra round trip
     * below to fetch the id when we lost the race. */
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

    /* Either we just inserted it, or another connection won the race and
     * DO NOTHING silently no-opped -- either way the row now exists. */
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

/* Builds a Postgres array literal like {"a","b\"c","d,e"} from `items`,
 * double-quoting and backslash-escaping each element so any character --
 * including a literal comma, quote, or backslash -- round-trips correctly
 * through unnest(). Verified directly against the real server (comma,
 * apostrophe, embedded quote, backslash all confirmed). Caller must
 * free() the result. Returns NULL on allocation failure. */
static char *build_text_array_literal(const char *const *items, size_t count) {
    size_t capacity = 3;
    for (size_t i = 0; i < count; i++) {
        /* Worst case every byte needs a backslash (2x), plus the two
         * quote chars and a comma separator. */
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

/* Fills `ids[i]` (still -1) for every terms[i] matching a row in
 * `res` (columns: id, term) -- shared by every phase of
 * pg_store_get_or_create_terms() below. O(rows * count), fine at
 * chunk scale (a few dozen terms), same tradeoff already made for
 * ingest.c's ingest_count_distinct_terms()'s dedup loop. */
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

/* Collects the still-unresolved (ids[i] == -1) distinct terms from
 * `terms` into a freshly allocated array (caller must free() the array
 * itself, not its contents -- it borrows the original term pointers).
 * *out_count is set to how many. Returns NULL (with *out_count == 0) if
 * every term is already resolved, or on allocation failure (check
 * `count > 0 && result == NULL` to distinguish, mirroring the two ways
 * "nothing to do" can happen). */
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

    /* Phase 2: one bulk INSERT ... ON CONFLICT DO NOTHING for whatever's
     * still unresolved (genuinely new terms). DO NOTHING, not DO UPDATE --
     * see pg_store_get_or_create_term() for why (concurrent speculative-
     * insertion deadlocks, verified directly). */
    {
        size_t missing_count = 0;
        const char **missing = collect_unresolved(terms, count, ids, &missing_count);
        if (missing == NULL && missing_count == 0) {
            /* Could be "nothing missing" (fine) or an allocation failure
             * (not fine) -- collect_unresolved() can't distinguish these
             * from its return alone when every term was already resolved
             * in phase 1, so only treat it as fatal if phase 1 didn't
             * actually resolve everything. */
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

    /* Phase 3: anything still unresolved lost the insert race to a
     * concurrent writer (DO NOTHING silently no-opped for it) -- one more
     * bulk re-SELECT picks up whatever's left. Rare in practice. */
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
            /* Shouldn't happen -- nothing in this codebase deletes terms
             * concurrently. Fail loudly rather than hand back a bogus id. */
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

    /* unnest() on two arrays in the same target list runs in lock-step
     * (zipped element-wise), not a cross product -- verified directly
     * against the real server before relying on it here. $2/$4 are each
     * bound once and reused for every row, not unnested themselves --
     * every posting from this one call shares the same passage_id and
     * therefore the same token_count. */
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

/* See pg_store.h's staging-tables comment for why these are UNLOGGED and
 * unconstrained. row_num is documents_raw's own ordering key (COPY does
 * not preserve any particular scan order for later readers, so Phase 2's
 * workers need something to range-partition on that isn't `pid` --
 * MS MARCO pids are arbitrary strings, not a dense/contiguous range). */
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

    /* The CSV file is already in the exact wire format COPY expects --
     * streamed straight through in fixed-size chunks, no parsing on this
     * side. See SPEED.md for why the file must actually be CSV-quoted
     * (not plain TSV) for this to be safe. */
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

    /* PQgetResult must be drained to NULL before this connection can run
     * another command -- COPY's protocol leaves one more (empty) result
     * queued after the command-status one. */
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

    /* Same lock-step unnest() zip as pg_store_insert_postings() -- see
     * that function's comment for why $2/$4 are bound once and reused,
     * not unnested themselves. */
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
    /* Session-local, not a global postgresql.conf change -- this
     * connection is about to run a DISTINCT and a hash JOIN over
     * hundreds of millions of staged rows, work no other connection in
     * this process does, so there's no reason to pay for a larger
     * work_mem anywhere else. */
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
