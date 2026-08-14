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
    /* Coordination bonus: after accumulation, a passage's score is
     * multiplied by 1 + coord_bonus * (distinct_terms_matched - 1) /
     * (num_query_terms - 1), so matching MORE of the query's distinct
     * terms beats matching one corpus-frequent term repeatedly (the
     * measured failure: ~19 heated-steering-wheel passages each matching
     * "steering"+"wheel" drowning the one cruise-control passage that
     * matched "buttons"+"steering"+"wheel"). 0 disables the bonus
     * entirely -- which is what a zero-initialized or two-field
     * designated initializer gets, preserving pre-bonus behavior. */
    double coord_bonus;
} BM25Params;

#define BM25_DEFAULT_K1 1.2
#define BM25_DEFAULT_B 0.75
#define BM25_DEFAULT_COORD_BONUS 0.25

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
 * that matched at least one query term so far. matched_terms counts how
 * many distinct query terms have contributed (each
 * bm25_result_set_add() call is one term's contribution -- a term
 * touches a passage at most once per search, postings' primary key
 * guarantees it); the coordination bonus reads it after accumulation. */
typedef struct {
    int64_t passage_id;
    double score;
    int matched_terms;
} BM25ScoredPassage;

/* Opaque hash index (passage_id -> its slot in BM25ResultSet.items[]),
 * defined in bm25.c. Internal implementation detail -- exists purely so
 * bm25_result_set_add() can find an existing passage_id in O(1)
 * amortized time instead of a linear scan over every entry added so far.
 * That linear scan was invisible at a handful of matching passages, but
 * a single common query term can match hundreds of thousands to
 * millions of passages at real corpus scale (measured directly against
 * the full MS MARCO ingest -- see LIMITATIONS.md), where an O(n) check
 * per insert becomes an O(n^2) accumulation overall. */
typedef struct BM25ResultIndex BM25ResultIndex;

/* A growable, unsorted collection of BM25ScoredPassage entries -- same
 * doubling-capacity growth pattern as TokenList (tokenizer.h). Lets
 * per-term scores accumulate into one running total per passage, before
 * the eventual sort + top-K truncation. `index` is an internal detail
 * (see BM25ResultIndex) -- callers should only ever touch `items`/
 * `count`. */
typedef struct {
    BM25ScoredPassage *items;
    size_t count;
    size_t capacity;
    BM25ResultIndex *index;
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

/* bm25_accumulate_term_scores() with the term's score contribution
 * multiplied by `weight` (1.0 = identical behavior; the unweighted
 * function is now a wrapper over this). See bm25_search_weighted(). */
int bm25_accumulate_term_scores_weighted(PgStore *store, int64_t term_id, BM25CorpusStats stats,
                                          BM25Params params, double weight,
                                          BM25ResultSet *results);

/* Runs a full BM25 search: looks up each of `num_terms` query terms
 * (unrecognized terms are silently skipped -- they simply contribute no
 * score, same as any other term with zero matching passages), accumulates
 * scores across every matching passage, sorts descending by score, and
 * truncates to the top `top_k`. `stats` must come from a prior
 * bm25_corpus_stats() call -- deliberately NOT computed internally here
 * anymore, since it's a full-corpus aggregate (a real cost at real corpus
 * scale, see LIMITATIONS.md) that stays valid for as long as the corpus
 * itself doesn't change; callers serving many searches in a row (a query
 * loop, an eval harness) should compute it once and reuse it, not pay for
 * it on every single search. Returns a result set the caller must free
 * with bm25_result_set_free(), or NULL on a database/allocation failure. */
BM25ResultSet *bm25_search(PgStore *store, const char **query_terms, size_t num_terms, size_t top_k,
                            BM25CorpusStats stats, BM25Params params);

/* bm25_search() with a per-term score weight. `term_weights` is parallel
 * to `query_terms`; NULL means every term weighs 1.0 (making this
 * exactly bm25_search(), which is now a wrapper over it). Exists so
 * query expansion can be DISCOUNTED rather than equal: original question
 * terms at 1.0, WordNet expansions at LEXIS_EXPANSION_WEIGHT, so an
 * expansion can assist a passage but a passage matching only expansions
 * cannot outrank one matching the question itself -- the failure the
 * king-tut postmortem measured (see LIMITATIONS.md). Also applies
 * params.coord_bonus (see BM25Params). */
BM25ResultSet *bm25_search_weighted(PgStore *store, const char **query_terms,
                                     const double *term_weights, size_t num_terms, size_t top_k,
                                     BM25CorpusStats stats, BM25Params params);

/* Shrinks an already-ranked `set` in place to the prefix worth sending to
 * a model, keeping results in rank order and stopping at the FIRST of
 * three limits:
 *
 *   - `max_passages`        a hard ceiling on how many to keep
 *   - `token_budget`        cumulative passage token_count
 *   - `score_floor_ratio`   drop anything scoring below this fraction of
 *                           the top result's score (0.0 disables)
 *
 * Replaces passing a fixed top_k straight into generation. A constant K is
 * unrelated to how big the chunks are or how sharply relevance falls off,
 * and measurement showed both bounds are needed. The token budget alone is
 * insufficient because BM25 scores plateau rather than fall to zero -- one
 * measured query ran 5.87, 5.74, 4.95, 4.79, 3.39, 2.88, 2.63, 2.61,
 * 2.61, 2.59, where everything from rank 6 on is noise a budget would
 * happily swallow. The floor alone is insufficient because a query with a
 * long flat run of genuinely similar scores would blow the context window.
 *
 * The ceiling exists because more context is not monotonically better:
 * measured on a local 2B model, retrieval depth 5 and 10 both answered a
 * "list every X" question correctly, depth 20 confused a matching label
 * from an unrelated section, and depth 30 collapsed the answer entirely.
 *
 * Only `set->count` changes; nothing is freed here, and the set remains
 * safe to pass to bm25_result_set_free(). Requires `store` because passage
 * token counts live in the database, not in the result set. Passages that
 * fail to load are counted as zero tokens and kept -- a display-time read
 * failure shouldn't silently change retrieval depth. */
void bm25_result_set_trim(PgStore *store, BM25ResultSet *set, size_t max_passages, int token_budget,
                           double score_floor_ratio);

#endif /* LEXIS_BM25_H */
