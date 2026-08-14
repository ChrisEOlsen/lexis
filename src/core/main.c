/*
 * CLI entrypoint for the LEXIS C core. Dispatches to bulk-ingest (build/
 * rebuild the index from a TSV corpus via bulk_ingest.c's three-phase
 * pipeline -- see SPEED.md), query (run the retrieval + generation
 * pipeline for a single question), or eval (score retrieval quality
 * against labeled queries, see eval.c), per spec section 5.1's
 * high-level data flow.
 *
 * Must be run from the project root -- data/stopwords, data/wordnet,
 * and the index file are all located via relative paths, same
 * convention the test suite already uses.
 */

#define _POSIX_C_SOURCE 200809L

#include "bm25.h"
#include "bulk_ingest.h"
#include "config.h"
#include "eval.h"
#include "generation.h"
#include "ingest.h"
#include "lemmatizer.h"
#include "local_llm_client.h"
#include "pg_store.h"
#include "query_formulation.h"
#include "query_log.h"
#include "retrieval.h"
#include "stopwords.h"
#include "wordnet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Postgres connection string -- points at the native Homebrew
 * postgresql@18 install (port 5434), the same instance the test suite's
 * TEST_CONNINFO now uses too (database lexis_test, vs. this database
 * lexis). Originally chosen over a Docker dev instance because Docker
 * Desktop on macOS runs everything inside a lightweight Linux VM, so
 * even "localhost" traffic to the containerized Postgres crosses that
 * VM boundary before reaching it -- real per-round-trip latency a
 * native install doesn't pay; the Docker instance was later removed
 * entirely once the test suite was verified passing against native
 * Postgres too (see CURRENT_STATE.md/SPEED.md). `make pg-start`/`make
 * pg-stop` manage this instance (see Makefile) -- it does not
 * auto-start on login. Separate from, and deliberately never touches,
 * this machine's pre-existing postgresql@14 instance on the default
 * port 5432 (unrelated projects' real data). */
#define LEXIS_DB_CONNINFO "host=127.0.0.1 port=5434 dbname=lexis user=lexis password=lexis_dev_only"
/* Display-only label -- never print LEXIS_DB_CONNINFO itself, it embeds
 * the dev password. */
#define LEXIS_DB_LABEL "127.0.0.1:5434/lexis (native)"
#define LEXIS_STOPWORDS_PATH "data/stopwords/english.txt"
#define LEXIS_WORDNET_DIR "data/wordnet"
#define LEXIS_CONFIG_PATH "config/lexis.conf"
/* The local GGUF model path now comes from config/lexis.conf's
 * `model_path` (config_load_model_path(), falling back to
 * LEXIS_DEFAULT_MODEL_PATH) -- see config.h for the full history of how
 * the model itself was chosen. Loaded once per process (see
 * local_llm_client.c) and reused for query formulation and generation;
 * tool routing is app-only (app/src/QueryWorker.cpp) but shares the
 * same model/path since only one model is loaded process-wide. */
#define LEXIS_CHUNK_SIZE 200
#define LEXIS_CHUNK_OVERLAP 40
/* Thread count for bulk_ingest.c's Phase 2 worker pool. 6 measured at
 * 3490.9 passages/sec on a real 200K-row slice against native Postgres
 * (see SPEED.md's three-phase-redesign section) -- not an exhaustive
 * sweep of this specific pipeline, just the count carried over from
 * earlier thread-count experiments on this 8 physical/logical-core
 * machine. Not auto-detected from core count yet; see LIMITATIONS.md. */
#define LEXIS_INGEST_THREADS 6

static long elapsed_ms(struct timespec start, struct timespec end) {
    long seconds = end.tv_sec - start.tv_sec;
    long nanoseconds = end.tv_nsec - start.tv_nsec;
    return seconds * 1000 + nanoseconds / 1000000;
}

static void print_usage(const char *program_name) {
    fprintf(stderr,
            "Usage:\n"
            "  %s bulk-ingest <tsv_path>                Build/rebuild the index from a TSV of \"<id><TAB><text>\" rows\n"
            "  %s query \"<question>\"                   Ask a question against the current index\n"
            "  %s eval <queries_tsv> <qrels_tsv> [--no-llm-expansion]\n"
            "                                            Score retrieval quality (MRR@10/Recall@K) against labeled queries.\n"
            "                                            --no-llm-expansion skips WordNet+LLM query expansion entirely,\n"
            "                                            scoring plain lemmatized query terms instead (no model load).\n"
            "\n"
            "Must be run from the project root.\n",
            program_name, program_name, program_name);
}

static int run_bulk_ingest(const char *tsv_path) {
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

    /* A quick connect-and-close up front: fails fast with one clear error
     * message (and creates the schema as a side effect of pg_store_open)
     * before spawning bulk_ingest_tsv()'s worker threads, rather than
     * having every worker independently discover the database is
     * unreachable. */
    PgStore *probe_store = pg_store_open(LEXIS_DB_CONNINFO);
    if (probe_store == NULL) {
        fprintf(stderr, "lexis: failed to open index at %s\n", LEXIS_DB_LABEL);
        stopword_set_free(stopwords);
        wordnet_table_free(wordnet);
        lemmatizer_free(lemmatizer);
        return 1;
    }
    pg_store_close(probe_store);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    long passages = bulk_ingest_tsv(LEXIS_DB_CONNINFO, NULL, stopwords, wordnet, lemmatizer, tsv_path,
                                     LEXIS_CHUNK_SIZE, LEXIS_CHUNK_OVERLAP, LEXIS_INGEST_THREADS);
    clock_gettime(CLOCK_MONOTONIC, &end);

    int exit_code = 0;
    if (passages < 0) {
        fprintf(stderr, "lexis: failed to bulk-ingest %s\n", tsv_path);
        exit_code = 1;
    } else {
        long ms = elapsed_ms(start, end);
        printf("Ingested %ld passages from %s into %s in %ldms (%d threads, %.1f passages/sec)\n",
               passages, tsv_path, LEXIS_DB_LABEL, ms, LEXIS_INGEST_THREADS,
               ms > 0 ? (double)passages / ((double)ms / 1000.0) : 0.0);
    }

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

    char *model_path = config_load_model_path(LEXIS_CONFIG_PATH);
    if (model_path == NULL || local_llm_client_init(model_path) != 0) {
        fprintf(stderr, "lexis: failed to load local model from %s\n",
                model_path != NULL ? model_path : "(out of memory)");
        free(model_path);
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

    /* The whole retrieval pipeline -- terms, sense-filtered expansion,
     * weighted+coordinated BM25, trim -- is ONE shared call
     * (src/core/retrieval.c), the same one the app's QueryWorker and
     * eval.c run. This function only owns what is CLI-specific:
     * printing, query_log observability (read from the run's artifacts,
     * not re-derived), and single-turn generation. */
    RetrievalPolicy policy = retrieval_default_policy();
    RetrievalRun *run =
        retrieval_run(store, question, NULL, stopwords, wordnet, lemmatizer, &policy);
    if (run == NULL) {
        fprintf(stderr, "lexis: retrieval failed\n");
        exit_code = 1;
        goto cleanup;
    }

    if (query_id != -1) {
        char *selected_terms_str = ingest_join_words(run->terms, 0, run->terms->count);
        query_log_insert_query_formulation_run(
            store, query_id, (int)run->original_count, run->expansion_prompt,
            run->expansion_response, run->used_fallback,
            selected_terms_str != NULL ? selected_terms_str : "", run->formulation_ms);
        free(selected_terms_str);
    }

    printf("Search terms: ");
    for (size_t i = 0; i < run->terms->count; i++) {
        printf("%s%s", run->terms->terms[i], (i + 1 < run->terms->count) ? ", " : "");
    }
    printf("\n\n");

    if (run->terms->count == 0) {
        printf("Nothing to search for -- the question was entirely stopwords.\n");
        retrieval_run_free(run);
        goto cleanup;
    }

    {
        BM25ResultSet *results = run->results;

        if (query_id != -1) {
            int64_t search_run_id = query_log_insert_search_run(
                store, query_id, LEXIS_SEARCH_MAX_PASSAGES, (int)results->count, run->search_ms);
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
            retrieval_run_free(run);
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
        if (mode == LEXIS_MODE_TESTING && gen_prompt != NULL) {
            printf("--- Generation prompt ---\n%s\n--- End generation prompt ---\n\n", gen_prompt);
        }
        /* Zero history turns: the history-aware generator degrades to
         * exactly the single-turn behavior (see generation.h) -- one
         * generation entry point for the CLI and the app alike. */
        char *answer = generation_generate_answer_with_history(question, store, results, NULL, 0);
        clock_gettime(CLOCK_MONOTONIC, &gen_end);
        retrieval_run_free(run);

        if (query_id != -1) {
            query_log_insert_generation_run(store, query_id, model_path, passages_included,
                                             passages_skipped, gen_prompt, answer, answer != NULL,
                                             elapsed_ms(gen_start, gen_end));
        }
        free(gen_prompt);

        if (answer == NULL) {
            fprintf(stderr, "lexis: could not generate an answer -- local model generation failed\n");
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
    local_llm_client_cleanup();
    free(model_path);
    pg_store_close(store);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    return exit_code;
}

static int run_eval(const char *queries_path, const char *qrels_path, int use_llm_expansion) {
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

    /* eval_run() only exercises query formulation (WordNet expansion +
     * local-model term selection), never generation_generate_answer() --
     * MRR@10/Recall@K don't depend on what the large model says about
     * the results. With use_llm_expansion, still needs the local model
     * loaded exactly once here, up front, for the same reason main.c's
     * other modes do: a fresh process-per-query would pay the ~9-19s
     * model-load cost thousands of times over (see LIMITATIONS.md).
     * Without it, query_formulation_terms_only() never calls the model
     * at all -- skip paying that load cost for nothing. */
    if (use_llm_expansion) {
        char *model_path = config_load_model_path(LEXIS_CONFIG_PATH);
        if (model_path == NULL || local_llm_client_init(model_path) != 0) {
            fprintf(stderr, "lexis: failed to load local model from %s\n",
                    model_path != NULL ? model_path : "(out of memory)");
            free(model_path);
            pg_store_close(store);
            stopword_set_free(stopwords);
            wordnet_table_free(wordnet);
            lemmatizer_free(lemmatizer);
            return 1;
        }
        free(model_path); /* only needed for init + the error message */
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    EvalMetrics metrics =
        eval_run(store, stopwords, wordnet, lemmatizer, queries_path, qrels_path, use_llm_expansion);
    clock_gettime(CLOCK_MONOTONIC, &end);

    int exit_code = 0;
    if (metrics.queries_evaluated < 0) {
        fprintf(stderr, "lexis: eval failed\n");
        exit_code = 1;
    } else {
        long ms = elapsed_ms(start, end);
        printf("\n=== Eval complete ===\n");
        printf("Queries evaluated: %ld (skipped %ld with no qrels judgments)\n",
               metrics.queries_evaluated, metrics.queries_skipped);
        printf("MRR@10:      %.4f\n", metrics.mrr_at_10);
        printf("Recall@10:   %.4f\n", metrics.recall_at_10);
        printf("Recall@100:  %.4f\n", metrics.recall_at_100);
        printf("Total time:  %.1f minutes\n", (double)ms / 60000.0);
    }

    local_llm_client_cleanup();
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

    if (strcmp(argv[1], "bulk-ingest") == 0) {
        return run_bulk_ingest(argv[2]);
    }
    if (strcmp(argv[1], "query") == 0) {
        return run_query(argv[2]);
    }
    if (strcmp(argv[1], "eval") == 0) {
        if (argc < 4) {
            print_usage(argv[0]);
            return 1;
        }
        int use_llm_expansion = 1;
        if (argc >= 5) {
            if (strcmp(argv[4], "--no-llm-expansion") != 0) {
                print_usage(argv[0]);
                return 1;
            }
            use_llm_expansion = 0;
        }
        return run_eval(argv[2], argv[3], use_llm_expansion);
    }

    print_usage(argv[0]);
    return 1;
}
