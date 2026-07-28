/*
 * Index and passage persistence via libsqlite3 (spec 5.2.1, 6, build order
 * Stage 2). Persists the inverted index and raw passage text to a durable,
 * queryable, zero-dependency store — no external database server required.
 *
 * Schema (normalized — see design discussion): passages holds raw chunk
 * text and length; terms maps each distinct term string to a small integer
 * id; postings is the inverted index itself, (term_id, passage_id) ->
 * term_frequency, keyed so lookups by term_id (the hot path — "find every
 * passage containing this term") hit the primary key's b-tree directly.
 */

#ifndef LEXIS_SQLITE_STORE_H
#define LEXIS_SQLITE_STORE_H

#include <sqlite3.h>

/* Wraps a single open connection to the index's SQLite database file. */
typedef struct {
    sqlite3 *db;
} SqliteStore;

/* Opens (creating if absent) the SQLite database at `path` and ensures the
 * passages/terms/postings tables exist. Returns NULL on failure to open
 * the file or create the schema. */
SqliteStore *sqlite_store_open(const char *path);

/* Closes the database connection and frees the SqliteStore. Safe to call
 * with store == NULL. */
void sqlite_store_close(SqliteStore *store);

/* Inserts a passage (a chunk of a source document) and returns its new
 * row id, or -1 on failure. */
sqlite3_int64 sqlite_store_insert_passage(SqliteStore *store,
                                           const char *document_name,
                                           int chunk_id, const char *text,
                                           int token_count);

/* Returns the id of `term` in the terms table, inserting it first if this
 * is the first time it's been seen. Returns -1 on failure. */
sqlite3_int64 sqlite_store_get_or_create_term(SqliteStore *store,
                                               const char *term);

/* Read-only counterpart to sqlite_store_get_or_create_term(): looks up
 * `term` without ever inserting it. Returns its id if the term has been
 * indexed before, or -1 if it hasn't been seen (a query-time term with no
 * corpus match at all) or on a real database error -- either way, the
 * caller's correct response is the same: this term contributes nothing. */
sqlite3_int64 sqlite_store_lookup_term(SqliteStore *store, const char *term);

/* Records that `term_id` occurs `term_frequency` times in `passage_id`.
 * Returns 0 on success, -1 on failure. */
int sqlite_store_insert_posting(SqliteStore *store, sqlite3_int64 term_id,
                                 sqlite3_int64 passage_id,
                                 int term_frequency);

/* One passage's stored data, read back from the database. `document_name`
 * and `text` are owned copies -- free via sqlite_store_passage_free(). */
typedef struct {
    char *document_name;
    int chunk_id;
    char *text;
    int token_count;
} SqliteStorePassage;

/* Reads back the passage stored at `passage_id` -- needed to turn a
 * BM25ResultSet's bare passage_id/score pairs back into actual
 * retrievable text (e.g. for answer generation, spec 5.2.7). Returns
 * NULL if no such passage exists or on a database/allocation error. */
SqliteStorePassage *sqlite_store_get_passage(SqliteStore *store, sqlite3_int64 passage_id);

/* Frees a passage's owned strings and the struct itself. Safe to call
 * with passage == NULL. */
void sqlite_store_passage_free(SqliteStorePassage *passage);

/* Explicit transaction control, so callers writing many rows in a batch
 * (e.g. ingest_document() across one document's chunks/terms/postings) can
 * wrap them in a single commit instead of paying an implicit-transaction
 * fsync on every individual INSERT -- both a large latency win and real
 * atomicity (a failure partway through rolls back the whole batch, rather
 * than leaving it partially committed). Each returns 0 on success, -1 on
 * failure. */
int sqlite_store_begin_transaction(SqliteStore *store);
int sqlite_store_commit_transaction(SqliteStore *store);
int sqlite_store_rollback_transaction(SqliteStore *store);

#endif /* LEXIS_SQLITE_STORE_H */
