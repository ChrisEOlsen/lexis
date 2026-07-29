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
 * closes the SELECT-then-INSERT race the SQLite version had to leave
 * undocumented as a known gap (see LIMITATIONS.md) via
 * `INSERT ... ON CONFLICT (term) DO NOTHING` plus a re-SELECT to fetch the
 * id either way -- deliberately not `DO UPDATE ... RETURNING id` (which
 * would avoid the extra round trip), since DO UPDATE takes a row lock even
 * for a no-op self-assignment, and that caused real Postgres deadlocks
 * under genuine concurrent writers (see concurrent_ingest.c). */

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
 * is the first time it's been seen (INSERT ... ON CONFLICT DO NOTHING,
 * plus a re-SELECT to fetch the id either way it landed -- see pg_store.c
 * for why not a single RETURNING call), safe under concurrent writers.
 * Returns -1 on failure. One round trip in the common (already-seen term)
 * case, up to three otherwise -- pg_store_get_or_create_terms() below is
 * the batch version, worth using instead when indexing more than one term
 * at a time (see LIMITATIONS.md on why per-term round trips dominated
 * ingestion latency here in a way they never did with SQLite). */
int64_t pg_store_get_or_create_term(PgStore *store, const char *term);

/* Batch version of pg_store_get_or_create_term(): resolves every term in
 * `terms[0..count)` (which may contain duplicates) to its id, returning a
 * newly allocated array of `count` ids in the caller-must-free()'d same
 * order as `terms` (result[i] corresponds to terms[i]). At most 3 round
 * trips total *regardless of count* -- one bulk SELECT for terms that
 * already exist, one bulk INSERT ... ON CONFLICT DO NOTHING for genuinely
 * new ones, one bulk re-SELECT for any a concurrent writer won the race
 * on -- versus 1-3 round trips *per term* calling
 * pg_store_get_or_create_term() in a loop would cost. Requires count >= 1.
 * Returns NULL on failure. */
int64_t *pg_store_get_or_create_terms(PgStore *store, const char *const *terms, size_t count);

/* Read-only counterpart to pg_store_get_or_create_term(): looks up `term`
 * without ever inserting it. Returns its id if seen before, or -1 if
 * never indexed (or on a real database error -- either way, the caller's
 * correct response is the same: this term contributes nothing). */
int64_t pg_store_lookup_term(PgStore *store, const char *term);

/* Records that `term_id` occurs `term_frequency` times in `passage_id`.
 * Returns 0 on success, -1 on failure. */
int pg_store_insert_posting(PgStore *store, int64_t term_id, int64_t passage_id, int term_frequency);

/* Batch version of pg_store_insert_posting(): records `count` postings in
 * one round trip, all against the same `passage_id` -- term_ids[i] occurs
 * term_frequencies[i] times (parallel arrays, length `count`). Requires
 * count >= 1. Returns 0 on success, -1 on failure. */
int pg_store_insert_postings(PgStore *store, const int64_t *term_ids, int64_t passage_id,
                              const int *term_frequencies, size_t count);

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
