/* Text normalization and tokenization: lowercase, strip punctuation, split into terms. */

#ifndef LEXIS_TOKENIZER_H
#define LEXIS_TOKENIZER_H

#include <stddef.h>

/* Growable list of owned term strings. count in use; capacity before next growth. */
typedef struct {
    char **terms;
    size_t count;
    size_t capacity;
} TokenList;

/* Allocates an empty TokenList. Returns NULL on allocation failure. */
TokenList *token_list_create(void);

/* Free terms, array, and struct. Safe with list == NULL. */
void token_list_free(TokenList *list);

/* Append a copy of term (doubling growth). 0 ok; -1 leaves the list unchanged. */
int token_list_append(TokenList *list, const char *term);

/* Lowercase ASCII letters/digits into words; other bytes split, except intra-word ' - . , (e.g. don't).
 * Caller frees list; NULL on alloc failure. */
TokenList *tokenize(const char *text);

#endif /* LEXIS_TOKENIZER_H */
