/* Strict RFC4180 CSV parsing: one document per data row. */

#ifndef LEXIS_CSV_PARSE_H
#define LEXIS_CSV_PARSE_H

#include "tokenizer.h"

/* Parse csv_path into one document (space-joined fields) per data row; header fixes field count.
 * Any malformed row fails the whole file (NULL). Caller frees list; empty list = header only. */
TokenList *csv_parse_file(const char *csv_path);

#endif /* LEXIS_CSV_PARSE_H */
