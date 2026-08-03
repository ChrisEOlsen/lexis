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
 * under genuine concurrent writers (see pg_store_get_or_create_term()'s
 * own doc comment below, and SPEED.md, for the full story). */

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

/* -- Multi-corpus support (groups) -- see APP_SPEC.md's "Core concept:
 * groups = one Postgres schema each" for the full design. Each corpus
 * ("group" in the app UI) is its own Postgres schema, holding its own
 * passages/terms/postings tables, isolated from every other corpus so
 * the whole bulk-ingest machinery above stays safely reusable per group.
 * The public.corpora table (created by pg_store_ensure_corpora_registry())
 * is the one exception -- it lives permanently in the default `public`
 * schema and tracks which corpora exist. -- */

/* Creates public.corpora if it doesn't already exist. Idempotent (IF NOT
 * EXISTS). Returns 0 on success, -1 on failure. Called automatically by
 * pg_store_create_corpus(); exposed separately for callers (e.g. a future
 * "list corpora") that only need to read the registry. */
int pg_store_ensure_corpora_registry(PgStore *store);

/* Creates a new corpus: a registry row in public.corpora plus a fresh
 * Postgres schema (an opaque, server-generated name -- "corpus_<id>",
 * never built from `display_name`, see APP_SPEC.md) holding its own
 * passages/terms/postings tables, identical in shape to the ones
 * pg_store_open() creates in `public`. Runs as one transaction, so a
 * failure partway through (e.g. schema creation fails after the registry
 * row is inserted) never leaves an orphaned registry entry pointing at a
 * schema that doesn't exist.
 *
 * On success, returns the new corpus's id (> 0) and sets *schema_name_out
 * to a newly malloc()'d string (caller must free()) holding its schema
 * name -- needed by a future "open/use this corpus" call to set
 * `search_path`. Returns -1 on failure (display_name NULL/empty,
 * schema_name_out NULL, or any database error), *schema_name_out left
 * untouched. */
int64_t pg_store_create_corpus(PgStore *store, const char *display_name, char **schema_name_out);

/* Scopes every subsequent query on `store`'s connection to `corpus_id`'s
 * schema by setting `search_path` (looked up from public.corpora, so the
 * caller only ever deals in ids/display names, never the opaque schema
 * name itself). This is what makes every existing, unqualified query in
 * this module and bm25.c/bulk_ingest.c actually operate on one chosen
 * group -- none of them need to change to become corpus-aware.
 *
 * Stays in effect for this connection until the next pg_store_use_corpus()
 * call or pg_store_close(); there is no "switch back to no corpus"
 * beyond selecting a different corpus_id. Returns 0 on success, -1 if
 * corpus_id doesn't exist in the registry or the SET fails. */
int pg_store_use_corpus(PgStore *store, int64_t corpus_id);

/* One corpus as read back from the registry -- display_name is an owned
 * copy, freed via pg_store_corpora_free(). schema_name is deliberately
 * not exposed here; callers only ever need id (to pass to
 * pg_store_use_corpus()/pg_store_delete_corpus()) and display_name (to
 * show the user). */
typedef struct {
    int64_t id;
    char *display_name;
} PgStoreCorpus;

/* Lists every registered corpus, oldest first (ORDER BY id). Creates the
 * registry first if it doesn't exist yet (see
 * pg_store_ensure_corpora_registry()), so this returns an empty array,
 * not an error, on a database where no corpus has ever been created.
 * Sets *count_out to the number of corpora found. Returns a newly
 * allocated array the caller must free via pg_store_corpora_free(), or
 * NULL (with *count_out unset) on a database or allocation error. */
PgStoreCorpus *pg_store_list_corpora(PgStore *store, size_t *count_out);

/* Frees an array returned by pg_store_list_corpora(), including each
 * entry's owned display_name. Safe to call with corpora == NULL. */
void pg_store_corpora_free(PgStoreCorpus *corpora, size_t count);

/* Permanently deletes a corpus: drops its schema (DROP SCHEMA ... CASCADE
 * -- removes passages/terms/postings and every row in them, atomically
 * and near-instantly, see APP_SPEC.md on why this beats a row-by-row
 * DELETE) and removes its row from public.corpora, as one transaction.
 * Does not check whether `corpus_id` is the connection's currently
 * active corpus (via pg_store_use_corpus()) -- deleting the active
 * corpus leaves search_path pointing at a schema that no longer exists;
 * the caller is responsible for not doing that, or for calling
 * pg_store_use_corpus() again with a different corpus afterward before
 * issuing any further passages/terms/postings query. Returns 0 on
 * success, -1 if corpus_id doesn't exist or any step fails (in which
 * case nothing is deleted -- the whole operation rolls back). */
int pg_store_delete_corpus(PgStore *store, int64_t corpus_id);

/* Closes the connection and frees the PgStore. Safe to call with
 * store == NULL. */
void pg_store_close(PgStore *store);

/* Inserts a passage (a chunk of a source document) and returns its new
 * row id, or -1 on failure. */
int64_t pg_store_insert_passage(PgStore *store, const char *document_name, int chunk_id,
                                 const char *text, int token_count);

/* Records `document_name`'s original, un-chunked `text` in the documents
 * table -- see pg_store.c's LEXIS_SCHEMA_SQL comment for why this exists
 * separately from passages (which only ever holds post-chunking
 * fragments) and documents_raw (which is transient, dropped at the end
 * of every bulk_ingest_tsv() run). ON CONFLICT (document_name) DO
 * NOTHING, not an error -- a Phase 2 batch retry re-processing the same
 * document is expected, not a bug (see bulk_ingest.c). Returns 0 on
 * success, -1 on failure. */
int pg_store_insert_document(PgStore *store, const char *document_name, const char *text);

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

/* Bracket Phase 3 with this (prepare, called first) and
 * pg_store_finish_bulk_load() (restore, called last) to defer postings'
 * PRIMARY KEY and both FOREIGN KEY constraints, and terms/postings'
 * durability, to one bulk pass at the very end instead of paying for
 * them per-row during the load. Measured directly (see SPEED.md): the
 * two foreign keys turned out to be the single largest lever found so
 * far in this whole pipeline -- bigger than the primary key itself, and
 * far bigger than parallelizing the join (which barely mattered).
 *
 * Drops (with IF EXISTS, so a prior crashed run's already-weakened state
 * doesn't wedge this one -- matches this pipeline's existing
 * "rebuildable, not crash-safe mid-run" philosophy, see
 * pg_store_disable_synchronous_commit()) postings_pkey,
 * postings_term_id_fkey, and postings_passage_id_fkey, then sets
 * `postings` and `terms` UNLOGGED. Constraints are dropped before the
 * UNLOGGED conversion specifically because Postgres refuses to weaken a
 * table's persistence while a still-LOGGED table holds a foreign key
 * referencing it -- dropping postings' FK to terms first removes that
 * dependency.
 *
 * `passages` is deliberately left untouched -- query_log.c's
 * search_results table (LOGGED, only populated in testing mode) holds a
 * foreign key referencing it, which the same rule above would block, and
 * passages isn't written by Phase 3 (the actual target) anyway.
 *
 * A run that fails after calling this and before calling
 * pg_store_finish_bulk_load() leaves the schema in this weakened state
 * until the next successful bulk-ingest run restores it -- an accepted
 * trade-off given the "just re-run it" philosophy already in place, not
 * a gap. Returns 0 on success, -1 on failure. */
int pg_store_prepare_bulk_load(PgStore *store);

/* Reverses pg_store_prepare_bulk_load(): re-adds postings' PRIMARY KEY
 * and both FOREIGN KEY constraints -- built/validated once in a single
 * bulk pass against whatever's actually in the table, far cheaper than
 * maintaining them incrementally during the load (see SPEED.md) -- then
 * sets `postings` and `terms` back to LOGGED. The constraints are
 * rebuilt before restoring LOGGED status, not after: an index or
 * constraint built on a still-UNLOGGED table is itself unlogged, so
 * building them first avoids paying WAL for that build entirely: SET
 * LOGGED then generates WAL once, in bulk, for the fully-built table
 * instead. Must be called after every successful
 * pg_store_prepare_bulk_load() -- unlike the throwaway staging tables,
 * `passages`/`terms`/`postings` are the real index, and a run that never
 * restores this leaves it durably weakened. Returns 0 on success, -1 on
 * failure. */
int pg_store_finish_bulk_load(PgStore *store);

#endif /* LEXIS_PG_STORE_H */
