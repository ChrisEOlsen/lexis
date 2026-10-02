/* Growable string buffer (doubling growth) for building strings of unknown final length. */

#ifndef LEXIS_STRING_BUILDER_H
#define LEXIS_STRING_BUILDER_H

#include <stddef.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StringBuilder;

/* Append text, growing as needed. 0 ok, -1 on failure (free builder->data, abandon the builder).
 * Usage: StringBuilder b = {0}; append...; success returns b.data (caller frees). */
int string_builder_append(StringBuilder *builder, const char *text);

#endif /* LEXIS_STRING_BUILDER_H */
