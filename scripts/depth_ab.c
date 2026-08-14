/*
 * A/B harness: does sending fewer passages produce better answers?
 *
 * Phase 0 showed BM25 puts a correct passage at rank 1 for 82% of DelucionQA
 * questions and inside the top 12 for 97%, so the twelve passages the app
 * currently sends are mostly distractors. Separate measurement showed answer
 * quality degrading as retrieval depth grew. This runs the real pipeline at
 * two passage counts over the same questions and the same candidate sets, so
 * the only variable is how many of them reach the model.
 *
 * Single-turn: no conversation history is passed, matching how DelucionQA
 * questions are posed and keeping history out of the comparison entirely.
 *
 * Output: TSV to stdout -- question_index, max_passages, n_sent,
 * passage_ids (comma separated), prompt_tokens, seconds, answer. Tabs and
 * newlines are stripped from the answer so the row stays parseable; scoring
 * against the reference answers happens in depth_ab_score.py.
 *
 * Build: see scripts/depth_ab_run.sh
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bm25.h"
#include "config.h"
#include "generation.h"
#include "lemmatizer.h"
#include "local_llm_client.h"
#include "pg_store.h"
#include "query_formulation.h"
#include "stopwords.h"
#include "tokenizer.h"
#include "wordnet.h"

#define CANDIDATE_CEILING 40
#define TOKEN_BUDGET 1500
#define SCORE_FLOOR 0.6
#define LINE_MAX_LEN 8192

/* The two configurations under test: today's 12, versus the 5 that Phase 0's
 * recall@5 of 93.5% suggests may be enough. */
static const size_t CONFIGS[] = {12, 5};

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* In place: collapse every tab/newline to a space so one answer stays one
 * TSV field. */
static void flatten(char *text) {
    for (char *p = text; *p != '\0'; p++) {
        if (*p == '\t' || *p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
}

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

    char *model_path = config_load_model_path("config/lexis.conf");
    if (model_path == NULL || local_llm_client_init(model_path) != 0) {
        fprintf(stderr, "model init failed\n");
        free(model_path);
        return 1;
    }
    free(model_path);
    PgStore *store = pg_store_open("host=127.0.0.1 port=5434 dbname=lexis user=lexis password=lexis_dev_only");
    if (store == NULL || pg_store_use_corpus(store, corpus_id) != 0) {
        fprintf(stderr, "cannot open corpus\n");
        return 1;
    }
    StopwordSet *sw = stopword_set_load("data/stopwords/english.txt");
    WordNetTable *wn = wordnet_table_load("data/wordnet");
    Lemmatizer *lm = lemmatizer_load("data/wordnet");

    BM25Params params = {BM25_DEFAULT_K1, BM25_DEFAULT_B};
    BM25CorpusStats stats = bm25_corpus_stats(store);

    char line[LINE_MAX_LEN];
    long index = 0;
    while (fgets(line, sizeof(line), questions) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            index++;
            continue;
        }

        for (size_t c = 0; c < sizeof(CONFIGS) / sizeof(CONFIGS[0]); c++) {
            size_t max_passages = CONFIGS[c];

            TokenList *terms = query_formulation_terms_union(line, NULL, sw, wn, lm);
            if (terms == NULL || terms->count == 0) {
                token_list_free(terms);
                continue;
            }
            const char **query_terms = malloc(sizeof(char *) * terms->count);
            for (size_t i = 0; i < terms->count; i++) {
                query_terms[i] = terms->terms[i];
            }
            BM25ResultSet *results =
                bm25_search(store, query_terms, terms->count, CANDIDATE_CEILING, stats, params);
            free(query_terms);
            token_list_free(terms);
            if (results == NULL || results->count == 0) {
                bm25_result_set_free(results);
                continue;
            }
            bm25_result_set_trim(store, results, max_passages, TOKEN_BUDGET, SCORE_FLOOR);

            char *prompt = generation_build_prompt(line, store, results);
            int prompt_tokens = (prompt != NULL) ? local_llm_count_tokens(prompt) : -1;
            free(prompt);

            double started = now_s();
            char *answer = generation_generate_answer_with_history(line, store, results, NULL, 0);
            double elapsed = now_s() - started;

            printf("%ld\t%zu\t%zu\t", index, max_passages, results->count);
            for (size_t i = 0; i < results->count; i++) {
                printf("%s%lld", i ? "," : "", (long long)results->items[i].passage_id);
            }
            if (answer != NULL) {
                flatten(answer);
            }
            printf("\t%d\t%.2f\t%s\n", prompt_tokens, elapsed, answer ? answer : "");
            fflush(stdout);

            free(answer);
            bm25_result_set_free(results);
        }

        index++;
        fprintf(stderr, "  question %ld\r", index);
    }
    fprintf(stderr, "  %ld questions done\n", index);

    fclose(questions);
    stopword_set_free(sw);
    wordnet_table_free(wn);
    lemmatizer_free(lm);
    pg_store_close(store);
    local_llm_client_cleanup();
    return 0;
}
