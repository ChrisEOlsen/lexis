/*
 * Implementation of the learned synonym table.
 * See include/synonym_table.h for the module's role. Chained-bucket hash
 * keyed by word, same shape as wordnet.c's table -- sized once at load
 * from the row count, no rehashing.
 */

#define _POSIX_C_SOURCE 200809L

#include "synonym_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SynonymEntry {
    char *word;
    TokenList *neighbors;
    struct SynonymEntry *next;
} SynonymEntry;

struct SynonymTable {
    SynonymEntry **buckets;
    size_t bucket_count;
};

/* djb2 -- same function family the other tables in this project use. */
static size_t hash_word(const char *word, size_t bucket_count) {
    size_t hash = 5381;
    for (const unsigned char *p = (const unsigned char *)word; *p != '\0'; p++) {
        hash = ((hash << 5) + hash) + *p;
    }
    return hash % bucket_count;
}

static void entry_free(SynonymEntry *entry) {
    while (entry != NULL) {
        SynonymEntry *next = entry->next;
        free(entry->word);
        token_list_free(entry->neighbors);
        free(entry);
        entry = next;
    }
}

void synonym_table_free(SynonymTable *table) {
    if (table == NULL) {
        return;
    }
    for (size_t i = 0; i < table->bucket_count; i++) {
        entry_free(table->buckets[i]);
    }
    free(table->buckets);
    free(table);
}

SynonymTable *synonym_table_load(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        /* Optional data file -- missing is a normal, quiet state. */
        return NULL;
    }

    SynonymTable *table = malloc(sizeof(SynonymTable));
    if (table == NULL) {
        fclose(fp);
        return NULL;
    }
    /* ~44K rows in the shipped file; 65536 buckets keeps chains short. */
    table->bucket_count = 65536;
    table->buckets = calloc(table->bucket_count, sizeof(SynonymEntry *));
    if (table->buckets == NULL) {
        free(table);
        fclose(fp);
        return NULL;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            continue;
        }
        *tab = '\0';
        char *rest = tab + 1;
        rest[strcspn(rest, "\n")] = '\0';

        TokenList *neighbors = token_list_create();
        if (neighbors == NULL) {
            goto fail;
        }
        char *saveptr;
        for (char *tok = strtok_r(rest, " ", &saveptr); tok != NULL;
             tok = strtok_r(NULL, " ", &saveptr)) {
            if (token_list_append(neighbors, tok) != 0) {
                token_list_free(neighbors);
                goto fail;
            }
        }
        if (neighbors->count == 0) {
            token_list_free(neighbors);
            continue;
        }

        SynonymEntry *entry = malloc(sizeof(SynonymEntry));
        char *word = strdup(line);
        if (entry == NULL || word == NULL) {
            free(entry);
            free(word);
            token_list_free(neighbors);
            goto fail;
        }
        entry->word = word;
        entry->neighbors = neighbors;
        size_t bucket = hash_word(word, table->bucket_count);
        entry->next = table->buckets[bucket];
        table->buckets[bucket] = entry;
    }

    fclose(fp);
    return table;

fail:
    fclose(fp);
    synonym_table_free(table);
    return NULL;
}

const TokenList *synonym_table_lookup(const SynonymTable *table, const char *word) {
    if (table == NULL || word == NULL) {
        return NULL;
    }
    for (const SynonymEntry *entry = table->buckets[hash_word(word, table->bucket_count)];
         entry != NULL; entry = entry->next) {
        if (strcmp(entry->word, word) == 0) {
            return entry->neighbors;
        }
    }
    return NULL;
}
