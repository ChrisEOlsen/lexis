/*
 * Implementation of text normalization and tokenization.
 * See include/tokenizer.h for the module's role (spec 5.2.1, Stage 1).
 */

/* Exposes POSIX.1-2008 declarations (e.g. strdup) in <string.h> even under
 * strict -std=c11, where glibc otherwise hides non-ISO-C extensions. Must
 * be defined before any header is included — if a header pulls in
 * <string.h> first via its own include chain, that header's include guard
 * makes a later inclusion a no-op and this macro would arrive too late. */
#define _POSIX_C_SOURCE 200809L

#include "tokenizer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define TOKEN_LIST_INITIAL_CAPACITY 8

/* Longest word tokenize() will buffer before truncating (silently drops
 * any excess bytes rather than growing without bound). Generous relative
 * to real English words (~45 chars max) — exists as a safety cap against
 * pathological input, e.g. a wall of non-space characters. */
#define WORD_BUFFER_SIZE 256

TokenList *token_list_create(void) {
    TokenList *list = malloc(sizeof(TokenList));
    if (list == NULL) {
        return NULL;
    }

    list->terms = malloc(TOKEN_LIST_INITIAL_CAPACITY * sizeof(char *));
    if (list->terms == NULL) {
        free(list);
        return NULL;
    }

    list->count = 0;
    list->capacity = TOKEN_LIST_INITIAL_CAPACITY;
    return list;
}

void token_list_free(TokenList *list) {
    if (list == NULL) {
        return;
    }

    for (size_t i = 0; i < list->count; i++) {
        free(list->terms[i]);
    }
    free(list->terms);
    free(list);
}

int token_list_append(TokenList *list, const char *term) {
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity * 2;
        char **new_terms = realloc(list->terms, new_capacity * sizeof(char *));
        if (new_terms == NULL) {
            return -1;
        }
        list->terms = new_terms;
        list->capacity = new_capacity;
    }

    char *term_copy = strdup(term);
    if (term_copy == NULL) {
        return -1;
    }

    list->terms[list->count] = term_copy;
    list->count++;
    return 0;
}

/* '\'', '-', '.', ',' -- the punctuation bytes that don't split a word when
 * they appear internally (see tokenize()'s doc comment for the exact rule
 * and worked examples). */
static int is_internal_connector(unsigned char c) {
    return c == '\'' || c == '-' || c == '.' || c == ',';
}

TokenList *tokenize(const char *text) {
    TokenList *list = token_list_create();
    if (list == NULL) {
        return NULL;
    }

    char word[WORD_BUFFER_SIZE];
    size_t word_len = 0;

    for (const unsigned char *p = (const unsigned char *)text;; p++) {
        unsigned char c = *p;
        /* Only safe to peek at p[1] when c itself isn't the NUL terminator
         * -- otherwise p[1] reads one byte past the allocated buffer. */
        int is_internal_punct = c != '\0' && word_len > 0 && is_internal_connector(c) &&
                                 p[1] < 0x80 && isalnum(p[1]);
        int is_word_char = ((c < 0x80) && isalnum(c)) || is_internal_punct;

        if (is_word_char) {
            if (word_len < sizeof(word) - 1) {
                word[word_len++] = (char)tolower(c);
            }
        } else {
            if (word_len > 0) {
                word[word_len] = '\0';
                if (token_list_append(list, word) != 0) {
                    token_list_free(list);
                    return NULL;
                }
                word_len = 0;
            }
            if (c == '\0') {
                break;
            }
        }
    }

    return list;
}
