/*
 * Implementation of tool routing.
 * See include/tool_router.h for the module's role.
 */

#define _POSIX_C_SOURCE 200809L

#include "tool_router.h"

#include "prompts.h"
#include "string_builder.h"

#include <stdlib.h>
#include <string.h>

/* Reserves room for this call's own prompt wrapper + the question itself
 * + the model's one-word answer -- deliberately tiny, since the whole
 * point of this call is a single-word decision, not prose. */
#define TOOL_ROUTER_RESERVED_TOKENS 500

/* The prompt itself and the reasoning-skip prefill both live in
 * prompts.h -- see LEXIS_PROMPT_TOOL_ROUTER_HEAD and
 * LEXIS_PREFILL_NO_THINK. Tool *choice* held up fine without a reasoning
 * pass in testing: a one-word classification doesn't need
 * chain-of-thought the way open-ended generation might. */

/* Same windowing algorithm as query_formulation.c's/generation.c's own
 * copies -- see either for the full behavior doc comment. Kept as its
 * own copy rather than a shared helper for the same reason those two
 * are separate from each other: each call site's budget differs, and
 * the walking logic itself is short enough that sharing it would cost
 * more in indirection than it'd save. */
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

/* Case-insensitive substring search -- strcasestr() is a BSD/glibc
 * extension, not portable C11, so this project rolls its own the same
 * way it already avoids other non-standard libc extensions elsewhere. */
static int contains_case_insensitive(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p != '\0'; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] != '\0' &&
               (p[i] | 0x20) == (needle[i] | 0x20)) { /* ASCII-only lowercase fold, matches this
                                                        * project's existing ASCII-only tokenizer
                                                        * assumption (see tokenizer.c/LIMITATIONS.md) */
            i++;
        }
        if (i == needle_len) {
            return 1;
        }
    }
    return 0;
}

ToolChoice tool_router_choose_tool(const char *question, const LocalLlmTurn *history, size_t history_count) {
    size_t windowed_count = 0;
    LocalLlmTurn *windowed =
        window_history(history, history_count, LOCAL_LLM_N_CTX - TOOL_ROUTER_RESERVED_TOKENS, &windowed_count);
    if (windowed == NULL) {
        return TOOL_SEARCH_PASSAGES; /* allocation failure -- fall back to the proven path */
    }

    StringBuilder builder = {NULL, 0, 0};
    if (string_builder_append(&builder, LEXIS_PROMPT_TOOL_ROUTER_HEAD) != 0 ||
        string_builder_append(&builder, question) != 0 || string_builder_append(&builder, "\"") != 0) {
        free(builder.data);
        free(windowed);
        return TOOL_SEARCH_PASSAGES;
    }

    LocalLlmTurn *turns = malloc(sizeof(LocalLlmTurn) * (windowed_count + 1));
    if (turns == NULL) {
        free(builder.data);
        free(windowed);
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

    /* CHAT is tested first, then SUMMARY, and SEARCH is what's left. Order
     * matters because this is a substring test, not an exact match: a
     * reply that editorialises ("CHAT -- no document lookup needed")
     * still routes correctly, and the most specific intent wins if the
     * model names more than one. SEARCH stays the fallback for an empty,
     * unparseable, or failed call -- see this function's doc comment. */
    ToolChoice choice = TOOL_SEARCH_PASSAGES;
    if (response != NULL) {
        if (contains_case_insensitive(response, "CHAT")) {
            choice = TOOL_CONVERSE;
        } else if (contains_case_insensitive(response, "SUMMAR")) {
            /* "SUMMAR", not "SUMMARY": matches both the requested word and
             * "summarize"/"summarise", which the model sometimes answers
             * with instead. */
            choice = TOOL_SUMMARIZE_CORPUS;
        }
    }
    free(response);
    return choice;
}
