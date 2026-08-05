/*
 * Implementation of answer generation.
 * See include/generation.h for the module's role (spec 5.2.7).
 */

#include "generation.h"

#include "local_llm_client.h"
#include "string_builder.h"

#include <stdio.h>
#include <stdlib.h>

char *generation_build_prompt(const char *query_text, PgStore *store,
                               const BM25ResultSet *results) {
    if (results->count == 0) {
        return NULL;
    }

    StringBuilder builder = {NULL, 0, 0};

    if (string_builder_append(&builder,
            "You are answering a question using only the provided context. If the "
            "context doesn't contain enough information to answer, say so rather "
            "than guessing.\n\nContext:\n\n") != 0) {
        goto fail;
    }

    /* A passage_id failing to load is skipped, not fatal -- track how
     * many actually made it in, since a prompt with zero real context
     * (every passage failed to load) isn't a grounded answer at all. */
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

/* Headroom reserved for the model's actual answer -- somewhat more than
 * LOCAL_LLM_MAX_NEW_TOKENS (512, local_llm_client.c) since the token
 * count this budget is computed against (local_llm_count_tokens() on the
 * already-built prompt) isn't run through the exact same
 * tokenize-the-full-formatted-prompt path the real call ends up using,
 * so a little slack is cheaper than risking a prompt that technically
 * doesn't fit. */
#define GENERATION_RESERVED_OUTPUT_TOKENS 600

/* Same windowing algorithm as query_formulation.c's own window_history()
 * -- kept as two small copies rather than one shared helper, since each
 * call site's budget is computed differently (this one reserves room for
 * an already-built passage context block; query_formulation's doesn't
 * have one yet) and the walking logic itself is short enough that
 * sharing it would cost more in indirection than it'd save. See that
 * file's copy for the full behavior doc comment. */
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

char *generation_generate_answer_with_history(const char *query_text, PgStore *store, const BM25ResultSet *results,
                                               const LocalLlmTurn *history, size_t history_count) {
    char *prompt = generation_build_prompt(query_text, store, results);
    if (prompt == NULL) {
        return NULL;
    }

    if (history_count == 0) {
        char *answer = local_llm_chat_completion(prompt);
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

    char *answer = local_llm_chat_completion_multi(turns, windowed_count + 1);
    free(turns);
    free(prompt);
    return answer;
}
