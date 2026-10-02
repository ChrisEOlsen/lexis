/* Bulk TSV ingestion: 3-phase deferred-term-resolution pipeline (see bulk_ingest.h).
 * Phase 2 never touches terms, so ON CONFLICT deadlocks are gone by design (see dev/SPEED.md). */

/* Must precede #includes: strdup/getline are POSIX, hidden under strict -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include "bulk_ingest.h"

#include "ingest.h"
#include "pg_store.h"
#include "tokenizer.h"
#include "time_util.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Rows claimed per round trip: amortizes network cost without stranding a worker on
 * an oversized final batch (see dev/SPEED.md for measured throughput). */
#define BULK_PHASE2_BATCH_SIZE 500

/* One transaction per batch: safe because Phase 2 never touches terms (the sole measured
 * deadlock source); passages/staged inserts don't lock across documents. */
#define BULK_PHASE2_BATCH_RETRIES 3

/* Read-only across workers except next_row/range_mutex (shared row_num cursor);
 * passages_ingested/failed are written only by the owning thread. */
typedef struct {
    const char *conninfo;
    const char *schema_name;
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

/* Claims up to BULK_PHASE2_BATCH_SIZE row_nums as [*start, *end). Returns 1 when
 * the table is fully claimed, 0 otherwise. */
/* See bulk_ingest.h's cancellation contract: checked per phase and per batch, so a
 * cancel lands within one batch of work. */
static atomic_int g_cancel_requested = 0;

void bulk_ingest_request_cancel(void) {
    atomic_store(&g_cancel_requested, 1);
}

void bulk_ingest_clear_cancel(void) {
    atomic_store(&g_cancel_requested, 0);
}

static int cancel_requested(void) {
    return atomic_load(&g_cancel_requested);
}

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

/* Stages one chunk's distinct terms as raw text against passage_id; id resolution is
 * Phase 3's job. 0 on success, -1 on error. */
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

/* Per-documents_raw-row work: chunk, tokenize, lemmatize, insert passage, stage
 * postings (term text, not ids). Returns passages ingested, or -1. */
static long phase2_process_document(PgStore *store, const StopwordSet *stopwords,
                                     const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                                     const char *text, const char *pid, size_t chunk_size, size_t overlap) {
    /* Original un-chunked text, once per document, so rebuild-on-append can re-chunk
     * consistently (see pg_store_insert_document()). */
    if (pg_store_insert_document(store, pid, text) != 0) {
        return -1;
    }

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

/* One batch in one transaction (safe here, see BULK_PHASE2_BATCH_RETRIES). Any failure
 * rolls back the whole batch; retried, then skipped. Returns passages, or -1. */
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

    /* Each worker owns its connection: one PGconn isn't thread-safe, but N separate
     * connections can write the same tables concurrently. */
    PgStore *store = pg_store_open(w->conninfo);
    if (store == NULL) {
        fprintf(stderr, "phase2_worker_run: failed to open a connection\n");
        w->failed = 1;
        return NULL;
    }
    if (w->schema_name != NULL && w->schema_name[0] != '\0' && pg_store_use_schema(store, w->schema_name) != 0) {
        fprintf(stderr, "phase2_worker_run: failed to select schema %s\n", w->schema_name);
        pg_store_close(store);
        w->failed = 1;
        return NULL;
    }
    /* Rebuildable index data, not irreplaceable (see pg_store_disable_synchronous_commit()). */
    pg_store_disable_synchronous_commit(store);

    while (1) {
        int64_t start, end;
        if (cancel_requested() || phase2_claim_batch(w, &start, &end) != 0) {
            break;
        }

        /* A failed batch costs at most its own documents (logged, skipped); w->failed is
         * only for "could never do any work" (see concurrent_worker_run()'s convention). */
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

long bulk_ingest_tsv(const char *conninfo, const char *schema_name, const StopwordSet *stopwords,
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
    if (schema_name != NULL && schema_name[0] != '\0' && pg_store_use_schema(coordinator, schema_name) != 0) {
        fprintf(stderr, "bulk_ingest_tsv: failed to select schema %s\n", schema_name);
        pg_store_close(coordinator);
        return -1;
    }

    /* Phase 1: one COPY, not one INSERT per row (real MS MARCO text needs CSV
     * quoting; see pg_store_copy_documents_raw(), dev/SPEED.md). */
    struct timespec phase1_start, phase1_end;
    clock_gettime(CLOCK_MONOTONIC, &phase1_start);

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

    clock_gettime(CLOCK_MONOTONIC, &phase1_end);

    if (cancel_requested()) {
        pg_store_drop_staging_tables(coordinator);
        pg_store_close(coordinator);
        return BULK_INGEST_CANCELLED;
    }

    /* Phase 2: thread_count workers on separate connections; race-free by design. */
    struct timespec phase2_start, phase2_end;
    clock_gettime(CLOCK_MONOTONIC, &phase2_start);

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
            .schema_name = schema_name,
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

    clock_gettime(CLOCK_MONOTONIC, &phase2_end);

    pthread_mutex_destroy(&range_mutex);
    free(threads);
    free(workers);

    if (any_failed) {
        pg_store_close(coordinator);
        return -1;
    }

    if (cancel_requested()) {
        /* Caller (or next run's defensive drop) owns schema cleanup; just drop staging. */
        pg_store_drop_staging_tables(coordinator);
        pg_store_close(coordinator);
        return BULK_INGEST_CANCELLED;
    }

    /* Defer PK/FK + durability to one bulk pass at the end (biggest measured lever; see
     * dev/SPEED.md). Failure before finish leaves schema weakened until next run -- accepted. */
    struct timespec prepare_start, prepare_end;
    clock_gettime(CLOCK_MONOTONIC, &prepare_start);

    if (pg_store_prepare_bulk_load(coordinator) != 0) {
        fprintf(stderr, "bulk_ingest_tsv: failed to prepare postings/terms for bulk load\n");
        pg_store_close(coordinator);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &prepare_end);

    /* Phase 3: single-threaded set-based term resolution -- the only point touching
     * terms, and the only writer while it does. */
    struct timespec phase3_start, phase3_end;
    clock_gettime(CLOCK_MONOTONIC, &phase3_start);

    long postings_written = pg_store_finalize_terms_and_postings(coordinator);
    if (postings_written < 0) {
        fprintf(stderr, "bulk_ingest_tsv: Phase 3 (finalize) failed\n");
        pg_store_close(coordinator);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &phase3_end);

    struct timespec restore_start, restore_end;
    clock_gettime(CLOCK_MONOTONIC, &restore_start);

    if (pg_store_finish_bulk_load(coordinator) != 0) {
        fprintf(stderr, "bulk_ingest_tsv: failed to restore postings/terms constraints -- schema "
                        "left weakened, will be restored by the next successful run\n");
        pg_store_close(coordinator);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &restore_end);

    pg_store_drop_staging_tables(coordinator);
    pg_store_close(coordinator);

    /* Unconditional per-phase timing on success (same convention as eval_run()): long
     * runs are meant to be watched, not just pass/failed. */
    printf("bulk_ingest_tsv: Phase 1 (raw append) %ldms, Phase 2 (parallel processing) %ldms, "
           "prepare (defer constraints) %ldms, Phase 3 (finalize) %ldms, "
           "restore (rebuild constraints) %ldms, %lld rows staged, %ld postings written\n",
           lexis_elapsed_ms(phase1_start, phase1_end), lexis_elapsed_ms(phase2_start, phase2_end),
           lexis_elapsed_ms(prepare_start, prepare_end), lexis_elapsed_ms(phase3_start, phase3_end),
           lexis_elapsed_ms(restore_start, restore_end), (long long)total_rows, postings_written);

    return total_passages;
}

/* One RFC4180 field (always quoted, quotes doubled) for COPY (FORMAT csv, DELIMITER
 * E'\t'). 0 on success, -1 on write error. */
static int write_rebuild_csv_field(FILE *fp, const char *field) {
    if (fputc('"', fp) == EOF) {
        return -1;
    }
    for (const char *p = field; *p != '\0'; p++) {
        if (*p == '"' && fputc('"', fp) == EOF) {
            return -1;
        }
        if (fputc((unsigned char)*p, fp) == EOF) {
            return -1;
        }
    }
    return fputc('"', fp) == EOF ? -1 : 0;
}

static int write_rebuild_csv_row(FILE *fp, const char *document_name, const char *text) {
    if (write_rebuild_csv_field(fp, document_name) != 0 || fputc('\t', fp) == EOF ||
        write_rebuild_csv_field(fp, text) != 0 || fputc('\n', fp) == EOF) {
        return -1;
    }
    return 0;
}

/* Existing + new docs to a temp CSV for bulk_ingest_tsv(); same-named new docs replace
 * existing (PK would reject dupes). Returns malloc'd path (caller removes/frees) or NULL. */
static char *materialize_combined_documents_csv(const PgStoreDocument *existing, size_t existing_count,
                                                  const char *const *new_names, const char *const *new_texts,
                                                  size_t new_count) {
    char path_template[] = "/tmp/lexis_rebuild_XXXXXX";
    int fd = mkstemp(path_template);
    if (fd == -1) {
        fprintf(stderr, "materialize_combined_documents_csv: mkstemp failed\n");
        return NULL;
    }
    FILE *fp = fdopen(fd, "wb");
    if (fp == NULL) {
        fprintf(stderr, "materialize_combined_documents_csv: fdopen failed\n");
        close(fd);
        remove(path_template);
        return NULL;
    }

    for (size_t i = 0; i < existing_count; i++) {
        int replaced = 0;
        for (size_t j = 0; j < new_count; j++) {
            if (strcmp(existing[i].document_name, new_names[j]) == 0) {
                replaced = 1;
                break;
            }
        }
        if (replaced) {
            continue;
        }
        if (write_rebuild_csv_row(fp, existing[i].document_name, existing[i].text) != 0) {
            fclose(fp);
            remove(path_template);
            return NULL;
        }
    }
    for (size_t j = 0; j < new_count; j++) {
        if (write_rebuild_csv_row(fp, new_names[j], new_texts[j]) != 0) {
            fclose(fp);
            remove(path_template);
            return NULL;
        }
    }

    if (fclose(fp) != 0) {
        remove(path_template);
        return NULL;
    }

    char *path = strdup(path_template);
    if (path == NULL) {
        remove(path_template);
        return NULL;
    }
    return path;
}

long bulk_ingest_rebuild_corpus(const char *conninfo, int64_t corpus_id, const char *const *new_document_names,
                                 const char *const *new_document_texts, size_t new_document_count,
                                 const StopwordSet *stopwords, const WordNetTable *wordnet,
                                 const Lemmatizer *lemmatizer, size_t chunk_size, size_t overlap, int thread_count) {
    PgStore *coordinator = pg_store_open(conninfo);
    if (coordinator == NULL) {
        return -1;
    }
    if (pg_store_use_corpus(coordinator, corpus_id) != 0) {
        fprintf(stderr, "bulk_ingest_rebuild_corpus: failed to select corpus %lld\n", (long long)corpus_id);
        pg_store_close(coordinator);
        return -1;
    }

    size_t existing_count = 0;
    PgStoreDocument *existing = pg_store_get_all_documents(coordinator, &existing_count);
    if (existing == NULL) {
        fprintf(stderr, "bulk_ingest_rebuild_corpus: failed to read corpus %lld's existing documents\n",
                (long long)corpus_id);
        pg_store_close(coordinator);
        return -1;
    }

    char *csv_path = materialize_combined_documents_csv(existing, existing_count, new_document_names,
                                                          new_document_texts, new_document_count);
    pg_store_documents_free(existing, existing_count);
    if (csv_path == NULL) {
        fprintf(stderr, "bulk_ingest_rebuild_corpus: failed to write the combined document CSV\n");
        pg_store_close(coordinator);
        return -1;
    }

    char temp_schema[64];
    snprintf(temp_schema, sizeof(temp_schema), "corpus_%lld_rebuild", (long long)corpus_id);

    /* Defensive drop first: a crashed earlier rebuild would leave this name occupied
     * (CREATE SCHEMA has no IF NOT EXISTS); matches the "rebuildable" philosophy. */
    if (pg_store_drop_bare_schema(coordinator, temp_schema) != 0 ||
        pg_store_create_bare_schema(coordinator, temp_schema) != 0) {
        fprintf(stderr, "bulk_ingest_rebuild_corpus: failed to prepare rebuild schema %s\n", temp_schema);
        remove(csv_path);
        free(csv_path);
        pg_store_close(coordinator);
        return -1;
    }

    /* Same fast pipeline as a fresh corpus, targeting temp_schema -- corpus_id's live
     * data is untouched by this step no matter what happens. */
    long total_passages = bulk_ingest_tsv(conninfo, temp_schema, stopwords, wordnet, lemmatizer, csv_path, chunk_size,
                                           overlap, thread_count);
    remove(csv_path);
    free(csv_path);

    if (total_passages == BULK_INGEST_CANCELLED || cancel_requested()) {
        fprintf(stderr, "bulk_ingest_rebuild_corpus: cancelled, corpus %lld left untouched\n",
                (long long)corpus_id);
        pg_store_drop_bare_schema(coordinator, temp_schema);
        pg_store_close(coordinator);
        return BULK_INGEST_CANCELLED;
    }
    if (total_passages < 0) {
        fprintf(stderr,
                "bulk_ingest_rebuild_corpus: ingest into the rebuild schema failed, corpus %lld left untouched\n",
                (long long)corpus_id);
        pg_store_drop_bare_schema(coordinator, temp_schema);
        pg_store_close(coordinator);
        return -1;
    }

    if (pg_store_swap_corpus_schema(coordinator, corpus_id, temp_schema) != 0) {
        fprintf(stderr,
                "bulk_ingest_rebuild_corpus: schema swap failed -- corpus %lld left untouched, rebuild "
                "schema %s left in place for inspection rather than silently discarded\n",
                (long long)corpus_id, temp_schema);
        pg_store_close(coordinator);
        return -1;
    }

    pg_store_close(coordinator);
    return total_passages;
}
