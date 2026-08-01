/*
 * Implementation of the WordNet flat-file loader.
 * See include/wordnet.h for the module's role (spec 5.2.3, Stage 5).
 */

/* See tokenizer.c for why this must come before any #include (strdup and
 * strtok_r are POSIX extensions hidden by glibc under strict -std=c11
 * otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "wordnet.h"

#include "ingest.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORDNET_HYPERNYM_SYMBOL "@"
#define WORDNET_HYPONYM_SYMBOL "~"

void wordnet_synset_free(WordNetSynset *synset) {
    if (synset == NULL) {
        return;
    }

    if (synset->words != NULL) {
        for (size_t i = 0; i < synset->word_count; i++) {
            free(synset->words[i]);
        }
        free(synset->words);
    }
    free(synset->hypernym_offsets);
    free(synset->hyponym_offsets);
    free(synset);
}

/* Field order (confirmed against real WordNet 3.0 data files and the
 * official wndb(5WN) format spec):
 *   synset_offset  lex_filenum  ss_type  w_cnt  [word  lex_id]*w_cnt
 *   p_cnt  [ptr_symbol  synset_offset  pos  source/target]*p_cnt
 *   [frame data for verbs...]  |  gloss
 *
 * w_cnt and lex_id are TWO-DIGIT HEXADECIMAL; synset_offset, lex_filenum,
 * and p_cnt are DECIMAL. Getting that backwards silently misparses every
 * line without crashing, so it's called out explicitly here rather than
 * left implicit in the strtol() base arguments below. */
WordNetSynset *wordnet_parse_data_line(const char *line, WordNetPOS pos) {
    char *mutable_line = strdup(line);
    if (mutable_line == NULL) {
        return NULL;
    }

    WordNetSynset *synset = calloc(1, sizeof(WordNetSynset));
    if (synset == NULL) {
        free(mutable_line);
        return NULL;
    }
    synset->pos = pos;

    char *saveptr;
    char *token = strtok_r(mutable_line, " ", &saveptr);
    if (token == NULL) {
        goto fail;
    }
    synset->offset = strtol(token, NULL, 10);

    token = strtok_r(NULL, " ", &saveptr); /* lex_filenum, unused */
    if (token == NULL) {
        goto fail;
    }

    token = strtok_r(NULL, " ", &saveptr); /* ss_type, unused -- pos comes from the caller */
    if (token == NULL) {
        goto fail;
    }

    token = strtok_r(NULL, " ", &saveptr); /* w_cnt -- hexadecimal */
    if (token == NULL) {
        goto fail;
    }
    long word_count = strtol(token, NULL, 16);
    if (word_count <= 0) {
        goto fail;
    }
    synset->word_count = (size_t)word_count;

    synset->words = malloc(synset->word_count * sizeof(char *));
    if (synset->words == NULL) {
        goto fail;
    }
    /* Zero every slot up front so a strdup failure partway through the
     * loop below leaves the rest NULL -- free(NULL) is a safe no-op, so
     * wordnet_synset_free() can unconditionally free every slot up to
     * word_count without needing to track how far the loop actually got. */
    for (size_t i = 0; i < synset->word_count; i++) {
        synset->words[i] = NULL;
    }

    for (size_t i = 0; i < synset->word_count; i++) {
        token = strtok_r(NULL, " ", &saveptr); /* word */
        if (token == NULL) {
            goto fail;
        }
        synset->words[i] = strdup(token);
        if (synset->words[i] == NULL) {
            goto fail;
        }

        token = strtok_r(NULL, " ", &saveptr); /* lex_id, unused */
        if (token == NULL) {
            goto fail;
        }
    }

    token = strtok_r(NULL, " ", &saveptr); /* p_cnt -- decimal */
    if (token == NULL) {
        goto fail;
    }
    long pointer_count = strtol(token, NULL, 10);
    if (pointer_count < 0) {
        goto fail;
    }

    /* Over-allocate to pointer_count -- at most this many pointers can be
     * hypernyms and at most this many can be hyponyms; most pointers are
     * neither (antonym, meronym, holonym, etc. are out of scope here), so
     * actual counts are usually much smaller. Avoids a second counting pass. */
    if (pointer_count > 0) {
        synset->hypernym_offsets = malloc((size_t)pointer_count * sizeof(long));
        synset->hyponym_offsets = malloc((size_t)pointer_count * sizeof(long));
        if (synset->hypernym_offsets == NULL || synset->hyponym_offsets == NULL) {
            goto fail;
        }
    }

    for (long i = 0; i < pointer_count; i++) {
        token = strtok_r(NULL, " ", &saveptr); /* ptr_symbol */
        if (token == NULL) {
            goto fail;
        }
        int is_hypernym = (strcmp(token, WORDNET_HYPERNYM_SYMBOL) == 0);
        int is_hyponym = (strcmp(token, WORDNET_HYPONYM_SYMBOL) == 0);

        char *offset_token = strtok_r(NULL, " ", &saveptr); /* target synset_offset -- decimal */
        if (offset_token == NULL) {
            goto fail;
        }

        token = strtok_r(NULL, " ", &saveptr); /* target pos, unused */
        if (token == NULL) {
            goto fail;
        }

        token = strtok_r(NULL, " ", &saveptr); /* source/target word field, unused */
        if (token == NULL) {
            goto fail;
        }

        if (is_hypernym) {
            synset->hypernym_offsets[synset->hypernym_count++] = strtol(offset_token, NULL, 10);
        } else if (is_hyponym) {
            synset->hyponym_offsets[synset->hyponym_count++] = strtol(offset_token, NULL, 10);
        }
    }

    free(mutable_line);
    return synset;

fail:
    free(mutable_line);
    wordnet_synset_free(synset);
    return NULL;
}

void wordnet_synset_index_free(WordNetSynsetIndex *index) {
    if (index == NULL) {
        return;
    }

    for (size_t i = 0; i < index->count; i++) {
        wordnet_synset_free(index->synsets[i]);
    }
    free(index->synsets);
    free(index);
}

/* qsort() comparator: array elements are WordNetSynset*, so each `const
 * void *` argument actually points at a WordNetSynset* slot in the
 * array -- hence the double pointer cast. */
static int wordnet_compare_synset_offsets(const void *a, const void *b) {
    const WordNetSynset *const *synset_a = (const WordNetSynset *const *)a;
    const WordNetSynset *const *synset_b = (const WordNetSynset *const *)b;
    if ((*synset_a)->offset < (*synset_b)->offset) {
        return -1;
    }
    if ((*synset_a)->offset > (*synset_b)->offset) {
        return 1;
    }
    return 0;
}

/* bsearch() comparator: the key is a bare `long` (the offset being
 * searched for), while each array element is still a WordNetSynset* --
 * key and element are different types here, which bsearch() supports as
 * long as the comparator treats each side correctly. */
static int wordnet_compare_offset_to_synset(const void *key, const void *element) {
    long target_offset = *(const long *)key;
    const WordNetSynset *const *synset_ptr = (const WordNetSynset *const *)element;
    long element_offset = (*synset_ptr)->offset;
    if (target_offset < element_offset) {
        return -1;
    }
    if (target_offset > element_offset) {
        return 1;
    }
    return 0;
}

const WordNetSynset *wordnet_synset_index_find(const WordNetSynsetIndex *index, long offset) {
    WordNetSynset **found = bsearch(&offset, index->synsets, index->count,
                                     sizeof(WordNetSynset *), wordnet_compare_offset_to_synset);
    return found != NULL ? *found : NULL;
}

WordNetSynsetIndex *wordnet_load_data_file(const char *path, WordNetPOS pos) {
    char *text = ingest_read_file(path);
    if (text == NULL) {
        return NULL;
    }

    WordNetSynsetIndex *index = malloc(sizeof(WordNetSynsetIndex));
    if (index == NULL) {
        free(text);
        return NULL;
    }
    index->synsets = NULL;
    index->count = 0;
    size_t capacity = 0;

    char *saveptr;
    char *line = strtok_r(text, "\n", &saveptr);
    while (line != NULL) {
        /* Real data lines always start with a digit (the synset_offset);
         * the file's leading copyright comment lines start with a space.
         * Skip both those and any blank line the split might produce. */
        if (line[0] != ' ' && line[0] != '\0') {
            WordNetSynset *synset = wordnet_parse_data_line(line, pos);
            if (synset == NULL) {
                fprintf(stderr, "wordnet_load_data_file: failed to parse a line in %s\n", path);
                free(text);
                wordnet_synset_index_free(index);
                return NULL;
            }

            if (index->count == capacity) {
                size_t new_capacity = (capacity == 0) ? 64 : capacity * 2;
                WordNetSynset **new_synsets =
                    realloc(index->synsets, new_capacity * sizeof(WordNetSynset *));
                if (new_synsets == NULL) {
                    wordnet_synset_free(synset);
                    free(text);
                    wordnet_synset_index_free(index);
                    return NULL;
                }
                index->synsets = new_synsets;
                capacity = new_capacity;
            }

            index->synsets[index->count++] = synset;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(text);

    qsort(index->synsets, index->count, sizeof(WordNetSynset *), wordnet_compare_synset_offsets);

    return index;
}

void wordnet_index_entry_free(WordNetIndexEntry *entry) {
    if (entry == NULL) {
        return;
    }

    free(entry->lemma);
    free(entry->synset_offsets);
    free(entry);
}

/* Field order (index.<pos>, confirmed against the official wndb(5WN)
 * spec): lemma pos synset_cnt p_cnt [ptr_symbol]*p_cnt sense_cnt
 * tagsense_cnt synset_offset*synset_cnt -- every field here is decimal,
 * unlike data.<pos>'s w_cnt/lex_id, so no base-16 parsing is needed. */
WordNetIndexEntry *wordnet_parse_index_line(const char *line) {
    char *mutable_line = strdup(line);
    if (mutable_line == NULL) {
        return NULL;
    }

    WordNetIndexEntry *entry = calloc(1, sizeof(WordNetIndexEntry));
    if (entry == NULL) {
        free(mutable_line);
        return NULL;
    }

    char *saveptr;
    char *token = strtok_r(mutable_line, " ", &saveptr); /* lemma */
    if (token == NULL) {
        goto fail;
    }
    entry->lemma = strdup(token);
    if (entry->lemma == NULL) {
        goto fail;
    }

    token = strtok_r(NULL, " ", &saveptr); /* pos, unused -- caller already knows it */
    if (token == NULL) {
        goto fail;
    }

    token = strtok_r(NULL, " ", &saveptr); /* synset_cnt -- decimal */
    if (token == NULL) {
        goto fail;
    }
    long synset_count = strtol(token, NULL, 10);
    if (synset_count <= 0) {
        goto fail;
    }
    entry->synset_count = (size_t)synset_count;

    token = strtok_r(NULL, " ", &saveptr); /* p_cnt -- decimal */
    if (token == NULL) {
        goto fail;
    }
    long pointer_symbol_count = strtol(token, NULL, 10);
    if (pointer_symbol_count < 0) {
        goto fail;
    }

    for (long i = 0; i < pointer_symbol_count; i++) {
        token = strtok_r(NULL, " ", &saveptr); /* ptr_symbol -- just skipped over here */
        if (token == NULL) {
            goto fail;
        }
    }

    token = strtok_r(NULL, " ", &saveptr); /* sense_cnt, unused (redundant with synset_cnt) */
    if (token == NULL) {
        goto fail;
    }

    token = strtok_r(NULL, " ", &saveptr); /* tagsense_cnt, unused */
    if (token == NULL) {
        goto fail;
    }

    entry->synset_offsets = malloc(entry->synset_count * sizeof(long));
    if (entry->synset_offsets == NULL) {
        goto fail;
    }

    for (size_t i = 0; i < entry->synset_count; i++) {
        token = strtok_r(NULL, " ", &saveptr);
        if (token == NULL) {
            goto fail;
        }
        entry->synset_offsets[i] = strtol(token, NULL, 10);
    }

    free(mutable_line);
    return entry;

fail:
    free(mutable_line);
    wordnet_index_entry_free(entry);
    return NULL;
}

void wordnet_word_index_free(WordNetWordIndex *index) {
    if (index == NULL) {
        return;
    }

    for (size_t i = 0; i < index->count; i++) {
        wordnet_index_entry_free(index->entries[i]);
    }
    free(index->entries);
    free(index);
}

/* qsort() comparator: array elements are WordNetIndexEntry*, sorted
 * alphabetically by lemma. */
static int wordnet_compare_index_entries(const void *a, const void *b) {
    const WordNetIndexEntry *const *entry_a = (const WordNetIndexEntry *const *)a;
    const WordNetIndexEntry *const *entry_b = (const WordNetIndexEntry *const *)b;
    return strcmp((*entry_a)->lemma, (*entry_b)->lemma);
}

/* bsearch() comparator: the key is a bare `const char *` lemma being
 * searched for, while each array element is still a WordNetIndexEntry*. */
static int wordnet_compare_lemma_to_entry(const void *key, const void *element) {
    const char *target_lemma = (const char *)key;
    const WordNetIndexEntry *const *entry_ptr = (const WordNetIndexEntry *const *)element;
    return strcmp(target_lemma, (*entry_ptr)->lemma);
}

const WordNetIndexEntry *wordnet_word_index_find(const WordNetWordIndex *index, const char *lemma) {
    WordNetIndexEntry **found = bsearch(lemma, index->entries, index->count,
                                         sizeof(WordNetIndexEntry *), wordnet_compare_lemma_to_entry);
    return found != NULL ? *found : NULL;
}

WordNetWordIndex *wordnet_load_index_file(const char *path) {
    char *text = ingest_read_file(path);
    if (text == NULL) {
        return NULL;
    }

    WordNetWordIndex *index = malloc(sizeof(WordNetWordIndex));
    if (index == NULL) {
        free(text);
        return NULL;
    }
    index->entries = NULL;
    index->count = 0;
    size_t capacity = 0;

    char *saveptr;
    char *line = strtok_r(text, "\n", &saveptr);
    while (line != NULL) {
        if (line[0] != ' ' && line[0] != '\0') {
            WordNetIndexEntry *entry = wordnet_parse_index_line(line);
            if (entry == NULL) {
                fprintf(stderr, "wordnet_load_index_file: failed to parse a line in %s\n", path);
                free(text);
                wordnet_word_index_free(index);
                return NULL;
            }

            if (index->count == capacity) {
                size_t new_capacity = (capacity == 0) ? 64 : capacity * 2;
                WordNetIndexEntry **new_entries =
                    realloc(index->entries, new_capacity * sizeof(WordNetIndexEntry *));
                if (new_entries == NULL) {
                    wordnet_index_entry_free(entry);
                    free(text);
                    wordnet_word_index_free(index);
                    return NULL;
                }
                index->entries = new_entries;
                capacity = new_capacity;
            }

            index->entries[index->count++] = entry;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(text);

    qsort(index->entries, index->count, sizeof(WordNetIndexEntry *), wordnet_compare_index_entries);

    return index;
}

/* Bucket count for the final table -- WordNet has roughly 150K distinct
 * words across all four parts of speech combined (fewer than the sum of
 * each file's entry count, since many words appear in more than one),
 * so this gives a load factor of roughly 1-2 entries per bucket. Fixed
 * rather than configurable: WordNet's size doesn't vary at runtime, so
 * there's nothing for a caller to actually tune. */
#define WORDNET_TABLE_BUCKET_COUNT 100003

/* djb2 (Dan Bernstein's well-known string hash): starts from an
 * arbitrary "magic" seed and repeatedly does hash = hash*33 + next_byte.
 * Cheap to compute, no real theory behind why 33 works well beyond
 * decades of empirical use, but it distributes ASCII text like word
 * lemmas into buckets evenly enough for a simple chained hash table --
 * the first hash function this project has needed, since every prior
 * lookup structure (StopwordSet, WordNetSynsetIndex, WordNetWordIndex)
 * used a sorted array + binary search instead. A hash table is the right
 * fit here because the final structure is queried over and over at
 * runtime by arbitrary query terms, rather than built once and then
 * mostly iterated in order like the sorted arrays above. */
static unsigned long wordnet_hash_string(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = (unsigned char)*str++) != 0) {
        hash = ((hash << 5) + hash) + (unsigned long)c;
    }
    return hash;
}

WordNetTable *wordnet_table_create(void) {
    WordNetTable *table = malloc(sizeof(WordNetTable));
    if (table == NULL) {
        return NULL;
    }

    table->bucket_count = WORDNET_TABLE_BUCKET_COUNT;
    table->buckets = calloc(table->bucket_count, sizeof(WordNetLookupResult *));
    if (table->buckets == NULL) {
        free(table);
        return NULL;
    }

    return table;
}

void wordnet_table_free(WordNetTable *table) {
    if (table == NULL) {
        return;
    }

    for (size_t i = 0; i < table->bucket_count; i++) {
        WordNetLookupResult *entry = table->buckets[i];
        while (entry != NULL) {
            WordNetLookupResult *next = entry->next;
            free(entry->word);
            token_list_free(entry->synonyms);
            token_list_free(entry->hypernyms);
            token_list_free(entry->hyponyms);
            free(entry);
            entry = next;
        }
    }
    free(table->buckets);
    free(table);
}

const WordNetLookupResult *wordnet_lookup(const WordNetTable *table, const char *word) {
    size_t bucket = wordnet_hash_string(word) % table->bucket_count;

    for (WordNetLookupResult *entry = table->buckets[bucket]; entry != NULL; entry = entry->next) {
        if (strcmp(entry->word, word) == 0) {
            return entry;
        }
    }

    return NULL;
}

/* Finds `word`'s entry in `table`, creating an empty one (three empty
 * TokenLists, chained into the right bucket) if this is the first time
 * `word` has been seen -- e.g. across different parts of speech, since
 * wordnet_table_load() (still to come) will call this once per word per
 * POS, and a word like "dog" appears in both index.noun and index.verb.
 * Returns NULL on allocation failure. */
static WordNetLookupResult *wordnet_table_find_or_create(WordNetTable *table, const char *word) {
    size_t bucket = wordnet_hash_string(word) % table->bucket_count;

    for (WordNetLookupResult *entry = table->buckets[bucket]; entry != NULL; entry = entry->next) {
        if (strcmp(entry->word, word) == 0) {
            return entry;
        }
    }

    WordNetLookupResult *entry = calloc(1, sizeof(WordNetLookupResult));
    if (entry == NULL) {
        return NULL;
    }

    entry->word = strdup(word);
    entry->synonyms = token_list_create();
    entry->hypernyms = token_list_create();
    entry->hyponyms = token_list_create();
    if (entry->word == NULL || entry->synonyms == NULL || entry->hypernyms == NULL ||
        entry->hyponyms == NULL) {
        free(entry->word);
        token_list_free(entry->synonyms);
        token_list_free(entry->hypernyms);
        token_list_free(entry->hyponyms);
        free(entry);
        return NULL;
    }

    entry->next = table->buckets[bucket];
    table->buckets[bucket] = entry;
    return entry;
}

/* Appends `word` to `list` only if it isn't already present -- a linear
 * scan, same tradeoff as bm25_result_set_add's and
 * ingest_count_distinct_terms's dedup checks: candidate lists here are a
 * handful of words at most, so O(n) per add is negligible. Returns 0 on
 * success (whether or not it was already present), -1 on allocation
 * failure. */
static int wordnet_token_list_add_unique(TokenList *list, const char *word) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->terms[i], word) == 0) {
            return 0;
        }
    }
    return token_list_append(list, word);
}

/* Resolves one word's candidates from `entry` (its synsets in one part
 * of speech) against `synsets` (every synset loaded for that SAME part
 * of speech), merging the result into `table`. Synonyms come from the
 * other words sharing a synset with this one; hypernyms/hyponyms come
 * from following each synset's hypernym/hyponym offsets to their target
 * synsets and collecting THEIR words. Merges into an existing table
 * entry (deduplicated) if `entry->lemma` was already added from a
 * different part of speech. Returns 0 on success, -1 on allocation
 * failure. */
static int wordnet_resolve_and_merge(WordNetTable *table, const WordNetIndexEntry *entry,
                                      const WordNetSynsetIndex *synsets) {
    WordNetLookupResult *result = wordnet_table_find_or_create(table, entry->lemma);
    if (result == NULL) {
        return -1;
    }

    for (size_t i = 0; i < entry->synset_count; i++) {
        const WordNetSynset *synset = wordnet_synset_index_find(synsets, entry->synset_offsets[i]);
        if (synset == NULL) {
            continue; /* shouldn't happen with real data; don't crash if it somehow does */
        }

        for (size_t j = 0; j < synset->word_count; j++) {
            if (strcmp(synset->words[j], entry->lemma) == 0) {
                continue; /* don't list the word as its own synonym */
            }
            if (wordnet_token_list_add_unique(result->synonyms, synset->words[j]) != 0) {
                return -1;
            }
        }

        for (size_t j = 0; j < synset->hypernym_count; j++) {
            const WordNetSynset *hypernym_synset =
                wordnet_synset_index_find(synsets, synset->hypernym_offsets[j]);
            if (hypernym_synset == NULL) {
                continue;
            }
            for (size_t k = 0; k < hypernym_synset->word_count; k++) {
                if (wordnet_token_list_add_unique(result->hypernyms, hypernym_synset->words[k]) != 0) {
                    return -1;
                }
            }
        }

        for (size_t j = 0; j < synset->hyponym_count; j++) {
            const WordNetSynset *hyponym_synset =
                wordnet_synset_index_find(synsets, synset->hyponym_offsets[j]);
            if (hyponym_synset == NULL) {
                continue;
            }
            for (size_t k = 0; k < hyponym_synset->word_count; k++) {
                if (wordnet_token_list_add_unique(result->hyponyms, hyponym_synset->words[k]) != 0) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

/* Maps a part of speech to the filename suffix WordNet's own files use
 * (index.noun/data.noun, index.verb/data.verb, etc.). All four
 * WordNetPOS values are covered; the trailing return is unreachable in
 * practice but keeps the function well-defined for any value regardless
 * of whether a given compiler can prove the switch is exhaustive. */
static const char *wordnet_pos_suffix(WordNetPOS pos) {
    switch (pos) {
        case WORDNET_NOUN:
            return "noun";
        case WORDNET_VERB:
            return "verb";
        case WORDNET_ADJECTIVE:
            return "adj";
        case WORDNET_ADVERB:
            return "adv";
    }
    return NULL;
}

/* Loads one part of speech's index.<pos>/data.<pos> pair and resolves
 * every word in it into `table`. Both the index and synset scaffolding
 * are freed before returning, regardless of success or failure -- only
 * `table` needs to survive past this call. Returns 0 on success, -1 on
 * any load or allocation failure. */
static int wordnet_load_pos_into_table(WordNetTable *table, const char *wordnet_dir, WordNetPOS pos) {
    const char *suffix = wordnet_pos_suffix(pos);

    char index_path[PATH_MAX];
    char data_path[PATH_MAX];
    snprintf(index_path, sizeof(index_path), "%s/index.%s", wordnet_dir, suffix);
    snprintf(data_path, sizeof(data_path), "%s/data.%s", wordnet_dir, suffix);

    WordNetWordIndex *word_index = wordnet_load_index_file(index_path);
    if (word_index == NULL) {
        return -1;
    }

    WordNetSynsetIndex *synset_index = wordnet_load_data_file(data_path, pos);
    if (synset_index == NULL) {
        wordnet_word_index_free(word_index);
        return -1;
    }

    int result = 0;
    for (size_t i = 0; i < word_index->count; i++) {
        if (wordnet_resolve_and_merge(table, word_index->entries[i], synset_index) != 0) {
            result = -1;
            break;
        }
    }

    wordnet_word_index_free(word_index);
    wordnet_synset_index_free(synset_index);
    return result;
}

WordNetTable *wordnet_table_load(const char *wordnet_dir) {
    WordNetTable *table = wordnet_table_create();
    if (table == NULL) {
        return NULL;
    }

    WordNetPOS all_pos[] = {WORDNET_NOUN, WORDNET_VERB, WORDNET_ADJECTIVE, WORDNET_ADVERB};
    for (size_t i = 0; i < sizeof(all_pos) / sizeof(all_pos[0]); i++) {
        if (wordnet_load_pos_into_table(table, wordnet_dir, all_pos[i]) != 0) {
            wordnet_table_free(table);
            return NULL;
        }
    }

    return table;
}
