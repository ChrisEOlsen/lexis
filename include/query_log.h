/*
 * Pipeline observability -- records what each stage of a single query's
 * run actually did (inputs, outputs, timing, success/failure), so behavior
 * and performance can be inspected and compared across runs after the
 * fact. A distinct concern from pg_store.c's index/passage schema --
 * this module owns its own tables on the same connection, tied together
 * by query_id.
 *
 * Deliberately a passive recorder: every insert function here is called
 * from main.c around the *existing* pipeline calls (query_formulation.c,
 * bm25.c, generation.c stay completely untouched). See LIMITATIONS.md for
 * what this can't distinguish (e.g. an LLM call that succeeds but returns
 * unparseable JSON logs the same as a clean non-fallback run).
 */

#ifndef LEXIS_QUERY_LOG_H
#define LEXIS_QUERY_LOG_H

#include <stdint.h>

#include "pg_store.h"

/* Ensures the queries/query_formulation_runs/search_runs/search_results/
 * generation_runs tables exist on `store`'s connection. Returns 0 on
 * success, -1 on failure. */
int query_log_init_schema(PgStore *store);

/* Records the anchor row for one query. Returns its new row id, or -1 on
 * failure -- callers should treat -1 as "logging unavailable" and skip
 * every other query_log_* call for this query, rather than letting a
 * logging failure disrupt the real pipeline. */
int64_t query_log_insert_query(PgStore *store, const char *question_text);

/* Closes out the anchor row once the whole pipeline finishes, recording
 * total wall-clock latency and whether a final answer was produced.
 * Returns 0 on success, -1 on failure. */
int query_log_finish_query(PgStore *store, int64_t query_id, long total_latency_ms, int succeeded);

/* Records one query formulation stage run. `prompt_text`/`llm_response_text`
 * may be NULL (nothing to expand -- an all-stopwords query never builds a
 * prompt or calls the LLM). `used_fallback` should be 1 whenever the
 * OpenRouter call itself failed (response == NULL) -- see the header
 * comment's note on what this can't detect. `selected_terms` is the final
 * space-joined term list actually handed to bm25_search(). Returns 0 on
 * success, -1 on failure. */
int query_log_insert_query_formulation_run(PgStore *store, int64_t query_id,
                                            int surviving_term_count, const char *prompt_text,
                                            const char *llm_response_text, int used_fallback,
                                            const char *selected_terms, long latency_ms);

/* Records one BM25 search stage run. Returns its new row id (needed to
 * attach search_results rows), or -1 on failure. */
int64_t query_log_insert_search_run(PgStore *store, int64_t query_id, int top_k, int result_count,
                                     long latency_ms);

/* Records one ranked result from a search run (rank is 1-based). Returns 0
 * on success, -1 on failure. */
int query_log_insert_search_result(PgStore *store, int64_t search_run_id, int rank,
                                    int64_t passage_id, double score);

/* Records one answer generation stage run. `prompt_text`/`answer_text` may
 * be NULL (prompt failed to build, or the API call failed with no
 * fallback -- see generation.c). Returns 0 on success, -1 on failure. */
int query_log_insert_generation_run(PgStore *store, int64_t query_id, const char *model,
                                     int passages_included, int passages_skipped,
                                     const char *prompt_text, const char *answer_text,
                                     int succeeded, long latency_ms);

#endif /* LEXIS_QUERY_LOG_H */
