/* Tool routing: one-shot per-message choice of SEARCH, SUMMARY, or CHAT (no tool loop). */

#ifndef LEXIS_TOOL_ROUTER_H
#define LEXIS_TOOL_ROUTER_H

#include <stddef.h>

#include "local_llm_client.h"

typedef enum {
    TOOL_SEARCH_PASSAGES,
    /* Answer broad whole-collection questions from the cached group overview. */
    TOOL_SUMMARIZE_CORPUS,
    /* No retrieval: greetings, thanks, and questions about the conversation itself. */
    TOOL_CONVERSE,
} ToolChoice;

/* Pick SEARCH/SUMMARY/CHAT for question given windowed history (reasoning skipped via prefill).
 * Falls back to TOOL_SEARCH_PASSAGES on ambiguity; model_failed_out (nullable) reports
 * whether the model was actually consulted, so callers can tell failure from a real SEARCH. */
/* previous_answer_used_documents: whether the last answer used retrieval (routes bare follow-ups).
 * Pass 0 when there is no previous answer or it was conversational. */
ToolChoice tool_router_choose_tool(const char *question, const LocalLlmTurn *history, size_t history_count,
                                    int previous_answer_used_documents, int *model_failed_out);

#endif /* LEXIS_TOOL_ROUTER_H */
