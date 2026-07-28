/*
 * WordNet flat-file loader (spec 5.2.3, 6, build order Stage 5).
 * Preprocesses the WordNet flat file (data/wordnet/) into an in-memory
 * hash table at startup, avoiding libwn's API complexity. Provides
 * synonym/hypernym/hyponym lookups used by synonym expansion.
 */

#ifndef LEXIS_WORDNET_H
#define LEXIS_WORDNET_H

#include <stddef.h>

#include "tokenizer.h"

/* WordNet's four parts of speech, each stored in its own pair of flat
 * files (index.<pos>, data.<pos>) sharing an identical format. */
typedef enum {
    WORDNET_NOUN,
    WORDNET_VERB,
    WORDNET_ADJECTIVE,
    WORDNET_ADVERB
} WordNetPOS;

/* One synset (synonym set) parsed from a data.<pos> file: the words that
 * are interchangeable in this specific sense (the actual synonyms),
 * plus the byte offsets of its directly broader (hypernym) and narrower
 * (hyponym) synsets -- still unresolved to words at this stage;
 * resolution to actual word lists happens once every synset is loaded
 * (see wordnet_table_load). `offset` is this synset's own byte position
 * in its data file -- unique only within one part of speech, not
 * globally, since each POS has its own separate file. Assumes hypernym/
 * hyponym pointer targets share the source synset's part of speech,
 * which holds in practice for these two relations even though the file
 * format technically allows cross-POS pointers. */
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

/* Parses one line of a data.<pos> file into a synset. `pos` is supplied
 * by the caller (which file this line came from), not derived from the
 * line's own ss_type field (a finer-grained distinction, e.g. adjective
 * vs adjective-satellite, not needed here). Only hypernym ('@') and
 * hyponym ('~') pointers are kept -- other relations (antonym, meronym,
 * holonym, etc.) are outside this module's scope per the spec. Returns
 * NULL on a malformed line or allocation failure. */
WordNetSynset *wordnet_parse_data_line(const char *line, WordNetPOS pos);

/* Frees a synset's words array, hypernym/hyponym offset arrays, and the
 * struct itself. Safe to call with synset == NULL. */
void wordnet_synset_free(WordNetSynset *synset);

/* Every synset from one data.<pos> file, sorted by offset for O(log n)
 * lookup via wordnet_synset_index_find(). Intermediate structure used
 * only while resolving hypernym/hyponym pointers to actual words at load
 * time -- not what callers query at runtime (see wordnet_table_load /
 * wordnet_lookup, still to come). */
typedef struct {
    WordNetSynset **synsets;
    size_t count;
} WordNetSynsetIndex;

/* Reads and parses every synset in `path` (a data.<pos> file), sorted by
 * offset. Skips the file's leading copyright-comment lines (they start
 * with a space; real data lines never do) and blank lines. Treats any
 * unparseable data line as a hard failure rather than skipping it --
 * this is trusted, official WordNet data, so a parse failure here means
 * a real bug, not malformed input worth silently tolerating. Returns
 * NULL on a file read failure, a parse failure, or allocation failure. */
WordNetSynsetIndex *wordnet_load_data_file(const char *path, WordNetPOS pos);

/* Frees every synset in the index, the synsets array, and the struct
 * itself. Safe to call with index == NULL. */
void wordnet_synset_index_free(WordNetSynsetIndex *index);

/* Binary-searches `index` for the synset at `offset`. Returns NULL if no
 * synset in this index has that offset. */
const WordNetSynset *wordnet_synset_index_find(const WordNetSynsetIndex *index, long offset);

/* One word's entry from an index.<pos> file: the word itself (always
 * lowercase, multi-word lemmas use underscores, e.g. "united_states"),
 * plus the byte offset of every synset it belongs to -- one per distinct
 * sense/meaning. Still just offsets here, not resolved to actual
 * synonym/hypernym/hyponym word lists; that happens once both this and
 * the matching WordNetSynsetIndex are loaded. */
typedef struct {
    char *lemma;
    long *synset_offsets;
    size_t synset_count;
} WordNetIndexEntry;

/* Parses one line of an index.<pos> file. Every field in this file is
 * decimal (unlike data.<pos>, which mixes in a couple of hexadecimal
 * fields) -- see wordnet_parse_data_line's comment for why that
 * distinction matters. Returns NULL on a malformed line or allocation
 * failure. */
WordNetIndexEntry *wordnet_parse_index_line(const char *line);

/* Frees an entry's lemma string, its synset_offsets array, and the
 * struct itself. Safe to call with entry == NULL. */
void wordnet_index_entry_free(WordNetIndexEntry *entry);

/* Every word from one index.<pos> file, sorted alphabetically by lemma
 * for O(log n) lookup via wordnet_word_index_find(). Same role as
 * WordNetSynsetIndex, just keyed by word instead of by offset. */
typedef struct {
    WordNetIndexEntry **entries;
    size_t count;
} WordNetWordIndex;

/* Reads and parses every entry in `path` (an index.<pos> file), sorted
 * by lemma. Same header/blank-line-skipping and hard-fail-on-malformed-
 * line behavior as wordnet_load_data_file, for the same reasons. Returns
 * NULL on a file read failure, a parse failure, or allocation failure. */
WordNetWordIndex *wordnet_load_index_file(const char *path);

/* Frees every entry in the index, the entries array, and the struct
 * itself. Safe to call with index == NULL. */
void wordnet_word_index_free(WordNetWordIndex *index);

/* Binary-searches `index` for `lemma`. Returns NULL if the word isn't
 * present in this index. */
const WordNetIndexEntry *wordnet_word_index_find(const WordNetWordIndex *index, const char *lemma);

/* One word's fully-resolved lookup result: every synonym, hypernym, and
 * hyponym across ALL of the word's senses, merged together and
 * deduplicated -- not grouped per sense. Sense disambiguation (deciding
 * which of a word's meanings actually applies to a given query) is a
 * downstream concern that needs query context this module doesn't have
 * (spec 5.2.3's small-model disambiguation step); this hands back raw
 * candidate data, not a ranked or sense-separated result. `next` is an
 * internal hash-bucket chaining pointer -- callers should ignore it. */
typedef struct WordNetLookupResult {
    char *word;
    TokenList *synonyms;
    TokenList *hypernyms;
    TokenList *hyponyms;
    struct WordNetLookupResult *next;
} WordNetLookupResult;

/* The final, query-facing hash table -- what wordnet_table_load() builds
 * (still to come) and wordnet_lookup() queries. A simple chained-bucket
 * hash table keyed by word. */
typedef struct {
    WordNetLookupResult **buckets;
    size_t bucket_count;
} WordNetTable;

/* Allocates an empty table with a fixed bucket count sized for the whole
 * of WordNet (~150K words across all parts of speech). Returns NULL on
 * allocation failure. */
WordNetTable *wordnet_table_create(void);

/* Frees every entry in every bucket (including each entry's synonym/
 * hypernym/hyponym lists), the bucket array, and the table itself. Safe
 * to call with table == NULL. */
void wordnet_table_free(WordNetTable *table);

/* Looks up `word` in the table. Returns NULL if the word was never seen
 * while loading (i.e. it's not in WordNet at all). */
const WordNetLookupResult *wordnet_lookup(const WordNetTable *table, const char *word);

/* Loads all four parts of speech from `wordnet_dir` (expects
 * index.<pos>/data.<pos> pairs directly inside it, e.g. data/wordnet/)
 * and resolves every word into one final table -- this is the actual
 * "preprocess WordNet into a hash table at startup" the spec describes.
 * The per-POS index/synset scaffolding used along the way is discarded
 * once each part of speech's words are resolved; only the final table
 * survives. Returns NULL on any file load or allocation failure. */
WordNetTable *wordnet_table_load(const char *wordnet_dir);

#endif /* LEXIS_WORDNET_H */
