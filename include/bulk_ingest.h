/* Bulk TSV ingestion: 3-phase deferred-term-resolution pipeline (COPY, parallel index, finalize). */

#ifndef LEXIS_BULK_INGEST_H
#define LEXIS_BULK_INGEST_H

#include <stddef.h>
#include <stdint.h>

#include "lemmatizer.h"
#include "stopwords.h"
#include "wordnet.h"

/* Cooperative cancellation: request aborts the run with BULK_INGEST_CANCELLED.
 * Callers must clear_cancel() before each run; rebuild cancellation is lossless. */
#define BULK_INGEST_CANCELLED (-2)
void bulk_ingest_request_cancel(void);
void bulk_ingest_clear_cancel(void);

/* Ingest every "<id><TAB><text>" row of tsv_path across thread_count workers (< 1 = 1).
 * schema_name (trusted id) retargets the run; NULL/empty keeps default. Passages ingested, or -1. */
long bulk_ingest_tsv(const char *conninfo, const char *schema_name, const StopwordSet *stopwords,
                      const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                      const char *tsv_path, size_t chunk_size, size_t overlap,
                      int thread_count);

/* Rebuild-on-append: re-ingest corpus_id's docs plus new ones (parallel arrays) from scratch.
 * Same-name docs replace; rebuilds in a scratch schema, swapped in only on success. -1 on failure. */
long bulk_ingest_rebuild_corpus(const char *conninfo, int64_t corpus_id, const char *const *new_document_names,
                                 const char *const *new_document_texts, size_t new_document_count,
                                 const StopwordSet *stopwords, const WordNetTable *wordnet,
                                 const Lemmatizer *lemmatizer, size_t chunk_size, size_t overlap, int thread_count);

#endif /* LEXIS_BULK_INGEST_H */
