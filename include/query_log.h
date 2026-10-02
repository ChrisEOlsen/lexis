/* Pipeline observability: passive per-stage recorder (inputs, outputs, timing) keyed by query_id. */

#ifndef LEXIS_QUERY_LOG_H
#define LEXIS_QUERY_LOG_H

#include <stdint.h>

#include "pg_store.h"

/* Ensure the query_log tables exist. 0 on success, -1 on failure. */
int query_log_init_schema(PgStore *store);

/* Anchor row for one query. New id, or -1 = logging unavailable (skip the rest, don't disrupt). */
int64_t query_log_insert_query(PgStore *store, const char *question_text);

/* Close the anchor row: total latency + whether an answer was produced. 0 ok, -1 on failure. */
int query_log_finish_query(PgStore *store, int64_t query_id, long total_latency_ms, int succeeded);

/* Log one formulation run. prompt/response NULL when nothing to expand; selected_terms = final list.
 * 0 on success, -1 on failure. */
int query_log_insert_query_formulation_run(PgStore *store, int64_t query_id,
                                            int surviving_term_count, const char *prompt_text,
                                            const char *llm_response_text, int used_fallback,
                                            const char *selected_terms, long latency_ms);

/* Log one search run. New row id (for search_results rows), or -1 on failure. */
int64_t query_log_insert_search_run(PgStore *store, int64_t query_id, int top_k, int result_count,
                                     long latency_ms);

/* Log one ranked result (rank 1-based). 0 on success, -1 on failure. */
int query_log_insert_search_result(PgStore *store, int64_t search_run_id, int rank,
                                    int64_t passage_id, double score);

/* Log one generation run. prompt/answer NULL when that stage failed. 0 ok, -1 on failure. */
int query_log_insert_generation_run(PgStore *store, int64_t query_id, const char *model,
                                     int passages_included, int passages_skipped,
                                     const char *prompt_text, const char *answer_text,
                                     int succeeded, long latency_ms);

#endif /* LEXIS_QUERY_LOG_H */
