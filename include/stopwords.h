/*
 * Stopword filtering (spec 5.2.1, build order Stage 1).
 * Strips high-frequency, meaning-free terms (the, is, a, of, ...) from a
 * token stream against a configurable stopword list loaded from
 * data/stopwords/.
 */

#ifndef LEXIS_STOPWORDS_H
#define LEXIS_STOPWORDS_H

#include <stddef.h>

#include "tokenizer.h"

/* A loaded stopword list, sorted (strcmp order) after loading so that
 * stopword_set_contains() can binary-search it. `words` is a heap array
 * of heap-allocated, NUL-terminated strings; `count` is how many are
 * stored. Unlike TokenList, this never grows after stopword_set_load()
 * returns — no append function needed. */
typedef struct {
    char **words;
    size_t count;
} StopwordSet;

/* Loads a stopword list from the file at `path`: one word per line, blank
 * lines and lines starting with '#' ignored (see data/stopwords/english.txt).
 * Sorts the result so stopword_set_contains() can binary-search it. Returns
 * NULL if the file can't be opened or on allocation failure. */
StopwordSet *stopword_set_load(const char *path);

/* Frees a StopwordSet: every stored word string, the words array itself,
 * and the StopwordSet struct. Safe to call with set == NULL. */
void stopword_set_free(StopwordSet *set);

/* Returns 1 if `word` is present in the set, 0 otherwise. Binary search
 * against the sorted `words` array — O(log n) rather than a linear scan. */
int stopword_set_contains(const StopwordSet *set, const char *word);

/* Removes every token in `list` that appears in `set`, mutating `list` in
 * place: each matched term's string is freed and the survivors are
 * compacted down, shrinking list->count to match. list->capacity is left
 * unchanged. No-op if list or set is NULL. */
void stopwords_filter(TokenList *list, const StopwordSet *set);

#endif /* LEXIS_STOPWORDS_H */
