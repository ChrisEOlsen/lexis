/*
 * BM25 scoring (spec 5.2.5, build order Stage 3). The performance-critical
 * retrieval loop: scores matching passages using term frequency, inverse
 * document frequency, and document length normalization, then ranks and
 * returns the top-K passages. Validate correctness against known datasets
 * before building anything on top of it.
 */

#ifndef LEXIS_BM25_H
#define LEXIS_BM25_H

#include <stddef.h>
#include <stdint.h>

#include "pg_store.h"

/* Standard BM25 free parameters. k1 controls term-frequency saturation —
 * how quickly repeated occurrences of a term stop adding to the score.
 * b controls how strongly passage length is penalized (0 = no length
 * normalization, 1 = full). 1.2 / 0.75 are the widely-used defaults from
 * the original Okapi BM25 literature. */
typedef struct {
    double k1;
    double b;
} BM25Params;

#define BM25_DEFAULT_K1 1.2
#define BM25_DEFAULT_B 0.75

/* Corpus-wide statistics BM25 needs before it can score anything: how many
 * passages exist in total (N) and their average length in tokens (avgdl).
 * Computed fresh via SQL aggregate each call rather than cached — the
 * index is MVP-scale and staleness risk isn't worth the complexity yet.
 * total_passages == -1 signals failure (empty store or query error). */
typedef struct {
    long total_passages;
    double avg_passage_length;
} BM25CorpusStats;

BM25CorpusStats bm25_corpus_stats(PgStore *store);

/* Returns n(t): the number of passages containing `term_id` at least once.
 * Returns -1 on failure. */
long bm25_document_frequency(PgStore *store, int64_t term_id);

/* IDF(t) = ln((N - n(t) + 0.5) / (n(t) + 0.5) + 1). Pure math, no database
 * access — callers supply N (bm25_corpus_stats) and n(t)
 * (bm25_document_frequency) themselves. The +0.5 smoothing keeps the
 * result well-defined (no log(0) or negative log) even when a term
 * appears in every passage. */
double bm25_idf(long total_passages, long document_frequency);

/* One query term's score contribution to one passage. Pure math again —
 * no database access, no query-term loop (that's a higher-level function
 * still to come). `idf` comes from bm25_idf(); `term_frequency` and
 * `passage_length` come from a single postings/passages row;
 * `avg_passage_length` comes from bm25_corpus_stats(). Assumes
 * avg_passage_length > 0 — only meaningful to call this when the corpus
 * is non-empty, which is guaranteed by the caller having a matching
 * posting row to begin with. */
double bm25_term_score(double idf, long term_frequency, long passage_length,
                        double avg_passage_length, BM25Params params);

/* One passage's accumulated BM25 score -- one entry per distinct passage
 * that matched at least one query term so far. */
typedef struct {
    int64_t passage_id;
    double score;
} BM25ScoredPassage;

/* A growable, unsorted collection of BM25ScoredPassage entries -- same
 * doubling-capacity growth pattern as TokenList (tokenizer.h). Lets
 * per-term scores accumulate into one running total per passage, before
 * the eventual sort + top-K truncation. */
typedef struct {
    BM25ScoredPassage *items;
    size_t count;
    size_t capacity;
} BM25ResultSet;

/* Allocates an empty result set. Returns NULL on allocation failure. */
BM25ResultSet *bm25_result_set_create(void);

/* Adds `score` to passage_id's running total, appending a new entry if
 * this is the first time passage_id has been seen in this set. Grows the
 * backing array (doubling) if needed. Returns 0 on success, -1 on
 * allocation failure. */
int bm25_result_set_add(BM25ResultSet *set, int64_t passage_id, double score);

/* Frees the result set's backing array and the struct itself. Safe to
 * call with set == NULL. */
void bm25_result_set_free(BM25ResultSet *set);

/* Scores every passage containing `term_id` and folds each score into
 * `results` -- bm25_result_set_add() merges automatically if a passage
 * was already touched by an earlier query term in the same search.
 * Computes n(t) and IDF(t) internally via bm25_document_frequency() and
 * bm25_idf(), using `stats` for N and avgdl. Returns 0 on success, -1 on
 * a database error. */
int bm25_accumulate_term_scores(PgStore *store, int64_t term_id, BM25CorpusStats stats,
                                 BM25Params params, BM25ResultSet *results);

/* Runs a full BM25 search: looks up each of `num_terms` query terms
 * (unrecognized terms are silently skipped -- they simply contribute no
 * score, same as any other term with zero matching passages), accumulates
 * scores across every matching passage, sorts descending by score, and
 * truncates to the top `top_k`. Returns a result set the caller must free
 * with bm25_result_set_free(), or NULL on a database/allocation failure. */
BM25ResultSet *bm25_search(PgStore *store, const char **query_terms, size_t num_terms, size_t top_k,
                            BM25Params params);

#endif /* LEXIS_BM25_H */
