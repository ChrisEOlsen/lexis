/*
 * The shared retrieval orchestrator -- THE pipeline from question to
 * ranked passages, run identically by the CLI (main.c's run_query), the
 * app (QueryWorker's SEARCH path), and the eval harness (eval.c).
 *
 * Two rules keep it single-sourced:
 *
 *   1. Differences between callers are RetrievalPolicy VALUES, never
 *      code branches on caller identity. There is no "cli" flag and
 *      there must never be one -- a flag would braid two pipelines back
 *      into one function. Eval's deeper, untrimmed search is a policy
 *      override, visible at its call site.
 *
 *   2. Observers read artifacts, they don't re-run stages. RetrievalRun
 *      carries what each stage saw and produced (the expansion prompt,
 *      the raw model reply, the final term list, timings) so query_log
 *      and the app's source inspector both consume the same record of
 *      the one run that actually happened.
 *
 * What deliberately stays OUTSIDE this module: tool routing, chat
 * history management and contextualization (the rewritten question is
 * an input here, produced by the app's chat layer), corpus scoping,
 * generation, and presentation. Those are the products around the
 * pipeline, not the pipeline.
 */

#ifndef LEXIS_RETRIEVAL_H
#define LEXIS_RETRIEVAL_H

#include <stddef.h>

#include "bm25.h"
#include "lemmatizer.h"
#include "pg_store.h"
#include "stopwords.h"
#include "tokenizer.h"
#include "wordnet.h"

/* How to retrieve. retrieval_default_policy() is the shared CLI/app
 * behavior (LEXIS_SEARCH_* depth/trim, expansion on, default BM25
 * params + coordination bonus). */
typedef struct {
    size_t candidate_ceiling; /* how deep BM25 ranks */
    size_t max_passages;      /* bm25_result_set_trim cap; 0 disables trimming (eval) */
    int token_budget;         /* trim: token budget (ignored when trimming is off) */
    double score_floor_ratio; /* trim: relative score floor (ignored when trimming is off) */
    int use_expansion;        /* 0 = plain lemmatized terms, no LLM call (eval --no-llm-expansion) */
    BM25Params bm25;
    /* NULL = compute corpus stats inside the run (fine for a single
     * interactive query). Long-lived batch callers (eval: thousands of
     * queries) pass their own copy computed once -- bm25_corpus_stats()
     * is a full-corpus aggregate, a real per-call cost at 8.8M-passage
     * scale, and it stays valid for as long as the corpus doesn't
     * change. */
    const BM25CorpusStats *corpus_stats;
} RetrievalPolicy;

/* A function, not a macro with a compound literal, so the C++ side
 * (QueryWorker.cpp) can use it too. */
RetrievalPolicy retrieval_default_policy(void);

/* Everything one retrieval produced -- the ranked passages AND the
 * per-stage artifacts. Fields are named for what they ARE, not for who
 * consumes them; a new observer should find what it needs here rather
 * than growing the pipeline a new out-parameter. */
typedef struct {
    /* Expansion artifacts. All NULL/0 when policy->use_expansion == 0
     * or when the question had no expandable terms. */
    char *expansion_prompt;   /* what the sense-filter model was shown */
    char *expansion_response; /* its raw reply; NULL when the model call failed */
    int used_fallback;        /* 1 = expansion attempted but degraded to plain terms */

    /* The lexical query BM25 actually ran: originals first (deduplicated
     * question terms), surviving expansions after. terms->count == 0 is
     * the valid "question was entirely stopwords" outcome -- results
     * stays NULL and the caller decides what to tell the user. */
    TokenList *terms;
    size_t original_count; /* boundary: [0, original_count) are question terms */

    /* Ranked (and, per policy, trimmed) passages. May legitimately have
     * count == 0 -- no matches is an outcome, not an error. */
    BM25ResultSet *results;

    long formulation_ms;
    long search_ms;
} RetrievalRun;

/* Runs the pipeline: terms (union of `question` and `rewritten_question`
 * when the latter is non-NULL, plain question terms otherwise) ->
 * policy-gated sense-filtered WordNet expansion (degrades to plain terms
 * on any model/parse failure -- expansion is an enhancement, never a
 * precondition) -> weighted BM25 (originals 1.0, expansions
 * LEXIS_EXPANSION_WEIGHT) -> policy-gated trim.
 *
 * `rewritten_question`, when present, is also what the expansion prompt
 * shows the model (it is standalone, so candidates sense-check without
 * chat history). Returns NULL only on real failure (allocation or
 * database error); every "no results" shape is a returned run. Caller
 * frees with retrieval_run_free(). */
RetrievalRun *retrieval_run(PgStore *store, const char *question, const char *rewritten_question,
                            const StopwordSet *stopwords, const WordNetTable *wordnet,
                            const Lemmatizer *lemmatizer, const RetrievalPolicy *policy);

void retrieval_run_free(RetrievalRun *run);

#endif /* LEXIS_RETRIEVAL_H */
