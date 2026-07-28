/*
 * Implementation of the growable string buffer.
 * See include/string_builder.h for the module's role.
 */

#include "string_builder.h"

#include <stdlib.h>
#include <string.h>

int string_builder_append(StringBuilder *builder, const char *text) {
    size_t text_len = strlen(text);
    size_t needed = builder->length + text_len + 1;

    if (needed > builder->capacity) {
        size_t new_capacity = (builder->capacity == 0) ? 256 : builder->capacity;
        while (new_capacity < needed) {
            new_capacity *= 2;
        }
        char *new_data = realloc(builder->data, new_capacity);
        if (new_data == NULL) {
            return -1;
        }
        builder->data = new_data;
        builder->capacity = new_capacity;
    }

    memcpy(builder->data + builder->length, text, text_len);
    builder->length += text_len;
    builder->data[builder->length] = '\0';
    return 0;
}
