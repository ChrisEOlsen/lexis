/*
 * Retrieval-quality evaluation harness (spec section 8). Runs the actual
 * LEXIS query path -- query formulation (WordNet expansion + local-model
 * term selection) then BM25 search -- against a labeled query set, and
 * scores the results against ground-truth relevance judgments (qrels) to
 * produce MRR@10 and Recall@K, the standard reporting points published
 * MS MARCO passage ranking baselines are compared against.
 *
 * Deliberately never calls generation_generate_answer(): MRR@10/Recall@K
 * are retrieval-quality metrics and don't depend on what the large model
 * says about the results, so this exercises one local-model call per
 * query (query formulation) instead of two. This also has to be a single
 * long-running process, not one CLI invocation per query -- see
 * LIMITATIONS.md on local_llm_client.c's ~9-19s per-process model-load
 * cost, which would otherwise dominate a multi-thousand-query run.
 */

#ifndef LEXIS_EVAL_H
#define LEXIS_EVAL_H

#include "lemmatizer.h"
#include "pg_store.h"
#include "stopwords.h"
#include "wordnet.h"

/* Aggregate metrics from one eval_run() call. queries_evaluated is how
 * many queries actually contributed to mrr_at_10/recall_at_10/
 * recall_at_100 (the three doubles are macro-averages over that count).
 * queries_skipped counts queries in `queries_tsv_path` whose id had no
 * matching rows in `qrels_tsv_path` -- nothing to score them against, not
 * a failure. queries_evaluated == -1 signals eval_run() itself failed
 * (bad file path, database error, allocation failure) before producing
 * any real numbers -- the three double fields are meaningless in that
 * case. */
typedef struct {
    double mrr_at_10;
    double recall_at_10;
    double recall_at_100;
    long queries_evaluated;
    long queries_skipped;
} EvalMetrics;

/* Runs the full evaluation: for every row in `queries_tsv_path`
 * ("<query_id><TAB><query_text>" rows, no header), runs the real LEXIS
 * retrieval path and scores the top 100 results against
 * `qrels_tsv_path` ("query-id<TAB>corpus-id<TAB>score" rows, header
 * required -- e.g. BeIR/msmarco-qrels' dev.tsv). A row whose score is 0
 * or negative doesn't count as relevant (matches qrels' own convention
 * for non-judged/non-relevant pairs); a query with no relevant rows at
 * all is skipped. Prints periodic progress (running metrics + elapsed/
 * estimated time remaining) to stdout, since a full 6,980-query run
 * with LLM query expansion takes on the order of hours (see
 * LIMITATIONS.md) and this is meant to run in the background with the
 * user checking in periodically.
 *
 * `use_llm_expansion` selects which query formulation path scores get
 * measured against: nonzero runs the real product path
 * (query_formulation_formulate_query() -- WordNet expansion + a local-
 * model call to select candidates, one model call per query); zero runs
 * query_formulation_terms_only() instead -- plain lemmatized query terms,
 * no WordNet expansion, no model call at all. Exists to answer directly,
 * with real numbers, how much the LLM expansion step is actually
 * contributing to retrieval quality versus its (measured: ~23s/query)
 * cost, rather than assuming either way. */
EvalMetrics eval_run(PgStore *store, const StopwordSet *stopwords, const WordNetTable *wordnet,
                      const Lemmatizer *lemmatizer, const char *queries_tsv_path,
                      const char *qrels_tsv_path, int use_llm_expansion);

#endif /* LEXIS_EVAL_H */
