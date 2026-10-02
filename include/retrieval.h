/* Shared retrieval orchestrator: question -> ranked passages. Caller differences are policy values. */

#ifndef LEXIS_RETRIEVAL_H
#define LEXIS_RETRIEVAL_H

#include <stddef.h>

#include "bm25.h"
#include "lemmatizer.h"
#include "pg_store.h"
#include "stopwords.h"
#include "tokenizer.h"
#include "wordnet.h"

/* How to retrieve. default_policy() = shared CLI/app behavior (config-tunable trim, expansion on). */
typedef struct {
    size_t candidate_ceiling; /* how deep BM25 ranks */
    size_t max_passages;      /* bm25_result_set_trim cap; 0 disables trimming (eval) */
    int token_budget;         /* trim: token budget (ignored when trimming is off) */
    double score_floor_ratio; /* trim: relative score floor (ignored when trimming is off) */
    int use_expansion;        /* 0 = plain lemmatized terms, no LLM call (eval --no-llm-expansion) */
    BM25Params bm25;
    /* NULL = compute stats per run. Batch callers pass one shared copy (full-corpus aggregate). */
    const BM25CorpusStats *corpus_stats;
} RetrievalPolicy;

/* A function, not a macro, so the C++ side (QueryWorker.cpp) can use it too.
 * Values: config file over compiled defaults, LEXIS_* env over config. */
RetrievalPolicy retrieval_default_policy(void);

/* User-facing reranker switch (app Settings). Second gate atop config; disabled = pure BM25.
 * Call between retrieval_run() calls, never during one. */
void retrieval_set_reranker_enabled(int enabled);

/* Everything one retrieval produced: ranked passages plus per-stage artifacts for observers. */
typedef struct {
    /* Expansion artifacts. All NULL/0 when expansion off or nothing expandable. */
    char *expansion_prompt;   /* what the sense-filter model was shown */
    char *expansion_response; /* its raw reply; NULL when the model call failed */
    int used_fallback;        /* 1 = expansion attempted but degraded to plain terms */

    /* Lexical query BM25 ran: originals first, expansions after. count == 0 = all-stopwords question. */
    TokenList *terms;
    size_t original_count; /* boundary: [0, original_count) are question terms */

    /* Ranked (per policy, trimmed) passages. count == 0 is an outcome, not an error. */
    BM25ResultSet *results;

    long formulation_ms;
    long search_ms;
} RetrievalRun;

/* Run the pipeline: terms (union with rewritten_question when non-NULL) -> expansion -> BM25 -> trim.
 * Expansion degrades to plain terms on failure. NULL only on real failure; caller frees with run_free. */
RetrievalRun *retrieval_run(PgStore *store, const char *question, const char *rewritten_question,
                            const StopwordSet *stopwords, const WordNetTable *wordnet,
                            const Lemmatizer *lemmatizer, const RetrievalPolicy *policy);

void retrieval_run_free(RetrievalRun *run);

#endif /* LEXIS_RETRIEVAL_H */
