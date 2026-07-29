/*
 * Index and passage persistence via libpq/PostgreSQL -- the
 * experiment/postgres-migration branch's replacement for sqlite_store.c
 * (see LIMITATIONS.md and that module's own history for why: SQLite
 * allows exactly one writer per connection, which caps concurrent
 * ingestion throughput at roughly 1.3x regardless of thread count,
 * measured on the experiment/sharded-ingestion branch. Postgres allows
 * genuinely concurrent writer connections instead).
 *
 * Same schema shape as the SQLite version (passages/terms/postings), with
 * two real differences: ids are `int64_t`, not a SQLite-specific typedef,
 * and there's no equivalent of sqlite3_last_insert_rowid() -- every insert
 * here uses `RETURNING id` instead. Also: pg_store_get_or_create_term()
 * uses a single atomic `INSERT ... ON CONFLICT ... RETURNING id`, fixing
 * the SELECT-then-INSERT race the SQLite version had to leave undocumented
 * as a known gap (see LIMITATIONS.md) -- Postgres makes the correct,
 * atomic version easy.
 */

#ifndef LEXIS_PG_STORE_H
#define LEXIS_PG_STORE_H

#include <libpq-fe.h>
#include <stdint.h>

/* Wraps a single open connection to the Postgres database. */
typedef struct {
    PGconn *conn;
} PgStore;

/* Opens a connection using `conninfo` (a libpq connection string, e.g.
 * "host=127.0.0.1 port=5433 dbname=lexis user=lexis password=...") and
 * ensures the passages/terms/postings tables exist. Returns NULL on
 * connection failure or if schema creation fails. */
PgStore *pg_store_open(const char *conninfo);

/* Closes the connection and frees the PgStore. Safe to call with
 * store == NULL. */
void pg_store_close(PgStore *store);

/* Inserts a passage (a chunk of a source document) and returns its new
 * row id, or -1 on failure. */
int64_t pg_store_insert_passage(PgStore *store, const char *document_name, int chunk_id,
                                 const char *text, int token_count);

/* Returns the id of `term` in the terms table, inserting it first if this
 * is the first time it's been seen -- atomically (INSERT ... ON CONFLICT
 * ... RETURNING id), safe under concurrent writers. Returns -1 on
 * failure. */
int64_t pg_store_get_or_create_term(PgStore *store, const char *term);

/* Read-only counterpart to pg_store_get_or_create_term(): looks up `term`
 * without ever inserting it. Returns its id if seen before, or -1 if
 * never indexed (or on a real database error -- either way, the caller's
 * correct response is the same: this term contributes nothing). */
int64_t pg_store_lookup_term(PgStore *store, const char *term);

/* Records that `term_id` occurs `term_frequency` times in `passage_id`.
 * Returns 0 on success, -1 on failure. */
int pg_store_insert_posting(PgStore *store, int64_t term_id, int64_t passage_id, int term_frequency);

/* One passage's stored data, read back from the database. `document_name`
 * and `text` are owned copies -- free via pg_store_passage_free(). */
typedef struct {
    char *document_name;
    int chunk_id;
    char *text;
    int token_count;
} PgStorePassage;

/* Reads back the passage stored at `passage_id`. Returns NULL if no such
 * passage exists or on a database/allocation error. */
PgStorePassage *pg_store_get_passage(PgStore *store, int64_t passage_id);

/* Frees a passage's owned strings and the struct itself. Safe to call
 * with passage == NULL. */
void pg_store_passage_free(PgStorePassage *passage);

/* Explicit transaction control -- see sqlite_store.h's original rationale
 * (batching a whole document's writes into one commit); the mechanism is
 * different here (Postgres MVCC vs SQLite's rollback journal) but the
 * calling convention is identical. Each returns 0 on success, -1 on
 * failure. */
int pg_store_begin_transaction(PgStore *store);
int pg_store_commit_transaction(PgStore *store);
int pg_store_rollback_transaction(PgStore *store);

#endif /* LEXIS_PG_STORE_H */
