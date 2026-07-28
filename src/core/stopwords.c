/*
 * Implementation of stopword filtering.
 * See include/stopwords.h for the module's role (spec 5.2.1, Stage 1).
 */

/* See tokenizer.c for why this must come before any #include (strdup is
 * a POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "stopwords.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STOPWORD_SET_INITIAL_CAPACITY 8

/* Longest line stopword_set_load() will read per word. Generous relative
 * to the longest real entry ("yourselves", 11 chars) plus newline/NUL. */
#define STOPWORD_LINE_BUFFER_SIZE 64

/* qsort()/bsearch() require this exact (const void *, const void *)
 * signature — strcmp's (const char *, const char *) doesn't match, so
 * this wrapper casts each element (a char **, since we're sorting an
 * array of char *) back to the real type before comparing. */
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

        /* fgets() stops at sizeof(line)-1 bytes if it hasn't hit a '\n'
         * yet — true for the file's long prose comment lines, which are
         * well over STOPWORD_LINE_BUFFER_SIZE. Without this check, the
         * next fgets() call would resume mid-line and read the back half
         * of that comment as if it were a fresh line (no longer starting
         * with '#'), silently adding it as a bogus "word". Detect that
         * case and consume the rest of the physical line here instead. */
        if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
            int c;
            while ((c = fgetc(file)) != '\n' && c != EOF) {
                /* discard remainder of the truncated line */
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
    /* bsearch's key must be the same type as an array element (char *),
     * so we pass the address of `word` itself — a char** — not `word`.
     * compare_words then casts both sides back and strcmp()s them. */
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
