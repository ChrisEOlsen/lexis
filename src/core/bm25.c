/*
 * Implementation of BM25 scoring.
 * See include/bm25.h for the module's role (spec 5.2.5, Stage 3).
 */

#include "bm25.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

BM25CorpusStats bm25_corpus_stats(PgStore *store) {
    BM25CorpusStats stats = {-1, 0.0};

    PGresult *res = PQexec(store->conn, "SELECT COUNT(*), AVG(token_count) FROM passages;");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "bm25_corpus_stats: query failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return stats;
    }

    stats.total_passages = atol(PQgetvalue(res, 0, 0));
    /* AVG() over an empty table returns SQL NULL -- PQgetvalue then
     * returns an empty string, and atof("") is 0.0, exactly what we want
     * when total_passages is 0. */
    stats.avg_passage_length = atof(PQgetvalue(res, 0, 1));

    PQclear(res);
    return stats;
}

long bm25_document_frequency(PgStore *store, int64_t term_id) {
    char term_id_str[32];
    snprintf(term_id_str, sizeof(term_id_str), "%lld", (long long)term_id);
    const char *params[1] = {term_id_str};

    PGresult *res = PQexecParams(store->conn, "SELECT COUNT(*) FROM postings WHERE term_id = $1;", 1,
                                  NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "bm25_document_frequency: query failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }

    long df = atol(PQgetvalue(res, 0, 0));
    PQclear(res);
    return df;
}

double bm25_idf(long total_passages, long document_frequency) {
    return log((total_passages - document_frequency + 0.5) / (document_frequency + 0.5) + 1.0);
}

double bm25_term_score(double idf, long term_frequency, long passage_length, double avg_passage_length,
                        BM25Params params) {
    double length_ratio = passage_length / avg_passage_length;
    double norm = (1 - params.b) + params.b * length_ratio;
    double numerator = term_frequency * (params.k1 + 1);
    double denominator = term_frequency + params.k1 * norm;
    return idf * (numerator / denominator);
}

BM25ResultSet *bm25_result_set_create(void) {
    int init_capacity = 8;
    BM25ResultSet *set = malloc(sizeof(BM25ResultSet));
    if (set == NULL) {
        fprintf(stderr, "malloc failed: bm25_result_set_create");
        return NULL;
    }
    set->items = malloc(sizeof(BM25ScoredPassage) * init_capacity);
    if (set->items == NULL) {
        fprintf(stderr, "malloc failed: bm25_result_set_create, result->items");
        free(set);
        return NULL;
    }
    set->count = 0;
    set->capacity = init_capacity;

    return set;
}

int bm25_result_set_add(BM25ResultSet *set, int64_t passage_id, double score) {
    for (size_t i = 0; i < set->count; i++) {
        if (set->items[i].passage_id == passage_id) {
            set->items[i].score += score;
            return 0;
        }
    }
    if (set->count == set->capacity) {
        size_t new_capacity = set->capacity * 2;
        BM25ScoredPassage *new_items = realloc(set->items, new_capacity * sizeof(BM25ScoredPassage));
        if (new_items == NULL) {
            fprintf(stderr, "failed realloc: bm25_result_set_add");
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

void bm25_result_set_free(BM25ResultSet *set) {
    if (set == NULL) return;
    free(set->items);
    free(set);
}

int bm25_accumulate_term_scores(PgStore *store, int64_t term_id, BM25CorpusStats stats,
                                 BM25Params params, BM25ResultSet *results) {
    long n = bm25_document_frequency(store, term_id);
    if (n < 0) {
        return -1;
    }

    double idf = bm25_idf(stats.total_passages, n);

    char term_id_str[32];
    snprintf(term_id_str, sizeof(term_id_str), "%lld", (long long)term_id);
    const char *params_arr[1] = {term_id_str};

    static const char *sql = "SELECT postings.passage_id, postings.term_frequency, passages.token_count "
                              "FROM postings "
                              "JOIN passages ON postings.passage_id = passages.id "
                              "WHERE postings.term_id = $1;";

    PGresult *res = PQexecParams(store->conn, sql, 1, NULL, params_arr, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "bm25_accumulate_term_scores: query failed: %s\n", PQerrorMessage(store->conn));
        PQclear(res);
        return -1;
    }

    /* libpq hands back every matching row at once (no step-by-step
     * cursor the way sqlite3_step() worked) -- iterate PQntuples(). */
    int row_count = PQntuples(res);
    for (int i = 0; i < row_count; i++) {
        int64_t passage_id = atoll(PQgetvalue(res, i, 0));
        long term_frequency = atol(PQgetvalue(res, i, 1));
        long passage_length = atol(PQgetvalue(res, i, 2));

        double score = bm25_term_score(idf, term_frequency, passage_length, stats.avg_passage_length,
                                        params);

        if (bm25_result_set_add(results, passage_id, score) != 0) {
            PQclear(res);
            return -1;
        }
    }

    PQclear(res);
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

BM25ResultSet *bm25_search(PgStore *store, const char **query_terms, size_t num_terms, size_t top_k,
                            BM25Params params) {
    BM25CorpusStats stats = bm25_corpus_stats(store);
    if (stats.total_passages < 0) {
        return NULL;
    }

    BM25ResultSet *results = bm25_result_set_create();
    if (results == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < num_terms; i++) {
        int64_t term_id = pg_store_lookup_term(store, query_terms[i]);
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
