/* Answer generation (spec 5.2.7); see generation.h. */

#include "generation.h"

#include "config.h"
#include "local_llm_client.h"
#include "paths.h"
#include "prompts.h"
#include "string_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reasoning pass per config `thinking` (default on); lazy once-per-process, no lock (LLM calls serialized).
 * Triples latency but rescued crowded-topic disambiguation (see dev/LIMITATIONS.md). */
static int thinking_enabled(void) {
    static int cached = -1;
    if (cached == -1) {
        cached = config_load_thinking(lexis_paths_config_file());
    }
    return cached;
}

/* Prompts live in prompts.h. Passage/summary answers honor `thinking`; NULL prefill alone won't enable it (see local_llm_client.c).
 * generate_answer() has no production caller; kept as tested never-thinking reference. */

char *generation_build_prompt(const char *query_text, PgStore *store,
                               const BM25ResultSet *results) {
    if (results->count == 0) {
        return NULL;
    }

    StringBuilder builder = {NULL, 0, 0};

    if (string_builder_append(&builder, LEXIS_PROMPT_ANSWER_FROM_PASSAGES_HEAD) != 0) {
        goto fail;
    }

    /* Skipped loads aren't fatal, but zero included passages means no grounded answer. */
    size_t passages_included = 0;
    for (size_t i = 0; i < results->count; i++) {
        PgStorePassage *passage = pg_store_get_passage(store, results->items[i].passage_id);
        if (passage == NULL) {
            continue;
        }

        char chunk_label[32];
        snprintf(chunk_label, sizeof(chunk_label), "%d", passage->chunk_id);

        if (string_builder_append(&builder, "[Source: ") != 0 ||
            string_builder_append(&builder, passage->document_name) != 0 ||
            string_builder_append(&builder, ", chunk ") != 0 ||
            string_builder_append(&builder, chunk_label) != 0 ||
            string_builder_append(&builder, "]\n") != 0 ||
            string_builder_append(&builder, passage->text) != 0 ||
            string_builder_append(&builder, "\n\n") != 0) {
            pg_store_passage_free(passage);
            goto fail;
        }

        pg_store_passage_free(passage);
        passages_included++;
    }

    if (passages_included == 0) {
        free(builder.data);
        return NULL;
    }

    if (string_builder_append(&builder, "Question: ") != 0 ||
        string_builder_append(&builder, query_text) != 0 ||
        string_builder_append(&builder, "\n\nAnswer:") != 0) {
        goto fail;
    }

    return builder.data;

fail:
    free(builder.data);
    return NULL;
}

char *generation_generate_answer(const char *query_text, PgStore *store,
                                  const BM25ResultSet *results) {
    char *prompt = generation_build_prompt(query_text, store, results);
    if (prompt == NULL) {
        return NULL;
    }

    char *answer = local_llm_chat_completion(prompt);
    free(prompt);
    return answer;
}

/* History-budget headroom for model output; derived from MAX_NEW_TOKENS so they move together.
 * +256 covers template markup, which token counting undercounts. */
#define GENERATION_RESERVED_OUTPUT_TOKENS (LOCAL_LLM_MAX_NEW_TOKENS + 256)

/* Same windowing as query_formulation.c; separate copies since each budget differs (see there). */
static LocalLlmTurn *window_history(const LocalLlmTurn *history, size_t history_count, int budget_tokens,
                                     size_t *out_count) {
    size_t start = history_count;
    int running_tokens = 0;
    for (size_t i = history_count; i-- > 0;) {
        int turn_tokens = local_llm_count_tokens(history[i].content);
        if (turn_tokens < 0) {
            turn_tokens = 0;
        }
        if (running_tokens + turn_tokens > budget_tokens) {
            break;
        }
        running_tokens += turn_tokens;
        start = i;
    }

    size_t kept = history_count - start;
    LocalLlmTurn *windowed = malloc(sizeof(LocalLlmTurn) * (kept > 0 ? kept : 1));
    if (windowed == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < kept; i++) {
        windowed[i] = history[start + i];
    }
    *out_count = kept;
    return windowed;
}

char *generation_generate_answer_with_history_stream(const char *query_text, PgStore *store, const BM25ResultSet *results,
                                                      const LocalLlmTurn *history, size_t history_count,
                                                      int thinking_override, LocalLlmStreamFn on_piece,
                                                      void *user_data) {
    /* -1 follows config `thinking`; 0/1 force off/on. Refusal retry forces ON (thinking rescued that case). */
    int think = (thinking_override < 0) ? thinking_enabled() : thinking_override;
    char *prompt = generation_build_prompt(query_text, store, results);
    if (prompt == NULL) {
        return NULL;
    }

    if (history_count == 0) {
        LocalLlmTurn turn = {.role = "user", .content = prompt};
        char *answer = local_llm_chat_completion_multi_ex_stream(&turn, 1, NULL, think, on_piece, user_data);
        free(prompt);
        return answer;
    }

    int prompt_tokens = local_llm_count_tokens(prompt);
    if (prompt_tokens < 0) {
        prompt_tokens = LOCAL_LLM_N_CTX; /* unmeasurable -- budget defensively to zero below rather than overflow */
    }
    int budget = LOCAL_LLM_N_CTX - prompt_tokens - GENERATION_RESERVED_OUTPUT_TOKENS;
    if (budget < 0) {
        budget = 0;
    }

    size_t windowed_count = 0;
    LocalLlmTurn *windowed = window_history(history, history_count, budget, &windowed_count);
    if (windowed == NULL) {
        free(prompt);
        return NULL;
    }

    LocalLlmTurn *turns = malloc(sizeof(LocalLlmTurn) * (windowed_count + 1));
    if (turns == NULL) {
        free(windowed);
        free(prompt);
        return NULL;
    }
    for (size_t i = 0; i < windowed_count; i++) {
        turns[i] = windowed[i];
    }
    free(windowed);
    turns[windowed_count] = (LocalLlmTurn){.role = "user", .content = prompt};

    char *answer =
        local_llm_chat_completion_multi_ex_stream(turns, windowed_count + 1, NULL, think, on_piece, user_data);
    free(turns);
    free(prompt);
    return answer;
}

char *generation_generate_answer_with_history(const char *query_text, PgStore *store, const BM25ResultSet *results,
                                               const LocalLlmTurn *history, size_t history_count,
                                               int thinking_override) {
    return generation_generate_answer_with_history_stream(query_text, store, results, history, history_count,
                                                           thinking_override, NULL, NULL);
}

/* SUMMARY-tool prompt; small by construction. Negative instructions address observed misbehavior (see prompts.h). */
static char *generation_build_summary_prompt(const char *query_text, const char *summary_text) {
    StringBuilder builder = {NULL, 0, 0};

    if (string_builder_append(&builder, LEXIS_PROMPT_ANSWER_FROM_SUMMARY_HEAD) != 0 ||
        string_builder_append(&builder, summary_text) != 0 ||
        string_builder_append(&builder, "\n\nQuestion: ") != 0 ||
        string_builder_append(&builder, query_text) != 0 ||
        string_builder_append(&builder, "\nAnswer: ") != 0) {
        free(builder.data);
        return NULL;
    }
    return builder.data;
}

char *generation_generate_answer_from_summary_stream(const char *query_text, const char *summary_text,
                                                     const LocalLlmTurn *history, size_t history_count,
                                                     int thinking_override, LocalLlmStreamFn on_piece,
                                                     void *user_data) {
    if (summary_text == NULL || summary_text[0] == '\0') {
        return NULL;
    }

    const int think = (thinking_override < 0) ? thinking_enabled() : thinking_override;

    char *prompt = generation_build_summary_prompt(query_text, summary_text);
    if (prompt == NULL) {
        return NULL;
    }

    if (history_count == 0) {
        LocalLlmTurn turn = {.role = "user", .content = prompt};
        char *answer = local_llm_chat_completion_multi_ex_stream(&turn, 1, NULL, think, on_piece, user_data);
        free(prompt);
        return answer;
    }

    int prompt_tokens = local_llm_count_tokens(prompt);
    if (prompt_tokens < 0) {
        prompt_tokens = LOCAL_LLM_N_CTX;
    }
    int budget = LOCAL_LLM_N_CTX - prompt_tokens - GENERATION_RESERVED_OUTPUT_TOKENS;
    if (budget < 0) {
        budget = 0;
    }

    size_t windowed_count = 0;
    LocalLlmTurn *windowed = window_history(history, history_count, budget, &windowed_count);
    if (windowed == NULL) {
        free(prompt);
        return NULL;
    }

    LocalLlmTurn *turns = malloc(sizeof(LocalLlmTurn) * (windowed_count + 1));
    if (turns == NULL) {
        free(windowed);
        free(prompt);
        return NULL;
    }
    for (size_t i = 0; i < windowed_count; i++) {
        turns[i] = windowed[i];
    }
    free(windowed);
    turns[windowed_count] = (LocalLlmTurn){.role = "user", .content = prompt};

    char *answer = local_llm_chat_completion_multi_ex_stream(turns, windowed_count + 1, NULL, think, on_piece,
                                                             user_data);
    free(turns);
    free(prompt);
    return answer;
}

char *generation_generate_answer_from_summary(const char *query_text, const char *summary_text,
                                              const LocalLlmTurn *history, size_t history_count) {
    return generation_generate_answer_from_summary_stream(query_text, summary_text, history, history_count, -1, NULL,
                                                           NULL);
}

/* Full-text read: whole documents as attributed blocks (the retired READ prompt, revived
 * as the SEARCH fallback for groups small enough to fit). Skipped NULL texts aren't
 * fatal, but zero included documents means no grounded answer. */
char *generation_build_documents_prompt(const char *query_text, const PgStoreDocument *docs,
                                        size_t doc_count) {
    if (doc_count == 0) {
        return NULL;
    }

    StringBuilder builder = {NULL, 0, 0};

    if (string_builder_append(&builder, LEXIS_PROMPT_ANSWER_FROM_DOCUMENTS_HEAD) != 0) {
        goto fail;
    }

    size_t docs_included = 0;
    for (size_t i = 0; i < doc_count; i++) {
        if (docs[i].text == NULL) {
            continue;
        }
        if (string_builder_append(&builder, "[Document: ") != 0 ||
            string_builder_append(&builder, docs[i].document_name) != 0 ||
            string_builder_append(&builder, "]\n") != 0 ||
            string_builder_append(&builder, docs[i].text) != 0 ||
            string_builder_append(&builder, "\n\n") != 0) {
            goto fail;
        }
        docs_included++;
    }

    if (docs_included == 0) {
        free(builder.data);
        return NULL;
    }

    if (string_builder_append(&builder, "Question: ") != 0 ||
        string_builder_append(&builder, query_text) != 0 ||
        string_builder_append(&builder, "\n\nAnswer:") != 0) {
        goto fail;
    }

    return builder.data;

fail:
    free(builder.data);
    return NULL;
}

/* Conservative 3 bytes/token (real ~4, same as corpus_summary.c). Model-free on purpose:
 * the fit decision must not depend on a loaded tokenizer. */
#define GENERATION_BYTES_PER_TOKEN 3

char *generation_generate_answer_from_documents_stream(const char *query_text, PgStore *store,
                                                       const LocalLlmTurn *history, size_t history_count,
                                                       int thinking_override, LocalLlmStreamFn on_piece,
                                                       void *user_data, int *too_big_out) {
    *too_big_out = 0;

    size_t doc_count = 0;
    PgStoreDocument *docs = pg_store_get_all_documents(store, &doc_count);
    if (docs == NULL || doc_count == 0) {
        pg_store_documents_free(docs, doc_count);
        return NULL;
    }

    char *prompt = generation_build_documents_prompt(query_text, docs, doc_count);
    pg_store_documents_free(docs, doc_count);
    if (prompt == NULL) {
        return NULL;
    }

    /* Whole corpora only: a partial read would present incomplete context as complete.
     * Over-estimate is safe (falls through to the summary); under-estimate is not. */
    size_t prompt_tokens_est = strlen(prompt) / GENERATION_BYTES_PER_TOKEN + 1;
    if (prompt_tokens_est + GENERATION_RESERVED_OUTPUT_TOKENS > (size_t)LOCAL_LLM_N_CTX) {
        free(prompt);
        *too_big_out = 1;
        return NULL;
    }

    const int think = (thinking_override < 0) ? thinking_enabled() : thinking_override;

    if (history_count == 0) {
        LocalLlmTurn turn = {.role = "user", .content = prompt};
        char *answer = local_llm_chat_completion_multi_ex_stream(&turn, 1, NULL, think, on_piece, user_data);
        free(prompt);
        return answer;
    }

    int prompt_tokens = local_llm_count_tokens(prompt);
    if (prompt_tokens < 0) {
        prompt_tokens = LOCAL_LLM_N_CTX;
    }
    int budget = LOCAL_LLM_N_CTX - prompt_tokens - GENERATION_RESERVED_OUTPUT_TOKENS;
    if (budget < 0) {
        budget = 0;
    }

    size_t windowed_count = 0;
    LocalLlmTurn *windowed = window_history(history, history_count, budget, &windowed_count);
    if (windowed == NULL) {
        free(prompt);
        return NULL;
    }

    LocalLlmTurn *turns = malloc(sizeof(LocalLlmTurn) * (windowed_count + 1));
    if (turns == NULL) {
        free(windowed);
        free(prompt);
        return NULL;
    }
    for (size_t i = 0; i < windowed_count; i++) {
        turns[i] = windowed[i];
    }
    free(windowed);
    turns[windowed_count] = (LocalLlmTurn){.role = "user", .content = prompt};

    char *answer = local_llm_chat_completion_multi_ex_stream(turns, windowed_count + 1, NULL, think, on_piece,
                                                             user_data);
    free(turns);
    free(prompt);
    return answer;
}

char *generation_generate_answer_from_documents(const char *query_text, PgStore *store,
                                                const LocalLlmTurn *history, size_t history_count,
                                                int thinking_override, int *too_big_out) {
    return generation_generate_answer_from_documents_stream(query_text, store, history, history_count,
                                                             thinking_override, NULL, NULL, too_big_out);
}
