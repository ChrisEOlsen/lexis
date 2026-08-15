/*
 * Learned synonym table: precomputed nearest neighbors from published
 * word embeddings (see scripts/build_synonym_table.py), shipped as a
 * data file exactly like WordNet -- data/synonyms/learned_neighbors.tsv,
 * "word<TAB>neighbor neighbor ..." rows.
 *
 * Covers the relation WordNet structurally can't: words used in the same
 * contexts without a dictionary link ("functions" ~ "controls"). Feeds
 * query expansion as one more candidate list; the LLM sense filter and
 * the 0.4 expansion weight apply to these exactly as to WordNet
 * candidates, so a bad neighbor gets vetoed or discounted, never
 * dominates. Zero ingest cost, zero query-time model -- a hash lookup.
 *
 * A missing file is a normal state (the table is optional, like
 * config/lexis.conf): synonym_table_load() returns NULL quietly and
 * every consumer treats NULL as "no learned candidates".
 */

#ifndef LEXIS_SYNONYM_TABLE_H
#define LEXIS_SYNONYM_TABLE_H

#include "tokenizer.h"

#define LEXIS_SYNONYMS_PATH_DEFAULT "data/synonyms/learned_neighbors.tsv"

typedef struct SynonymTable SynonymTable;

/* NULL on missing/unreadable file (quietly -- optional data) or
 * allocation failure. */
SynonymTable *synonym_table_load(const char *path);

/* The word's precomputed neighbors, or NULL if the word has none.
 * Returned list is owned by the table -- do not free or mutate. */
const TokenList *synonym_table_lookup(const SynonymTable *table, const char *word);

void synonym_table_free(SynonymTable *table);

#endif /* LEXIS_SYNONYM_TABLE_H */
