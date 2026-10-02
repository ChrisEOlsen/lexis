/* Group summaries: one cached, lazily-built model overview per group, backing the SUMMARY tool. */

#ifndef LEXIS_CORPUS_SUMMARY_H
#define LEXIS_CORPUS_SUMMARY_H

#include <stdint.h>

#include "pg_store.h"

/* corpus_id's summary, built and cached first if missing/stale. store must be scoped via use_corpus.
 * Caller frees; NULL = no summary available (surface as an answer, not a failed query). */
char *corpus_summary_get_or_build(PgStore *store, int64_t corpus_id);

/* Build a summary bypassing the cache (tests / future regenerate). Caller frees; NULL on failure. */
char *corpus_summary_build(PgStore *store);

#endif /* LEXIS_CORPUS_SUMMARY_H */
