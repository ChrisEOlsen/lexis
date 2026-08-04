/*
 * Strict RFC4180 CSV parsing (see APP_SPEC.md's "CSV" section) -- turns
 * a real corporate CSV export into one document per data row. Real CSV
 * exports routinely contain quoted fields with embedded commas,
 * newlines, or escaped quotes; the naive delimiter-split bulk_ingest.c's
 * Phase 1 uses for MS MARCO's clean, uniform, machine-generated TSV is
 * unsafe here -- it would silently misparse those fields into garbage
 * instead of failing, the opposite of what a CSV import needs.
 */

#ifndef LEXIS_CSV_PARSE_H
#define LEXIS_CSV_PARSE_H

#include "tokenizer.h"

/* Parses the RFC4180 CSV file at `csv_path` -- comma-delimited, first
 * row a header used only to fix the expected field count for every
 * subsequent row (its own field values aren't exposed to the caller) --
 * into one document per data row: that row's fields joined with a
 * single space, in column order. This is v1's default document mapping
 * (see APP_SPEC.md's "CSV" section) -- every column concatenated into
 * the indexed text; selecting a specific text-bearing column instead is
 * a deferred future enhancement.
 *
 * A data row whose field count doesn't match the header's, or a field
 * with an unterminated quote (or a stray, non-doubled quote appearing
 * anywhere other than the very start of a field), fails the WHOLE file
 * -- returns NULL -- rather than silently skipping or misparsing it,
 * matching bulk_ingest.c's Phase 1 "one malformed row fails the entire
 * load" philosophy for a bulk loader whose whole point is trusting its
 * source file's format.
 *
 * Returns a TokenList (reused purely as a generic growable string list,
 * same convention as ingest_chunk_words() -- each "term" here is
 * actually one whole row's joined text, not a token) the caller must
 * token_list_free(). An empty TokenList (0 rows) is a valid, non-NULL
 * result for a CSV with only a header and no data rows. Returns NULL if
 * the file can't be opened/read, is completely empty (no header at
 * all), or is malformed. */
TokenList *csv_parse_file(const char *csv_path);

#endif /* LEXIS_CSV_PARSE_H */
