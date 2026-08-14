/*
 * Tool routing for the interactive chat pipeline: decides, per user
 * message, whether to run the BM25 search-and-generate pipeline, answer
 * from the group's cached summary, or answer conversationally with no
 * retrieval at all. This is
 * deliberately a single one-shot decision, not a multi-step tool loop,
 * and no tool takes a model-supplied argument (SUMMARY always means the
 * active group as a whole).
 */

#ifndef LEXIS_TOOL_ROUTER_H
#define LEXIS_TOOL_ROUTER_H

#include <stddef.h>

#include "local_llm_client.h"

typedef enum {
    TOOL_SEARCH_PASSAGES,
    /* Answers broad, whole-collection questions from a cached, generated
     * overview of the group (see corpus_summary.h).
     *
     * This replaced a TOOL_READ_DOCUMENTS choice that answered the same
     * questions by feeding whole documents through the context window on
     * every request. Two reasons for the swap: that path was the slowest
     * operation in the app and got slower as a group grew, and the two
     * tools overlapped so heavily -- both meaning "whole-corpus scope" --
     * that offering both made the routing decision harder for no
     * capability the summary doesn't cover. Three sharply separated
     * choices route more reliably than four with two near-duplicates. */
    TOOL_SUMMARIZE_CORPUS,
    /* No retrieval at all: the user is not asking anything of the
     * documents. Greetings, thanks, acknowledgements, and questions about
     * the conversation itself ("what did I just ask?") land here, and the
     * caller answers straight from the model.
     *
     * This exists because both retrieval tools produce nonsense for
     * conversational input. "Lovely! Thank you so much" has no searchable
     * terms, so SEARCH either returns the "not enough to search for"
     * canned reply or, worse, matches on an incidental stopword-surviving
     * token and answers a question nobody asked; SUMMARY would describe
     * the whole collection in reply to "thanks". */
    TOOL_CONVERSE,
} ToolChoice;

/* Asks the model to pick SEARCH, SUMMARY or CHAT for `question`, given
 * `history` (oldest first, alternating "user"/"assistant" turns, windowed
 * internally to fit this call's own token budget). The prompt is
 * prefilled with an empty, already-closed <think></think> block (see
 * local_llm_chat_completion_multi()'s own doc comment) so this decision
 * costs no reasoning-pass latency. Falls back to TOOL_SEARCH_PASSAGES --
 * today's proven pipeline -- on anything ambiguous, unparseable, or if
 * the model call itself fails; this function has no failure return of
 * its own for that reason. */
/* `previous_answer_used_documents` tells the router whether the last answer
 * in this conversation came from a retrieval tool (SEARCH or SUMMARY). It
 * exists because an elliptical follow-up -- "is that all?", "what about the
 * others?" -- names no subject of its own, so judged in isolation it looks
 * like conversational filler and routes to CHAT. Observed: after three
 * answers drawn from a vehicle manual, "Is that all? What about the other
 * buttons on the steering wheel?" routed to CHAT and was answered with a
 * generic essay about cars that asked the user for their make and model.
 * Pass 0 when there is no previous answer or it was conversational. */
ToolChoice tool_router_choose_tool(const char *question, const LocalLlmTurn *history, size_t history_count,
                                    int previous_answer_used_documents);

#endif /* LEXIS_TOOL_ROUTER_H */
