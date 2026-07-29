/*
 * CLI entrypoint for the LEXIS C core. Dispatches to ingestion mode
 * (build/update the index from a corpus) or query mode (run the
 * retrieval + generation pipeline for a single question), per spec
 * section 5.1's high-level data flow.
 *
 * Must be run from the project root -- data/stopwords, data/wordnet,
 * and the index file are all located via relative paths, same
 * convention the test suite already uses.
 */

#define _POSIX_C_SOURCE 200809L

#include "bm25.h"
#include "config.h"
#include "generation.h"
#include "ingest.h"
#include "lemmatizer.h"
#include "openrouter_client.h"
#include "pg_store.h"
#include "query_formulation.h"
#include "query_log.h"
#include "stopwords.h"
#include "wordnet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Postgres connection string -- matches docker-compose.yml's dev instance
 * (experiment/postgres-migration branch only; see LIMITATIONS.md for the
 * still-open question of how/whether the shipped app would ever need this
 * to be user-configurable). */
#define LEXIS_DB_CONNINFO "host=127.0.0.1 port=5433 dbname=lexis user=lexis password=lexis_dev_only"
/* Display-only label -- never print LEXIS_DB_CONNINFO itself, it embeds
 * the dev password. */
#define LEXIS_DB_LABEL "127.0.0.1:5433/lexis"
#define LEXIS_STOPWORDS_PATH "data/stopwords/english.txt"
#define LEXIS_WORDNET_DIR "data/wordnet"
#define LEXIS_CONFIG_PATH "config/lexis.conf"
#define LEXIS_MODEL "openai/gpt-4o-mini"
#define LEXIS_CHUNK_SIZE 200
#define LEXIS_CHUNK_OVERLAP 40
#define LEXIS_TOP_K 5

static long elapsed_ms(struct timespec start, struct timespec end) {
    long seconds = end.tv_sec - start.tv_sec;
    long nanoseconds = end.tv_nsec - start.tv_nsec;
    return seconds * 1000 + nanoseconds / 1000000;
}

static void print_usage(const char *program_name) {
    fprintf(stderr,
            "Usage:\n"
            "  %s ingest <corpus_dir>    Build/rebuild the index from a directory of documents\n"
            "  %s query \"<question>\"     Ask a question against the current index\n"
            "\n"
            "Must be run from the project root.\n",
            program_name, program_name);
}

static int run_ingest(const char *corpus_dir) {
    StopwordSet *stopwords = stopword_set_load(LEXIS_STOPWORDS_PATH);
    WordNetTable *wordnet = wordnet_table_load(LEXIS_WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(LEXIS_WORDNET_DIR);
    if (stopwords == NULL || wordnet == NULL || lemmatizer == NULL) {
        fprintf(stderr,
                "lexis: failed to load stopwords/wordnet/lemmatizer -- "
                "are you running this from the project root?\n");
        stopword_set_free(stopwords);
        wordnet_table_free(wordnet);
        lemmatizer_free(lemmatizer);
        return 1;
    }

    PgStore *store = pg_store_open(LEXIS_DB_CONNINFO);
    if (store == NULL) {
        fprintf(stderr, "lexis: failed to open index at %s\n", LEXIS_DB_LABEL);
        stopword_set_free(stopwords);
        wordnet_table_free(wordnet);
        lemmatizer_free(lemmatizer);
        return 1;
    }

    long passages = ingest_corpus(store, stopwords, wordnet, lemmatizer, corpus_dir,
                                   LEXIS_CHUNK_SIZE, LEXIS_CHUNK_OVERLAP);
    int exit_code = 0;
    if (passages < 0) {
        fprintf(stderr, "lexis: failed to ingest %s\n", corpus_dir);
        exit_code = 1;
    } else {
        printf("Ingested %ld passages from %s into %s\n", passages, corpus_dir, LEXIS_DB_LABEL);
    }

    pg_store_close(store);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    return exit_code;
}

static int run_query(const char *question) {
    StopwordSet *stopwords = stopword_set_load(LEXIS_STOPWORDS_PATH);
    WordNetTable *wordnet = wordnet_table_load(LEXIS_WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(LEXIS_WORDNET_DIR);
    if (stopwords == NULL || wordnet == NULL || lemmatizer == NULL) {
        fprintf(stderr,
                "lexis: failed to load stopwords/wordnet/lemmatizer -- "
                "are you running this from the project root?\n");
        stopword_set_free(stopwords);
        wordnet_table_free(wordnet);
        lemmatizer_free(lemmatizer);
        return 1;
    }

    PgStore *store = pg_store_open(LEXIS_DB_CONNINFO);
    if (store == NULL) {
        fprintf(stderr, "lexis: failed to open index at %s\n", LEXIS_DB_LABEL);
        stopword_set_free(stopwords);
        wordnet_table_free(wordnet);
        lemmatizer_free(lemmatizer);
        return 1;
    }

    /* Pipeline logging is only active in testing mode (config/lexis.conf)
     * -- in production mode query_id stays -1 and every query_log_* call
     * below is skipped, the same way it already would be if logging had
     * failed to initialize. Measured overhead is small (~2.5ms p50 on top
     * of a ~5ms bare BM25 search, see LIMITATIONS.md) but production
     * traffic shouldn't have to pay it just so testing can observe it. */
    LexisMode mode = config_load_mode(LEXIS_CONFIG_PATH);
    if (mode == LEXIS_MODE_TESTING && query_log_init_schema(store) != 0) {
        fprintf(stderr, "lexis: warning: pipeline logging unavailable, continuing without it\n");
    }

    if (openrouter_client_init() != 0) {
        fprintf(stderr, "lexis: failed to initialize the OpenRouter HTTP client\n");
        pg_store_close(store);
        stopword_set_free(stopwords);
        wordnet_table_free(wordnet);
        lemmatizer_free(lemmatizer);
        return 1;
    }

    int exit_code = 0;
    int pipeline_succeeded = 0;
    int64_t query_id =
        (mode == LEXIS_MODE_TESTING) ? query_log_insert_query(store, question) : -1;

    struct timespec pipeline_start, pipeline_end;
    clock_gettime(CLOCK_MONOTONIC, &pipeline_start);

    /* Query formulation, decomposed into its own public sub-steps (rather
     * than the query_formulation_formulate_query() convenience wrapper) so
     * every intermediate artifact -- the prompt, the raw LLM response,
     * whether the API-failure fallback fired -- is visible here for
     * logging, with zero changes to query_formulation.c itself. */
    struct timespec qf_start, qf_end;
    clock_gettime(CLOCK_MONOTONIC, &qf_start);

    QueryFormulationCandidates *candidates =
        query_formulation_gather_candidates(question, stopwords, wordnet, lemmatizer);
    if (candidates == NULL) {
        fprintf(stderr, "lexis: query formulation failed\n");
        exit_code = 1;
        goto cleanup;
    }

    char *qf_prompt = NULL;
    char *qf_response = NULL;
    int used_fallback = 0;
    TokenList *terms = NULL;

    if (candidates->count == 0) {
        terms = token_list_create();
    } else {
        qf_prompt = query_formulation_build_prompt(question, candidates);
        if (qf_prompt != NULL) {
            qf_response = openrouter_chat_completion(LEXIS_MODEL, qf_prompt);
            used_fallback = (qf_response == NULL);
        }
        /* query_formulation_parse_selected_terms() falls back to
         * `candidates`'s original terms on a NULL response too (cJSON_Parse
         * safely treats NULL as unparseable) -- same fallback behavior
         * query_formulation_formulate_query() itself relies on. */
        terms = query_formulation_parse_selected_terms(qf_response, candidates);
    }

    clock_gettime(CLOCK_MONOTONIC, &qf_end);

    if (terms == NULL) {
        fprintf(stderr, "lexis: query formulation failed\n");
        free(qf_prompt);
        free(qf_response);
        query_formulation_candidates_free(candidates);
        exit_code = 1;
        goto cleanup;
    }

    if (query_id != -1) {
        char *selected_terms_str = ingest_join_words(terms, 0, terms->count);
        query_log_insert_query_formulation_run(
            store, query_id, (int)candidates->count, qf_prompt, qf_response, used_fallback,
            selected_terms_str != NULL ? selected_terms_str : "", elapsed_ms(qf_start, qf_end));
        free(selected_terms_str);
    }

    free(qf_prompt);
    free(qf_response);
    query_formulation_candidates_free(candidates);

    printf("Search terms: ");
    for (size_t i = 0; i < terms->count; i++) {
        printf("%s%s", terms->terms[i], (i + 1 < terms->count) ? ", " : "");
    }
    printf("\n\n");

    if (terms->count == 0) {
        printf("Nothing to search for -- the question was entirely stopwords.\n");
        token_list_free(terms);
        goto cleanup;
    }

    {
        const char **query_terms = malloc(terms->count * sizeof(char *));
        if (query_terms == NULL) {
            fprintf(stderr, "lexis: out of memory\n");
            token_list_free(terms);
            exit_code = 1;
            goto cleanup;
        }
        for (size_t i = 0; i < terms->count; i++) {
            query_terms[i] = terms->terms[i];
        }

        struct timespec search_start, search_end;
        clock_gettime(CLOCK_MONOTONIC, &search_start);
        BM25Params params = {BM25_DEFAULT_K1, BM25_DEFAULT_B};
        BM25ResultSet *results = bm25_search(store, query_terms, terms->count, LEXIS_TOP_K, params);
        clock_gettime(CLOCK_MONOTONIC, &search_end);
        free(query_terms);
        token_list_free(terms);

        if (results == NULL) {
            fprintf(stderr, "lexis: search failed\n");
            exit_code = 1;
            goto cleanup;
        }

        if (query_id != -1) {
            int64_t search_run_id = query_log_insert_search_run(
                store, query_id, LEXIS_TOP_K, (int)results->count,
                elapsed_ms(search_start, search_end));
            if (search_run_id != -1) {
                for (size_t i = 0; i < results->count; i++) {
                    query_log_insert_search_result(store, search_run_id, (int)i + 1,
                                                    results->items[i].passage_id,
                                                    results->items[i].score);
                }
            }
        }

        if (results->count == 0) {
            printf("No matching passages found. Have you run '%s ingest <corpus_dir>' yet?\n",
                   "lexis");
            bm25_result_set_free(results);
            goto cleanup;
        }

        printf("Top matches:\n");
        int passages_included = 0;
        int passages_skipped = 0;
        for (size_t i = 0; i < results->count; i++) {
            PgStorePassage *passage = pg_store_get_passage(store, results->items[i].passage_id);
            if (passage != NULL) {
                printf("  [%.3f] %s (chunk %d)\n", results->items[i].score, passage->document_name,
                       passage->chunk_id);
                pg_store_passage_free(passage);
                passages_included++;
            } else {
                passages_skipped++;
            }
        }
        printf("\n");

        struct timespec gen_start, gen_end;
        clock_gettime(CLOCK_MONOTONIC, &gen_start);
        char *gen_prompt = generation_build_prompt(question, store, results);
        char *answer = generation_generate_answer(question, LEXIS_MODEL, store, results);
        clock_gettime(CLOCK_MONOTONIC, &gen_end);
        bm25_result_set_free(results);

        if (query_id != -1) {
            query_log_insert_generation_run(store, query_id, LEXIS_MODEL, passages_included,
                                             passages_skipped, gen_prompt, answer, answer != NULL,
                                             elapsed_ms(gen_start, gen_end));
        }
        free(gen_prompt);

        if (answer == NULL) {
            fprintf(stderr, "lexis: could not generate an answer -- is OPENROUTER_API_KEY set?\n");
            exit_code = 1;
            goto cleanup;
        }

        printf("Answer: %s\n", answer);
        free(answer);
        pipeline_succeeded = 1;
    }

cleanup:
    clock_gettime(CLOCK_MONOTONIC, &pipeline_end);
    if (query_id != -1) {
        query_log_finish_query(store, query_id, elapsed_ms(pipeline_start, pipeline_end),
                                pipeline_succeeded);
    }
    openrouter_client_cleanup();
    pg_store_close(store);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    return exit_code;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "ingest") == 0) {
        return run_ingest(argv[2]);
    }
    if (strcmp(argv[1], "query") == 0) {
        return run_query(argv[2]);
    }

    print_usage(argv[0]);
    return 1;
}
