/*
 * Implementation of the retrieval-quality evaluation harness.
 * See include/eval.h for the module's role and why it skips generation.
 */

/* See tokenizer.c for why this must come before any #include (strdup and
 * getline are POSIX extensions hidden by glibc under strict -std=c11
 * otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "eval.h"

#include "bm25.h"
#include "query_formulation.h"
#include "retrieval.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char *query_id;
    char *query_text;
} EvalQuery;

typedef struct {
    char *query_id;
    char *corpus_id;
    int score;
} EvalQrel;

static void free_queries(EvalQuery *queries, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(queries[i].query_id);
        free(queries[i].query_text);
    }
    free(queries);
}

static void free_qrels(EvalQrel *qrels, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(qrels[i].query_id);
        free(qrels[i].corpus_id);
    }
    free(qrels);
}

/* Reads "<query_id><TAB><query_text>" rows (no header) into a freshly
 * allocated, growable array. Returns the row count (>= 0) on success, or
 * -1 if the file can't be opened or on allocation failure. A row with no
 * tab is logged and skipped, not fatal. */
static long load_queries_tsv(const char *path, EvalQuery **out_queries) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "eval_run: could not open queries file %s\n", path);
        return -1;
    }

    size_t capacity = 1024;
    EvalQuery *queries = malloc(sizeof(EvalQuery) * capacity);
    if (queries == NULL) {
        fclose(fp);
        return -1;
    }
    size_t count = 0;

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    while ((line_len = getline(&line, &line_cap, fp)) >= 0) {
        if (line_len > 0 && line[line_len - 1] == '\n') {
            line[--line_len] = '\0';
        }
        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            fprintf(stderr, "eval_run: skipping malformed queries row (no tab)\n");
            continue;
        }
        *tab = '\0';

        if (count == capacity) {
            capacity *= 2;
            EvalQuery *bigger = realloc(queries, sizeof(EvalQuery) * capacity);
            if (bigger == NULL) {
                free(line);
                free_queries(queries, count);
                fclose(fp);
                return -1;
            }
            queries = bigger;
        }

        queries[count].query_id = strdup(line);
        queries[count].query_text = strdup(tab + 1);
        if (queries[count].query_id == NULL || queries[count].query_text == NULL) {
            free(queries[count].query_id);
            free(queries[count].query_text);
            free(line);
            free_queries(queries, count);
            fclose(fp);
            return -1;
        }
        count++;
    }
    free(line);
    fclose(fp);

    *out_queries = queries;
    return (long)count;
}

/* Reads "query-id<TAB>corpus-id<TAB>score" rows (header required and
 * discarded) into a freshly allocated, growable array. Returns the row
 * count (>= 0) on success, or -1 if the file can't be opened/is empty, or
 * on allocation failure. A row missing either tab is logged and skipped,
 * not fatal. */
static long load_qrels_tsv(const char *path, EvalQrel **out_qrels) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "eval_run: could not open qrels file %s\n", path);
        return -1;
    }

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len = getline(&line, &line_cap, fp); /* header row, discarded */
    if (line_len < 0) {
        fprintf(stderr, "eval_run: qrels file %s is empty\n", path);
        free(line);
        fclose(fp);
        return -1;
    }

    size_t capacity = 1024;
    EvalQrel *qrels = malloc(sizeof(EvalQrel) * capacity);
    if (qrels == NULL) {
        free(line);
        fclose(fp);
        return -1;
    }
    size_t count = 0;

    while ((line_len = getline(&line, &line_cap, fp)) >= 0) {
        if (line_len > 0 && line[line_len - 1] == '\n') {
            line[--line_len] = '\0';
        }
        char *tab1 = strchr(line, '\t');
        char *tab2 = (tab1 != NULL) ? strchr(tab1 + 1, '\t') : NULL;
        if (tab1 == NULL || tab2 == NULL) {
            fprintf(stderr, "eval_run: skipping malformed qrels row\n");
            continue;
        }
        *tab1 = '\0';
        *tab2 = '\0';

        if (count == capacity) {
            capacity *= 2;
            EvalQrel *bigger = realloc(qrels, sizeof(EvalQrel) * capacity);
            if (bigger == NULL) {
                free(line);
                free_qrels(qrels, count);
                fclose(fp);
                return -1;
            }
            qrels = bigger;
        }

        qrels[count].query_id = strdup(line);
        qrels[count].corpus_id = strdup(tab1 + 1);
        qrels[count].score = atoi(tab2 + 1);
        if (qrels[count].query_id == NULL || qrels[count].corpus_id == NULL) {
            free(qrels[count].query_id);
            free(qrels[count].corpus_id);
            free(line);
            free_qrels(qrels, count);
            fclose(fp);
            return -1;
        }
        count++;
    }
    free(line);
    fclose(fp);

    *out_qrels = qrels;
    return (long)count;
}

/* Collects the corpus_ids of every qrels row for `query_id` with a
 * positive score -- a borrowed-pointer array into `qrels`'s own owned
 * strings, freed by the caller as just the array, not its contents.
 * Returns NULL (with *out_count == 0) on allocation failure. */
static const char **collect_relevant_ids(const EvalQrel *qrels, size_t qrels_count,
                                          const char *query_id, size_t *out_count) {
    const char **relevant = malloc(sizeof(char *) * (qrels_count > 0 ? qrels_count : 1));
    if (relevant == NULL) {
        *out_count = 0;
        return NULL;
    }
    size_t n = 0;
    for (size_t i = 0; i < qrels_count; i++) {
        if (qrels[i].score > 0 && strcmp(qrels[i].query_id, query_id) == 0) {
            relevant[n++] = qrels[i].corpus_id;
        }
    }
    *out_count = n;
    return relevant;
}

static void eval_cleanup(const char **relevant_ids, EvalQrel *qrels, long qrels_count,
                          EvalQuery *queries, long query_count) {
    free(relevant_ids);
    free_qrels(qrels, (size_t)qrels_count);
    free_queries(queries, (size_t)query_count);
}

EvalMetrics eval_run(PgStore *store, const StopwordSet *stopwords, const WordNetTable *wordnet,
                      const Lemmatizer *lemmatizer, const char *queries_tsv_path,
                      const char *qrels_tsv_path, int use_llm_expansion) {
    EvalMetrics failure = {0.0, 0.0, 0.0, -1, 0};

    EvalQuery *queries = NULL;
    long query_count = load_queries_tsv(queries_tsv_path, &queries);
    if (query_count < 0) {
        return failure;
    }

    EvalQrel *qrels = NULL;
    long qrels_count = load_qrels_tsv(qrels_tsv_path, &qrels);
    if (qrels_count < 0) {
        free_queries(queries, (size_t)query_count);
        return failure;
    }

    printf("eval_run: loaded %ld queries, %ld qrels rows\n", query_count, qrels_count);
    fflush(stdout);

    /* Computed once and reused for every query in this run, not
     * recomputed per search -- bm25_corpus_stats() is a full-corpus
     * aggregate (COUNT(*)/AVG(token_count) over every passage), and the
     * corpus doesn't change during an eval run. At MS MARCO's real scale
     * this was measured at several seconds per call -- multiplied across
     * thousands of queries, recomputing it per search would have cost
     * hours by itself. See LIMITATIONS.md. */
    BM25CorpusStats stats = bm25_corpus_stats(store);
    if (stats.total_passages < 0) {
        fprintf(stderr, "eval_run: failed to compute corpus stats\n");
        free_qrels(qrels, (size_t)qrels_count);
        free_queries(queries, (size_t)query_count);
        return failure;
    }

    double sum_reciprocal_rank = 0.0;
    double sum_recall_10 = 0.0;
    double sum_recall_100 = 0.0;
    long evaluated = 0;
    long skipped = 0;

    struct timespec run_start;
    clock_gettime(CLOCK_MONOTONIC, &run_start);

    /* The shared pipeline, eval-shaped by POLICY, not by a separate
     * driver: rank 100 deep for Recall@100, no trim (metrics need the
     * full ranked list), expansion per the caller's flag, and corpus
     * stats computed once for the whole run rather than per query. */
    RetrievalPolicy policy = retrieval_default_policy();
    policy.candidate_ceiling = 100;
    policy.max_passages = 0;
    policy.use_expansion = use_llm_expansion;
    policy.corpus_stats = &stats;

    for (long qi = 0; qi < query_count; qi++) {
        const char *query_id = queries[qi].query_id;
        const char *query_text = queries[qi].query_text;

        size_t relevant_count = 0;
        const char **relevant_ids =
            collect_relevant_ids(qrels, (size_t)qrels_count, query_id, &relevant_count);
        if (relevant_ids == NULL) {
            eval_cleanup(NULL, qrels, qrels_count, queries, query_count);
            return failure;
        }
        if (relevant_count == 0) {
            /* No qrels judgments for this query -- nothing to score
             * against, not a failure. */
            free(relevant_ids);
            skipped++;
            continue;
        }

        RetrievalRun *run =
            retrieval_run(store, query_text, NULL, stopwords, wordnet, lemmatizer, &policy);
        if (run == NULL) {
            fprintf(stderr, "eval_run: retrieval failed on query %s\n", query_id);
            eval_cleanup(relevant_ids, qrels, qrels_count, queries, query_count);
            return failure;
        }
        TokenList *terms = run->terms; /* alias: owned by `run` */

        double reciprocal_rank = 0.0;
        double recall_10 = 0.0;
        double recall_100 = 0.0;

        if (terms->count > 0) {
            BM25ResultSet *results = run->results; /* alias: owned by `run` */

            if (results->count > 0) {
                int64_t *passage_ids = malloc(results->count * sizeof(int64_t));
                if (passage_ids == NULL) {
                    retrieval_run_free(run);
                    eval_cleanup(relevant_ids, qrels, qrels_count, queries, query_count);
                    return failure;
                }
                for (size_t i = 0; i < results->count; i++) {
                    passage_ids[i] = results->items[i].passage_id;
                }

                char **document_names = pg_store_get_document_names(store, passage_ids, results->count);
                free(passage_ids);

                if (document_names == NULL) {
                    fprintf(stderr, "eval_run: document name lookup failed on query %s\n", query_id);
                    retrieval_run_free(run);
                    eval_cleanup(relevant_ids, qrels, qrels_count, queries, query_count);
                    return failure;
                }

                /* A relevant pid can appear more than once in the ranked
                 * results if that document split into multiple chunks
                 * (rare -- MS MARCO passages average ~56 words, well
                 * under LEXIS_CHUNK_SIZE=200, so almost every document
                 * is exactly one chunk, but a few aren't). Track which
                 * relevant_ids[] entries have already been counted so a
                 * multi-chunk document can't inflate recall past 1.0. */
                int *relevant_counted_10 = calloc(relevant_count, sizeof(int));
                int *relevant_counted_100 = calloc(relevant_count, sizeof(int));
                if (relevant_counted_10 == NULL || relevant_counted_100 == NULL) {
                    free(relevant_counted_10);
                    free(relevant_counted_100);
                    for (size_t i = 0; i < results->count; i++) {
                        free(document_names[i]);
                    }
                    free(document_names);
                    retrieval_run_free(run);
                    eval_cleanup(relevant_ids, qrels, qrels_count, queries, query_count);
                    return failure;
                }

                size_t found_at_10 = 0;
                size_t found_at_100 = 0;
                size_t top_10_limit = results->count < 10 ? results->count : 10;

                for (size_t rank = 0; rank < results->count; rank++) {
                    if (document_names[rank] == NULL) {
                        continue;
                    }
                    for (size_t r = 0; r < relevant_count; r++) {
                        if (strcmp(document_names[rank], relevant_ids[r]) != 0) {
                            continue;
                        }
                        if (!relevant_counted_100[r]) {
                            relevant_counted_100[r] = 1;
                            found_at_100++;
                        }
                        if (rank < top_10_limit && !relevant_counted_10[r]) {
                            relevant_counted_10[r] = 1;
                            found_at_10++;
                            if (reciprocal_rank == 0.0) {
                                reciprocal_rank = 1.0 / (double)(rank + 1);
                            }
                        }
                        break;
                    }
                }

                free(relevant_counted_10);
                free(relevant_counted_100);

                recall_10 = (double)found_at_10 / (double)relevant_count;
                recall_100 = (double)found_at_100 / (double)relevant_count;

                for (size_t i = 0; i < results->count; i++) {
                    free(document_names[i]);
                }
                free(document_names);
            }
        }

        retrieval_run_free(run);
        free(relevant_ids);

        sum_reciprocal_rank += reciprocal_rank;
        sum_recall_10 += recall_10;
        sum_recall_100 += recall_100;
        evaluated++;

        if (evaluated % 50 == 0 || qi == query_count - 1) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed_sec =
                (double)(now.tv_sec - run_start.tv_sec) + (double)(now.tv_nsec - run_start.tv_nsec) / 1e9;
            double per_query = evaluated > 0 ? elapsed_sec / (double)evaluated : 0.0;
            double remaining_sec = per_query * (double)(query_count - qi - 1);
            printf("[%ld/%ld] MRR@10=%.4f Recall@10=%.4f Recall@100=%.4f elapsed=%.1fmin ETA=%.1fmin\n",
                   qi + 1, query_count, sum_reciprocal_rank / (double)evaluated,
                   sum_recall_10 / (double)evaluated, sum_recall_100 / (double)evaluated,
                   elapsed_sec / 60.0, remaining_sec / 60.0);
            fflush(stdout);
        }
    }

    free_qrels(qrels, (size_t)qrels_count);
    free_queries(queries, (size_t)query_count);

    EvalMetrics metrics;
    metrics.queries_evaluated = evaluated;
    metrics.queries_skipped = skipped;
    metrics.mrr_at_10 = evaluated > 0 ? sum_reciprocal_rank / (double)evaluated : 0.0;
    metrics.recall_at_10 = evaluated > 0 ? sum_recall_10 / (double)evaluated : 0.0;
    metrics.recall_at_100 = evaluated > 0 ? sum_recall_100 / (double)evaluated : 0.0;
    return metrics;
}
