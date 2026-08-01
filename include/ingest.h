/*
 * Document chunking/tokenizing/lemmatizing primitives (spec 5.2.1). Turns
 * raw document text into overlapping passage-sized chunks and, per chunk,
 * a lemmatized, deduped-with-frequency term list ready to persist. Used
 * exclusively by bulk_ingest.c's Phase 2 worker (see bulk_ingest.c) --
 * this module owns the text-processing logic, bulk_ingest.c owns turning
 * it into database writes.
 */

#ifndef LEXIS_INGEST_H
#define LEXIS_INGEST_H

#include <stddef.h>

#include "lemmatizer.h"
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
 * query_formulation.c looks words up under at query time. Returns NULL on
 * allocation failure. */
TokenList *ingest_lemmatize_terms(const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                                   const TokenList *terms);

/* Computes each distinct term in `terms` (first-occurrence order) and how
 * many times it occurs, into freshly malloc()'d parallel arrays of
 * length *distinct_count_out -- the frequency-counting step bulk_ingest.c's
 * Phase 2 worker needs before staging a chunk's postings (one row per
 * distinct term, not one per occurrence). O(n^2) over terms->count -- fine
 * at chunk scale (a few hundred terms at most), same tradeoff
 * bm25_result_set_add()'s linear scan makes at a different scale.
 * *distinct_terms_out borrows terms->terms[i] pointers directly (no
 * copies) and is only valid as long as `terms` is alive; *frequencies_out
 * is a freshly allocated int array. Caller must free() both (but not
 * distinct_terms_out's contents, which it doesn't own). An empty `terms`
 * sets *distinct_count_out to 0 and both outputs to NULL. Returns 0 on
 * success, -1 on allocation failure (outputs left unset). */
int ingest_count_distinct_terms(const TokenList *terms, const char ***distinct_terms_out,
                                 int **frequencies_out, size_t *distinct_count_out);

#endif /* LEXIS_INGEST_H */
