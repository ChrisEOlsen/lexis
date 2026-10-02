/* Stopword filtering: strip high-frequency, meaning-free terms against a loaded list. */

#ifndef LEXIS_STOPWORDS_H
#define LEXIS_STOPWORDS_H

#include <stddef.h>

#include "tokenizer.h"

/* Loaded stopword list, sorted for binary search. Fixed size after load (no append). */
typedef struct {
    char **words;
    size_t count;
} StopwordSet;

/* Load one-word-per-line list (blanks and #-comments ignored), sorted. NULL on failure. */
StopwordSet *stopword_set_load(const char *path);

/* Free words, array, and struct. Safe with set == NULL. */
void stopword_set_free(StopwordSet *set);

/* 1 if word is in the set, 0 otherwise (binary search). */
int stopword_set_contains(const StopwordSet *set, const char *word);

/* Remove set members from list in place (count shrinks; capacity unchanged). NULL-safe no-op. */
void stopwords_filter(TokenList *list, const StopwordSet *set);

#endif /* LEXIS_STOPWORDS_H */
