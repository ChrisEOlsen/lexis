/*
 * Optional embedding reranker: reorders BM25's candidate list by
 * meaning before the trim step, so a passage that matches the QUESTION
 * (semantically) outranks passages that merely match its words. This is
 * the fix for the measured "right document arrived but the key chunk
 * got cut" bucket (TESTING.md's 913-run attribution: 34 of 74 retrieval
 * misses) -- BM25 stays the recall gate and the explainability story,
 * the reranker only reorders what BM25 already surfaced.
 *
 * Runs a SECOND, tiny llama.cpp model (bge-small class, ~67MB, config
 * `reranker_model_path`) alongside the chat model -- embedding, not
 * generation: one vector per text, cosine similarity, reciprocal-rank
 * fusion with the BM25 order. Adds roughly one second per query for a
 * 40-candidate list. Entirely optional: no config line, no reranker, no
 * behavior change.
 *
 * Same serialization contract as local_llm_client.h: callers are
 * already single-threaded through the retrieval path; this module adds
 * no locking of its own.
 */

#ifndef LEXIS_RERANKER_H
#define LEXIS_RERANKER_H

#include "bm25.h"
#include "pg_store.h"

/* Loads the embedding model. 0 on success, -1 on failure (logged).
 * Safe to call once per process; reranker_available() reflects it. */
int reranker_init(const char *model_path);

int reranker_available(void);

/* Reorders `set` in place: embeds `query_text` and every candidate
 * passage, fuses cosine ranking with the existing BM25 ranking via
 * reciprocal-rank fusion (k=60), rewrites scores with the fused values,
 * and re-sorts. Returns 0 on success; on any failure (-1) the set is
 * left in its original BM25 order -- reranking degrades to a no-op,
 * never breaks retrieval. */
int reranker_rescore(PgStore *store, const char *query_text, BM25ResultSet *set);

void reranker_cleanup(void);

#endif /* LEXIS_RERANKER_H */
