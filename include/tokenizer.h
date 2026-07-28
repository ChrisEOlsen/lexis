/*
 * Text normalization and tokenization (spec 5.2.1, build order Stage 1).
 * Lowercases, strips punctuation, applies Unicode normalization, and
 * splits normalized text into terms. Everything downstream — indexing,
 * BM25 scoring, query formulation — depends on clean term lists from here.
 */

#ifndef LEXIS_TOKENIZER_H
#define LEXIS_TOKENIZER_H

#include <stddef.h>

/* A growable list of term strings produced by the tokenizer.
 * `terms` is a heap array of heap-allocated, NUL-terminated strings.
 * `count` is how many are in use; 
 * `capacity` is how many the array can hold before the next append must grow it. */
typedef struct {
    char **terms;
    size_t count;
    size_t capacity;
} TokenList;

/* Allocates an empty TokenList. Returns NULL on allocation failure. */
TokenList *token_list_create(void);

/* Frees a TokenList: every stored term string, the terms array itself,
 * and the TokenList struct. Safe to call with list == NULL. */
void token_list_free(TokenList *list);

/* Appends a copy of `term` to the list, growing capacity (doubling) via
 * realloc() if the array is full. Returns 0 on success, -1 on allocation
 * failure — on failure the list is left exactly as it was before the call. */
int token_list_append(TokenList *list, const char *term);

/* Normalizes and splits `text` into terms: ASCII letters/digits are
 * lowercased and accumulated into words; any other byte — whitespace,
 * punctuation, or non-ASCII (>= 0x80) — ends the current word, UNLESS it's
 * one of '\'', '-', '.', ',' appearing *internal* to a word (already inside
 * a word, and immediately followed by another alphanumeric byte) — e.g.
 * "2.4-meter" and "don't" survive as single tokens, while a sentence-ending
 * "telescope." or quoted 'hello' still split normally, since the
 * punctuation there isn't followed by another word character. Non-ASCII
 * text is not yet handled beyond this (see tokenizer.c for rationale);
 * this is a documented MVP limitation, not an oversight.
 * Returns a new TokenList (caller must token_list_free() it), or NULL on
 * allocation failure. */
TokenList *tokenize(const char *text);

#endif /* LEXIS_TOKENIZER_H */
