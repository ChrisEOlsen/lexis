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

/* Ingests one document end-to-end: reads it from `path`, splits it into
 * overlapping word-window chunks (chunk_size/overlap, in words), and for
 * each chunk tokenizes + strips stopwords + lemmatizes each surviving term
 * (via `lemmatizer`/`wordnet`, e.g. "nicknamed" -> "nickname") + persists
 * both the passage (raw, un-lemmatized chunk text) and its term postings
 * (with per-chunk term frequency, keyed by lemma) to `store`. Lemmatizing
 * here, not just at query time, is what lets a query for "call" actually
 * match a passage that only ever said "called" or "calling" -- see
 * lemmatizer.c. `document_name` is stored alongside each passage for
 * source attribution. The whole document's writes are wrapped in one
 * transaction (see pg_store_begin_transaction()) -- a failure partway
 * through rolls back cleanly rather than leaving the document
 * half-indexed. Returns the number of passages ingested (>= 0) on
 * success, or -1 on failure. */
long ingest_document(PgStore *store, const StopwordSet *stopwords,
                      const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                      const char *path, const char *document_name,
                      size_t chunk_size, size_t overlap);

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
