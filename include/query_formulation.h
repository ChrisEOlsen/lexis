/*
 * Small-model BM25 query formulation (spec 5.2.4, build order Stage 7).
 * Takes the original query plus per-term candidate sets from WordNet
 * (src/core/wordnet.c) and selects the most likely variants, producing a
 * flat list of terms ready for bm25_search(). Acts as an intelligent
 * filter so synonym expansion doesn't degrade precision via noisy unions.
 *
 * Deliberately simpler than the spec's fuller vision of "a boolean BM25
 * query with phrase groupings and term weights" -- bm25_search() only
 * accepts a flat term list, with no boolean operators, phrase grouping,
 * or per-term weighting anywhere in bm25.c. Output here matches what the
 * retrieval engine can actually consume. See LIMITATIONS.md.
 */

#ifndef LEXIS_QUERY_FORMULATION_H
#define LEXIS_QUERY_FORMULATION_H

#include <stddef.h>

#include "lemmatizer.h"
#include "stopwords.h"
#include "tokenizer.h"
#include "wordnet.h"

/* One query term and its WordNet-derived candidates -- the raw material
 * handed to the small model so it can decide which specific words are
 * worth adding to the actual BM25 query. `term` is the LEMMATIZED base
 * form (e.g. "call", not "called") -- not the raw surface form the user
 * typed -- so it matches the lemmatized terms bulk_ingest.c's Phase 2
 * worker stores in the index (see ingest.c's ingest_lemmatize_terms()).
 * An owned copy. `candidates` is a
 * borrowed pointer into `wordnet` (the long-lived table loaded once at
 * startup) -- never freed here. `candidates` is NULL if the lemma isn't
 * in WordNet at all; it's still a valid term to search for on its own,
 * just has nothing to expand it with. */
typedef struct {
    char *term;
    const WordNetLookupResult *candidates;
} QueryFormulationTermCandidates;

/* Every surviving term from one query, each with its candidates. */
typedef struct {
    QueryFormulationTermCandidates *terms;
    size_t count;
} QueryFormulationCandidates;

/* Tokenizes `query_text`, strips stopwords, lemmatizes each surviving
 * term (e.g. "called" -> "call") via `lemmatizer`, and looks up each
 * lemma in `wordnet` for candidate synonyms/hypernyms/hyponyms. Returns
 * NULL on allocation failure. */
QueryFormulationCandidates *query_formulation_gather_candidates(
    const char *query_text, const StopwordSet *stopwords, const WordNetTable *wordnet,
    const Lemmatizer *lemmatizer);

/* Frees every term's owned copy, the terms array, and the struct itself.
 * Does NOT free anything reachable through `candidates` -- those are
 * borrowed from the WordNetTable, which outlives this call. Safe to
 * call with candidates == NULL. */
void query_formulation_candidates_free(QueryFormulationCandidates *candidates);

/* Builds the prompt asking a small model to select which candidate words
 * are worth adding to the search query, instructing it to respond with
 * ONLY a JSON array of strings. Each candidate category (synonyms/
 * hypernyms/hyponyms) is capped at QUERY_FORMULATION_MAX_CANDIDATES (see
 * .c file) per term -- an arbitrary first-N cap, not a ranked top-N,
 * since candidate lists don't carry any relevance ordering. See
 * LIMITATIONS.md. Returns NULL on allocation failure. */
char *query_formulation_build_prompt(const char *query_text,
                                      const QueryFormulationCandidates *candidates);

/* Parses `response_text` as a JSON array of strings into a flat term
 * list ready for bm25_search(). Falls back to `fallback_candidates`'s
 * original (unexpanded) terms if the response isn't valid JSON, isn't an
 * array, or parses to zero usable strings -- an unreliable external LLM
 * response degrades search gracefully rather than breaking it. Returns
 * NULL only on allocation failure. */
TokenList *query_formulation_parse_selected_terms(const char *response_text,
                                                   const QueryFormulationCandidates *fallback_candidates);

/* Runs the full query formulation step: gathers WordNet candidates,
 * builds the prompt, calls the local model, and parses the result --
 * falling back to the original stopword-filtered query terms if the call
 * itself fails (local_llm_chat_completion returns NULL) or the response
 * is unparseable. Returns an empty TokenList (not NULL) if nothing
 * survives stopword filtering -- a valid "nothing to search for" outcome,
 * not a failure. Returns NULL only on allocation failure. */
TokenList *query_formulation_formulate_query(const char *query_text,
                                              const StopwordSet *stopwords,
                                              const WordNetTable *wordnet,
                                              const Lemmatizer *lemmatizer);

/* The local-only half of query_formulation_formulate_query() -- tokenize,
 * stopword-filter, lemmatize, done. Skips WordNet candidate gathering's
 * *purpose* entirely: no prompt is built, no local model call is made, no
 * synonym/hypernym/hyponym expansion happens at all. Exists to answer a
 * real question this project didn't have real numbers for -- how much is
 * the LLM expansion-selection step (query_formulation_formulate_query())
 * actually improving retrieval quality, versus just plain lemmatized
 * query terms straight into bm25_search()? -- by making that comparison
 * directly runnable (see eval.c's `use_llm_expansion` parameter). Same
 * "empty TokenList (not NULL) if nothing survives stopword filtering, or
 * NULL only on allocation failure" contract. */
TokenList *query_formulation_terms_only(const char *query_text, const StopwordSet *stopwords,
                                         const WordNetTable *wordnet, const Lemmatizer *lemmatizer);

#endif /* LEXIS_QUERY_FORMULATION_H */
