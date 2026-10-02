/* Text normalization and tokenization (spec 5.2.1, Stage 1); see tokenizer.h. */

/* Before any #include: exposes strdup under strict -std=c11; later would be too late via include guards. */
#define _POSIX_C_SOURCE 200809L

#include "tokenizer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define TOKEN_LIST_INITIAL_CAPACITY 8

/* Max buffered word length; truncates overflow. Safety cap vs pathological input. */
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

/* Punctuation kept word-internal (see tokenize() doc in tokenizer.h). */
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
        /* Peek p[1] only when c isn't NUL, else reads past the buffer. */
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
