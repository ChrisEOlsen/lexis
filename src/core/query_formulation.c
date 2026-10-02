/* Small-model query formulation (spec 5.2.4, Stage 7; see query_formulation.h). */

/* Must precede #includes: strdup is POSIX, hidden under strict -std=c11. */
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

/* Words per category (syn/hyper/hypo) per term in the prompt. Arbitrary first-N, not
 * ranked (TokenList has no relevance order; see dev/LIMITATIONS.md). */
#define QUERY_FORMULATION_MAX_CANDIDATES 8

/* Appends up to QUERY_FORMULATION_MAX_CANDIDATES words from list, comma-separated. */
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

    /* count==0 is valid, not failure: return before malloc(0), whose NULL-or-not is
     * implementation-defined and would misread as allocation failure. */
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
        /* Learned neighbors, copied (table owns its lists, this struct its own). NULL
         * table or no entry both mean "no learned candidates". */
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

    /* Lemmatize before lookup ("called"->"call") so candidates come from the right entry
     * and search terms match the (lemmatized) index. */
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
            /* Hyponyms deliberately NOT offered: they enumerate answers, not paraphrases
             * ("which dynasty?" + Bourbon_dynasty ranks wrong dynasties; see dev/LIMITATIONS.md). */
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

/* Case-insensitive "was this word offered?" across syn/hyper/learned lists: the model
 * can only keep/veto shown terms, never invent new ones. */
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

/* Lowercase copy of word, or NULL unless pure ASCII alnum. Rejects WordNet "a_b"/
 * hyphen strings (tokenizer strips punctuation, so they can't exist in the index). */
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
    /* Original question terms are searched unconditionally; the model can only ADD
     * expansions, never remove the question (see dev/LIMITATIONS.md king-tut postmortem). */
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

    /* Expansions need: parseable, offered, index-shaped, not present. Unparseable
     * degrades to originals-only (same graceful floor as the old fallback). */
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
        /* Nothing survived stopword filtering: valid empty outcome, not failure. */
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

    /* NULL flows through: cJSON_Parse(NULL) is unparseable, and originals-first
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

    /* Same empty-outcome handling as formulate_query(). parse_selected_terms(NULL, ...)
     * contributes no expansions: exactly the deduped lemmatized terms. */
    TokenList *terms = (candidates->count == 0)
                           ? token_list_create()
                           : query_formulation_parse_selected_terms(NULL, candidates, NULL);
    query_formulation_candidates_free(candidates);
    return terms;
}

/* Headroom for prompt wrapper + question + rewritten question (short, unlike
 * generation.c's answer-sized reservation). */
#define QUERY_FORMULATION_CONTEXTUALIZE_RESERVED_TOKENS 1000

/* Newest history suffix fitting budget_tokens (content only; markup negligible). Returns
 * malloc'd borrowed-entry array, *out_count kept (0 valid); NULL on alloc failure. */
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
        /* Even the newest turn didn't fit: nothing usable to contextualize against. */
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
        /* Failed/empty response: fall back to the original question (same graceful
         * degradation as formulate_query()'s fallback). */
        free(response);
        return strdup(question);
    }
    return response;
}
