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

/* Records that `term_id` occurs `term_frequency` times in `passage_id`,
 * which is itself `token_count` tokens long. `token_count` is a
 * deliberate denormalization of passages.token_count -- see pg_store.c's
 * schema comment for why (BM25's length-normalization term needs each
 * matching passage's own length, and fetching it via a JOIN against
 * passages meant one random-access lookup per matching posting row --
 * measured directly at real MS MARCO scale, see LIMITATIONS.md). Returns
 * 0 on success, -1 on failure. */
int pg_store_insert_posting(PgStore *store, int64_t term_id, int64_t passage_id, int term_frequency,
                             int token_count);

/* Batch version of pg_store_insert_posting(): records `count` postings in
 * one round trip, all against the same `passage_id` (and therefore the
 * same `token_count`) -- term_ids[i] occurs term_frequencies[i] times
 * (parallel arrays, length `count`). Requires count >= 1. Returns 0 on
 * success, -1 on failure. */
int pg_store_insert_postings(PgStore *store, const int64_t *term_ids, int64_t passage_id,
                              const int *term_frequencies, int token_count, size_t count);

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

/* Batch counterpart to pg_store_get_passage() that returns only each
 * passage's document_name (not chunk_id/text/token_count) -- for callers
 * like the eval harness that need to map many result passage_ids back to
 * their source document_name (e.g. an MS MARCO pid) without paying for
 * the full passage payload over the wire, and without one round trip per
 * id. Returns a newly allocated array of `count` strings in the same
 * order as `passage_ids` (result[i] corresponds to passage_ids[i]);
 * caller must free() each non-NULL entry and then the array itself. NULL
 * at index i means passage_ids[i] doesn't exist. One round trip
 * regardless of count. Requires count >= 1. Returns NULL (the whole
 * array, nothing to free) on a database or allocation failure. */
char **pg_store_get_document_names(PgStore *store, const int64_t *passage_ids, size_t count);

/* Explicit transaction control -- see sqlite_store.h's original rationale
 * (batching a whole document's writes into one commit); the mechanism is
 * different here (Postgres MVCC vs SQLite's rollback journal) but the
 * calling convention is identical. Each returns 0 on success, -1 on
 * failure. */
int pg_store_begin_transaction(PgStore *store);
int pg_store_commit_transaction(PgStore *store);
int pg_store_rollback_transaction(PgStore *store);

/* Disables synchronous commit on this connection (`SET synchronous_commit
 * = off`) -- every COMMIT from here on returns as soon as its WAL record
 * is written to the OS, without waiting for a physical disk fsync.
 * Trades a small, bounded durability window (the last few not-yet-
 * flushed commits could be lost on a hard crash/power loss, though the
 * database itself never corrupts) for real throughput -- appropriate for
 * a rebuildable index build (if ingestion crashes, the fix is re-running
 * it, not recovering unflushed commits), not for connections serving
 * live, irreplaceable writes. Measured directly: ~16% higher ingestion
 * throughput with this enabled (see SPEED.md). Returns 0 on success, -1
 * on failure. */
int pg_store_disable_synchronous_commit(PgStore *store);

/* -- Bulk staging tables (deferred-term-resolution ingestion, spec section
 * 8's three-phase redesign -- see bulk_ingest.c) --
 *
 * documents_raw holds Phase 1's raw (pid, text) rows, loaded via a single
 * COPY rather than one INSERT per row. postings_staged holds Phase 2's
 * per-passage term postings keyed by the term's own text, not its
 * terms.id -- Phase 2's whole point is that worker threads never touch
 * the terms table (the sole source of every deadlock measured in this
 * project's concurrent ingestion, see SPEED.md), so there is no term_id
 * to write yet. Both are UNLOGGED (no WAL, no crash durability) and
 * carry no constraints beyond documents_raw's ordering key -- this data
 * is fully rebuildable from tsv_path by re-running the load, exactly
 * like the rest of this ingestion pipeline, so paying for durability or
 * constraint checking here buys nothing. Phase 3 (see
 * pg_store_finalize_terms_and_postings()) is what actually populates the
 * real terms/postings tables, and is also what makes duplicate/invalid
 * staged rows harmless -- ON CONFLICT DO NOTHING on terms, and a plain
 * JOIN on postings, both no-op on garbage rather than erroring. */

/* Creates documents_raw/postings_staged if they don't already exist.
 * Idempotent (IF NOT EXISTS). Returns 0 on success, -1 on failure. */
int pg_store_create_staging_tables(PgStore *store);

/* Empties both staging tables (TRUNCATE, not DELETE -- instant regardless
 * of prior row count, and resets documents_raw's row_num identity
 * sequence back to 1 so a fresh run's row ranges start clean). Call once
 * before Phase 1 so a prior run's leftover rows can't mix into this
 * one. Returns 0 on success, -1 on failure. */
int pg_store_truncate_staging_tables(PgStore *store);

/* Drops both staging tables entirely, reclaiming their disk space --
 * call once Phase 3 has finished and their data has been folded into the
 * real terms/postings tables, since nothing after that point needs them.
 * Returns 0 on success, -1 on failure. */
int pg_store_drop_staging_tables(PgStore *store);

/* Phase 1: loads every row of the CSV file at `tsv_path` (columns:
 * pid, text -- RFC4180 CSV, tab-delimited, no header; see SPEED.md for
 * why plain TSV without CSV-style quoting isn't safe here -- real
 * MS MARCO passages contain literal, unescaped backslash and
 * double-quote characters) into documents_raw via a single COPY, using
 * libpq's COPY protocol (PQputCopyData) rather than any per-row INSERT.
 * `tsv_path` is read client-side in fixed-size chunks and streamed to
 * the server, so this works the same whether the file is local to the
 * machine running lexis or not (unlike server-side `COPY FROM
 * '<path>'`, which requires the file to be readable by the Postgres
 * server process itself). Returns the number of rows loaded (>= 0) on
 * success, or -1 if the file can't be opened or the COPY fails. */
int64_t pg_store_copy_documents_raw(PgStore *store, const char *tsv_path);

/* One row read back from documents_raw -- `pid`/`text` are owned copies,
 * see pg_store_raw_documents_free(). */
typedef struct {
    int64_t row_num;
    char *pid;
    char *text;
} PgStoreRawDocument;

/* Phase 2: fetches every documents_raw row with row_num in
 * [start_row, end_row) (start inclusive, end exclusive), ordered by
 * row_num, as a single round trip -- the batch a Phase 2 worker claims
 * and processes independently of every other worker (see bulk_ingest.c;
 * plain SELECTs against a range never lock anything, unlike the
 * ON CONFLICT-driven terms-table contention that motivated this whole
 * redesign, see SPEED.md). Sets *count_out to the number of rows
 * actually returned (may be less than end_row - start_row at the tail
 * end of the table). Returns a newly allocated array the caller must
 * free via pg_store_raw_documents_free(), or NULL (with *count_out
 * unset) on a database or allocation error. */
PgStoreRawDocument *pg_store_get_raw_documents_range(PgStore *store, int64_t start_row, int64_t end_row,
                                                      size_t *count_out);

/* Frees an array returned by pg_store_get_raw_documents_range(), including
 * each row's owned pid/text. Safe to call with docs == NULL. */
void pg_store_raw_documents_free(PgStoreRawDocument *docs, size_t count);

/* Phase 2: records that `passage_id` (already inserted into the real
 * passages table) contains each of terms[0..count) `term_frequencies[i]`
 * times, out of `token_count` total tokens -- staged by the term's own
 * text, not a resolved terms.id, since Phase 2 never touches the terms
 * table at all (see pg_store.h's staging-tables comment for why). One
 * round trip regardless of count, same unnest()-zip technique as
 * pg_store_insert_postings(). Requires count >= 1. Returns 0 on success,
 * -1 on failure. */
int pg_store_insert_staged_postings(PgStore *store, int64_t passage_id, const char *const *terms,
                                     const int *term_frequencies, int token_count, size_t count);

/* Phase 3: the single-threaded, set-based finalize step -- resolves every
 * distinct term in postings_staged to a real terms.id (inserting new
 * ones, `ON CONFLICT (term) DO NOTHING` for ones another run already
 * created), then writes the real postings rows by joining postings_staged
 * against terms on term text. Exactly one writer, so unlike Phase 2's
 * worker pool this has zero contention risk by construction -- this is
 * also where a large work_mem genuinely pays off (a hash join/distinct
 * over hundreds of millions of staged rows), which this raises for the
 * duration of the call. Meant to run once, after every Phase 2 worker
 * has finished writing to postings_staged. Returns the number of postings
 * rows written (>= 0) on success, or -1 on failure. */
long pg_store_finalize_terms_and_postings(PgStore *store);

#endif /* LEXIS_PG_STORE_H */
