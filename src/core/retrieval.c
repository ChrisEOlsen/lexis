/* Shared retrieval orchestrator; see retrieval.h (policy values, observers read artifacts). */

#define _POSIX_C_SOURCE 200809L

#include "retrieval.h"

#include "local_llm_client.h"
#include "query_formulation.h"
#include "config.h"
#include "paths.h"
#include "reranker.h"
#include "synonym_table.h"
#include "time_util.h"

#include <stdlib.h>
#include <time.h>

/* Learned synonyms, lazy once-per-process. Missing file = NULL = no learned candidates. */
static const SynonymTable *learned_synonyms(void) {
    static SynonymTable *table = NULL;
    static int attempted = 0;
    if (!attempted) {
        attempted = 1;
        char *table_path = lexis_paths_resource(LEXIS_SYNONYMS_PATH_DEFAULT);
        if (table_path != NULL) {
            table = synonym_table_load(table_path);
            free(table_path);
        }
    }
    return table;
}

/* Optional reranker from config reranker_model_path, lazy once-per-process.
 * User toggle gates init so disable skips loading the ~67MB model. */
static int reranker_user_enabled = 1; /* default: whatever the config says */

/* Lazy-init state, file-scope so the toggle below can reset it. */
static int reranker_attempted = 0;
static int reranker_loaded = 0;

void retrieval_set_reranker_enabled(int enabled) {
    const int want = enabled ? 1 : 0;
    if (want && !reranker_user_enabled) {
        /* Re-enable resets attempt flag for one fresh lazy load; already-loaded model is kept. */
        reranker_attempted = 0;
    }
    reranker_user_enabled = want;
}

static int reranker_ready(void) {
    if (!reranker_user_enabled) {
        return 0;
    }
    if (!reranker_attempted) {
        reranker_attempted = 1;
        char *path = config_load_reranker_model_path(lexis_paths_config_file());
        if (path != NULL) {
            reranker_loaded = (reranker_init(path) == 0);
            free(path);
        }
    }
    return reranker_loaded;
}

RetrievalPolicy retrieval_default_policy(void) {
    RetrievalPolicy policy;
    /* Config first (compiled defaults when unset), tuning-sweep env wins; see dev/TESTING.md. */
    const char *config = lexis_paths_config_file();
    policy.candidate_ceiling = config_load_candidate_ceiling(config);
    policy.max_passages = config_load_max_passages(config);
    policy.token_budget = config_load_token_budget(config);
    policy.score_floor_ratio = config_load_score_floor_ratio(config);
    policy.use_expansion = 1;
    policy.bm25.k1 = config_load_bm25_k1(config);
    policy.bm25.b = config_load_bm25_b(config);
    policy.bm25.coord_bonus = BM25_DEFAULT_COORD_BONUS;
    policy.corpus_stats = NULL;

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

    /* 1. Original terms; with a rewrite, the union of both (rewrites can drop key terms). */
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
        /* All stopwords: valid outcome, nothing to expand or search. */
        clock_gettime(CLOCK_MONOTONIC, &formulation_end);
        run->formulation_ms = lexis_elapsed_ms(formulation_start, formulation_end);
        return run;
    }

    /* 2. Policy-gated WordNet expansion (standalone rewrite as prompt question); failure keeps plain terms. */
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
    run->formulation_ms = lexis_elapsed_ms(formulation_start, formulation_end);

    /* 3. Weighted search: expansions discounted so they can't outrank original-term matches. */
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

    /* 4. Optional rerank before trim (rescues sub-cutoff passages); failure keeps BM25 order. */
    int reranked = 0;
    if (reranker_ready()) {
        const char *embed_question = (rewritten_question != NULL) ? rewritten_question : question;
        reranked = (reranker_rescore(store, embed_question, run->results) == 0);
    }

    /* 5. Trim per policy (model and observers share this list); score floor disabled after rerank (different scale). */
    if (policy->max_passages > 0) {
        double floor_ratio = reranked ? 0.0 : policy->score_floor_ratio;
        bm25_result_set_trim(store, run->results, policy->max_passages, policy->token_budget,
                             floor_ratio);
    }
    clock_gettime(CLOCK_MONOTONIC, &search_end);
    run->search_ms = lexis_elapsed_ms(search_start, search_end);

    return run;
}
