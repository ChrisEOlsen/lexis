/*
 * Implementation of the shared retrieval orchestrator.
 * See include/retrieval.h for the module's role and its two rules
 * (policy values not caller flags; observers read artifacts).
 */

#define _POSIX_C_SOURCE 200809L

#include "retrieval.h"

#include "local_llm_client.h"
#include "query_formulation.h"
#include "config.h"
#include "reranker.h"
#include "synonym_table.h"

#include <stdlib.h>
#include <time.h>

/* The learned synonym table, loaded once per process, lazily -- same
 * pattern as generation.c's thinking gate. Missing file = NULL = no
 * learned candidates, quietly (the table is optional shipped data). */
static const SynonymTable *learned_synonyms(void) {
    static SynonymTable *table = NULL;
    static int attempted = 0;
    if (!attempted) {
        attempted = 1;
        table = synonym_table_load(LEXIS_SYNONYMS_PATH_DEFAULT);
    }
    return table;
}

/* The optional embedding reranker, initialized once per process from
 * config `reranker_model_path` -- same lazy pattern as the synonym
 * table above. No config line = never initialized = pure BM25 order. */
static int reranker_ready(void) {
    static int attempted = 0;
    static int ready = 0;
    if (!attempted) {
        attempted = 1;
        char *path = config_load_reranker_model_path(LEXIS_CONFIG_PATH_DEFAULT);
        if (path != NULL) {
            ready = (reranker_init(path) == 0);
            free(path);
        }
    }
    return ready;
}

static long elapsed_ms(struct timespec start, struct timespec end) {
    long seconds = end.tv_sec - start.tv_sec;
    long nanoseconds = end.tv_nsec - start.tv_nsec;
    return seconds * 1000 + nanoseconds / 1000000;
}

RetrievalPolicy retrieval_default_policy(void) {
    RetrievalPolicy policy;
    policy.candidate_ceiling = LEXIS_SEARCH_CANDIDATE_CEILING;
    policy.max_passages = LEXIS_SEARCH_MAX_PASSAGES;
    policy.token_budget = LEXIS_SEARCH_TOKEN_BUDGET;
    policy.score_floor_ratio = LEXIS_SEARCH_SCORE_FLOOR_RATIO;
    policy.use_expansion = 1;
    policy.bm25.k1 = BM25_DEFAULT_K1;
    policy.bm25.b = BM25_DEFAULT_B;
    policy.bm25.coord_bonus = BM25_DEFAULT_COORD_BONUS;
    policy.corpus_stats = NULL;

    /* Experiment overrides for tuning sweeps (see TESTING.md) -- unset
     * means the shipped defaults above. Kept in the default-policy
     * constructor, not scattered through callers, so a sweep tunes every
     * driver (CLI, app, eval) identically. */
    const char *k1_env = getenv("LEXIS_BM25_K1");
    const char *b_env = getenv("LEXIS_BM25_B");
    if (k1_env != NULL && atof(k1_env) > 0.0) {
        policy.bm25.k1 = atof(k1_env);
    }
    if (b_env != NULL && atof(b_env) >= 0.0) {
        policy.bm25.b = atof(b_env);
    }
    return policy;
}

void retrieval_run_free(RetrievalRun *run) {
    if (run == NULL) {
        return;
    }
    free(run->expansion_prompt);
    free(run->expansion_response);
    token_list_free(run->terms);
    bm25_result_set_free(run->results);
    free(run);
}

RetrievalRun *retrieval_run(PgStore *store, const char *question, const char *rewritten_question,
                            const StopwordSet *stopwords, const WordNetTable *wordnet,
                            const Lemmatizer *lemmatizer, const RetrievalPolicy *policy) {
    RetrievalRun *run = calloc(1, sizeof(RetrievalRun));
    if (run == NULL) {
        return NULL;
    }

    struct timespec formulation_start, formulation_end;
    clock_gettime(CLOCK_MONOTONIC, &formulation_start);

    /* 1. The original terms. With a rewritten question, the UNION of
     * both questions' terms, raw first -- reformulation resolves
     * follow-ups into something searchable, but it paraphrases, and a
     * paraphrase can drop the one term the index is keyed on (measured:
     * "license classes" -> a rewrite that lost "class"); the union lets
     * the rewrite only ever add. */
    TokenList *base =
        (rewritten_question != NULL)
            ? query_formulation_terms_union(question, rewritten_question, stopwords, wordnet,
                                             lemmatizer)
            : query_formulation_terms_only(question, stopwords, wordnet, lemmatizer);
    if (base == NULL) {
        free(run);
        return NULL;
    }
    run->terms = base;
    run->original_count = base->count;

    if (base->count == 0) {
        /* Entirely stopwords -- a valid outcome the caller words for the
         * user; nothing to expand or search. */
        clock_gettime(CLOCK_MONOTONIC, &formulation_end);
        run->formulation_ms = elapsed_ms(formulation_start, formulation_end);
        return run;
    }

    /* 2. Sense-filtered WordNet expansion, policy-gated. The prompt
     * shows the rewritten question when there is one (standalone, so
     * the model can sense-check candidates without chat history). Any
     * failure degrades to the plain terms already in run->terms. */
    if (policy->use_expansion) {
        const char *prompt_question = (rewritten_question != NULL) ? rewritten_question : question;
        QueryFormulationCandidates *candidates =
            query_formulation_gather_candidates_from_terms(base, wordnet, learned_synonyms());
        if (candidates != NULL && candidates->count > 0) {
            run->expansion_prompt = query_formulation_build_prompt(prompt_question, candidates);
            if (run->expansion_prompt != NULL) {
                run->expansion_response = local_llm_chat_completion(run->expansion_prompt);
            }
            run->used_fallback = (run->expansion_response == NULL);

            size_t original_count = 0;
            TokenList *expanded = query_formulation_parse_selected_terms(
                run->expansion_response, candidates, &original_count);
            if (expanded != NULL) {
                token_list_free(base);
                base = NULL;
                run->terms = expanded;
                run->original_count = original_count;
            }
        }
        query_formulation_candidates_free(candidates);
    }
    clock_gettime(CLOCK_MONOTONIC, &formulation_end);
    run->formulation_ms = elapsed_ms(formulation_start, formulation_end);

    /* 3. Weighted search: originals at full weight, expansions
     * discounted so they can assist a passage but never let one outrank
     * a passage matching the question itself. */
    struct timespec search_start, search_end;
    clock_gettime(CLOCK_MONOTONIC, &search_start);

    const char **query_terms = malloc(run->terms->count * sizeof(char *));
    double *term_weights = malloc(run->terms->count * sizeof(double));
    if (query_terms == NULL || term_weights == NULL) {
        free(query_terms);
        free(term_weights);
        retrieval_run_free(run);
        return NULL;
    }
    for (size_t i = 0; i < run->terms->count; i++) {
        query_terms[i] = run->terms->terms[i];
        term_weights[i] = (i < run->original_count) ? 1.0 : LEXIS_EXPANSION_WEIGHT;
    }

    BM25CorpusStats stats =
        (policy->corpus_stats != NULL) ? *policy->corpus_stats : bm25_corpus_stats(store);
    run->results = (stats.total_passages >= 0)
                       ? bm25_search_weighted(store, query_terms, term_weights, run->terms->count,
                                               policy->candidate_ceiling, stats, policy->bm25)
                       : NULL;
    free(query_terms);
    free(term_weights);
    if (run->results == NULL) {
        retrieval_run_free(run);
        return NULL;
    }

    /* 4. Optional meaning-based reorder of the whole candidate list,
     * BEFORE the trim -- the point is exactly to rescue passages BM25
     * ranked below the cutoff. The question embedded is the standalone
     * one when a rewrite exists. On any reranker failure the BM25 order
     * stands untouched. */
    int reranked = 0;
    if (reranker_ready()) {
        const char *embed_question = (rewritten_question != NULL) ? rewritten_question : question;
        reranked = (reranker_rescore(store, embed_question, run->results) == 0);
    }

    /* 5. Rank deep, send shallow -- trim to what is worth putting in
     * front of the model, per policy. Done here, once, so "what the
     * model reads" and "what any observer shows" are the same list.
     * The score floor is a BM25-scale heuristic; fused reciprocal-rank
     * scores live on a different scale, so it is disabled when the
     * reranker actually ran (the passage cap and token budget still
     * bound the prompt). */
    if (policy->max_passages > 0) {
        double floor_ratio = reranked ? 0.0 : policy->score_floor_ratio;
        bm25_result_set_trim(store, run->results, policy->max_passages, policy->token_budget,
                             floor_ratio);
    }
    clock_gettime(CLOCK_MONOTONIC, &search_end);
    run->search_ms = elapsed_ms(search_start, search_end);

    return run;
}
