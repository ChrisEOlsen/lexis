/*
 * Implementation of answer generation.
 * See include/generation.h for the module's role (spec 5.2.7).
 */

#include "generation.h"

#include "local_llm_client.h"
#include "prompts.h"
#include "string_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every prompt in this file lives in prompts.h.
 *
 * The two user-facing answer generators below -- from retrieved passages
 * and from a group summary -- now run WITH the model's reasoning pass
 * enabled, via local_llm_chat_completion_multi_ex(..., force_thinking=1).
 * Passing prefill = NULL alone would not do it: that leaves the template's
 * enable_thinking override unset, and Gemma 4's template defaults it to
 * false on its own (see local_llm_client.c). Any reasoning the model emits
 * is stripped before the answer is returned.
 *
 * The rationale for disabling it was latency, on the theory that a
 * grounded answer needs no deliberation. That theory went untested until
 * a retrieved passage set where every candidate was legitimately on-topic
 * and the model, given no opportunity to weigh them, simply answered from
 * whichever it read first.
 *
 * The plain generation_generate_answer() is deliberately untouched -- it
 * stays as-is for main.c/eval.c, where changing generation behavior would
 * invalidate recorded eval runs. */

char *generation_build_prompt(const char *query_text, PgStore *store,
                               const BM25ResultSet *results) {
    if (results->count == 0) {
        return NULL;
    }

    StringBuilder builder = {NULL, 0, 0};

    if (string_builder_append(&builder, LEXIS_PROMPT_ANSWER_FROM_PASSAGES_HEAD) != 0) {
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

/* Headroom held back from the history budget for the model's own output.
 *
 * Derived from LOCAL_LLM_MAX_NEW_TOKENS rather than written as a literal,
 * because the two must move together and once did not: this was 600, chosen
 * when the generation cap was 512, and stayed at 600 after the cap became
 * 2048 for thinking mode. That combination is silently lossy -- history is
 * allowed to fill the window to within 600 tokens of the end, generation then
 * runs out of context part-way through (it stops cleanly, see
 * local_llm_client.c's n_ctx_used check) and the user gets a truncated answer
 * with nothing indicating it was cut short. Reasoning traces alone have been
 * measured past 512 tokens, so the old margin was not close to enough.
 *
 * The extra slack on top covers the chat template's own markup: this budget
 * is computed from local_llm_count_tokens() on the pre-template prompt, which
 * undercounts what the real call ends up tokenizing. */
#define GENERATION_RESERVED_OUTPUT_TOKENS (LOCAL_LLM_MAX_NEW_TOKENS + 256)

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
        LocalLlmTurn turn = {.role = "user", .content = prompt};
        char *answer = local_llm_chat_completion_multi_ex(&turn, 1, NULL, /*force_thinking=*/1);
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

    char *answer = local_llm_chat_completion_multi_ex(turns, windowed_count + 1, NULL, /*force_thinking=*/1);
    free(turns);
    free(prompt);
    return answer;
}

/* Room reserved for history + the question + the model's output, held
 * out of the document-text budget below BEFORE any document text gets
 * included -- unlike generation_build_prompt()'s passages (already
 * capped at TOP_K=5 small chunks, so naturally bounded), document text
 * is effectively unbounded, so this function has to reserve room for
 * everything else up front rather than measuring an already-built
 * prompt afterward the way generation_generate_answer_with_history()
 * does for history. */
#define GENERATION_DOCUMENT_CONTEXT_RESERVED_TOKENS 3000

/* Only used to size a byte-length truncation of a single oversized
 * document (see generation_build_document_prompt()) -- an estimate, not
 * an exact token count. Conservative (real English text averages closer
 * to 4 bytes/token) so the truncated text undershoots its token budget
 * rather than overshoots it; the real formatted prompt still gets
 * checked against LOCAL_LLM_N_CTX before generation runs regardless, so
 * this only needs to be roughly right, not exact. */
#define GENERATION_BYTES_PER_TOKEN_ESTIMATE 3

/* Builds the "read the whole group" context prompt: every document
 * currently in the active corpus (pg_store_get_all_documents(), the same
 * call rebuild-on-append uses), included whole and in the order returned
 * until the next one wouldn't fit `budget_tokens`, then stops -- document
 * order has no inherent recency the way chat turns do, so unlike
 * window_history() this doesn't walk backward, it just stops once full.
 * If NO document has been included yet and even the first one alone
 * doesn't fit, that document is truncated to fit rather than returning
 * an empty context -- see LIMITATIONS.md for why this is a byte-length
 * estimate, not exact. Returns NULL if the corpus has no documents, on
 * allocation failure, or if the DB read itself fails. */
static char *generation_build_document_prompt(const char *query_text, PgStore *store, int budget_tokens) {
    size_t doc_count = 0;
    PgStoreDocument *docs = pg_store_get_all_documents(store, &doc_count);
    if (docs == NULL) {
        return NULL;
    }
    if (doc_count == 0) {
        pg_store_documents_free(docs, doc_count);
        return NULL;
    }

    StringBuilder builder = {NULL, 0, 0};
    if (string_builder_append(&builder, LEXIS_PROMPT_ANSWER_FROM_DOCUMENTS_HEAD) != 0) {
        goto fail;
    }

    int running_tokens = 0;
    size_t documents_included = 0;
    for (size_t i = 0; i < doc_count; i++) {
        int doc_tokens = local_llm_count_tokens(docs[i].text);
        if (doc_tokens < 0) {
            doc_tokens = 0;
        }

        const char *text_to_include = docs[i].text;
        char *truncated = NULL;
        if (running_tokens + doc_tokens > budget_tokens) {
            if (documents_included > 0) {
                /* Already have at least one whole document in -- stop
                 * here rather than dilute it with a truncated fragment
                 * of the next one. */
                break;
            }
            size_t max_bytes = (size_t)budget_tokens * GENERATION_BYTES_PER_TOKEN_ESTIMATE;
            size_t text_len = strlen(docs[i].text);
            if (max_bytes < text_len) {
                truncated = malloc(max_bytes + 1);
                if (truncated == NULL) {
                    goto fail;
                }
                memcpy(truncated, docs[i].text, max_bytes);
                truncated[max_bytes] = '\0';
                text_to_include = truncated;
            }
        }

        int appended = string_builder_append(&builder, "[Source: ") != 0 ||
                        string_builder_append(&builder, docs[i].document_name) != 0 ||
                        string_builder_append(&builder, "]\n") != 0 ||
                        string_builder_append(&builder, text_to_include) != 0 ||
                        string_builder_append(&builder, "\n\n") != 0;
        free(truncated);
        if (appended) {
            goto fail;
        }

        running_tokens += doc_tokens;
        documents_included++;
        if (running_tokens >= budget_tokens) {
            break;
        }
    }
    pg_store_documents_free(docs, doc_count);

    if (documents_included == 0) {
        free(builder.data);
        return NULL;
    }

    if (string_builder_append(&builder, "Question: ") != 0 ||
        string_builder_append(&builder, query_text) != 0 ||
        string_builder_append(&builder, "\n\nAnswer:") != 0) {
        goto fail_no_docs;
    }

    return builder.data;

fail:
    pg_store_documents_free(docs, doc_count);
fail_no_docs:
    free(builder.data);
    return NULL;
}

char *generation_generate_answer_from_documents(const char *query_text, PgStore *store, const LocalLlmTurn *history,
                                                 size_t history_count) {
    char *prompt = generation_build_document_prompt(query_text, store,
                                                      LOCAL_LLM_N_CTX - GENERATION_DOCUMENT_CONTEXT_RESERVED_TOKENS);
    if (prompt == NULL) {
        return NULL;
    }

    if (history_count == 0) {
        LocalLlmTurn turn = {.role = "user", .content = prompt};
        char *answer = local_llm_chat_completion_multi(&turn, 1, LEXIS_PREFILL_NO_THINK);
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

    char *answer = local_llm_chat_completion_multi(turns, windowed_count + 1, LEXIS_PREFILL_NO_THINK);
    free(turns);
    free(prompt);
    return answer;
}

/* Builds the SUMMARY-tool prompt. Small by construction -- a group summary
 * is a few hundred tokens regardless of corpus size, which is the entire
 * reason this path exists instead of re-reading documents.
 *
 * The two negative instructions are both there in response to observed
 * behavior, not as boilerplate. "Never ask the user to provide documents"
 * addresses a real answer -- "Please provide the documents you are
 * referring to" -- produced for a question about a corpus whose text was
 * already in the prompt: with chatty history ahead of it, the model
 * followed the conversational framing instead of the attached context.
 * "Don't mention the summary" stops the reply becoming "the summary says
 * ...", which tells the reader about the plumbing rather than answering
 * them. */
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

char *generation_generate_answer_from_summary(const char *query_text, const char *summary_text,
                                              const LocalLlmTurn *history, size_t history_count) {
    if (summary_text == NULL || summary_text[0] == '\0') {
        return NULL;
    }

    char *prompt = generation_build_summary_prompt(query_text, summary_text);
    if (prompt == NULL) {
        return NULL;
    }

    if (history_count == 0) {
        LocalLlmTurn turn = {.role = "user", .content = prompt};
        char *answer = local_llm_chat_completion_multi_ex(&turn, 1, NULL, /*force_thinking=*/1);
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

    char *answer = local_llm_chat_completion_multi_ex(turns, windowed_count + 1, NULL, /*force_thinking=*/1);
    free(turns);
    free(prompt);
    return answer;
}
