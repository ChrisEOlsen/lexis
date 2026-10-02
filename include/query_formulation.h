/* Query formulation: sense-filter WordNet expansion candidates into a flat term list for BM25. */

#ifndef LEXIS_QUERY_FORMULATION_H
#define LEXIS_QUERY_FORMULATION_H

#include <stddef.h>

#include "lemmatizer.h"
#include "local_llm_client.h"
#include "stopwords.h"
#include "synonym_table.h"
#include "tokenizer.h"
#include "wordnet.h"

/* One query term (LEMMATIZED base form, owned) + its WordNet candidates (borrowed; NULL if absent).
 * NULL candidates still searches fine alone -- just nothing to expand with. */
typedef struct {
    char *term;
    const WordNetLookupResult *candidates;
    /* Learned-synonym neighbors, or NULL. Owned here; same sense filter and weights as WordNet. */
    TokenList *learned;
} QueryFormulationTermCandidates;

/* BM25 weight for expansion terms (originals weigh 1.0): expansions assist, never outrank. */
#define LEXIS_EXPANSION_WEIGHT 0.4

/* Every surviving term from one query, each with its candidates. */
typedef struct {
    QueryFormulationTermCandidates *terms;
    size_t count;
} QueryFormulationCandidates;

/* Tokenize, strip stopwords, lemmatize, look up each lemma's WordNet candidates. NULL on failure. */
/* Candidates from an ALREADY tokenized/filtered/lemmatized term list. NULL on failure. */
QueryFormulationCandidates *query_formulation_gather_candidates_from_terms(
    const TokenList *terms, const WordNetTable *wordnet, const SynonymTable *learned);

QueryFormulationCandidates *query_formulation_gather_candidates(
    const char *query_text, const StopwordSet *stopwords, const WordNetTable *wordnet,
    const Lemmatizer *lemmatizer);

/* Free terms, array, and struct. candidates (borrowed from WordNetTable) untouched. NULL-safe. */
void query_formulation_candidates_free(QueryFormulationCandidates *candidates);

/* Build the candidate-selection prompt (JSON array reply). Categories capped per term. NULL on failure. */
char *query_formulation_build_prompt(const char *query_text,
                                      const QueryFormulationCandidates *candidates);

/* Final term list: originals first (unconditional, deduped), then surviving expansions from response_text.
 * Unparseable/NULL response = originals-only. original_count_out = originals boundary. NULL on failure. */
TokenList *query_formulation_parse_selected_terms(const char *response_text,
                                                   const QueryFormulationCandidates *candidates,
                                                   size_t *original_count_out);

/* Full step: gather, prompt, call model, parse. Falls back to plain terms on model/parse failure.
 * Empty list (not NULL) if nothing survives filtering; NULL only on alloc failure. */
TokenList *query_formulation_formulate_query(const char *query_text,
                                              const StopwordSet *stopwords,
                                              const WordNetTable *wordnet,
                                              const Lemmatizer *lemmatizer,
                                              size_t *original_count_out);

/* Formulate without expansion: tokenize, filter, lemmatize, done. No model call.
 * Same empty-list/NULL contract as formulate_query. */
/* Union of terms_only(raw) and terms_only(rewritten), deduped, raw first. NULL rewritten = raw only.
 * Same empty-list/NULL contract as terms_only. */
TokenList *query_formulation_terms_union(const char *raw_query, const char *rewritten_query,
                                          const StopwordSet *stopwords, const WordNetTable *wordnet,
                                          const Lemmatizer *lemmatizer);

TokenList *query_formulation_terms_only(const char *query_text, const StopwordSet *stopwords,
                                         const WordNetTable *wordnet, const Lemmatizer *lemmatizer);

/* Rewrite question standalone against history (windowed under LOCAL_LLM_N_CTX), for search only.
 * history_count == 0 or model failure = copy of question. NULL only on alloc failure. */
char *query_formulation_contextualize_question(const char *question, const LocalLlmTurn *history,
                                                size_t history_count);

#endif /* LEXIS_QUERY_FORMULATION_H */
