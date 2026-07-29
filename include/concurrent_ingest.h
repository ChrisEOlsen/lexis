/*
 * Genuinely concurrent ingestion, made possible by the Postgres migration
 * (see LIMITATIONS.md / pg_store.h): unlike SQLite, which allows exactly
 * one writer connection and capped the sharded-SQLite experiment's best
 * speedup at ~1.3x (experiment/sharded-ingestion branch), Postgres allows
 * many connections to write to the same tables at once. So there's no
 * sharding or merge step here at all -- `thread_count` worker threads each
 * open their own connection to the *same* database and call the existing,
 * unchanged ingest_document() directly. Safe because:
 *   - passages.id / terms.id use GENERATED ALWAYS AS IDENTITY (Postgres
 *     sequences), which are safe under concurrent inserts by construction.
 *   - pg_store_get_or_create_term() is a single atomic
 *     INSERT ... ON CONFLICT ... RETURNING id (see pg_store.c) -- two
 *     threads racing on the same new term both land on the same row,
 *     never a duplicate.
 *   - StopwordSet/WordNetTable/Lemmatizer are read-only after load and
 *     safely shared (const pointers) across every worker thread.
 */

#ifndef LEXIS_CONCURRENT_INGEST_H
#define LEXIS_CONCURRENT_INGEST_H

#include <stddef.h>

#include "lemmatizer.h"
#include "stopwords.h"
#include "wordnet.h"

/* Ingests every regular file directly inside `dir_path` across
 * `thread_count` worker threads, each with its own connection (via
 * `conninfo`) to the same database. thread_count < 1 is treated as 1.
 * Returns the total number of passages ingested (>= 0) on success, or -1
 * if the directory can't be opened or any worker's connection fails to
 * open. A single document failing to ingest is logged and skipped, same
 * as ingest_corpus(). */
long concurrent_ingest_corpus(const char *conninfo, const StopwordSet *stopwords,
                               const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                               const char *dir_path, size_t chunk_size, size_t overlap,
                               int thread_count);

#endif /* LEXIS_CONCURRENT_INGEST_H */
