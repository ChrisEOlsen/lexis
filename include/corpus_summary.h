/*
 * Group summaries: one cached, model-generated overview of what a group
 * contains, backing the SUMMARY tool (see tool_router.h).
 *
 * The problem this solves is cost, not capability. Answering "what is
 * this collection about?" by reading the corpus works -- that is what the
 * old READ tool did -- but it feeds whole documents through a 16k context
 * on every such question, which is slow enough to be the one operation in
 * the app that feels broken. A summary is generated once, cached in
 * public.corpus_summaries, and thereafter costs a single small prompt.
 *
 * Generation is LAZY: built on the first broad question about a group, not
 * during ingestion. Ingestion stays exactly as fast as it is, and, more
 * importantly, this keeps every local_llm_* call on the one code path that
 * already serializes them. local_llm_client has a single global context
 * with no concurrency support (see local_llm_client.h), and AppController
 * is documented as responsible for never running a QueryWorker alongside
 * a ModelLoader or another QueryWorker -- summarizing inside IngestWorker
 * would add a third, unserialized LLM user on a background thread.
 *
 * Coverage is a token-budgeted SAMPLE, not the full text: the head of
 * each document plus evenly spaced excerpts, up to a budget. A summary
 * built this way is representative rather than exhaustive -- it can miss a
 * topic that appears only in a section that wasn't sampled. That is the
 * accepted trade for one LLM call instead of one per chunk; see
 * LIMITATIONS.md.
 */

#ifndef LEXIS_CORPUS_SUMMARY_H
#define LEXIS_CORPUS_SUMMARY_H

#include <stdint.h>

#include "pg_store.h"

/* Returns corpus_id's summary, building and caching it first if there is
 * no cached one or if the cached one is stale (generated at a different
 * document count than the group currently has).
 *
 * `store` must already be scoped to corpus_id via pg_store_use_corpus():
 * the document text this reads comes from the corpus's own schema, while
 * the cache it reads and writes lives in public. Both are reached through
 * this one connection, which is why the corpus id has to be passed
 * explicitly rather than inferred from the connection's search_path.
 *
 * Returns a newly malloc()'d string the caller must free(), or NULL if the
 * group has no documents, if generation fails, or on a database/allocation
 * error. A NULL means "no summary is available", which the caller should
 * surface as an answer rather than as a failed query. */
char *corpus_summary_get_or_build(PgStore *store, int64_t corpus_id);

/* Builds a summary from the group's current documents WITHOUT consulting
 * or writing the cache. Exposed for tests and for a future "regenerate
 * now" action; ordinary callers want corpus_summary_get_or_build().
 * Returns a newly malloc()'d string the caller must free(), or NULL. */
char *corpus_summary_build(PgStore *store);

#endif /* LEXIS_CORPUS_SUMMARY_H */
