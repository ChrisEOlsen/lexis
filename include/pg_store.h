/* Index and passage persistence via libpq/PostgreSQL (passages/terms/postings, int64_t ids). */

#ifndef LEXIS_PG_STORE_H
#define LEXIS_PG_STORE_H

#include <libpq-fe.h>
#include <stdint.h>

/* Wraps a single open connection to the Postgres database. */
typedef struct {
    PGconn *conn;
} PgStore;

/* Open via conninfo; ensure passages/terms/postings exist. NULL on connection/schema failure. */
PgStore *pg_store_open(const char *conninfo);

/* -- Multi-corpus support: each corpus is its own schema; public.corpora tracks them. -- */

/* Create public.corpora if missing. Idempotent. 0 on success, -1 on failure. */
int pg_store_ensure_corpora_registry(PgStore *store);

/* Create a corpus: registry row + fresh schema, one transaction. New id (> 0) and malloc'd
 * schema_name_out (caller frees); -1 on failure, schema_name_out untouched. */
int64_t pg_store_create_corpus(PgStore *store, const char *display_name, char **schema_name_out);

/* Scope this connection to corpus_id's schema (persists until next use_corpus/close).
 * 0 on success, -1 if corpus_id missing or SET fails. */
int pg_store_use_corpus(PgStore *store, int64_t corpus_id);

/* Set search_path from schema_name directly (no registry lookup). schema_name must be a trusted id.
 * 0 on success, -1 on failure. */
int pg_store_use_schema(PgStore *store, const char *schema_name);

/* One corpus: id + owned display_name (freed via pg_store_corpora_free()). */
typedef struct {
    int64_t id;
    char *display_name;
} PgStoreCorpus;

/* List every corpus, oldest first (empty array if none). Caller frees via corpora_free.
 * NULL (count_out unset) on DB/alloc error. */
PgStoreCorpus *pg_store_list_corpora(PgStore *store, size_t *count_out);

/* Free a list_corpora array incl. display_names. Safe with corpora == NULL. */
void pg_store_corpora_free(PgStoreCorpus *corpora, size_t count);

/* Delete a corpus: drop schema CASCADE + remove registry row, one transaction (rolls back on failure).
 * Caller must not leave the connection scoped to the deleted corpus. 0 ok, -1 on failure. */
int pg_store_delete_corpus(PgStore *store, int64_t corpus_id);

/* -- Chat history: chat_sessions/chat_messages live in public, never in a corpus schema. -- */

/* One chat session: id + owned title/created_at (raw TIMESTAMPTZ text; caller parses). */
typedef struct {
    int64_t id;
    char *title;
    char *created_at;
} PgStoreChatSession;

/* Create chat tables if missing. Idempotent. 0 on success, -1 on failure. */
int pg_store_ensure_chat_tables(PgStore *store);

/* Create a chat session under corpus_id with title. New id (> 0), or -1 on failure. */
int64_t pg_store_create_chat_session(PgStore *store, int64_t corpus_id, const char *title);

/* List corpus_id's sessions, newest first (0 = none, not an error). Caller frees via sessions_free.
 * NULL (count_out unset) on DB/alloc error. */
PgStoreChatSession *pg_store_list_chat_sessions(PgStore *store, int64_t corpus_id, size_t *count_out);

/* Free a list_chat_sessions array incl. titles. Safe with sessions == NULL. */
void pg_store_chat_sessions_free(PgStoreChatSession *sessions, size_t count);

/* -- Group summaries: cached overview per corpus in public; document_count is the staleness key. -- */

/* Create public.corpus_summaries if missing. Idempotent. 0 on success, -1 on failure. */
int pg_store_ensure_summary_table(PgStore *store);

/* Read corpus_id's cached summary. Caller frees; NULL = absent or failed (both mean: build it).
 * document_count_out = count it was generated at, untouched on NULL. */
char *pg_store_get_corpus_summary(PgStore *store, int64_t corpus_id, int *document_count_out);

/* Inserts or replaces corpus_id's cached summary (one row per corpus).
 * Returns 0 on success, -1 on failure. */
int pg_store_set_corpus_summary(PgStore *store, int64_t corpus_id, const char *text, int document_count);

/* Delete a chat session and its messages (CASCADE). 0 on success, -1 if missing/failed. */
int pg_store_delete_chat_session(PgStore *store, int64_t session_id);

/* One chat message: owned text + sources_json (NULL for user msgs; stored/returned verbatim). */
typedef struct {
    int is_user;
    char *text;
    char *sources_json;
} PgStoreChatMessage;

/* Append one message to session_id. sources_json NULL for user msgs. 0 ok, -1 on failure. */
int pg_store_append_chat_message(PgStore *store, int64_t session_id, int is_user, const char *text,
                                  const char *sources_json);

/* Read session_id's messages, oldest first (0 = none, not an error). Caller frees via messages_free.
 * NULL (count_out unset) on DB/alloc error. */
PgStoreChatMessage *pg_store_get_chat_messages(PgStore *store, int64_t session_id, size_t *count_out);

/* Free a get_chat_messages array incl. text/sources_json. Safe with messages == NULL. */
void pg_store_chat_messages_free(PgStoreChatMessage *messages, size_t count);

/* Replace session_id's newest message if it is an assistant row ("try harder" retry). -1 otherwise. */
int pg_store_update_last_assistant_message(PgStore *store, int64_t session_id, const char *text,
                                            const char *sources_json);

/* -- Per-document reads and deletion (operate on the currently-selected schema). -- */

/* One document's chunk stats; owned document_name (freed via document_stats_free). */
typedef struct {
    char *document_name;
    long passage_count;
    long total_tokens;
} PgStoreDocumentStats;

/* List every document with passage/token counts, ordered by name. Caller frees via stats_free.
 * NULL (count_out unset) on DB/alloc error. */
PgStoreDocumentStats *pg_store_list_document_stats(PgStore *store, size_t *count_out);

/* Free a list_document_stats array incl. names. Safe with stats == NULL. */
void pg_store_document_stats_free(PgStoreDocumentStats *stats, size_t count);

/* Read one document's stored text (as indexed; the DB is the source of truth). Caller frees.
 * NULL if missing or on DB/alloc error. */
char *pg_store_get_document_text(PgStore *store, const char *document_name);

/* One document chunk in chunk order; owned text (freed via document_passages_free). */
typedef struct {
    int chunk_id;
    char *text;
    int token_count;
} PgStoreDocumentPassage;

/* Read document_name's chunks in order (0 = none/missing, not an error). Caller frees via passages_free.
 * NULL (count_out unset) on DB/alloc error. */
PgStoreDocumentPassage *pg_store_get_document_passages(PgStore *store, const char *document_name,
                                                       size_t *count_out);

/* Free a get_document_passages array incl. text. Safe with passages == NULL. */
void pg_store_document_passages_free(PgStoreDocumentPassage *passages, size_t count);

/* Remove one document: postings, passages, documents row, orphaned terms. One transaction (rolls back).
 * 0 on success, -1 if missing or any step fails. */
int pg_store_remove_document(PgStore *store, const char *document_name);

/* -- Rebuild-on-append primitives: ingest into a scratch schema, then swap it in. -- */

/* Create a bare schema + tables (no registry row): scratch space for a rebuild. Trusted id only.
 * 0 on success, -1 on failure (incl. already exists). */
int pg_store_create_bare_schema(PgStore *store, const char *schema_name);

/* Drop a bare schema IF EXISTS (missing is fine). Trusted id only. 0 on success, -1 on failure. */
int pg_store_drop_bare_schema(PgStore *store, const char *schema_name);

/* Atomically swap corpus_id's schema for new_schema_name (one transaction; registry id unchanged).
 * Caller builds/drops new_schema_name. 0 on success, -1 on failure. */
int pg_store_swap_corpus_schema(PgStore *store, int64_t corpus_id, const char *new_schema_name);

/* One document: owned name + text (freed via pg_store_documents_free). */
typedef struct {
    char *document_name;
    char *text;
} PgStoreDocument;

/* Read the selected schema's documents, ordered by name. Caller frees via documents_free.
 * NULL (count_out unset) on DB/alloc error. */
PgStoreDocument *pg_store_get_all_documents(PgStore *store, size_t *count_out);

/* Free a get_all_documents array incl. name/text. Safe with docs == NULL. */
void pg_store_documents_free(PgStoreDocument *docs, size_t count);

/* Close the connection and free the store. Safe with store == NULL. */
void pg_store_close(PgStore *store);

/* Inserts a passage (a chunk of a source document) and returns its new
 * row id, or -1 on failure. */
int64_t pg_store_insert_passage(PgStore *store, const char *document_name, int chunk_id,
                                 const char *text, int token_count);

/* Record document_name's original un-chunked text. ON CONFLICT DO NOTHING. 0 ok, -1 on failure. */
int pg_store_insert_document(PgStore *store, const char *document_name, const char *text);

/* Test helper, no production caller: term's id, inserting first if new. -1 on failure. */
int64_t pg_store_get_or_create_term(PgStore *store, const char *term);

/* Test helper, no production caller: batch ids for terms[0..count) in order (caller frees).
 * count >= 1 required. NULL on failure. */
int64_t *pg_store_get_or_create_terms(PgStore *store, const char *const *terms, size_t count);

/* term's id without inserting. -1 if never indexed or on DB error (both = contributes nothing). */
int64_t pg_store_lookup_term(PgStore *store, const char *term);

/* Record term_id x term_frequency in passage_id (token_count denormalized). 0 ok, -1 on failure. */
int pg_store_insert_posting(PgStore *store, int64_t term_id, int64_t passage_id, int term_frequency,
                             int token_count);

/* Batch insert: count postings for one passage_id in one round trip. count >= 1. 0 ok, -1 on failure. */
int pg_store_insert_postings(PgStore *store, const int64_t *term_ids, int64_t passage_id,
                              const int *term_frequencies, int token_count, size_t count);

/* One passage's stored data; owned document_name/text (free via pg_store_passage_free). */
typedef struct {
    char *document_name;
    int chunk_id;
    char *text;
    int token_count;
} PgStorePassage;

/* Reads back the passage stored at `passage_id`. Returns NULL if no such
 * passage exists or on a database/allocation error. */
PgStorePassage *pg_store_get_passage(PgStore *store, int64_t passage_id);

/* Free a passage's strings and struct. Safe with passage == NULL. */
void pg_store_passage_free(PgStorePassage *passage);

/* Batch document_names for passage_ids in order (one round trip); NULL entry = missing. Free each + array.
 * count >= 1. NULL (nothing to free) on DB/alloc failure. */
char **pg_store_get_document_names(PgStore *store, const int64_t *passage_ids, size_t count);

/* Explicit transaction control (batch a document's writes into one commit). 0 ok, -1 on failure. */
int pg_store_begin_transaction(PgStore *store);
int pg_store_commit_transaction(PgStore *store);
int pg_store_rollback_transaction(PgStore *store);

/* SET synchronous_commit = off: throughput over durability. For rebuildable index builds only.
 * 0 on success, -1 on failure. */
int pg_store_disable_synchronous_commit(PgStore *store);

/* -- Bulk staging tables: UNLOGGED, rebuildable; Phase 3 folds them into terms/postings. -- */

/* Creates documents_raw/postings_staged if they don't already exist.
 * Idempotent (IF NOT EXISTS). Returns 0 on success, -1 on failure. */
int pg_store_create_staging_tables(PgStore *store);

/* TRUNCATE both staging tables (resets row_num to 1). Call once before Phase 1. 0 ok, -1 on failure. */
int pg_store_truncate_staging_tables(PgStore *store);

/* Drop both staging tables; call once Phase 3 has folded their data in. 0 ok, -1 on failure. */
int pg_store_drop_staging_tables(PgStore *store);

/* Phase 1: COPY every (pid, text) row of tsv_path into documents_raw (client-side streamed).
 * Rows loaded (>= 0), or -1 if the file can't open or COPY fails. */
int64_t pg_store_copy_documents_raw(PgStore *store, const char *tsv_path);

/* One documents_raw row; owned pid/text (see pg_store_raw_documents_free). */
typedef struct {
    int64_t row_num;
    char *pid;
    char *text;
} PgStoreRawDocument;

/* Phase 2: fetch documents_raw rows in [start_row, end_row) ordered by row_num, one round trip.
 * Caller frees via raw_documents_free; NULL (count_out unset) on DB/alloc error. */
PgStoreRawDocument *pg_store_get_raw_documents_range(PgStore *store, int64_t start_row, int64_t end_row,
                                                      size_t *count_out);

/* Free a get_raw_documents_range array incl. pid/text. Safe with docs == NULL. */
void pg_store_raw_documents_free(PgStoreRawDocument *docs, size_t count);

/* Phase 2: stage passage_id's postings keyed by term text (one round trip). count >= 1.
 * 0 on success, -1 on failure. */
int pg_store_insert_staged_postings(PgStore *store, int64_t passage_id, const char *const *terms,
                                     const int *term_frequencies, int token_count, size_t count);

/* Phase 3: resolve staged terms to ids, write real postings via join. Run once, after Phase 2.
 * Postings rows written (>= 0), or -1 on failure. */
long pg_store_finalize_terms_and_postings(PgStore *store);

/* Prepare bulk load: drop postings PK/FKs (IF EXISTS), set terms/postings UNLOGGED. passages untouched.
 * A failed run leaves this weakened state until the next run restores it. 0 ok, -1 on failure. */
int pg_store_prepare_bulk_load(PgStore *store);

/* Reverse prepare: rebuild postings PK/FKs in one bulk pass, set terms/postings back to LOGGED.
 * Must follow every successful prepare. 0 on success, -1 on failure. */
int pg_store_finish_bulk_load(PgStore *store);

#endif /* LEXIS_PG_STORE_H */
