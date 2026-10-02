/* Strict RFC4180 parsing; see csv_parse.h. Single-pass byte scan:
 * quoted fields may contain newlines, so lines aren't parseable units. */

#define _POSIX_C_SOURCE 200809L

#include "csv_parse.h"

#include "ingest.h"
#include "string_builder.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    CSV_FIELD_START,      /* nothing consumed yet for this field */
    CSV_UNQUOTED,         /* mid-field, not inside quotes */
    CSV_QUOTED,           /* mid-field, inside an opening " */
    CSV_QUOTED_QUOTE_SEEN /* " seen in quotes: next " = escaped quote, else field closed */
} CsvState;

/* Single-char append via a 2-byte scratch buffer. */
static int append_char(StringBuilder *builder, char c) {
    char buf[2] = {c, '\0'};
    return string_builder_append(builder, buf);
}

/* Append accumulated field to row, reset field. 0 ok, -1 alloc fail. */
static int end_field(StringBuilder *field, TokenList *row) {
    int result = token_list_append(row, field->data == NULL ? "" : field->data);
    free(field->data);
    *field = (StringBuilder){0};
    return result;
}

/* Finish field+row; first row sets required field count, later rows must match.
 * Always resets field/row for the next row, even on failure. 0 ok, -1 mismatch/alloc fail. */
static int finish_row(StringBuilder *field, TokenList **row_ptr, TokenList *rows, int *have_header,
                       size_t *expected_field_count) {
    int ok = end_field(field, *row_ptr) == 0;

    if (ok && !*have_header) {
        *expected_field_count = (*row_ptr)->count;
        *have_header = 1;
    } else if (ok && (*row_ptr)->count != *expected_field_count) {
        ok = 0;
    } else if (ok) {
        char *joined = ingest_join_words(*row_ptr, 0, (*row_ptr)->count);
        ok = joined != NULL && token_list_append(rows, joined) == 0;
        free(joined);
    }

    token_list_free(*row_ptr);
    *row_ptr = token_list_create();
    if (*row_ptr == NULL) {
        ok = 0;
    }
    return ok ? 0 : -1;
}

TokenList *csv_parse_file(const char *csv_path) {
    char *contents = ingest_read_file(csv_path);
    if (contents == NULL) {
        return NULL;
    }
    size_t len = strlen(contents);

    TokenList *rows = token_list_create();
    TokenList *row = token_list_create();
    if (rows == NULL || row == NULL) {
        free(contents);
        token_list_free(rows);
        token_list_free(row);
        return NULL;
    }

    StringBuilder field = {0};
    CsvState state = CSV_FIELD_START;
    int have_header = 0;
    size_t expected_field_count = 0;
    int failed = 0;
    int row_has_content = 0;

    /* i == len stands in for EOF so it reuses the newline-finishing logic. */
    for (size_t i = 0; i <= len && !failed; i++) {
        int eof = (i == len);
        char c = eof ? '\0' : contents[i];
        int is_newline = !eof && (c == '\r' || c == '\n');

        if (state == CSV_QUOTED) {
            if (eof) {
                failed = 1; /* unterminated quoted field */
            } else if (c == '"') {
                state = CSV_QUOTED_QUOTE_SEEN;
            } else {
                failed = append_char(&field, c) != 0;
                row_has_content = 1;
            }
            continue;
        }

        if (state == CSV_QUOTED_QUOTE_SEEN) {
            if (c == '"') {
                failed = append_char(&field, '"') != 0;
                state = CSV_QUOTED;
                continue;
            }
            if (!eof && c == ',') {
                failed = end_field(&field, row) != 0;
                state = CSV_FIELD_START;
                continue;
            }
            if (eof || is_newline) {
                if (is_newline && c == '\r' && i + 1 < len && contents[i + 1] == '\n') {
                    i++; /* consume the paired \n of a CRLF terminator */
                }
                failed = finish_row(&field, &row, rows, &have_header, &expected_field_count) != 0;
                state = CSV_FIELD_START;
                row_has_content = 0;
                continue;
            }
            failed = 1; /* junk immediately after a quoted field's closing quote */
            continue;
        }

        if (!eof && c == '"' && state == CSV_FIELD_START) {
            state = CSV_QUOTED;
            row_has_content = 1;
            continue;
        }
        if (!eof && c == '"') {
            failed = 1; /* stray quote mid-unquoted-field */
            continue;
        }
        if (!eof && c == ',') {
            failed = end_field(&field, row) != 0;
            state = CSV_FIELD_START;
            row_has_content = 1;
            continue;
        }
        if (eof || is_newline) {
            if (is_newline && c == '\r' && i + 1 < len && contents[i + 1] == '\n') {
                i++;
            }
            if (state == CSV_FIELD_START && !row_has_content && field.length == 0 && row->count == 0) {
                /* Blank line or trailing EOF after last newline: not a row. */
                if (eof) {
                    break;
                }
                continue;
            }
            failed = finish_row(&field, &row, rows, &have_header, &expected_field_count) != 0;
            state = CSV_FIELD_START;
            row_has_content = 0;
            continue;
        }
        failed = append_char(&field, c) != 0;
        row_has_content = 1;
        state = CSV_UNQUOTED;
    }

    free(contents);
    free(field.data);
    token_list_free(row);

    if (failed || !have_header) {
        token_list_free(rows);
        return NULL;
    }
    return rows;
}
