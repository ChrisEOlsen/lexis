/* Stopword filtering (spec 5.2.1, Stage 1); see stopwords.h. */

/* Before any #include: exposes strdup under strict -std=c11 (see tokenizer.c). */
#define _POSIX_C_SOURCE 200809L

#include "stopwords.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STOPWORD_SET_INITIAL_CAPACITY 8

/* Per-word read cap; generous vs longest real entry ("yourselves", 11 chars). */
#define STOPWORD_LINE_BUFFER_SIZE 64

/* qsort/bsearch comparator: strcmp needs this signature; elements are char**. */
static int compare_words(const void *a, const void *b) {
    const char *word_a = *(const char *const *)a;
    const char *word_b = *(const char *const *)b;
    return strcmp(word_a, word_b);
}

StopwordSet *stopword_set_load(const char *path) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return NULL;
    }

    StopwordSet *set = malloc(sizeof(StopwordSet));
    if (set == NULL) {
        fclose(file);
        return NULL;
    }

    set->words = malloc(STOPWORD_SET_INITIAL_CAPACITY * sizeof(char *));
    if (set->words == NULL) {
        free(set);
        fclose(file);
        return NULL;
    }
    set->count = 0;
    size_t capacity = STOPWORD_SET_INITIAL_CAPACITY;

    char line[STOPWORD_LINE_BUFFER_SIZE];
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t len = strlen(line);

        /* Truncated long comment line: consume the rest so its back half isn't read as a word. */
        if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
            int c;
            while ((c = fgetc(file)) != '\n' && c != EOF) {
            }
        }

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (len == 0 || line[0] == '#') {
            continue;
        }

        if (set->count == capacity) {
            capacity *= 2;
            char **new_words = realloc(set->words, capacity * sizeof(char *));
            if (new_words == NULL) {
                stopword_set_free(set);
                fclose(file);
                return NULL;
            }
            set->words = new_words;
        }

        char *word_copy = strdup(line);
        if (word_copy == NULL) {
            stopword_set_free(set);
            fclose(file);
            return NULL;
        }
        set->words[set->count++] = word_copy;
    }

    fclose(file);
    qsort(set->words, set->count, sizeof(char *), compare_words);
    return set;
}

void stopword_set_free(StopwordSet *set) {
    if (set == NULL) {
        return;
    }

    for (size_t i = 0; i < set->count; i++) {
        free(set->words[i]);
    }
    free(set->words);
    free(set);
}

int stopword_set_contains(const StopwordSet *set, const char *word) {
    /* Key must be char** like elements, so pass &word. */
    return bsearch(&word, set->words, set->count, sizeof(char *),
                   compare_words) != NULL;
}

void stopwords_filter(TokenList *list, const StopwordSet *set) {
    if (list == NULL || set == NULL) {
        return;
    }

    size_t write = 0;
    for (size_t read = 0; read < list->count; read++) {
        if (stopword_set_contains(set, list->terms[read])) {
            free(list->terms[read]);
        } else {
            list->terms[write++] = list->terms[read];
        }
    }
    list->count = write;
}
