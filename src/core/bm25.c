/*
 * Implementation of BM25 scoring.
 * See include/bm25.h for the module's role (spec 5.2.5, Stage 3).
 */

#include "bm25.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Open-addressing (linear probing) hash table mapping passage_id -> its
 * index into the owning BM25ResultSet's items[] array. See bm25.h's
 * BM25ResultIndex forward declaration for why this exists. */
typedef struct {
    int64_t passage_id;
    size_t item_index;
    int occupied;
} BM25IndexSlot;

struct BM25ResultIndex {
    BM25IndexSlot *slots;
    size_t capacity;
    size_t count;
};

/* Multiplicative hash (Knuth's method, the golden-ratio constant) --
 * passage_ids are sequential (GENERATED ALWAYS AS IDENTITY), so a naive
 * modulo would cluster badly; this scrambles the bits well regardless of
 * table size. */
static uint64_t bm25_hash_passage_id(int64_t passage_id) {
    uint64_t x = (uint64_t)passage_id;
    x *= 0x9E3779B97F4A7C15ULL;
    return x;
}

/* Finds passage_id's slot via linear probing -- either an existing
 * occupied slot (already present) or the first empty slot found along
 * the probe sequence (not present, ready for insertion there). Assumes
 * the table is never allowed to fill completely (see the load-factor
 * check in bm25_result_index_insert()), so this always terminates. */
static size_t bm25_result_index_find_slot(const BM25IndexSlot *slots, size_t capacity,
                                           int64_t passage_id) {
    size_t slot = (size_t)(bm25_hash_passage_id(passage_id) % capacity);
    while (slots[slot].occupied && slots[slot].passage_id != passage_id) {
        slot = (slot + 1) % capacity;
    }
    return slot;
}

static BM25ResultIndex *bm25_result_index_create(void) {
    BM25ResultIndex *index = malloc(sizeof(BM25ResultIndex));
    if (index == NULL) {
        return NULL;
    }
    index->capacity = 16;
    index->slots = calloc(index->capacity, sizeof(BM25IndexSlot));
    if (index->slots == NULL) {
        free(index);
        return NULL;
    }
    index->count = 0;
    return index;
}

static void bm25_result_index_free(BM25ResultIndex *index) {
    if (index == NULL) {
        return;
    }
    free(index->slots);
    free(index);
}

/* Doubles the table and re-inserts every occupied slot at its new
 * position -- probe sequences depend on capacity, so old positions can't
 * just be copied over. Returns 0 on success, -1 on allocation failure
 * (the table is left unchanged). */
static int bm25_result_index_grow(BM25ResultIndex *index) {
    size_t new_capacity = index->capacity * 2;
    BM25IndexSlot *new_slots = calloc(new_capacity, sizeof(BM25IndexSlot));
    if (new_slots == NULL) {
        return -1;
    }
    for (size_t i = 0; i < index->capacity; i++) {
        if (!index->slots[i].occupied) {
            continue;
        }
        size_t slot = bm25_result_index_find_slot(new_slots, new_capacity, index->slots[i].passage_id);
        new_slots[slot] = index->slots[i];
    }
    free(index->slots);
    index->slots = new_slots;
    index->capacity = new_capacity;
    return 0;
}

/* Returns 1 and sets *out_item_index if passage_id is already indexed, 0
 * if not (nothing to look up yet). */
static int bm25_result_index_lookup(const BM25ResultIndex *index, int64_t passage_id,
                                     size_t *out_item_index) {
    size_t slot = bm25_result_index_find_slot(index->slots, index->capacity, passage_id);
    if (index->slots[slot].occupied) {
        *out_item_index = index->slots[slot].item_index;
        return 1;
    }
    return 0;
}

/* Records that passage_id lives at item_index. Grows the table first if
 * inserting would push the load factor past 0.7 (kept low to keep probe
 * sequences short). Returns 0 on success, -1 on allocation failure. */
static int bm25_result_index_insert(BM25ResultIndex *index, int64_t passage_id, size_t item_index) {
    if ((index->count + 1) * 10 >= index->capacity * 7) {
        if (bm25_result_index_grow(index) != 0) {
            return -1;
        }
    }
    size_t slot = bm25_result_index_find_slot(index->slots, index->capacity, passage_id);
    index->slots[slot].passage_id = passage_id;
    index->slots[slot].item_index = item_index;
    index->slots[slot].occupied = 1;
    index->count++;
    return 0;
}

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
    set->index = bm25_result_index_create();
    if (set->index == NULL) {
        fprintf(stderr, "malloc failed: bm25_result_set_create, result->index");
        free(set->items);
        free(set);
        return NULL;
    }
    set->count = 0;
    set->capacity = init_capacity;

    return set;
}

int bm25_result_set_add(BM25ResultSet *set, int64_t passage_id, double score) {
    size_t item_index;
    if (bm25_result_index_lookup(set->index, passage_id, &item_index)) {
        set->items[item_index].score += score;
        set->items[item_index].matched_terms++;
        return 0;
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
    set->items[set->count].matched_terms = 1;

    if (bm25_result_index_insert(set->index, passage_id, set->count) != 0) {
        fprintf(stderr, "failed to grow index: bm25_result_set_add");
        return -1;
    }

    set->count++;

    return 0;
}

void bm25_result_set_trim(PgStore *store, BM25ResultSet *set, size_t max_passages, int token_budget,
                           double score_floor_ratio) {
    if (set == NULL || set->count == 0) {
        return;
    }

    /* set->items is already sorted descending by bm25_search(), so the top
     * score is item 0 and a single forward pass can apply all three
     * limits. */
    double floor_score = (score_floor_ratio > 0.0) ? set->items[0].score * score_floor_ratio : 0.0;

    size_t kept = 0;
    int running_tokens = 0;
    for (size_t i = 0; i < set->count && kept < max_passages; i++) {
        if (score_floor_ratio > 0.0 && set->items[i].score < floor_score) {
            break;
        }

        PgStorePassage *passage = pg_store_get_passage(store, set->items[i].passage_id);
        int passage_tokens = 0;
        if (passage != NULL) {
            passage_tokens = passage->token_count;
            pg_store_passage_free(passage);
        }

        /* Always keep the top result, however long it is: returning zero
         * passages because the single best match happens to exceed the
         * budget would turn a good answer into no answer. */
        if (kept > 0 && running_tokens + passage_tokens > token_budget) {
            break;
        }

        running_tokens += passage_tokens;
        kept++;
    }

    set->count = kept;
}

void bm25_result_set_free(BM25ResultSet *set) {
    if (set == NULL) return;
    free(set->items);
    bm25_result_index_free(set->index);
    free(set);
}

int bm25_accumulate_term_scores(PgStore *store, int64_t term_id, BM25CorpusStats stats,
                                 BM25Params params, BM25ResultSet *results) {
    return bm25_accumulate_term_scores_weighted(store, term_id, stats, params, 1.0, results);
}

int bm25_accumulate_term_scores_weighted(PgStore *store, int64_t term_id, BM25CorpusStats stats,
                                          BM25Params params, double weight,
                                          BM25ResultSet *results) {
    long n = bm25_document_frequency(store, term_id);
    if (n < 0) {
        return -1;
    }

    double idf = bm25_idf(stats.total_passages, n);

    char term_id_str[32];
    snprintf(term_id_str, sizeof(term_id_str), "%lld", (long long)term_id);
    const char *params_arr[1] = {term_id_str};

    /* No JOIN against passages -- token_count is denormalized directly
     * onto postings (see pg_store.c's schema comment) specifically so
     * this stays a single index-only scan. Measured directly at real MS
     * MARCO scale: the joined version cost 11-14+ seconds for a term
     * with 100K+ matches (one random-access lookup into passages per
     * matching row), worse for genuinely common words -- see
     * LIMITATIONS.md. */
    static const char *sql =
        "SELECT passage_id, term_frequency, token_count FROM postings WHERE term_id = $1;";

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

        double score = weight * bm25_term_score(idf, term_frequency, passage_length,
                                                 stats.avg_passage_length, params);

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
    const BM25ScoredPassage *pa = (const BM25ScoredPassage *)a;
    const BM25ScoredPassage *pb = (const BM25ScoredPassage *)b;
    if (pa->score < pb->score) {
        return 1;
    }
    if (pa->score > pb->score) {
        return -1;
    }
    /* Tie-break on passage_id so equal scores rank deterministically --
     * qsort is not stable, and score ties are common on short queries
     * (measured: NFCorpus nDCG@10 wobbled ~0.02 between identical runs
     * purely from tie ordering). Ranking is now reproducible for a given
     * ingest; re-ingesting still reassigns ids across parallel workers,
     * so cross-ingest tie order (and tie-heavy metrics with it) can
     * still shift -- see TESTING.md's sweep notes. */
    if (pa->passage_id < pb->passage_id) {
        return -1;
    }
    if (pa->passage_id > pb->passage_id) {
        return 1;
    }
    return 0;
}

BM25ResultSet *bm25_search(PgStore *store, const char **query_terms, size_t num_terms,
                            size_t top_k, BM25CorpusStats stats, BM25Params params) {
    return bm25_search_weighted(store, query_terms, NULL, num_terms, top_k, stats, params);
}

BM25ResultSet *bm25_search_weighted(PgStore *store, const char **query_terms,
                                     const double *term_weights, size_t num_terms,
                                     size_t top_k, BM25CorpusStats stats, BM25Params params) {
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

        double weight = (term_weights != NULL) ? term_weights[i] : 1.0;
        if (bm25_accumulate_term_scores_weighted(store, term_id, stats, params, weight,
                                                  results) != 0) {
            bm25_result_set_free(results);
            return NULL;
        }
    }

    /* Coordination bonus (see BM25Params.coord_bonus): applied after all
     * terms have accumulated, before ranking. num_terms == 1 means every
     * passage matched the same single term -- nothing to coordinate. */
    if (params.coord_bonus > 0.0 && num_terms > 1) {
        for (size_t i = 0; i < results->count; i++) {
            double matched = (double)(results->items[i].matched_terms - 1);
            results->items[i].score *= 1.0 + params.coord_bonus * matched / (double)(num_terms - 1);
        }
    }

    qsort(results->items, results->count, sizeof(BM25ScoredPassage), bm25_compare_score_desc);

    if (results->count > top_k) {
        results->count = top_k;
    }

    return results;
}
