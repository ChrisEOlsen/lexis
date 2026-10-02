/* Learned synonym table: precomputed embedding neighbors feeding query expansion. Optional data. */

#ifndef LEXIS_SYNONYM_TABLE_H
#define LEXIS_SYNONYM_TABLE_H

#include "tokenizer.h"

#define LEXIS_SYNONYMS_PATH_DEFAULT "data/synonyms/learned_neighbors.tsv"

typedef struct SynonymTable SynonymTable;

/* NULL on missing/unreadable file (quietly -- optional) or allocation failure. */
SynonymTable *synonym_table_load(const char *path);

/* The word's neighbors, or NULL if none. Table-owned: do not free or mutate. */
const TokenList *synonym_table_lookup(const SynonymTable *table, const char *word);

void synonym_table_free(SynonymTable *table);

#endif /* LEXIS_SYNONYM_TABLE_H */
