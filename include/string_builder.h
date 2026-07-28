/*
 * Growable string buffer -- appends text pieces, growing (doubling) the
 * underlying allocation as needed. For building strings whose final
 * length isn't known in advance and involves too many variable-length,
 * conditionally-included pieces for a two-pass measure-then-build
 * approach (like ingest_join_words's) to be practical -- e.g. assembling
 * an LLM prompt from a variable number of terms or passages.
 */

#ifndef LEXIS_STRING_BUILDER_H
#define LEXIS_STRING_BUILDER_H

#include <stddef.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StringBuilder;

/* Appends `text`, growing the underlying allocation if needed. Returns 0
 * on success, -1 on allocation failure. On failure, free(builder->data)
 * directly (it's a plain heap pointer, nothing more to clean up) rather
 * than continuing to use the builder.
 *
 * Usage: declare `StringBuilder b = {0};`, append repeatedly, then on
 * success return `b.data` (caller now owns it, frees with plain free());
 * on failure, `free(b.data); return NULL;`. */
int string_builder_append(StringBuilder *builder, const char *text);

#endif /* LEXIS_STRING_BUILDER_H */
