/*
 * Large-model answer generation (spec 5.2.7). Assembles retrieved
 * passages (with source attribution) into a structured context block,
 * builds the generation prompt with the original question, and calls
 * the large model via OpenRouter to produce the final grounded answer.
 */

#ifndef LEXIS_GENERATION_H
#define LEXIS_GENERATION_H

#include "bm25.h"
#include "local_llm_client.h"
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

/* Runs the full generation step: builds the prompt and calls the local
 * model to produce the final answer. Returns the answer text (caller
 * must free()), or NULL if prompt-building or generation itself fails.
 * Unlike query formulation, there's no fallback here -- if generation
 * itself fails, there's no lesser answer to fall back to; the caller has
 * to decide how to handle "no answer available". */
char *generation_generate_answer(const char *query_text, PgStore *store,
                                  const BM25ResultSet *results);

/* History-aware counterpart to generation_generate_answer(): builds the
 * same context-block-plus-question prompt (via generation_build_prompt(),
 * reused unchanged) as the final "user" turn, preceded by `history`
 * (oldest first, alternating "user"/"assistant") windowed down to
 * whatever fits under LOCAL_LLM_N_CTX alongside the already-built
 * prompt's own real token count and a reserved output budget. `query_text`
 * should be the user's original question, not a reformulated search
 * query -- the reformulation step (see
 * query_formulation_contextualize_question()) exists only to help
 * retrieval, not to replace what the user actually asked. Falls back to
 * generation_generate_answer()'s plain single-turn behavior when
 * `history_count == 0`. Same failure contract: NULL if prompt-building
 * or generation itself fails. */
/* thinking_override: -1 follows the config `thinking` setting; 0/1
 * force the reasoning pass off/on for this call (the refusal retry
 * forces on). */
char *generation_generate_answer_with_history(const char *query_text, PgStore *store, const BM25ResultSet *results,
                                               const LocalLlmTurn *history, size_t history_count, int thinking_override);

/* "Read the whole group" counterpart: instead of BM25 passages, the
 * context block is every document currently in the active corpus (via
 * pg_store_get_all_documents()), included whole and in order until the
 * next one wouldn't fit the model's context budget, then stopped -- see
 * this function's .c-file implementation comment for the single-
 * oversized-document edge case. `query_text` is the user's original
 * question. `history` is windowed the same way
 * generation_generate_answer_with_history() windows it, against
 * whatever room is left after the document context block. Falls back to
 * a plain single-turn call when `history_count == 0`. Returns NULL if
 * the corpus has no documents, or if prompt-building or generation
 * itself fails. */
char *generation_generate_answer_from_documents(const char *query_text, PgStore *store, const LocalLlmTurn *history,
                                                 size_t history_count);

/* Answers a broad question from an already-generated group summary (see
 * corpus_summary.h) rather than from document text. This is what the
 * SUMMARY tool runs, and it replaced
 * generation_generate_answer_from_documents() on that path: the summary is
 * a few hundred tokens, so this is a small, fast prompt no matter how
 * large the group is.
 *
 * `summary_text` must be non-NULL and non-empty -- there is no
 * "summarize nothing" behavior here, the caller decides what to do when a
 * group has no summary. `history` is windowed against whatever room is
 * left after the summary block, same as the other history-aware
 * generators. Returns NULL on prompt-building or generation failure. */
char *generation_generate_answer_from_summary(const char *query_text, const char *summary_text,
                                              const LocalLlmTurn *history, size_t history_count);

#endif /* LEXIS_GENERATION_H */
