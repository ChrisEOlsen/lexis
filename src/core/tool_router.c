/* Tool routing; see tool_router.h. */

#define _POSIX_C_SOURCE 200809L

#include "tool_router.h"

#include "prompts.h"
#include "string_builder.h"

#include <stdlib.h>
#include <string.h>

/* Headroom for prompt wrapper + question + one-word answer. */
#define TOOL_ROUTER_RESERVED_TOKENS 500

/* Prompt/prefill in prompts.h; one-word choice needs no reasoning pass. */

/* Same windowing as query_formulation.c/generation.c; separate since each budget differs. */
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

/* Case-insensitive substring; strcasestr() isn't portable C11. */
static int contains_case_insensitive(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p != '\0'; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] != '\0' &&
               (p[i] | 0x20) == (needle[i] | 0x20)) { /* ASCII-only fold, matching tokenizer assumption (see dev/LIMITATIONS.md) */
            i++;
        }
        if (i == needle_len) {
            return 1;
        }
    }
    return 0;
}

ToolChoice tool_router_choose_tool(const char *question, const LocalLlmTurn *history, size_t history_count,
                                    int previous_answer_used_documents, int *model_failed_out) {
    /* Every early return below means the model was never consulted. */
    size_t windowed_count = 0;
    LocalLlmTurn *windowed =
        window_history(history, history_count, LOCAL_LLM_N_CTX - TOOL_ROUTER_RESERVED_TOKENS, &windowed_count);
    if (windowed == NULL) {
        if (model_failed_out != NULL) {
            *model_failed_out = 1;
        }
        return TOOL_SEARCH_PASSAGES;
    }

    StringBuilder builder = {NULL, 0, 0};
    if (string_builder_append(&builder, LEXIS_PROMPT_TOOL_ROUTER_HEAD) != 0 ||
        string_builder_append(&builder, question) != 0 || string_builder_append(&builder, "\"") != 0 ||
        (previous_answer_used_documents &&
         string_builder_append(&builder, LEXIS_PROMPT_TOOL_ROUTER_PRIOR_RETRIEVAL) != 0)) {
        free(builder.data);
        free(windowed);
        if (model_failed_out != NULL) {
            *model_failed_out = 1;
        }
        return TOOL_SEARCH_PASSAGES;
    }

    LocalLlmTurn *turns = malloc(sizeof(LocalLlmTurn) * (windowed_count + 1));
    if (turns == NULL) {
        free(builder.data);
        free(windowed);
        if (model_failed_out != NULL) {
            *model_failed_out = 1;
        }
        return TOOL_SEARCH_PASSAGES;
    }
    for (size_t i = 0; i < windowed_count; i++) {
        turns[i] = windowed[i];
    }
    free(windowed);
    turns[windowed_count] = (LocalLlmTurn){.role = "user", .content = builder.data};

    char *response = local_llm_chat_completion_multi(turns, windowed_count + 1, LEXIS_PREFILL_NO_THINK);
    free(turns);
    free(builder.data);
    if (model_failed_out != NULL) {
        *model_failed_out = (response == NULL);
    }

    /* Substring test, CHAT first then SUMMARY, SEARCH fallback; order matters if model names several. */
    ToolChoice choice = TOOL_SEARCH_PASSAGES;
    if (response != NULL) {
        if (contains_case_insensitive(response, "CHAT")) {
            choice = TOOL_CONVERSE;
        } else if (contains_case_insensitive(response, "SUMMAR")) {
            /* "SUMMAR" matches SUMMARY/summarize/summarise variants. */
            choice = TOOL_SUMMARIZE_CORPUS;
        }
    }
    free(response);
    return choice;
}
