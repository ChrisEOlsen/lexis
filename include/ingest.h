/*
 * Offline ingestion pipeline orchestration (spec 5.2.1, build order Stages
 * 1-2). Reads raw documents from disk, chunks them into passages
 * (configurable size/overlap), runs them through tokenizer + stopwords,
 * and builds the inverted index — parallelized across documents via
 * pthreads for large corpora. Runs once per corpus, or incrementally when
 * documents change.
 */

#ifndef LEXIS_INGEST_H
#define LEXIS_INGEST_H

#include <stddef.h>

#include "lemmatizer.h"
#include "pg_store.h"
#include "stopwords.h"
#include "term_cache.h"
#include "tokenizer.h"
#include "wordnet.h"

/* Reads the entire contents of the file at `path` into a single
 * heap-allocated, NUL-terminated buffer. Caller owns the buffer and must
 * free() it. Returns NULL if the file can't be opened, read, or on
 * allocation failure. */
char *ingest_read_file(const char *path);

/* Splits `text` on whitespace runs (space, tab, \r, \n) into a TokenList
 * of raw words -- punctuation stays attached (e.g. "hypertension,"),
 * unlike tokenize() which strips it and lowercases everything. Reuses
 * TokenList purely as a generic growable string list; despite the name,
 * this is NOT tokenizer output -- it's the first step of chunking, before
 * each chunk gets separately run through the real tokenizer for indexing.
 * Returns NULL on allocation failure. */
TokenList *ingest_split_words(const char *text);

/* Joins words->terms[start] through words->terms[end - 1] (end exclusive)
 * into a single space-separated heap string -- the reconstructed raw text
 * for one chunk. Returns an empty string if start == end. Returns NULL on
 * allocation failure. */
char *ingest_join_words(const TokenList *words, size_t start, size_t end);

/* Groups `words` into overlapping windows of `chunk_size` words each, with
 * `overlap` words shared between consecutive windows, joining each window
 * into a single string via ingest_join_words(). Reuses TokenList again --
 * each "term" here is actually one whole passage's text. Requires
 * chunk_size > 0 and overlap < chunk_size. Returns an empty TokenList if
 * `words` is empty. Returns NULL on invalid parameters or allocation
 * failure. */
TokenList *ingest_chunk_words(const TokenList *words, size_t chunk_size, size_t overlap);

/* Lemmatizes every term in `terms` (e.g. "nicknamed" -> "nickname") into a
 * freshly allocated TokenList, so the index stores the same base forms
 * query_formulation.c looks words up under at query time. Exposed
 * (not `static`) so bulk_ingest.c's Phase 2 worker loop (see
 * bulk_ingest.c) can reuse the exact same tokenize -> stopword-filter ->
 * lemmatize pipeline ingest_document_from_text() uses, without
 * duplicating it -- the two paths must lemmatize identically, or a
 * passage indexed via one path could silently fail to match a query that
 * would have matched had it gone through the other. Returns NULL on
 * allocation failure. */
TokenList *ingest_lemmatize_terms(const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                                   const TokenList *terms);

/* Computes each distinct term in `terms` (first-occurrence order) and how
 * many times it occurs, into freshly malloc()'d parallel arrays of
 * length *distinct_count_out. Shared by ingest_index_chunk_terms() (which
 * resolves each distinct term to a terms.id via pg_store/TermCache) and
 * bulk_ingest.c's Phase 2 worker (which instead stages the raw term text
 * directly -- see bulk_ingest.c's staged-postings design) -- both need
 * the exact same dedup+frequency-count logic, so it lives here once
 * rather than risking the two drifting apart. O(n^2) over terms->count --
 * fine at chunk scale (a few hundred terms at most), same tradeoff
 * bm25_result_set_add()'s linear scan makes at a different scale.
 * *distinct_terms_out borrows terms->terms[i] pointers directly (no
 * copies) and is only valid as long as `terms` is alive; *frequencies_out
 * is a freshly allocated int array. Caller must free() both (but not
 * distinct_terms_out's contents, which it doesn't own). An empty `terms`
 * sets *distinct_count_out to 0 and both outputs to NULL. Returns 0 on
 * success, -1 on allocation failure (outputs left unset). */
int ingest_count_distinct_terms(const TokenList *terms, const char ***distinct_terms_out,
                                 int **frequencies_out, size_t *distinct_count_out);

/* Ingests one document end-to-end from an in-memory string: splits `text`
 * into overlapping word-window chunks (chunk_size/overlap, in words), and
 * for each chunk tokenizes + strips stopwords + lemmatizes each surviving
 * term (via `lemmatizer`/`wordnet`, e.g. "nicknamed" -> "nickname") +
 * persists both the passage (raw, un-lemmatized chunk text) and its term
 * postings (with per-chunk term frequency, keyed by lemma) to `store`.
 * Lemmatizing here, not just at query time, is what lets a query for
 * "call" actually match a passage that only ever said "called" or
 * "calling" -- see lemmatizer.c. `document_name` is stored alongside each
 * passage for source attribution -- for a bulk loader indexing rows that
 * already have a natural ID (e.g. an MS MARCO pid), pass that ID directly
 * rather than a filename. The whole document's writes are wrapped in one
 * transaction (see pg_store_begin_transaction()) -- a failure partway
 * through rolls back cleanly rather than leaving the document
 * half-indexed. `cache`, if non-NULL, is consulted/populated instead of
 * calling pg_store_get_or_create_terms() directly -- a shared, thread-
 * safe in-memory term cache (see term_cache.h) that turns "already
 * resolved by any concurrent worker" into a fast in-process lookup
 * instead of a Postgres round trip. Pass NULL for the original
 * uncached behavior (e.g. ingest_corpus()'s single-threaded path, where
 * there's no cross-thread benefit to share). Returns the number of
 * passages ingested (>= 0) on success, or -1 on failure. */
long ingest_document_from_text(PgStore *store, const StopwordSet *stopwords,
                                const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                                const char *text, const char *document_name,
                                size_t chunk_size, size_t overlap, TermCache *cache);

/* Thin wrapper around ingest_document_from_text(): reads `path` into
 * memory first, then ingests it the same way. See
 * ingest_document_from_text() for the real logic, including `cache`.
 * Returns -1 if `path` can't be read, otherwise whatever
 * ingest_document_from_text() returns. */
long ingest_document(PgStore *store, const StopwordSet *stopwords,
                      const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                      const char *path, const char *document_name,
                      size_t chunk_size, size_t overlap, TermCache *cache);

/* Ingests every regular file directly inside `dir_path` (NOT recursive --
 * subdirectories are skipped, see LIMITATIONS.md) by calling
 * ingest_document() on each, using the file's own name as its
 * document_name. A single file that fails to ingest is logged and
 * skipped rather than aborting the whole corpus. Returns the total
 * number of passages ingested across the directory (>= 0), or -1 if the
 * directory itself can't be opened. */
long ingest_corpus(PgStore *store, const StopwordSet *stopwords,
                    const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                    const char *dir_path, size_t chunk_size, size_t overlap);

#endif /* LEXIS_INGEST_H */
