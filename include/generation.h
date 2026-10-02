/* Answer generation: assemble retrieved passages + question into a prompt; generate a grounded answer. */

#ifndef LEXIS_GENERATION_H
#define LEXIS_GENERATION_H

#include "bm25.h"
#include "local_llm_client.h"
#include "pg_store.h"

/* Build the prompt: passages as an attributed context block, then the question. Bad passages skipped.
 * NULL on alloc failure, empty results, or no passage loadable. */
char *generation_build_prompt(const char *query_text, PgStore *store,
                               const BM25ResultSet *results);

/* Build the prompt and generate the answer. Caller frees; NULL on failure (no fallback). */
char *generation_generate_answer(const char *query_text, PgStore *store,
                                  const BM25ResultSet *results);

/* History-aware answer: same prompt as final "user" turn, history windowed under LOCAL_LLM_N_CTX.
 * query_text = original question; history_count == 0 = single-turn. NULL on failure. */
/* thinking_override: -1 follows config; 0/1 force the reasoning pass off/on. */
char *generation_generate_answer_with_history(const char *query_text, PgStore *store, const BM25ResultSet *results,
                                               const LocalLlmTurn *history, size_t history_count, int thinking_override);

/* Streaming twin: same prompt/answer, plus live pieces via on_piece. Returned string stays authoritative. */
char *generation_generate_answer_with_history_stream(const char *query_text, PgStore *store,
                                                      const BM25ResultSet *results, const LocalLlmTurn *history,
                                                      size_t history_count, int thinking_override,
                                                      LocalLlmStreamFn on_piece, void *user_data);

/* Answer a broad question from the cached group summary (SUMMARY tool path).
 * summary_text must be non-NULL/non-empty; history windowed. NULL on failure. */
char *generation_generate_answer_from_summary(const char *query_text, const char *summary_text,
                                              const LocalLlmTurn *history, size_t history_count);

/* Streaming twin of the summary generator. thinking_override: -1 = config, 0/1 forced. */
char *generation_generate_answer_from_summary_stream(const char *query_text, const char *summary_text,
                                                      const LocalLlmTurn *history, size_t history_count,
                                                      int thinking_override, LocalLlmStreamFn on_piece,
                                                      void *user_data);

/* -- Full-text read (SEARCH fallback for small groups) ---------------------- */

/* Build the prompt: every document's full text as attributed blocks, then the question.
 * docs/count from pg_store_get_all_documents(); NULL-text entries skipped. NULL on alloc
 * failure, zero documents, or nothing includable. Pure builder: no model, no fit check. */
char *generation_build_documents_prompt(const char *query_text, const PgStoreDocument *docs,
                                        size_t doc_count);

/* Answer from the group's full text. Fits whole corpora only: when the documents exceed
 * the context budget this makes no model call and returns NULL with *too_big_out = 1
 * (caller falls through to the cached summary). *too_big_out = 0 on every other outcome,
 * including model failure (NULL) -- NULL with 0 means the model went quiet, not "too big".
 * too_big_out must be non-NULL. thinking_override: -1 = config, 0/1 forced. */
char *generation_generate_answer_from_documents(const char *query_text, PgStore *store,
                                                const LocalLlmTurn *history, size_t history_count,
                                                int thinking_override, int *too_big_out);

/* Streaming twin: same reply and fit contract, plus live pieces via on_piece. */
char *generation_generate_answer_from_documents_stream(const char *query_text, PgStore *store,
                                                       const LocalLlmTurn *history, size_t history_count,
                                                       int thinking_override, LocalLlmStreamFn on_piece,
                                                       void *user_data, int *too_big_out);

#endif /* LEXIS_GENERATION_H */
