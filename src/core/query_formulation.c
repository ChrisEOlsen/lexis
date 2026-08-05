/*
 * Implementation of small-model query formulation.
 * See include/query_formulation.h for the module's role (spec 5.2.4, Stage 7).
 */

/* See tokenizer.c for why this must come before any #include (strdup is a
 * POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "query_formulation.h"

#include "local_llm_client.h"
#include "string_builder.h"
#include "tokenizer.h"

#include <cJSON.h>
#include <stdlib.h>
#include <string.h>

/* Cap on how many words from each candidate category (synonyms/
 * hypernyms/hyponyms) go into the prompt per term. Arbitrary first-N,
 * not a ranked top-N -- TokenList carries no relevance ordering, just
 * synset-iteration order. See LIMITATIONS.md. */
#define QUERY_FORMULATION_MAX_CANDIDATES 8

/* Appends up to QUERY_FORMULATION_MAX_CANDIDATES words from `list`,
 * comma-separated. */
static int append_capped_word_list(StringBuilder *builder, const TokenList *list) {
    size_t limit = (list->count < QUERY_FORMULATION_MAX_CANDIDATES)
                       ? list->count
                       : QUERY_FORMULATION_MAX_CANDIDATES;

    for (size_t i = 0; i < limit; i++) {
        if (i > 0 && string_builder_append(builder, ", ") != 0) {
            return -1;
        }
        if (string_builder_append(builder, list->terms[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

void query_formulation_candidates_free(QueryFormulationCandidates *candidates) {
    if (candidates == NULL) {
        return;
    }

    for (size_t i = 0; i < candidates->count; i++) {
        free(candidates->terms[i].term);
    }
    free(candidates->terms);
    free(candidates);
}

QueryFormulationCandidates *query_formulation_gather_candidates(
    const char *query_text, const StopwordSet *stopwords, const WordNetTable *wordnet,
    const Lemmatizer *lemmatizer) {
    TokenList *terms = tokenize(query_text);
    if (terms == NULL) {
        return NULL;
    }
    stopwords_filter(terms, stopwords);

    QueryFormulationCandidates *result = malloc(sizeof(QueryFormulationCandidates));
    if (result == NULL) {
        token_list_free(terms);
        return NULL;
    }
    result->count = 0;
    result->terms = NULL;

    /* terms->count == 0 (a query that was entirely stopwords) is a valid
     * outcome, not a failure -- guard the malloc explicitly rather than
     * calling malloc(0), which is implementation-defined and could
     * return NULL, getting misread as an allocation failure below. */
    if (terms->count == 0) {
        token_list_free(terms);
        return result;
    }

    result->terms = malloc(terms->count * sizeof(QueryFormulationTermCandidates));
    if (result->terms == NULL) {
        free(result);
        token_list_free(terms);
        return NULL;
    }

    for (size_t i = 0; i < terms->count; i++) {
        /* Lemmatize before lookup ("called" -> "call") so candidates
         * come from the right WordNet entry, and before storing `term`
         * itself so the eventual BM25 search term matches what
         * bulk_ingest.c's Phase 2 worker stores in the index (also
         * lemmatized). */
        char *lemma = lemmatize(lemmatizer, wordnet, terms->terms[i]);
        if (lemma == NULL) {
            query_formulation_candidates_free(result);
            token_list_free(terms);
            return NULL;
        }
        result->terms[i].term = lemma;
        result->terms[i].candidates = wordnet_lookup(wordnet, lemma);
        result->count++;
    }

    token_list_free(terms);
    return result;
}

char *query_formulation_build_prompt(const char *query_text,
                                      const QueryFormulationCandidates *candidates) {
    StringBuilder builder = {NULL, 0, 0};

    if (string_builder_append(&builder,
            "You are helping build a search query for a keyword-based (BM25) search engine.\n\n") != 0) {
        goto fail;
    }
    if (string_builder_append(&builder, "Original question: \"") != 0) {
        goto fail;
    }
    if (string_builder_append(&builder, query_text) != 0) {
        goto fail;
    }
    if (string_builder_append(&builder,
            "\"\n\nFor each query term below, candidate related words are listed: synonyms "
            "(same meaning), hypernyms (broader terms), and hyponyms (narrower terms). Select "
            "every word likely to appear in a document relevant to the original question, "
            "including the original term itself when it is a good search term. Respond with "
            "ONLY a JSON array of strings -- no other text.\n\n") != 0) {
        goto fail;
    }

    for (size_t i = 0; i < candidates->count; i++) {
        const QueryFormulationTermCandidates *term = &candidates->terms[i];

        if (string_builder_append(&builder, "Term: \"") != 0) {
            goto fail;
        }
        if (string_builder_append(&builder, term->term) != 0) {
            goto fail;
        }
        if (string_builder_append(&builder, "\"\n") != 0) {
            goto fail;
        }

        if (term->candidates == NULL) {
            if (string_builder_append(&builder, "  (not found in WordNet -- no related words)\n") != 0) {
                goto fail;
            }
        } else {
            if (term->candidates->synonyms->count > 0) {
                if (string_builder_append(&builder, "  synonyms: ") != 0 ||
                    append_capped_word_list(&builder, term->candidates->synonyms) != 0 ||
                    string_builder_append(&builder, "\n") != 0) {
                    goto fail;
                }
            }
            if (term->candidates->hypernyms->count > 0) {
                if (string_builder_append(&builder, "  hypernyms: ") != 0 ||
                    append_capped_word_list(&builder, term->candidates->hypernyms) != 0 ||
                    string_builder_append(&builder, "\n") != 0) {
                    goto fail;
                }
            }
            if (term->candidates->hyponyms->count > 0) {
                if (string_builder_append(&builder, "  hyponyms: ") != 0 ||
                    append_capped_word_list(&builder, term->candidates->hyponyms) != 0 ||
                    string_builder_append(&builder, "\n") != 0) {
                    goto fail;
                }
            }
        }

        if (string_builder_append(&builder, "\n") != 0) {
            goto fail;
        }
    }

    return builder.data;

fail:
    free(builder.data);
    return NULL;
}

/* Copies `candidates`'s original terms into a fresh TokenList -- used
 * whenever the small model's response can't be trusted (API failure or
 * an unparseable/empty response), so search still runs on the plain
 * stopword-filtered query instead of failing outright. */
static TokenList *query_formulation_fallback_terms(const QueryFormulationCandidates *candidates) {
    TokenList *result = token_list_create();
    if (result == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < candidates->count; i++) {
        if (token_list_append(result, candidates->terms[i].term) != 0) {
            token_list_free(result);
            return NULL;
        }
    }

    return result;
}

TokenList *query_formulation_parse_selected_terms(
    const char *response_text, const QueryFormulationCandidates *fallback_candidates) {
    cJSON *parsed = cJSON_Parse(response_text);
    if (parsed == NULL || !cJSON_IsArray(parsed)) {
        cJSON_Delete(parsed);
        return query_formulation_fallback_terms(fallback_candidates);
    }

    TokenList *result = token_list_create();
    if (result == NULL) {
        cJSON_Delete(parsed);
        return NULL;
    }

    int array_size = cJSON_GetArraySize(parsed);
    for (int i = 0; i < array_size; i++) {
        cJSON *item = cJSON_GetArrayItem(parsed, i);
        if (item != NULL && cJSON_IsString(item)) {
            if (token_list_append(result, item->valuestring) != 0) {
                cJSON_Delete(parsed);
                token_list_free(result);
                return NULL;
            }
        }
    }
    cJSON_Delete(parsed);

    if (result->count == 0) {
        token_list_free(result);
        return query_formulation_fallback_terms(fallback_candidates);
    }

    return result;
}

TokenList *query_formulation_formulate_query(const char *query_text,
                                              const StopwordSet *stopwords,
                                              const WordNetTable *wordnet,
                                              const Lemmatizer *lemmatizer) {
    QueryFormulationCandidates *candidates =
        query_formulation_gather_candidates(query_text, stopwords, wordnet, lemmatizer);
    if (candidates == NULL) {
        return NULL;
    }

    if (candidates->count == 0) {
        /* Nothing survived stopword filtering -- nothing to expand or
         * search for. A valid empty outcome, not a failure. */
        query_formulation_candidates_free(candidates);
        return token_list_create();
    }

    char *prompt = query_formulation_build_prompt(query_text, candidates);
    if (prompt == NULL) {
        query_formulation_candidates_free(candidates);
        return NULL;
    }

    char *response = local_llm_chat_completion(prompt);
    free(prompt);

    TokenList *selected_terms;
    if (response == NULL) {
        selected_terms = query_formulation_fallback_terms(candidates);
    } else {
        selected_terms = query_formulation_parse_selected_terms(response, candidates);
        free(response);
    }

    query_formulation_candidates_free(candidates);
    return selected_terms;
}

TokenList *query_formulation_terms_only(const char *query_text, const StopwordSet *stopwords,
                                         const WordNetTable *wordnet, const Lemmatizer *lemmatizer) {
    QueryFormulationCandidates *candidates =
        query_formulation_gather_candidates(query_text, stopwords, wordnet, lemmatizer);
    if (candidates == NULL) {
        return NULL;
    }

    /* Same "nothing survived stopword filtering" empty-outcome handling
     * as query_formulation_formulate_query() -- no prompt, no model call,
     * just the plain lemmatized terms query_formulation_fallback_terms()
     * would have produced anyway had the LLM call failed. */
    TokenList *terms = (candidates->count == 0) ? token_list_create()
                                                 : query_formulation_fallback_terms(candidates);
    query_formulation_candidates_free(candidates);
    return terms;
}

/* Reserves room for this call's own prompt wrapper + the question itself
 * + the model's reformulated-question output -- generous since a
 * rewritten question is always short, unlike generation's own budget
 * (see generation.c's equivalent constant), which also has to leave room
 * for a much longer answer. */
#define QUERY_FORMULATION_CONTEXTUALIZE_RESERVED_TOKENS 1000

/* Trims `history` to the newest suffix that fits within `budget_tokens`
 * (measured via local_llm_count_tokens() per turn's own content -- the
 * per-turn chat-template markup itself is a small, roughly fixed
 * overhead this doesn't bother accounting for separately), walking
 * backward from the most recent turn so oldest turns are the ones
 * dropped. Sets *out_count to the number of turns kept (0 is valid --
 * the whole history got dropped because even the single most recent
 * turn alone doesn't fit). Returns a newly allocated array of
 * *out_count entries whose contents are borrowed from `history` (safe
 * since `history` outlives the caller's use of the result) -- caller
 * must free() the array itself, not its entries -- or NULL on
 * allocation failure. */
static LocalLlmTurn *window_history(const LocalLlmTurn *history, size_t history_count, int budget_tokens,
                                     size_t *out_count) {
    size_t start = history_count; /* first surviving index; history_count itself means "keep nothing" */
    int running_tokens = 0;
    for (size_t i = history_count; i-- > 0;) {
        int turn_tokens = local_llm_count_tokens(history[i].content);
        if (turn_tokens < 0) {
            turn_tokens = 0; /* a count failure shouldn't drop an otherwise-fitting turn */
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

char *query_formulation_contextualize_question(const char *question, const LocalLlmTurn *history,
                                                size_t history_count) {
    if (history_count == 0) {
        return strdup(question);
    }

    size_t windowed_count = 0;
    LocalLlmTurn *windowed = window_history(
        history, history_count, LOCAL_LLM_N_CTX - QUERY_FORMULATION_CONTEXTUALIZE_RESERVED_TOKENS, &windowed_count);
    if (windowed == NULL) {
        return NULL;
    }
    if (windowed_count == 0) {
        /* Even the single most recent turn didn't fit the budget --
         * nothing usable to contextualize against. */
        free(windowed);
        return strdup(question);
    }

    StringBuilder builder = {NULL, 0, 0};
    if (string_builder_append(&builder,
            "Given the conversation so far, rewrite the following question as a standalone "
            "question that makes sense with no prior context -- resolve any pronouns or "
            "references to what was discussed earlier. Respond with ONLY the rewritten "
            "question, no other text.\n\nQuestion: \"") != 0 ||
        string_builder_append(&builder, question) != 0 || string_builder_append(&builder, "\"") != 0) {
        free(builder.data);
        free(windowed);
        return strdup(question);
    }

    LocalLlmTurn *turns = malloc(sizeof(LocalLlmTurn) * (windowed_count + 1));
    if (turns == NULL) {
        free(builder.data);
        free(windowed);
        return strdup(question);
    }
    for (size_t i = 0; i < windowed_count; i++) {
        turns[i] = windowed[i];
    }
    free(windowed);
    turns[windowed_count] = (LocalLlmTurn){.role = "user", .content = builder.data};

    char *response = local_llm_chat_completion_multi(turns, windowed_count + 1, NULL);
    free(turns);
    free(builder.data);

    if (response == NULL || response[0] == '\0') {
        /* Call failed or produced nothing usable -- fall back to the
         * original question unresolved, same graceful-degradation shape
         * as query_formulation_formulate_query()'s WordNet-selection
         * fallback: an unreliable LLM step degrades search, doesn't
         * break it. */
        free(response);
        return strdup(question);
    }
    return response;
}
