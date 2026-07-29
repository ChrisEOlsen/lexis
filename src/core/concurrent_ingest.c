/*
 * Implementation of genuinely concurrent ingestion.
 * See include/concurrent_ingest.h for the module's role and why this is
 * safe with Postgres in a way it never could be with SQLite.
 */

#define _POSIX_C_SOURCE 200809L

#include "concurrent_ingest.h"

#include "ingest.h"
#include "pg_store.h"
#include "tokenizer.h"

#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Shared, read-only across all workers except next_index/index_mutex (the
 * work-stealing cursor into `filenames`) and the two output fields, which
 * only this worker's own thread ever writes. */
typedef struct {
    const char *conninfo;
    const char *dir_path;
    char **filenames;
    size_t file_count;
    size_t *next_index;
    pthread_mutex_t *index_mutex;
    const StopwordSet *stopwords;
    const WordNetTable *wordnet;
    const Lemmatizer *lemmatizer;
    size_t chunk_size;
    size_t overlap;
    long passages_ingested;
    int failed;
} ConcurrentWorker;

static void *concurrent_worker_run(void *arg) {
    ConcurrentWorker *w = (ConcurrentWorker *)arg;

    /* Each worker owns its own connection -- a single PGconn, like a
     * single sqlite3*, isn't safe for concurrent use from multiple
     * threads. Unlike SQLite, though, nothing stops N separate
     * connections from all writing to the same tables at once. */
    PgStore *store = pg_store_open(w->conninfo);
    if (store == NULL) {
        fprintf(stderr, "concurrent_worker_run: failed to open a connection\n");
        w->failed = 1;
        return NULL;
    }

    while (1) {
        pthread_mutex_lock(w->index_mutex);
        size_t idx = *w->next_index;
        if (idx < w->file_count) {
            (*w->next_index)++;
        }
        pthread_mutex_unlock(w->index_mutex);

        if (idx >= w->file_count) {
            break;
        }

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", w->dir_path, w->filenames[idx]);

        /* Postgres's ON CONFLICT uses "speculative insertion" internally;
         * concurrent speculative inserts targeting the same unique index
         * (terms.term) can trigger a genuine, expected Postgres deadlock
         * under real write concurrency -- verified directly (deadlock
         * detected, silently dropping the whole document) on a
         * 3000-document corpus at 4+ threads. Postgres's own docs are
         * explicit that the fix is retrying the transaction, not trying
         * to structurally prevent it. ingest_document() already wraps
         * each document in its own transaction and rolls back cleanly on
         * failure, so retrying the whole document from scratch is safe.
         * A small fixed retry count, not infinite -- a genuinely broken
         * document (not a transient deadlock) should still eventually
         * give up rather than loop forever. */
        long passages = -1;
        int attempt;
        for (attempt = 0; attempt < 3 && passages < 0; attempt++) {
            passages = ingest_document(store, w->stopwords, w->wordnet, w->lemmatizer, full_path,
                                        w->filenames[idx], w->chunk_size, w->overlap);
        }
        if (passages < 0) {
            fprintf(stderr, "concurrent_worker_run: failed to ingest %s after %d attempts, skipping\n",
                    full_path, attempt);
            continue;
        }
        w->passages_ingested += passages;
    }

    pg_store_close(store);
    return NULL;
}

long concurrent_ingest_corpus(const char *conninfo, const StopwordSet *stopwords,
                               const WordNetTable *wordnet, const Lemmatizer *lemmatizer,
                               const char *dir_path, size_t chunk_size, size_t overlap,
                               int thread_count) {
    if (thread_count < 1) {
        thread_count = 1;
    }

    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        fprintf(stderr, "concurrent_ingest_corpus: could not open directory %s\n", dir_path);
        return -1;
    }

    TokenList *filenames = token_list_create();
    if (filenames == NULL) {
        closedir(dir);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (token_list_append(filenames, entry->d_name) != 0) {
            token_list_free(filenames);
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);

    pthread_t *threads = malloc(sizeof(pthread_t) * (size_t)thread_count);
    ConcurrentWorker *workers = malloc(sizeof(ConcurrentWorker) * (size_t)thread_count);
    if (threads == NULL || workers == NULL) {
        free(threads);
        free(workers);
        token_list_free(filenames);
        return -1;
    }

    pthread_mutex_t index_mutex = PTHREAD_MUTEX_INITIALIZER;
    size_t next_index = 0;

    for (int i = 0; i < thread_count; i++) {
        workers[i] = (ConcurrentWorker){
            .conninfo = conninfo,
            .dir_path = dir_path,
            .filenames = filenames->terms,
            .file_count = filenames->count,
            .next_index = &next_index,
            .index_mutex = &index_mutex,
            .stopwords = stopwords,
            .wordnet = wordnet,
            .lemmatizer = lemmatizer,
            .chunk_size = chunk_size,
            .overlap = overlap,
            .passages_ingested = 0,
            .failed = 0,
        };
        pthread_create(&threads[i], NULL, concurrent_worker_run, &workers[i]);
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
    pthread_mutex_destroy(&index_mutex);
    token_list_free(filenames);
    free(threads);
    free(workers);

    if (any_failed) {
        return -1;
    }
    return total_passages;
}
