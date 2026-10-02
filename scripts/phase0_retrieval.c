/*
 * Phase 0 harness: batch BM25 retrieval over questions; TSV to stdout (scored by phase0_score.py).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bm25.h"
#include "config.h"
#include "lemmatizer.h"
#include "pg_store.h"
#include "query_formulation.h"
#include "stopwords.h"
#include "tokenizer.h"
#include "wordnet.h"

#define DEPTH 40
#define LINE_MAX_LEN 8192

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <corpus_id> <questions_file>\n", argv[0]);
        return 2;
    }
    int64_t corpus_id = strtoll(argv[1], NULL, 10);

    FILE *questions = fopen(argv[2], "r");
    if (questions == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[2]);
        return 1;
    }

    char *db_conninfo = config_load_db_conninfo("config/lexis.conf");
    if (db_conninfo == NULL) {
        fprintf(stderr, "no database configured -- set db_conninfo in config/lexis.conf\n");
        return 1;
    }
    PgStore *store = pg_store_open(db_conninfo);
    if (store == NULL || pg_store_use_corpus(store, corpus_id) != 0) {
        fprintf(stderr, "cannot open corpus %lld\n", (long long)corpus_id);
        return 1;
    }
    StopwordSet *sw = stopword_set_load("data/stopwords/english.txt");
    WordNetTable *wn = wordnet_table_load("data/wordnet");
    Lemmatizer *lm = lemmatizer_load("data/wordnet");
    if (sw == NULL || wn == NULL || lm == NULL) {
        fprintf(stderr, "cannot load language data\n");
        return 1;
    }

    BM25Params params = {.k1 = BM25_DEFAULT_K1, .b = BM25_DEFAULT_B};
    /* Once per run: a full-corpus aggregate, valid while the corpus is unchanged. */
    BM25CorpusStats stats = bm25_corpus_stats(store);

    char line[LINE_MAX_LEN];
    long index = 0;
    while (fgets(line, sizeof(line), questions) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            index++;
            continue;
        }

        TokenList *terms = query_formulation_terms_union(line, NULL, sw, wn, lm);
        if (terms == NULL || terms->count == 0) {
            /* Zero results, not skipped: the scorer must count this as a miss. */
            token_list_free(terms);
            index++;
            continue;
        }

        const char **query_terms = malloc(sizeof(char *) * terms->count);
        for (size_t i = 0; i < terms->count; i++) {
            query_terms[i] = terms->terms[i];
        }
        BM25ResultSet *results = bm25_search(store, query_terms, terms->count, DEPTH, stats, params);
        free(query_terms);
        token_list_free(terms);

        if (results != NULL) {
            for (size_t i = 0; i < results->count; i++) {
                printf("%ld\t%zu\t%lld\t%.4f\n", index, i + 1, (long long)results->items[i].passage_id,
                       results->items[i].score);
            }
            bm25_result_set_free(results);
        }

        index++;
        if (index % 50 == 0) {
            fprintf(stderr, "  %ld questions\r", index);
        }
    }
    fprintf(stderr, "  %ld questions done\n", index);

    fclose(questions);
    stopword_set_free(sw);
    wordnet_table_free(wn);
    lemmatizer_free(lm);
    pg_store_close(store);
    return 0;
}
