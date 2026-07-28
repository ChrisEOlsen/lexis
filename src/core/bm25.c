/*
 * Implementation of BM25 scoring.
 * See include/bm25.h for the module's role (spec 5.2.5, Stage 3).
 */

#include "bm25.h"
#include <sqlite3.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

BM25CorpusStats bm25_corpus_stats(SqliteStore *store) {
    BM25CorpusStats stats = {-1, 0.0};

    static const char *sql = "SELECT COUNT(*), AVG(token_count) FROM passages;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "bm25_corpus_stats: prepare failed: %s\n",
                sqlite3_errmsg(store->db));
        return stats;
    }

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        fprintf(stderr, "bm25_corpus_stats: query failed: %s\n",
                sqlite3_errmsg(store->db));
        sqlite3_finalize(stmt);
        return stats;
    }

    stats.total_passages = (long)sqlite3_column_int64(stmt, 0);
    /* AVG() over an empty table returns SQL NULL, which sqlite3_column_double
     * reads back as 0.0 — exactly what we want when total_passages is 0. */
    stats.avg_passage_length = sqlite3_column_double(stmt, 1);

    sqlite3_finalize(stmt);
    return stats;
}

long bm25_document_frequency(SqliteStore *store, sqlite3_int64 term_id)
{
    static const char *sql = "SELECT COUNT(*) FROM postings WHERE term_id =?";
    sqlite3_stmt *stmt = NULL;
    if(sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "bm25_document_frequency: prepare failed: %s\n", 
                sqlite3_errmsg(store->db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, term_id);

    if(sqlite3_step(stmt) != SQLITE_ROW)
    {
        fprintf(stderr, "bm25_document_frequency: query failed: %s\n",
                sqlite3_errmsg(store->db));
        sqlite3_finalize(stmt);
        return -1;
    }
    long df = (long)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return df;
}

double bm25_idf(long total_passages, long document_frequency)
{
    return log((total_passages - document_frequency + 0.5) / (document_frequency + 0.5) + 1.0);
}

double bm25_term_score(double idf, long term_frequency, long passage_length, double avg_passage_length, BM25Params params)
{
    double length_ratio = passage_length / avg_passage_length;
    double norm = (1 - params.b) + params.b * length_ratio;
    double numerator = term_frequency * (params.k1 + 1);
    double denominator = term_frequency + params.k1 * norm;
    return idf * (numerator / denominator);
}

BM25ResultSet *bm25_result_set_create(void)
{
    int init_capacity = 8;
    BM25ResultSet *set = malloc(sizeof(BM25ResultSet));
    if (set == NULL)
    {
        fprintf(stderr, "malloc failed: bm25_result_set_create");
        return NULL;
    }
   set->items = malloc(sizeof(BM25ScoredPassage) * init_capacity);
   if (set->items == NULL)
   {
        fprintf(stderr, "malloc failed: bm25_result_set_create, result->items");
        free(set);
        return NULL;
   }
   set->count = 0;
   set->capacity = init_capacity;

   return set;
}

int bm25_result_set_add(BM25ResultSet *set, sqlite3_int64 passage_id, double score)
{
    for (size_t i = 0; i < set->count; i++)
    {
        if (set->items[i].passage_id == passage_id)
        {
            set->items[i].score += score;
            return 0;
        }
    }
    if (set->count == set->capacity)
    {
        size_t new_capacity = set->capacity * 2;
        BM25ScoredPassage *new_items = realloc(set->items, new_capacity * sizeof(BM25ScoredPassage));
        if (new_items == NULL)
        {
            fprintf(stderr,"failed realloc: bm25_result_set_add");
            return -1;
        }
        set->items = new_items;
        set->capacity = new_capacity;
    }

    set->items[set->count].passage_id = passage_id;
    set->items[set->count].score = score;
    set->count++;

    return 0;
}

void bm25_result_set_free(BM25ResultSet *set)
{
    if (set == NULL) return;
    free(set->items);
    free(set);
}

int bm25_accumulate_term_scores(SqliteStore *store, sqlite3_int64 term_id,
                                 BM25CorpusStats stats, BM25Params params,
                                 BM25ResultSet *results) {
    long n = bm25_document_frequency(store, term_id);
    if (n < 0) {
        return -1;
    }

    double idf = bm25_idf(stats.total_passages, n);

    static const char *sql =
        "SELECT postings.passage_id, postings.term_frequency, passages.token_count "
        "FROM postings "
        "JOIN passages ON postings.passage_id = passages.id "
        "WHERE postings.term_id = ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "bm25_accumulate_term_scores: prepare failed: %s\n",
                sqlite3_errmsg(store->db));
        return -1;
    }
    /* Bind index first, then value -- sqlite3_bind_int64(stmt, index, value). */
    sqlite3_bind_int64(stmt, 1, term_id);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        sqlite3_int64 passage_id = sqlite3_column_int64(stmt, 0);
        long term_frequency = (long)sqlite3_column_int64(stmt, 1);
        long passage_length = (long)sqlite3_column_int64(stmt, 2);

        double score = bm25_term_score(idf, term_frequency, passage_length,
                                        stats.avg_passage_length, params);

        if (bm25_result_set_add(results, passage_id, score) != 0) {
            sqlite3_finalize(stmt);
            return -1;
        }
    }

    sqlite3_finalize(stmt);
    return 0;
}

/* Descending-score comparator for qsort(). Compares via < / > rather than
 * subtraction -- (a - b) cast through qsort's int return type would
 * truncate or overflow for doubles, giving wrong ordering for close or
 * very large/small scores. */
static int bm25_compare_score_desc(const void *a, const void *b) {
    double score_a = ((const BM25ScoredPassage *)a)->score;
    double score_b = ((const BM25ScoredPassage *)b)->score;
    if (score_a < score_b) {
        return 1;
    }
    if (score_a > score_b) {
        return -1;
    }
    return 0;
}

BM25ResultSet *bm25_search(SqliteStore *store, const char **query_terms,
                            size_t num_terms, size_t top_k, BM25Params params) {
    BM25CorpusStats stats = bm25_corpus_stats(store);
    if (stats.total_passages < 0) {
        return NULL;
    }

    BM25ResultSet *results = bm25_result_set_create();
    if (results == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < num_terms; i++) {
        sqlite3_int64 term_id = sqlite_store_lookup_term(store, query_terms[i]);
        if (term_id == -1) {
            /* Term was never indexed -- contributes nothing, not an error. */
            continue;
        }

        if (bm25_accumulate_term_scores(store, term_id, stats, params, results) != 0) {
            bm25_result_set_free(results);
            return NULL;
        }
    }

    qsort(results->items, results->count, sizeof(BM25ScoredPassage), bm25_compare_score_desc);

    if (results->count > top_k) {
        results->count = top_k;
    }

    return results;
}