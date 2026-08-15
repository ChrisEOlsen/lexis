/*
 * Implementation of small-model query formulation.
 * See include/query_formulation.h for the module's role (spec 5.2.4, Stage 7).
 */

/* See tokenizer.c for why this must come before any #include (strdup is a
 * POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "query_formulation.h"

#include "prompts.h"

#include "local_llm_client.h"
#include "string_builder.h"
#include "tokenizer.h"

#include <cJSON.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
        token_list_free(candidates->terms[i].learned);
    }
    free(candidates->terms);
    free(candidates);
}

QueryFormulationCandidates *query_formulation_gather_candidates_from_terms(
    const TokenList *terms, const WordNetTable *wordnet, const SynonymTable *learned) {
    QueryFormulationCandidates *result = malloc(sizeof(QueryFormulationCandidates));
    if (result == NULL) {
        return NULL;
    }
    result->count = 0;
    result->terms = NULL;

    /* terms->count == 0 is a valid outcome, not a failure -- guard the
     * malloc explicitly rather than calling malloc(0), which is
     * implementation-defined and could return NULL, getting misread as
     * an allocation failure below. */
    if (terms->count == 0) {
        return result;
    }

    result->terms = malloc(terms->count * sizeof(QueryFormulationTermCandidates));
    if (result->terms == NULL) {
        free(result);
        return NULL;
    }

    for (size_t i = 0; i < terms->count; i++) {
        char *term = strdup(terms->terms[i]);
        if (term == NULL) {
            query_formulation_candidates_free(result);
            return NULL;
        }
        result->terms[i].term = term;
        result->terms[i].candidates = wordnet_lookup(wordnet, term);
        /* Learned neighbors, copied (the table owns its lists, this
         * struct owns its own) -- NULL table or no entry both mean "no
         * learned candidates", which every consumer handles. */
        result->terms[i].learned = NULL;
        const TokenList *neighbors = synonym_table_lookup(learned, term);
        if (neighbors != NULL && neighbors->count > 0) {
            TokenList *copy = token_list_create();
            if (copy == NULL) {
                result->count++; /* term itself is valid; free path handles it */
                query_formulation_candidates_free(result);
                return NULL;
            }
            for (size_t n = 0; n < neighbors->count; n++) {
                if (token_list_append(copy, neighbors->terms[n]) != 0) {
                    token_list_free(copy);
                    result->count++;
                    query_formulation_candidates_free(result);
                    return NULL;
                }
            }
            result->terms[i].learned = copy;
        }
        result->count++;
    }

    return result;
}

QueryFormulationCandidates *query_formulation_gather_candidates(
    const char *query_text, const StopwordSet *stopwords, const WordNetTable *wordnet,
    const Lemmatizer *lemmatizer) {
    TokenList *terms = tokenize(query_text);
    if (terms == NULL) {
        return NULL;
    }
    stopwords_filter(terms, stopwords);

    /* Lemmatize before lookup ("called" -> "call") so candidates come
     * from the right WordNet entry, and so the eventual BM25 search term
     * matches what bulk_ingest.c's Phase 2 worker stores in the index
     * (also lemmatized). */
    TokenList *lemmas = token_list_create();
    if (lemmas == NULL) {
        token_list_free(terms);
        return NULL;
    }
    for (size_t i = 0; i < terms->count; i++) {
        char *lemma = lemmatize(lemmatizer, wordnet, terms->terms[i]);
        if (lemma == NULL || token_list_append(lemmas, lemma) != 0) {
            free(lemma);
            token_list_free(lemmas);
            token_list_free(terms);
            return NULL;
        }
        free(lemma);
    }
    token_list_free(terms);

    QueryFormulationCandidates *result =
        query_formulation_gather_candidates_from_terms(lemmas, wordnet, NULL);
    token_list_free(lemmas);
    return result;
}

char *query_formulation_build_prompt(const char *query_text,
                                      const QueryFormulationCandidates *candidates) {
    StringBuilder builder = {NULL, 0, 0};

    if (string_builder_append(&builder, LEXIS_PROMPT_QUERY_TERMS_HEAD) != 0) {
        goto fail;
    }
    if (string_builder_append(&builder, "Original question: \"") != 0) {
        goto fail;
    }
    if (string_builder_append(&builder, query_text) != 0) {
        goto fail;
    }
    if (string_builder_append(&builder, LEXIS_PROMPT_QUERY_TERMS_CANDIDATES) != 0) {
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
            if (term->learned != NULL && term->learned->count > 0) {
                if (string_builder_append(&builder, "  related (words used in similar contexts): ") != 0 ||
                    append_capped_word_list(&builder, term->learned) != 0 ||
                    string_builder_append(&builder, "\n") != 0) {
                    goto fail;
                }
            } else if (string_builder_append(&builder, "  (no related words known)\n") != 0) {
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
            if (term->learned != NULL && term->learned->count > 0) {
                if (string_builder_append(&builder, "  related (words used in similar contexts): ") != 0 ||
                    append_capped_word_list(&builder, term->learned) != 0 ||
                    string_builder_append(&builder, "\n") != 0) {
                    goto fail;
                }
            }
            /* Hyponyms are deliberately NOT offered. They enumerate the
             * answer space rather than paraphrase the question -- "which
             * dynasty?" expanded with Bourbon_dynasty/Han_dynasty pulls
             * passages about the wrong dynasties up the ranking. Measured
             * as part of the expansion-hurts-retrieval finding in
             * LIMITATIONS.md. */
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

static int token_list_contains(const TokenList *list, const char *word) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->terms[i], word) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Case-insensitive "was this word actually offered as a candidate?"
 * check across every term's synonym/hypernym lists. Constrains the
 * model to vetoing/keeping what it was shown -- an invented term can't
 * enter the query. Hyponyms aren't checked because build_prompt() no
 * longer offers them. */
static int is_offered_candidate(const QueryFormulationCandidates *candidates, const char *word) {
    for (size_t i = 0; i < candidates->count; i++) {
        const TokenList *learned = candidates->terms[i].learned;
        if (learned != NULL) {
            for (size_t j = 0; j < learned->count; j++) {
                if (strcasecmp(learned->terms[j], word) == 0) {
                    return 1;
                }
            }
        }
        const WordNetLookupResult *entry = candidates->terms[i].candidates;
        if (entry == NULL) {
            continue;
        }
        const TokenList *lists[2] = {entry->synonyms, entry->hypernyms};
        for (size_t l = 0; l < 2; l++) {
            for (size_t j = 0; j < lists[l]->count; j++) {
                if (strcasecmp(lists[l]->terms[j], word) == 0) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* Lowercase copy of `word`, or NULL if it contains anything but ASCII
 * letters/digits. Rejects WordNet collocations ("family_line") and
 * hyphenations outright: the ingest tokenizer strips punctuation, so no
 * such string can ever exist in the terms table -- they'd be dead
 * weight in the query. Lowercasing is what lets "Rex" match the
 * all-lowercase index. */
static char *normalize_expansion(const char *word) {
    size_t len = strlen(word);
    if (len == 0) {
        return NULL;
    }
    char *normalized = malloc(len + 1);
    if (normalized == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)word[i];
        if (!isalnum(c)) {
            free(normalized);
            return NULL;
        }
        normalized[i] = (char)tolower(c);
    }
    normalized[len] = '\0';
    return normalized;
}

TokenList *query_formulation_parse_selected_terms(
    const char *response_text, const QueryFormulationCandidates *candidates,
    size_t *original_count_out) {
    /* The original question terms are searched unconditionally -- the
     * model's selection can only ADD sense-checked expansions, never
     * remove the question itself from its own search. (The old contract
     * let the model drop original terms, and it did: see the king-tut
     * postmortem in LIMITATIONS.md.) */
    TokenList *result = token_list_create();
    if (result == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < candidates->count; i++) {
        if (token_list_contains(result, candidates->terms[i].term)) {
            continue; /* a question can repeat a word; search it once */
        }
        if (token_list_append(result, candidates->terms[i].term) != 0) {
            token_list_free(result);
            return NULL;
        }
    }
    if (original_count_out != NULL) {
        *original_count_out = result->count;
    }

    /* Expansions: accepted only if parseable, offered in the prompt,
     * index-shaped (single lowercase word), and not already present. An
     * unparseable response degrades to originals-only -- same graceful
     * floor the old fallback provided. */
    cJSON *parsed = cJSON_Parse(response_text);
    if (parsed != NULL && cJSON_IsArray(parsed)) {
        int array_size = cJSON_GetArraySize(parsed);
        for (int i = 0; i < array_size; i++) {
            cJSON *item = cJSON_GetArrayItem(parsed, i);
            if (item == NULL || !cJSON_IsString(item)) {
                continue;
            }
            char *normalized = normalize_expansion(item->valuestring);
            if (normalized == NULL) {
                continue;
            }
            if (token_list_contains(result, normalized) ||
                !is_offered_candidate(candidates, normalized)) {
                free(normalized);
                continue;
            }
            if (token_list_append(result, normalized) != 0) {
                free(normalized);
                cJSON_Delete(parsed);
                token_list_free(result);
                return NULL;
            }
            free(normalized);
        }
    }
    cJSON_Delete(parsed);

    return result;
}

TokenList *query_formulation_formulate_query(const char *query_text,
                                              const StopwordSet *stopwords,
                                              const WordNetTable *wordnet,
                                              const Lemmatizer *lemmatizer,
                                              size_t *original_count_out) {
    QueryFormulationCandidates *candidates =
        query_formulation_gather_candidates(query_text, stopwords, wordnet, lemmatizer);
    if (candidates == NULL) {
        return NULL;
    }

    if (candidates->count == 0) {
        /* Nothing survived stopword filtering -- nothing to expand or
         * search for. A valid empty outcome, not a failure. */
        query_formulation_candidates_free(candidates);
        if (original_count_out != NULL) {
            *original_count_out = 0;
        }
        return token_list_create();
    }

    char *prompt = query_formulation_build_prompt(query_text, candidates);
    if (prompt == NULL) {
        query_formulation_candidates_free(candidates);
        return NULL;
    }

    char *response = local_llm_chat_completion(prompt);
    free(prompt);

    /* A NULL response flows through unchanged -- cJSON_Parse treats NULL
     * as unparseable, and the parser's originals-first contract already
     * degrades that to plain question terms. */
    TokenList *selected_terms =
        query_formulation_parse_selected_terms(response, candidates, original_count_out);
    free(response);

    query_formulation_candidates_free(candidates);
    return selected_terms;
}

TokenList *query_formulation_terms_union(const char *raw_query, const char *rewritten_query,
                                          const StopwordSet *stopwords, const WordNetTable *wordnet,
                                          const Lemmatizer *lemmatizer) {
    TokenList *combined = query_formulation_terms_only(raw_query, stopwords, wordnet, lemmatizer);
    if (combined == NULL) {
        return NULL;
    }
    if (rewritten_query == NULL || strcmp(rewritten_query, raw_query) == 0) {
        return combined;
    }

    TokenList *extra = query_formulation_terms_only(rewritten_query, stopwords, wordnet, lemmatizer);
    if (extra == NULL) {
        /* The rewrite's terms are an enhancement, not a precondition --
         * the raw query's terms alone are a perfectly good search. */
        return combined;
    }

    for (size_t i = 0; i < extra->count; i++) {
        int already_present = 0;
        for (size_t j = 0; j < combined->count; j++) {
            if (strcmp(extra->terms[i], combined->terms[j]) == 0) {
                already_present = 1;
                break;
            }
        }
        /* O(n*m) over two short lists -- a handful of terms each, so a
         * hash set would cost more in machinery than it saves. */
        if (!already_present && token_list_append(combined, extra->terms[i]) != 0) {
            break; /* Out of memory: keep what we have rather than lose the query. */
        }
    }

    token_list_free(extra);
    return combined;
}

TokenList *query_formulation_terms_only(const char *query_text, const StopwordSet *stopwords,
                                         const WordNetTable *wordnet, const Lemmatizer *lemmatizer) {
    QueryFormulationCandidates *candidates =
        query_formulation_gather_candidates(query_text, stopwords, wordnet, lemmatizer);
    if (candidates == NULL) {
        return NULL;
    }

    /* Same "nothing survived stopword filtering" empty-outcome handling
     * as query_formulation_formulate_query() -- no prompt, no model call.
     * parse_selected_terms(NULL, ...) is the originals-only path: a NULL
     * response contributes no expansions, leaving exactly the plain
     * deduplicated lemmatized terms. */
    TokenList *terms = (candidates->count == 0)
                           ? token_list_create()
                           : query_formulation_parse_selected_terms(NULL, candidates, NULL);
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
    if (string_builder_append(&builder, LEXIS_PROMPT_CONTEXTUALIZE_HEAD) != 0 ||
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
