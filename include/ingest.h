/* Text-processing primitives: chunk raw text, lemmatize, count term frequencies. */

#ifndef LEXIS_INGEST_H
#define LEXIS_INGEST_H

#include <stddef.h>

#include "lemmatizer.h"
#include "tokenizer.h"
#include "wordnet.h"

/* Read path fully into a NUL-terminated buffer. Caller frees; NULL on failure. */
char *ingest_read_file(const char *path);

/* Split text on whitespace into raw words (punctuation kept). Not tokenizer output. NULL on failure. */
TokenList *ingest_split_words(const char *text);

/* Join words[start, end) into one space-separated string. Empty if start == end; NULL on failure. */
char *ingest_join_words(const TokenList *words, size_t start, size_t end);

/* Group words into overlapping chunk_size windows (overlap shared); each "term" is one passage text.
 * Requires chunk_size > 0, overlap < chunk_size. Empty in = empty out; NULL on failure. */
TokenList *ingest_chunk_words(const TokenList *words, size_t chunk_size, size_t overlap);

/* Lemmatize every term into a fresh list (index stores base forms). NULL on failure. */
TokenList *ingest_lemmatize_terms(const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                                   const TokenList *terms);

/* Distinct terms + frequencies into parallel arrays; terms borrowed (valid while terms lives). Free both.
 * Empty in = 0/NULL/NULL. 0 on success, -1 on alloc failure (outputs unset). */
int ingest_count_distinct_terms(const TokenList *terms, const char ***distinct_terms_out,
                                 int **frequencies_out, size_t *distinct_count_out);

#endif /* LEXIS_INGEST_H */
