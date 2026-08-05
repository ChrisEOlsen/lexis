/*
 * Tool routing for the interactive chat pipeline: decides, per user
 * message, whether to run today's BM25 search-and-generate pipeline or
 * to answer directly from every document's full text instead. See
 * NOTES.md/the "Tool-routed chat" plan for the full design -- this is
 * deliberately a single one-shot decision, not a multi-step tool loop,
 * and neither tool takes a model-supplied argument (READ always means
 * every document currently in the active group).
 */

#ifndef LEXIS_TOOL_ROUTER_H
#define LEXIS_TOOL_ROUTER_H

#include <stddef.h>

#include "local_llm_client.h"

typedef enum {
    TOOL_SEARCH_PASSAGES,
    TOOL_READ_DOCUMENTS,
} ToolChoice;

/* Asks the model to pick SEARCH or READ for `question`, given `history`
 * (oldest first, alternating "user"/"assistant" turns, windowed
 * internally to fit this call's own token budget). The prompt is
 * prefilled with an empty, already-closed <think></think> block (see
 * local_llm_chat_completion_multi()'s own doc comment) so this decision
 * costs no reasoning-pass latency. Falls back to TOOL_SEARCH_PASSAGES --
 * today's proven pipeline -- on anything ambiguous, unparseable, or if
 * the model call itself fails; this function has no failure return of
 * its own for that reason. */
ToolChoice tool_router_choose_tool(const char *question, const LocalLlmTurn *history, size_t history_count);

#endif /* LEXIS_TOOL_ROUTER_H */
