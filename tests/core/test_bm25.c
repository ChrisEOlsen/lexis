/*
 * Tests for src/core/bm25.c — corpus-wide statistics used by the BM25
 * formula (total passage count, average passage length).
 */

#include "bm25.h"
#include "sqlite_store.h"
#include "test_utils.h"

#include <math.h>
#include <stdio.h>

/* Transcendental functions like log() accumulate floating-point rounding
 * error, so tests compare within a tolerance rather than for exact
 * equality. Expected values computed independently via Python's math.log. */
#define IDF_EPSILON 1e-9

#define TEST_DB_PATH "build/test_bm25.db"

static SqliteStore *open_fresh_store(void) {
    remove(TEST_DB_PATH);
    return sqlite_store_open(TEST_DB_PATH);
}

static void test_corpus_stats_empty_store(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    BM25CorpusStats stats = bm25_corpus_stats(store);
    TEST_ASSERT(stats.total_passages == 0, "expected 0 passages, got %ld",
                stats.total_passages);
    TEST_ASSERT(stats.avg_passage_length == 0.0,
                "expected avg length 0.0 on empty store, got %f",
                stats.avg_passage_length);

    sqlite_store_close(store);
}

static void test_corpus_stats_computes_n_and_avgdl(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite_store_insert_passage(store, "doc1.txt", 0, "hello world", 2);
    sqlite_store_insert_passage(store, "doc1.txt", 1, "second chunk here", 3);
    sqlite_store_insert_passage(store, "doc2.txt", 0, "one", 1);

    BM25CorpusStats stats = bm25_corpus_stats(store);
    TEST_ASSERT(stats.total_passages == 3, "expected 3 passages, got %ld",
                stats.total_passages);
    /* avgdl = (2 + 3 + 1) / 3 = 2.0 */
    TEST_ASSERT(stats.avg_passage_length == 2.0,
                "expected avg length 2.0, got %f", stats.avg_passage_length);

    sqlite_store_close(store);
}

static void test_document_frequency_counts_matching_passages(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 p1 = sqlite_store_insert_passage(store, "doc1.txt", 0, "hypertension risk", 2);
    sqlite3_int64 p2 = sqlite_store_insert_passage(store, "doc1.txt", 1, "treatment plan", 2);
    sqlite3_int64 p3 = sqlite_store_insert_passage(store, "doc2.txt", 0, "hypertension causes", 2);

    sqlite3_int64 term_id = sqlite_store_get_or_create_term(store, "hypertension");
    sqlite_store_insert_posting(store, term_id, p1, 1);
    sqlite_store_insert_posting(store, term_id, p3, 1);

    /* p2 never gets a posting for this term — df should count only p1/p3. */
    (void)p2;

    long df = bm25_document_frequency(store, term_id);
    TEST_ASSERT(df == 2, "expected document frequency 2, got %ld", df);

    sqlite_store_close(store);
}

static void test_document_frequency_zero_for_unseen_term(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 term_id = sqlite_store_get_or_create_term(store, "orphan");

    long df = bm25_document_frequency(store, term_id);
    TEST_ASSERT(df == 0, "expected document frequency 0 for a term with no postings, got %ld", df);

    sqlite_store_close(store);
}

static void test_idf_rare_term_scores_high(void) {
    /* n=0 out of N=10: term never seen, but still well-defined thanks to
     * +0.5 smoothing rather than blowing up on a divide-by-zero. */
    double idf = bm25_idf(10, 0);
    double expected = 3.091042453358316;
    TEST_ASSERT(fabs(idf - expected) < IDF_EPSILON,
                "expected idf %.9f, got %.9f", expected, idf);
}

static void test_idf_universal_term_scores_near_zero(void) {
    /* n == N: term appears in every passage. Smoothing keeps this small
     * and positive instead of log(0) or negative. */
    double idf = bm25_idf(10, 10);
    double expected = 0.04652001563489291;
    TEST_ASSERT(fabs(idf - expected) < IDF_EPSILON,
                "expected idf %.9f, got %.9f", expected, idf);
    TEST_ASSERT(idf > 0.0, "expected a universal term's idf to stay positive, got %f", idf);
}

static void test_idf_typical_case(void) {
    double idf = bm25_idf(100, 5);
    double expected = 2.9103724246028344;
    TEST_ASSERT(fabs(idf - expected) < IDF_EPSILON,
                "expected idf %.9f, got %.9f", expected, idf);
}

static void test_idf_rarer_term_scores_higher(void) {
    /* Same N, smaller n(t) should always score higher — rarer terms are
     * more informative. */
    double rare_idf = bm25_idf(100, 2);
    double common_idf = bm25_idf(100, 50);
    TEST_ASSERT(rare_idf > common_idf,
                "expected rarer term to have higher idf: rare=%f common=%f",
                rare_idf, common_idf);
}

static void test_term_score_at_average_length(void) {
    BM25Params params = {1.2, 0.75};
    /* tf=3, passage exactly at average length (5/5.0 -> length_ratio 1) */
    double score = bm25_term_score(2.0, 3, 5, 5.0, params);
    double expected = 3.142857142857143;
    TEST_ASSERT(fabs(score - expected) < IDF_EPSILON,
                "expected score %.9f, got %.9f", expected, score);
}

static void test_term_score_penalizes_longer_passages(void) {
    BM25Params params = {1.2, 0.75};
    double at_avg_length = bm25_term_score(2.0, 3, 5, 5.0, params);
    double longer_than_avg = bm25_term_score(2.0, 3, 10, 5.0, params);
    TEST_ASSERT(longer_than_avg < at_avg_length,
                "expected a longer-than-average passage to score lower for "
                "the same term frequency: at_avg=%f longer=%f",
                at_avg_length, longer_than_avg);

    double expected_longer = 2.5882352941176476;
    TEST_ASSERT(fabs(longer_than_avg - expected_longer) < IDF_EPSILON,
                "expected score %.9f, got %.9f", expected_longer, longer_than_avg);
}

static void test_term_score_tf_saturates(void) {
    BM25Params params = {1.2, 0.75};
    /* Doubling term frequency should NOT double the score -- TF's
     * contribution saturates (diminishing returns), it doesn't scale
     * linearly. */
    double tf1 = bm25_term_score(1.0, 1, 5, 5.0, params);
    double tf2 = bm25_term_score(1.0, 2, 5, 5.0, params);
    double tf4 = bm25_term_score(1.0, 4, 5, 5.0, params);

    TEST_ASSERT(fabs(tf1 - 1.0) < IDF_EPSILON, "expected tf1 score 1.0, got %f", tf1);
    TEST_ASSERT(fabs(tf2 - 1.375) < IDF_EPSILON, "expected tf2 score 1.375, got %f", tf2);
    TEST_ASSERT(fabs(tf4 - 1.6923076923076923) < IDF_EPSILON,
                "expected tf4 score 1.692307..., got %f", tf4);

    TEST_ASSERT(tf2 < 2 * tf1, "expected saturation: tf2 (%f) should be < 2x tf1 (%f)", tf2, tf1);
    TEST_ASSERT(tf4 < 2 * tf2, "expected saturation: tf4 (%f) should be < 2x tf2 (%f)", tf4, tf2);
}

static void test_result_set_create_starts_empty(void) {
    BM25ResultSet *set = bm25_result_set_create();
    TEST_ASSERT(set != NULL, "expected bm25_result_set_create to succeed");
    TEST_ASSERT(set->count == 0, "expected a fresh set to have count 0, got %zu", set->count);
    bm25_result_set_free(set);
}

static void test_result_set_add_first_entry(void) {
    BM25ResultSet *set = bm25_result_set_create();
    TEST_ASSERT(set != NULL, "expected bm25_result_set_create to succeed");

    int result = bm25_result_set_add(set, 42, 5.0);
    TEST_ASSERT(result == 0, "expected add to succeed");
    TEST_ASSERT(set->count == 1, "expected count 1 after first add, got %zu", set->count);
    TEST_ASSERT(set->items[0].passage_id == 42, "expected passage_id 42, got %lld",
                (long long)set->items[0].passage_id);
    TEST_ASSERT(fabs(set->items[0].score - 5.0) < IDF_EPSILON,
                "expected score 5.0, got %f", set->items[0].score);

    bm25_result_set_free(set);
}

static void test_result_set_add_accumulates_same_passage(void) {
    BM25ResultSet *set = bm25_result_set_create();
    TEST_ASSERT(set != NULL, "expected bm25_result_set_create to succeed");

    bm25_result_set_add(set, 7, 2.0);
    bm25_result_set_add(set, 7, 3.0);

    TEST_ASSERT(set->count == 1,
                "expected repeated passage_id to accumulate into one entry, got count %zu",
                set->count);
    TEST_ASSERT(fabs(set->items[0].score - 5.0) < IDF_EPSILON,
                "expected accumulated score 5.0, got %f", set->items[0].score);

    bm25_result_set_free(set);
}

static void test_result_set_add_distinct_passages_stay_separate(void) {
    BM25ResultSet *set = bm25_result_set_create();
    TEST_ASSERT(set != NULL, "expected bm25_result_set_create to succeed");

    bm25_result_set_add(set, 1, 1.0);
    bm25_result_set_add(set, 2, 1.0);
    bm25_result_set_add(set, 3, 1.0);
    bm25_result_set_add(set, 99, 5.0);

    TEST_ASSERT(set->count == 4, "expected 4 distinct entries, got %zu", set->count);
    TEST_ASSERT(set->items[3].passage_id == 99, "expected the 4th entry to be passage 99, got %lld",
                (long long)set->items[3].passage_id);
    TEST_ASSERT(fabs(set->items[3].score - 5.0) < IDF_EPSILON,
                "expected passage 99's score to be exactly 5.0 (not duplicated), got %f",
                set->items[3].score);

    bm25_result_set_free(set);
}

static void test_result_set_add_grows_past_initial_capacity(void) {
    BM25ResultSet *set = bm25_result_set_create();
    TEST_ASSERT(set != NULL, "expected bm25_result_set_create to succeed");

    /* Initial capacity is 8 -- add 20 distinct passages to force at least
     * two reallocations, and verify every entry survives the growth
     * intact (an old bug class: realloc into the wrong pointer would
     * silently lose everything already stored). */
    for (sqlite3_int64 i = 0; i < 20; i++) {
        int result = bm25_result_set_add(set, i, (double)i);
        TEST_ASSERT(result == 0, "expected add %lld to succeed", (long long)i);
    }

    TEST_ASSERT(set->count == 20, "expected 20 entries after growth, got %zu", set->count);
    for (sqlite3_int64 i = 0; i < 20; i++) {
        TEST_ASSERT(set->items[i].passage_id == i,
                    "expected entry %lld to have passage_id %lld, got %lld",
                    (long long)i, (long long)i, (long long)set->items[i].passage_id);
        TEST_ASSERT(fabs(set->items[i].score - (double)i) < IDF_EPSILON,
                    "expected entry %lld to have score %f, got %f",
                    (long long)i, (double)i, set->items[i].score);
    }

    bm25_result_set_free(set);
}

static void test_result_set_free_null_is_safe(void) {
    bm25_result_set_free(NULL);
}

/* Seeds a small realistic corpus: P1 matches both query terms, P2 and P4
 * match only "hypertension", P3 matches neither -- for search-level tests
 * further down (bm25_accumulate_term_scores, bm25_search). */
static void seed_search_corpus(SqliteStore *store, sqlite3_int64 *hypertension_id,
                                sqlite3_int64 *treatment_id, sqlite3_int64 *p1,
                                sqlite3_int64 *p2, sqlite3_int64 *p3, sqlite3_int64 *p4) {
    *p1 = sqlite_store_insert_passage(store, "doc1.txt", 0, "hypertension treatment options", 3);
    *p2 = sqlite_store_insert_passage(store, "doc1.txt", 1, "hypertension causes and risk factors", 6);
    *p3 = sqlite_store_insert_passage(store, "doc2.txt", 0, "cardiac arrest emergency response", 4);
    *p4 = sqlite_store_insert_passage(store, "doc2.txt", 1, "hypertension is a common condition", 6);

    *hypertension_id = sqlite_store_get_or_create_term(store, "hypertension");
    *treatment_id = sqlite_store_get_or_create_term(store, "treatment");

    sqlite_store_insert_posting(store, *hypertension_id, *p1, 1);
    sqlite_store_insert_posting(store, *hypertension_id, *p2, 1);
    sqlite_store_insert_posting(store, *hypertension_id, *p4, 1);
    sqlite_store_insert_posting(store, *treatment_id, *p1, 1);
}

static void test_accumulate_term_scores_matches_only_relevant_passages(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 hypertension_id, treatment_id, p1, p2, p3, p4;
    seed_search_corpus(store, &hypertension_id, &treatment_id, &p1, &p2, &p3, &p4);

    BM25CorpusStats stats = bm25_corpus_stats(store);
    BM25Params params = {BM25_DEFAULT_K1, BM25_DEFAULT_B};
    BM25ResultSet *results = bm25_result_set_create();
    TEST_ASSERT(results != NULL, "expected bm25_result_set_create to succeed");

    int rc = bm25_accumulate_term_scores(store, hypertension_id, stats, params, results);
    TEST_ASSERT(rc == 0, "expected bm25_accumulate_term_scores to succeed");

    /* "hypertension" appears in p1, p2, p4 -- never p3. */
    TEST_ASSERT(results->count == 3, "expected 3 matching passages, got %zu", results->count);
    for (size_t i = 0; i < results->count; i++) {
        TEST_ASSERT(results->items[i].passage_id != p3,
                    "passage p3 should never appear -- it doesn't contain \"hypertension\"");
        TEST_ASSERT(results->items[i].score > 0.0,
                    "expected a positive score, got %f", results->items[i].score);
    }

    bm25_result_set_free(results);
    sqlite_store_close(store);
}

static void test_search_ranks_multi_term_match_highest(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 hypertension_id, treatment_id, p1, p2, p3, p4;
    seed_search_corpus(store, &hypertension_id, &treatment_id, &p1, &p2, &p3, &p4);

    const char *query_terms[] = {"hypertension", "treatment"};
    BM25Params params = {BM25_DEFAULT_K1, BM25_DEFAULT_B};
    BM25ResultSet *results = bm25_search(store, query_terms, 2, 10, params);
    TEST_ASSERT(results != NULL, "expected bm25_search to succeed");

    /* p1 matches both query terms; p2/p4 match only one; p3 matches
     * neither and should never appear. */
    TEST_ASSERT(results->count == 3, "expected 3 matching passages, got %zu", results->count);
    TEST_ASSERT(results->items[0].passage_id == p1,
                "expected p1 (matches both terms) to rank first, got passage_id %lld",
                (long long)results->items[0].passage_id);

    for (size_t i = 0; i < results->count; i++) {
        TEST_ASSERT(results->items[i].passage_id != p3,
                    "passage p3 should never appear in results");
    }

    /* Results must be sorted strictly descending by score. */
    for (size_t i = 1; i < results->count; i++) {
        TEST_ASSERT(results->items[i - 1].score >= results->items[i].score,
                    "expected descending order at index %zu: %f then %f",
                    i, results->items[i - 1].score, results->items[i].score);
    }

    bm25_result_set_free(results);
    sqlite_store_close(store);
}

static void test_search_respects_top_k(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 hypertension_id, treatment_id, p1, p2, p3, p4;
    seed_search_corpus(store, &hypertension_id, &treatment_id, &p1, &p2, &p3, &p4);

    const char *query_terms[] = {"hypertension", "treatment"};
    BM25Params params = {BM25_DEFAULT_K1, BM25_DEFAULT_B};
    BM25ResultSet *results = bm25_search(store, query_terms, 2, 1, params);
    TEST_ASSERT(results != NULL, "expected bm25_search to succeed");

    TEST_ASSERT(results->count == 1, "expected top_k=1 to truncate to 1 result, got %zu", results->count);
    TEST_ASSERT(results->items[0].passage_id == p1,
                "expected the single result to be the best match (p1), got %lld",
                (long long)results->items[0].passage_id);

    bm25_result_set_free(results);
    sqlite_store_close(store);
}

static void test_search_skips_unindexed_query_terms(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 hypertension_id, treatment_id, p1, p2, p3, p4;
    seed_search_corpus(store, &hypertension_id, &treatment_id, &p1, &p2, &p3, &p4);

    const char *query_terms[] = {"nonexistentword"};
    BM25Params params = {BM25_DEFAULT_K1, BM25_DEFAULT_B};
    BM25ResultSet *results = bm25_search(store, query_terms, 1, 10, params);
    TEST_ASSERT(results != NULL,
                "expected an unindexed query term to produce an empty result set, not a failure");
    TEST_ASSERT(results->count == 0, "expected 0 matches for a term never in the corpus, got %zu",
                results->count);

    bm25_result_set_free(results);
    sqlite_store_close(store);
}

static void test_search_empty_corpus_returns_empty_results(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    const char *query_terms[] = {"anything"};
    BM25Params params = {BM25_DEFAULT_K1, BM25_DEFAULT_B};
    BM25ResultSet *results = bm25_search(store, query_terms, 1, 10, params);
    TEST_ASSERT(results != NULL, "expected an empty corpus to return an empty result set, not NULL");
    TEST_ASSERT(results->count == 0, "expected 0 results from an empty corpus, got %zu", results->count);

    bm25_result_set_free(results);
    sqlite_store_close(store);
}

int main(void) {
    test_corpus_stats_empty_store();
    test_corpus_stats_computes_n_and_avgdl();
    test_document_frequency_counts_matching_passages();
    test_document_frequency_zero_for_unseen_term();
    test_idf_rare_term_scores_high();
    test_idf_universal_term_scores_near_zero();
    test_idf_typical_case();
    test_idf_rarer_term_scores_higher();
    test_term_score_at_average_length();
    test_term_score_penalizes_longer_passages();
    test_term_score_tf_saturates();
    test_result_set_create_starts_empty();
    test_result_set_add_first_entry();
    test_result_set_add_accumulates_same_passage();
    test_result_set_add_distinct_passages_stay_separate();
    test_result_set_add_grows_past_initial_capacity();
    test_result_set_free_null_is_safe();
    test_accumulate_term_scores_matches_only_relevant_passages();
    test_search_ranks_multi_term_match_highest();
    test_search_respects_top_k();
    test_search_skips_unindexed_query_terms();
    test_search_empty_corpus_returns_empty_results();
    remove(TEST_DB_PATH);
    return test_summary();
}
