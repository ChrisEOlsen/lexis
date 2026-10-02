/* BM25 scoring: rank passages by term frequency, IDF, and length norm; return top-K. */

#ifndef LEXIS_BM25_H
#define LEXIS_BM25_H

#include <stddef.h>
#include <stdint.h>

#include "pg_store.h"

/* BM25 free params: k1 = TF saturation, b = length norm (0 none, 1 full). Defaults 1.2/0.75. */
typedef struct {
    double k1;
    double b;
    /* Coordination bonus: rewards matching more distinct query terms. 0 disables. */
    double coord_bonus;
} BM25Params;

#define BM25_DEFAULT_K1 1.2
#define BM25_DEFAULT_B 0.75
#define BM25_DEFAULT_COORD_BONUS 0.25

/* Corpus stats: total passages (N) and avg length (avgdl). total_passages == -1 signals failure. */
typedef struct {
    long total_passages;
    double avg_passage_length;
} BM25CorpusStats;

BM25CorpusStats bm25_corpus_stats(PgStore *store);

/* n(t): passages containing term_id. -1 on failure. */
long bm25_document_frequency(PgStore *store, int64_t term_id);

/* IDF(t) = ln((N-n(t)+0.5)/(n(t)+0.5)+1). Pure math, no DB access. */
double bm25_idf(long total_passages, long document_frequency);

/* One term's score contribution to one passage. Pure math; requires avg_passage_length > 0. */
double bm25_term_score(double idf, long term_frequency, long passage_length,
                        double avg_passage_length, BM25Params params);

/* One passage's accumulated score. matched_terms = distinct query terms contributing. */
typedef struct {
    int64_t passage_id;
    double score;
    int matched_terms;
} BM25ScoredPassage;

/* Opaque passage_id -> slot index (see bm25.c). Internal: O(1) lookup for result_set_add. */
typedef struct BM25ResultIndex BM25ResultIndex;

/* Growable unsorted scored-passage collection. Callers touch only items/count; index is internal. */
typedef struct {
    BM25ScoredPassage *items;
    size_t count;
    size_t capacity;
    BM25ResultIndex *index;
} BM25ResultSet;

/* Allocates an empty result set. Returns NULL on allocation failure. */
BM25ResultSet *bm25_result_set_create(void);

/* Add score to passage_id's total (append if new). 0 on success, -1 on allocation failure. */
int bm25_result_set_add(BM25ResultSet *set, int64_t passage_id, double score);

/* Frees the set and its backing array. Safe with set == NULL. */
void bm25_result_set_free(BM25ResultSet *set);

/* Score every passage containing term_id into results (stats supplies N/avgdl). 0 ok, -1 on DB error. */
int bm25_accumulate_term_scores(PgStore *store, int64_t term_id, BM25CorpusStats stats,
                                 BM25Params params, BM25ResultSet *results);

/* Weighted accumulate: contribution * weight (1.0 = unweighted behavior). */
int bm25_accumulate_term_scores_weighted(PgStore *store, int64_t term_id, BM25CorpusStats stats,
                                          BM25Params params, double weight,
                                          BM25ResultSet *results);

/* Full search: accumulate, sort desc, keep top_k (unknown terms skipped).
 * stats from bm25_corpus_stats(); caller frees result; NULL on failure. */
BM25ResultSet *bm25_search(PgStore *store, const char **query_terms, size_t num_terms, size_t top_k,
                            BM25CorpusStats stats, BM25Params params);

/* bm25_search with per-term weights (parallel to query_terms; NULL = all 1.0). Applies coord_bonus. */
BM25ResultSet *bm25_search_weighted(PgStore *store, const char **query_terms,
                                     const double *term_weights, size_t num_terms, size_t top_k,
                                     BM25CorpusStats stats, BM25Params params);

/* Trim ranked set in place to first of max_passages / token_budget / score floor (0.0 disables).
 * Only count changes; still freed with result_set_free. Unloadable passages count 0 tokens, kept. */
void bm25_result_set_trim(PgStore *store, BM25ResultSet *set, size_t max_passages, int token_budget,
                           double score_floor_ratio);

/* Shared retrieval depth/trim policy: rank deep (ceiling), send shallow (trim). */
#define LEXIS_SEARCH_CANDIDATE_CEILING 40
#define LEXIS_SEARCH_MAX_PASSAGES 12
#define LEXIS_SEARCH_TOKEN_BUDGET 1500
#define LEXIS_SEARCH_SCORE_FLOOR_RATIO 0.6

#endif /* LEXIS_BM25_H */
