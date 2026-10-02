/* WordNet flat-file loader: preprocess data/wordnet/ into an in-memory lookup table at startup. */

#ifndef LEXIS_WORDNET_H
#define LEXIS_WORDNET_H

#include <stddef.h>

#include "tokenizer.h"

/* WordNet's four parts of speech, each in its own index.<pos>/data.<pos> file pair. */
typedef enum {
    WORDNET_NOUN,
    WORDNET_VERB,
    WORDNET_ADJECTIVE,
    WORDNET_ADVERB
} WordNetPOS;

/* One synset: sense words + hypernym/hyponym offsets (unresolved until load completes).
 * offset unique within its POS file only; targets assumed same-POS. */
typedef struct {
    WordNetPOS pos;
    long offset;
    char **words;
    size_t word_count;
    long *hypernym_offsets;
    size_t hypernym_count;
    long *hyponym_offsets;
    size_t hyponym_count;
} WordNetSynset;

/* Parse one data.<pos> line (pos from caller, not ss_type). Keeps @/~ pointers only.
 * NULL on malformed line or alloc failure. */
WordNetSynset *wordnet_parse_data_line(const char *line, WordNetPOS pos);

/* Free words, offset arrays, and struct. Safe with synset == NULL. */
void wordnet_synset_free(WordNetSynset *synset);

/* One data.<pos> file's synsets, sorted by offset. Load-time intermediate; not queried at runtime. */
typedef struct {
    WordNetSynset **synsets;
    size_t count;
} WordNetSynsetIndex;

/* Load every synset in path, sorted by offset. Skips comments/blanks; any bad line fails hard.
 * NULL on file/parse/alloc failure. */
WordNetSynsetIndex *wordnet_load_data_file(const char *path, WordNetPOS pos);

/* Free every synset, the array, and the struct. Safe with index == NULL. */
void wordnet_synset_index_free(WordNetSynsetIndex *index);

/* Binary-search index for the synset at offset. NULL if absent. */
const WordNetSynset *wordnet_synset_index_find(const WordNetSynsetIndex *index, long offset);

/* One index.<pos> entry: lowercase lemma + its synsets' offsets (one per sense; unresolved here). */
typedef struct {
    char *lemma;
    long *synset_offsets;
    size_t synset_count;
} WordNetIndexEntry;

/* Parse one index.<pos> line (all fields decimal). NULL on malformed line or alloc failure. */
WordNetIndexEntry *wordnet_parse_index_line(const char *line);

/* Free lemma, offsets, and struct. Safe with entry == NULL. */
void wordnet_index_entry_free(WordNetIndexEntry *entry);

/* One index.<pos> file's entries, sorted by lemma. Load-time intermediate, like WordNetSynsetIndex. */
typedef struct {
    WordNetIndexEntry **entries;
    size_t count;
} WordNetWordIndex;

/* Load every entry in path, sorted by lemma. Same skip/fail-hard rules. NULL on failure. */
WordNetWordIndex *wordnet_load_index_file(const char *path);

/* Free every entry, the array, and the struct. Safe with index == NULL. */
void wordnet_word_index_free(WordNetWordIndex *index);

/* Binary-search index for lemma. NULL if absent. */
const WordNetIndexEntry *wordnet_word_index_find(const WordNetWordIndex *index, const char *lemma);

/* One word's resolved result: synonyms/hypernyms/hyponyms merged across all senses, deduped.
 * next is an internal bucket pointer; callers ignore it. */
typedef struct WordNetLookupResult {
    char *word;
    TokenList *synonyms;
    TokenList *hypernyms;
    TokenList *hyponyms;
    struct WordNetLookupResult *next;
} WordNetLookupResult;

/* Query-facing hash table: chained buckets keyed by word. */
typedef struct {
    WordNetLookupResult **buckets;
    size_t bucket_count;
} WordNetTable;

/* Empty table, bucket count sized for all of WordNet (~150K words). NULL on alloc failure. */
WordNetTable *wordnet_table_create(void);

/* Free every entry, the bucket array, and the table. Safe with table == NULL. */
void wordnet_table_free(WordNetTable *table);

/* Look up word. NULL if not in WordNet. */
const WordNetLookupResult *wordnet_lookup(const WordNetTable *table, const char *word);

/* Load all four POS pairs from wordnet_dir into one resolved table. NULL on file/alloc failure. */
WordNetTable *wordnet_table_load(const char *wordnet_dir);

#endif /* LEXIS_WORDNET_H */
