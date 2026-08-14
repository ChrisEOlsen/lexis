/*
 * One-shot headless group ingestion: create a group and ingest a
 * directory of .txt files into it through the exact pipeline the app's
 * IngestWorker runs (bulk_ingest_rebuild_corpus, chunk 200/overlap 40/
 * 6 threads -- keep in sync with IngestWorker.cpp's constants). Exists
 * so eval corpora (DelucionQA) can be (re)built on a fresh machine or
 * after a lemmatizer change without driving the GUI by hand.
 *
 * Usage: ingest_group "<display name>" <dir-of-txt-files>
 * Prints the new corpus id on success -- lexis_eval and
 * scripts/phase0_run.sh take it as their first argument.
 *
 * Build: same ad hoc clang invocation as scripts/phase0_run.sh uses for
 * phase0_retrieval.c (core sources + jinja_chat_template.o).
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bulk_ingest.h"
#include "ingest.h"
#include "lemmatizer.h"
#include "pg_store.h"
#include "stopwords.h"
#include "wordnet.h"

#define CONNINFO "host=127.0.0.1 port=5434 dbname=lexis user=lexis password=lexis_dev_only"
#define MAX_DOCS 256

static int compare_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s \"<display name>\" <dir-of-txt-files>\n", argv[0]);
        return 2;
    }
    const char *display_name = argv[1];
    const char *dir_path = argv[2];

    /* Collect *.txt names first, sorted for a deterministic ingest order. */
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        fprintf(stderr, "cannot open directory %s\n", dir_path);
        return 1;
    }
    char *names[MAX_DOCS];
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < MAX_DOCS) {
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcmp(entry->d_name + len - 4, ".txt") == 0) {
            names[count++] = strdup(entry->d_name);
        }
    }
    closedir(dir);
    if (count == 0) {
        fprintf(stderr, "no .txt files in %s\n", dir_path);
        return 1;
    }
    qsort(names, count, sizeof(char *), compare_names);

    char *texts[MAX_DOCS];
    for (size_t i = 0; i < count; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir_path, names[i]);
        texts[i] = ingest_read_file(path);
        if (texts[i] == NULL) {
            fprintf(stderr, "failed to read %s\n", path);
            return 1;
        }
    }

    StopwordSet *stopwords = stopword_set_load("data/stopwords/english.txt");
    WordNetTable *wordnet = wordnet_table_load("data/wordnet");
    Lemmatizer *lemmatizer = lemmatizer_load("data/wordnet");
    if (stopwords == NULL || wordnet == NULL || lemmatizer == NULL) {
        fprintf(stderr, "failed to load language data -- run from the project root\n");
        return 1;
    }

    PgStore *store = pg_store_open(CONNINFO);
    if (store == NULL) {
        fprintf(stderr, "cannot connect -- is Postgres running (make pg-start)?\n");
        return 1;
    }
    char *schema_name = NULL;
    int64_t corpus_id = pg_store_create_corpus(store, display_name, &schema_name);
    pg_store_close(store);
    if (corpus_id <= 0) {
        fprintf(stderr, "pg_store_create_corpus failed\n");
        return 1;
    }
    free(schema_name);

    long total = bulk_ingest_rebuild_corpus(CONNINFO, corpus_id, (const char *const *)names,
                                            (const char *const *)texts, count, stopwords, wordnet,
                                            lemmatizer, 200, 40, 6);
    if (total < 0) {
        fprintf(stderr, "ingest failed for corpus %lld\n", (long long)corpus_id);
        return 1;
    }

    fprintf(stderr, "corpus \"%s\": %zu documents, %ld passages\n", display_name, count, total);
    printf("%lld\n", (long long)corpus_id);
    return 0;
}
