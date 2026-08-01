/*
 * Implementation of bulk TSV ingestion.
 * See include/bulk_ingest.h for the module's role and the three-phase
 * deferred-term-resolution design this implements (Phase 1: COPY raw
 * rows in; Phase 2: parallel, contention-free tokenize/lemmatize/stage;
 * Phase 3: single-pass, single-writer term resolution). See SPEED.md for
 * the full story of why this replaced the original per-document,
 * term-cache-based pipeline: every deadlock ever measured under real
 * write concurrency was Postgres's ON CONFLICT speculative insertion
 * racing on terms.term's unique index -- Phase 2 never touches that
 * table at all, so the contention is gone by construction rather than
 * tuned around.
 */

/* See tokenizer.c for why this must come before any #include (strdup and
 * getline are POSIX extensions hidden by glibc under strict -std=c11
 * otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "bulk_ingest.h"

#include "ingest.h"
#include "pg_store.h"
#include "tokenizer.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Rows claimed per round trip -- large enough to amortize the network
 * round trip across many documents, small enough that one worker
 * finishing early doesn't sit idle for long while another grinds through
 * an oversized final batch. Not tuned exhaustively; see SPEED.md for the
 * measured throughput this whole redesign achieves. */
#define BULK_PHASE2_BATCH_SIZE 500

/* A batch's documents are wrapped in one transaction -- safe to do here
 * (unlike the earlier, reverted attempt at batching the OLD per-document
 * pipeline, see SPEED.md) specifically because Phase 2 never touches
 * terms, the sole source of every deadlock this project has measured
 * under concurrency. passages' IDENTITY inserts and postings_staged's
 * unconstrained inserts don't lock across documents at all. */
#define BULK_PHASE2_BATCH_RETRIES 3

/* Shared, read-only across all workers except next_row/range_mutex (the
 * shared cursor into documents_raw's row_num range) and the two output
 * fields, which only this worker's own thread ever writes. */
typedef struct {
    const char *conninfo;
    int64_t *next_row;
    int64_t total_rows;
    pthread_mutex_t *range_mutex;
    const StopwordSet *stopwords;
    const WordNetTable *wordnet;
    const Lemmatizer *lemmatizer;
    size_t chunk_size;
    size_t overlap;
    long passages_ingested;
    int failed;
} Phase2Worker;

/* Claims the next batch of up to BULK_PHASE2_BATCH_SIZE row_nums under
 * w->range_mutex. Sets *start and *end to the claimed half-open range,
 * [*start, *end). Returns 0 if a (possibly empty, at the tail) range was
 * claimed, or 1 if the whole table has already been claimed by other
 * workers (nothing left to do). */
static int phase2_claim_batch(Phase2Worker *w, int64_t *start, int64_t *end) {
    pthread_mutex_lock(w->range_mutex);
    if (*w->next_row > w->total_rows) {
        pthread_mutex_unlock(w->range_mutex);
        return 1;
    }
    *start = *w->next_row;
    *end = *start + BULK_PHASE2_BATCH_SIZE;
    *w->next_row = *end;
    pthread_mutex_unlock(w->range_mutex);
    return 0;
}

/* Stages one already-tokenized/stopword-filtered/lemmatized chunk's
 * distinct terms (see ingest_count_distinct_terms()) into postings_staged
 * against `passage_id` -- writes raw term text rather than resolving a
 * terms.id, since Phase 2 never touches the terms table at all (that's
 * Phase 3's job, see pg_store_finalize_terms_and_postings()). Returns 0
 * on success, -1 on a database or allocation error. */
static int phase2_stage_chunk_terms(PgStore *store, const TokenList *terms, int64_t passage_id) {
    if (terms->count == 0) {
        return 0;
    }

    const char **distinct_terms;
    int *frequencies;
    size_t distinct_count;
    if (ingest_count_distinct_terms(terms, &distinct_terms, &frequencies, &distinct_count) != 0) {
        return -1;
    }

    int result = pg_store_insert_staged_postings(store, passage_id, distinct_terms, frequencies,
                                                  (int)terms->count, distinct_count);
    free(distinct_terms);
    free(frequencies);
    return result;
}

/* The real per-document work for one documents_raw row: chunk, tokenize,
 * lemmatize, insert the passage, stage its postings -- built on ingest.c's
 * chunking/lemmatizing primitives, but staging term text directly instead
 * of resolving term ids against Postgres. Returns the number of passages
 * ingested (>= 0) on success, or -1 on failure. */
static long phase2_process_document(PgStore *store, const StopwordSet *stopwords,
                                     const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                                     const char *text, const char *pid, size_t chunk_size, size_t overlap) {
    TokenList *words = ingest_split_words(text);
    if (words == NULL) {
        return -1;
    }

    TokenList *chunks = ingest_chunk_words(words, chunk_size, overlap);
    token_list_free(words);
    if (chunks == NULL) {
        return -1;
    }

    long passages_ingested = 0;
    for (size_t i = 0; i < chunks->count; i++) {
        const char *chunk_text = chunks->terms[i];

        TokenList *terms = tokenize(chunk_text);
        if (terms == NULL) {
            token_list_free(chunks);
            return -1;
        }
        stopwords_filter(terms, stopwords);

        TokenList *lemmas = ingest_lemmatize_terms(wordnet, lemmatizer, terms);
        token_list_free(terms);
        if (lemmas == NULL) {
            token_list_free(chunks);
            return -1;
        }

        int64_t passage_id = pg_store_insert_passage(store, pid, (int)i, chunk_text, (int)lemmas->count);
        if (passage_id == -1) {
            token_list_free(lemmas);
            token_list_free(chunks);
            return -1;
        }

        if (phase2_stage_chunk_terms(store, lemmas, passage_id) != 0) {
            token_list_free(lemmas);
            token_list_free(chunks);
            return -1;
        }

        token_list_free(lemmas);
        passages_ingested++;
    }
    token_list_free(chunks);
    return passages_ingested;
}

/* Processes one claimed batch of documents_raw rows inside a single
 * transaction (see BULK_PHASE2_BATCH_RETRIES's comment for why this is
 * safe here, unlike the earlier reverted batching attempt). Any failure
 * partway through rolls back the WHOLE batch -- retried up to
 * BULK_PHASE2_BATCH_RETRIES times as a fresh transaction before giving
 * up and skipping it entirely. Returns the number of passages ingested
 * across the batch (>= 0), or -1 if every retry failed. */
static long phase2_process_batch(PgStore *store, const StopwordSet *stopwords, const WordNetTable *wordnet,
                                  const Lemmatizer *lemmatizer, size_t chunk_size, size_t overlap,
                                  const PgStoreRawDocument *docs, size_t doc_count) {
    for (int attempt = 0; attempt < BULK_PHASE2_BATCH_RETRIES; attempt++) {
        if (pg_store_begin_transaction(store) != 0) {
            continue;
        }

        long passages_ingested = 0;
        int failed = 0;
        for (size_t i = 0; i < doc_count; i++) {
            long passages = phase2_process_document(store, stopwords, wordnet, lemmatizer, docs[i].text,
                                                      docs[i].pid, chunk_size, overlap);
            if (passages < 0) {
                failed = 1;
                break;
            }
            passages_ingested += passages;
        }

        if (failed) {
            pg_store_rollback_transaction(store);
            continue;
        }

        if (pg_store_commit_transaction(store) != 0) {
            continue;
        }

        return passages_ingested;
    }

    fprintf(stderr, "phase2_process_batch: giving up on a batch of %zu documents after %d attempts\n",
            doc_count, BULK_PHASE2_BATCH_RETRIES);
    return -1;
}

static void *phase2_worker_run(void *arg) {
    Phase2Worker *w = (Phase2Worker *)arg;

    /* Each worker owns its own connection -- a single PGconn isn't safe
     * for concurrent use from multiple threads, and nothing stops N
     * separate connections from all writing to the same tables at once. */
    PgStore *store = pg_store_open(w->conninfo);
    if (store == NULL) {
        fprintf(stderr, "phase2_worker_run: failed to open a connection\n");
        w->failed = 1;
        return NULL;
    }
    /* Rebuildable index build, not live/irreplaceable data -- see
     * pg_store_disable_synchronous_commit()'s doc comment. */
    pg_store_disable_synchronous_commit(store);

    while (1) {
        int64_t start, end;
        if (phase2_claim_batch(w, &start, &end) != 0) {
            break;
        }

        /* A failure here or below (a transient query error, or a batch
         * that exhausts phase2_process_batch()'s retries) costs at most
         * this one batch's documents, logged and skipped -- not fatal to
         * the run. w->failed is reserved for "this worker could never do
         * any work at all" (the connection-open check above), matching
         * concurrent_worker_run()'s established convention: losing one
         * batch out of thousands shouldn't discard every passage every
         * other worker already committed. */
        size_t doc_count = 0;
        PgStoreRawDocument *docs = pg_store_get_raw_documents_range(store, start, end, &doc_count);
        if (docs == NULL) {
            fprintf(stderr, "phase2_worker_run: failed to fetch rows [%lld, %lld), skipping batch\n",
                    (long long)start, (long long)end);
            continue;
        }
        if (doc_count == 0) {
            pg_store_raw_documents_free(docs, doc_count);
            continue;
        }

        long passages = phase2_process_batch(store, w->stopwords, w->wordnet, w->lemmatizer, w->chunk_size,
                                              w->overlap, docs, doc_count);
        pg_store_raw_documents_free(docs, doc_count);

        if (passages > 0) {
            w->passages_ingested += passages;
        }
    }

    pg_store_close(store);
    return NULL;
}

long bulk_ingest_tsv(const char *conninfo, const StopwordSet *stopwords,
                      const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                      const char *tsv_path, size_t chunk_size, size_t overlap,
                      int thread_count) {
    if (thread_count < 1) {
        thread_count = 1;
    }

    PgStore *coordinator = pg_store_open(conninfo);
    if (coordinator == NULL) {
        return -1;
    }

    /* Phase 1: one big COPY, not one INSERT per row -- see
     * pg_store_copy_documents_raw() and SPEED.md for the format-safety
     * investigation (real MS MARCO passages contain literal, unescaped
     * backslash and double-quote characters) that led here. */
    if (pg_store_create_staging_tables(coordinator) != 0 ||
        pg_store_truncate_staging_tables(coordinator) != 0) {
        fprintf(stderr, "bulk_ingest_tsv: failed to prepare staging tables\n");
        pg_store_close(coordinator);
        return -1;
    }

    int64_t total_rows = pg_store_copy_documents_raw(coordinator, tsv_path);
    if (total_rows < 0) {
        fprintf(stderr, "bulk_ingest_tsv: Phase 1 (COPY) failed\n");
        pg_store_close(coordinator);
        return -1;
    }

    /* Phase 2: thread_count workers, each with its own connection,
     * race-free by construction -- see this file's header comment. */
    pthread_t *threads = malloc(sizeof(pthread_t) * (size_t)thread_count);
    Phase2Worker *workers = malloc(sizeof(Phase2Worker) * (size_t)thread_count);
    if (threads == NULL || workers == NULL) {
        free(threads);
        free(workers);
        pg_store_close(coordinator);
        return -1;
    }

    int64_t next_row = 1;
    pthread_mutex_t range_mutex = PTHREAD_MUTEX_INITIALIZER;

    for (int i = 0; i < thread_count; i++) {
        workers[i] = (Phase2Worker){
            .conninfo = conninfo,
            .next_row = &next_row,
            .total_rows = total_rows,
            .range_mutex = &range_mutex,
            .stopwords = stopwords,
            .wordnet = wordnet,
            .lemmatizer = lemmatizer,
            .chunk_size = chunk_size,
            .overlap = overlap,
            .passages_ingested = 0,
            .failed = 0,
        };
        pthread_create(&threads[i], NULL, phase2_worker_run, &workers[i]);
    }

    long total_passages = 0;
    int any_failed = 0;
    for (int i = 0; i < thread_count; i++) {
        pthread_join(threads[i], NULL);
        total_passages += workers[i].passages_ingested;
        if (workers[i].failed) {
            any_failed = 1;
        }
    }

    pthread_mutex_destroy(&range_mutex);
    free(threads);
    free(workers);

    if (any_failed) {
        pg_store_close(coordinator);
        return -1;
    }

    /* Phase 3: single-threaded, set-based term resolution -- the only
     * point in this whole pipeline that touches the terms table, and the
     * only writer when it does. */
    long postings_written = pg_store_finalize_terms_and_postings(coordinator);
    if (postings_written < 0) {
        fprintf(stderr, "bulk_ingest_tsv: Phase 3 (finalize) failed\n");
        pg_store_close(coordinator);
        return -1;
    }

    pg_store_drop_staging_tables(coordinator);
    pg_store_close(coordinator);

    return total_passages;
}
