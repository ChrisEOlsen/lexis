/* WordNet-style lemmatizer: inflected word -> base form (exception list, then validated suffix rules). */

#ifndef LEXIS_LEMMATIZER_H
#define LEXIS_LEMMATIZER_H

#include <stddef.h>

#include "wordnet.h"

/* One irregular inflected form mapped to its base form(s), from WordNet's .exc files. */
typedef struct {
    char *inflected;
    char **bases;
    size_t base_count;
} LemmatizerException;

/* All four .exc files merged into one table, sorted by inflected form (no POS tagger to split by). */
typedef struct {
    LemmatizerException **exceptions;
    size_t count;
} Lemmatizer;

/* Load all four .exc files from wordnet_dir. NULL on file/alloc failure. */
Lemmatizer *lemmatizer_load(const char *wordnet_dir);

/* Frees every entry, the array, and the struct. Safe with lemmatizer == NULL. */
void lemmatizer_free(Lemmatizer *lemmatizer);

/* Reduce word to its base form (exceptions first, then validated suffix rules).
 * Unchanged copy if nothing validates; always owned. Caller frees. */
char *lemmatize(const Lemmatizer *lemmatizer, const WordNetTable *wordnet, const char *word);

#endif /* LEXIS_LEMMATIZER_H */
