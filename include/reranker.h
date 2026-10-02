/* Optional embedding reranker: reorder BM25 candidates by meaning (RRF fusion). Off unless configured. */

#ifndef LEXIS_RERANKER_H
#define LEXIS_RERANKER_H

#include "bm25.h"
#include "pg_store.h"

/* Load the embedding model, once per process. 0 on success, -1 on failure (logged). */
int reranker_init(const char *model_path);

int reranker_available(void);

/* Rescore set in place via RRF fusion (k=60) of cosine + BM25 rank. 0 ok; -1 leaves BM25 order. */
int reranker_rescore(PgStore *store, const char *query_text, BM25ResultSet *set);

void reranker_cleanup(void);

#endif /* LEXIS_RERANKER_H */
