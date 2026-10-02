/* Retrieval-quality eval: run the real query path against qrels; report MRR@10, Recall@K, nDCG@10. */

#ifndef LEXIS_EVAL_H
#define LEXIS_EVAL_H

#include "lemmatizer.h"
#include "pg_store.h"
#include "stopwords.h"
#include "wordnet.h"

/* Aggregate metrics: macro-averages over queries_evaluated; skipped = no qrels rows. -1 evaluated = failed. */
typedef struct {
    double mrr_at_10;
    double recall_at_10;
    double recall_at_100;
    /* Macro-averaged nDCG@10, linear gains (trec_eval ndcg_cut convention). */
    double ndcg_at_10;
    long queries_evaluated;
    long queries_skipped;
} EvalMetrics;

/* Score the top 100 per query against qrels (score <= 0 not relevant; no relevant rows = skipped).
 * use_llm_expansion: real product path vs plain lemmatized terms. Prints progress to stdout. */
EvalMetrics eval_run(PgStore *store, const StopwordSet *stopwords, const WordNetTable *wordnet,
                      const Lemmatizer *lemmatizer, const char *queries_tsv_path,
                      const char *qrels_tsv_path, int use_llm_expansion);

#endif /* LEXIS_EVAL_H */
