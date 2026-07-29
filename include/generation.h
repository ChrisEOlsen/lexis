/*
 * Large-model answer generation (spec 5.2.7). Assembles retrieved
 * passages (with source attribution) into a structured context block,
 * builds the generation prompt with the original question, and calls
 * the large model via OpenRouter to produce the final grounded answer.
 */

#ifndef LEXIS_GENERATION_H
#define LEXIS_GENERATION_H

#include "bm25.h"
#include "pg_store.h"

/* Builds the generation prompt: every passage in `results` is fetched
 * from `store` by passage_id and assembled into a context block with
 * source attribution (document name, chunk id), followed by the
 * original question. A passage_id that fails to load (shouldn't happen
 * against a consistent index, but is a real possibility if the DB
 * changed between when `results` was computed and now) is skipped
 * rather than aborting the whole prompt over one bad passage. Returns
 * NULL on allocation failure, or if `results` is empty, or if NO
 * passage could be loaded at all -- there's nothing to ground an answer
 * in either way. */
char *generation_build_prompt(const char *query_text, PgStore *store,
                               const BM25ResultSet *results);

/* Runs the full generation step: builds the prompt and calls `model` via
 * OpenRouter to produce the final answer. Returns the answer text
 * (caller must free()), or NULL if prompt-building or the API call
 * fails. Unlike query formulation, there's no fallback here -- if
 * generation itself fails, there's no lesser answer to fall back to;
 * the caller has to decide how to handle "no answer available". */
char *generation_generate_answer(const char *query_text, const char *model,
                                  PgStore *store, const BM25ResultSet *results);

#endif /* LEXIS_GENERATION_H */
