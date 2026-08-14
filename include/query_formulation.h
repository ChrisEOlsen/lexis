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
#include "local_llm_client.h"
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

/* BM25 weight for expansion terms (everything after original_count in
 * parse_selected_terms's output) -- originals weigh 1.0. 0.4 sits in the
 * classical Rocchio/RM3 interpolation range: expansions assist ranking
 * but a passage matching only expansions cannot outrank one matching
 * the question's own words. Shared by every caller that searches an
 * expanded query (CLI, eval). */
#define LEXIS_EXPANSION_WEIGHT 0.4

/* Every surviving term from one query, each with its candidates. */
typedef struct {
    QueryFormulationTermCandidates *terms;
    size_t count;
} QueryFormulationCandidates;

/* Tokenizes `query_text`, strips stopwords, lemmatizes each surviving
 * term (e.g. "called" -> "call") via `lemmatizer`, and looks up each
 * lemma in `wordnet` for candidate synonyms/hypernyms/hyponyms. Returns
 * NULL on allocation failure. */
/* Builds candidates from an ALREADY tokenized/filtered/lemmatized term
 * list -- each term looked up in WordNet as given. This is the shared
 * entry into the expansion machinery: the CLI reaches it through
 * query_formulation_gather_candidates() (which does the text
 * processing first), the app's QueryWorker feeds it the raw+rewritten
 * question terms union directly. One retrieval pipeline, not two.
 * Returns NULL on allocation failure. */
QueryFormulationCandidates *query_formulation_gather_candidates_from_terms(
    const TokenList *terms, const WordNetTable *wordnet);

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

/* Builds the final search-term list: `candidates`'s original question
 * terms FIRST and unconditionally (deduplicated -- the model cannot
 * remove the question from its own search), then any expansions from
 * `response_text` (a JSON array of strings) that survive four checks:
 * parseable, actually offered in the prompt's candidate lists
 * (case-insensitive -- an invented term can't enter the query),
 * index-shaped after lowercasing (single ASCII word; WordNet
 * collocations like "family_line" can never match the tokenized terms
 * table), and not already present. An unparseable/NULL response
 * degrades to originals-only. If `original_count_out` is non-NULL it
 * receives how many leading entries are original terms -- the boundary
 * bm25's per-term weighting needs to discount expansions. Returns NULL
 * only on allocation failure. */
TokenList *query_formulation_parse_selected_terms(const char *response_text,
                                                   const QueryFormulationCandidates *candidates,
                                                   size_t *original_count_out);

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
                                              const Lemmatizer *lemmatizer,
                                              size_t *original_count_out);

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
/* The union of query_formulation_terms_only(`raw_query`) and
 * query_formulation_terms_only(`rewritten_query`), deduplicated, with the
 * raw query's terms first.
 *
 * Exists because contextual rewriting and lexical search want opposite
 * things. query_formulation_contextualize_question() resolves pronouns by
 * PARAPHRASING -- which is right for comprehension and wrong for BM25,
 * because a paraphrase can drop the one word the index is keyed on.
 * Measured against a real corpus: "What are the license classes?" was
 * rewritten to "What are the different types of driver licenses in New
 * York State?", which discarded "class" -- the only discriminative term,
 * since the source text literally reads "Class D", "Class M". Searching
 * the raw question put the two passages holding every license class at
 * ranks 1 and 2; searching the rewrite returned none of them at all.
 *
 * Taking the union means the rewrite can only ever ADD terms, never lose
 * one. `rewritten_query` may be NULL (or equal to `raw_query`), in which
 * case this degrades to plain terms_only() on the raw text.
 *
 * Duplicates are removed rather than left in: bm25_search() sums a score
 * contribution per supplied term, so a term appearing twice would be
 * double-weighted by accident rather than by design.
 *
 * Same "empty TokenList (not NULL) if nothing survives stopword
 * filtering, NULL only on allocation failure" contract as terms_only(). */
TokenList *query_formulation_terms_union(const char *raw_query, const char *rewritten_query,
                                          const StopwordSet *stopwords, const WordNetTable *wordnet,
                                          const Lemmatizer *lemmatizer);

TokenList *query_formulation_terms_only(const char *query_text, const StopwordSet *stopwords,
                                         const WordNetTable *wordnet, const Lemmatizer *lemmatizer);

/* Rewrites `question` as a standalone question given `history` (oldest
 * first, alternating "user"/"assistant" turns) -- resolves pronouns and
 * references against what was said earlier in the conversation (e.g.
 * "what about that instead?" -> "what is the minimum age for a Junior
 * Operator Class MJ license?"), so the *search* step gets a
 * self-contained query even though the user's actual question wasn't
 * one. Returns a copy of `question` unchanged if `history_count == 0` --
 * no LLM call, since there's nothing to resolve against on the first
 * message of a session. `history` is windowed internally (oldest turns
 * dropped first) to whatever fits under LOCAL_LLM_N_CTX alongside this
 * call's own prompt overhead. Falls back to a copy of `question` if the
 * model call fails or returns nothing usable -- same graceful-
 * degradation shape as query_formulation_formulate_query()'s WordNet-
 * selection fallback. Returns NULL only on allocation failure. */
char *query_formulation_contextualize_question(const char *question, const LocalLlmTurn *history,
                                                size_t history_count);

#endif /* LEXIS_QUERY_FORMULATION_H */
