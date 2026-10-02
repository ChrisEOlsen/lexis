/* CLI entrypoint: bulk-ingest/query/eval (spec 5.1). Must run from project root (relative data paths). */

#define _POSIX_C_SOURCE 200809L

#include "bm25.h"
#include "bulk_ingest.h"
#include "config.h"
#include "eval.h"
#include "generation.h"
#include "ingest.h"
#include "lemmatizer.h"
#include "local_llm_client.h"
#include "paths.h"
#include "pg_store.h"
#include "query_log.h"
#include "retrieval.h"
#include "stopwords.h"
#include "time_util.h"
#include "wordnet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Native Homebrew postgresql@18 on :5434 (see Makefile pg-start/pg-stop); distinct from :5432 postgres@14. */
/* Conninfo from config db_conninfo (embeds password, untracked); no hardcoded fallback. */
static const char *g_db_conninfo = NULL;
/* Display label only; never print conninfo (embeds password). */
#define LEXIS_DB_LABEL "127.0.0.1:5434/lexis (native)"
#define LEXIS_STOPWORDS_PATH "data/stopwords/english.txt"
#define LEXIS_WORDNET_DIR "data/wordnet"
/* GGUF path from config model_path (see config.h); loaded once per process, shared by all LLM uses. */
/* Chunking/worker tunables from config chunk_size/chunk_overlap/ingest_threads (see config.h).
 * 6 threads measured ~3490 passages/sec (see dev/SPEED.md); not auto-detected yet (dev/LIMITATIONS.md). */

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

    /* Probe connect first: fail fast before spawning workers, not once per worker. */
    PgStore *probe_store = pg_store_open(g_db_conninfo);
    if (probe_store == NULL) {
        fprintf(stderr, "lexis: failed to open index at %s\n", LEXIS_DB_LABEL);
        stopword_set_free(stopwords);
        wordnet_table_free(wordnet);
        lemmatizer_free(lemmatizer);
        return 1;
    }
    pg_store_close(probe_store);

    struct timespec start, end;
    /* Config first, tuning-sweep env wins (see docs/configuration.md). Huge chunk size = one passage per doc. */
    size_t chunk_size = config_load_chunk_size(lexis_paths_config_file());
    size_t chunk_overlap = config_load_chunk_overlap(lexis_paths_config_file());
    int ingest_threads = config_load_ingest_threads(lexis_paths_config_file());
    const char *chunk_env = getenv("LEXIS_CHUNK_SIZE");
    const char *overlap_env = getenv("LEXIS_CHUNK_OVERLAP");
    if (chunk_env != NULL && atol(chunk_env) > 0) {
        chunk_size = (size_t)atol(chunk_env);
    }
    if (overlap_env != NULL && atol(overlap_env) >= 0) {
        chunk_overlap = (size_t)atol(overlap_env);
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    long passages = bulk_ingest_tsv(g_db_conninfo, NULL, stopwords, wordnet, lemmatizer, tsv_path,
                                     chunk_size, chunk_overlap, ingest_threads);
    clock_gettime(CLOCK_MONOTONIC, &end);

    int exit_code = 0;
    if (passages < 0) {
        fprintf(stderr, "lexis: failed to bulk-ingest %s\n", tsv_path);
        exit_code = 1;
    } else {
        long ms = lexis_elapsed_ms(start, end);
        printf("Ingested %ld passages from %s into %s in %ldms (%d threads, %.1f passages/sec)\n",
               passages, tsv_path, LEXIS_DB_LABEL, ms, ingest_threads,
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

    PgStore *store = pg_store_open(g_db_conninfo);
    if (store == NULL) {
        fprintf(stderr, "lexis: failed to open index at %s\n", LEXIS_DB_LABEL);
        stopword_set_free(stopwords);
        wordnet_table_free(wordnet);
        lemmatizer_free(lemmatizer);
        return 1;
    }

    /* Logging only in testing mode (query_id -1 skips it); spares production the ~2.5ms overhead. */
    LexisMode mode = config_load_mode(lexis_paths_config_file());
    if (mode == LEXIS_MODE_TESTING && query_log_init_schema(store) != 0) {
        fprintf(stderr, "lexis: warning: pipeline logging unavailable, continuing without it\n");
    }

    char *model_path = config_load_model_path(lexis_paths_config_file());
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

    /* One shared retrieval_run() call (same as app/eval); here owns only printing, logging, generation. */
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
                store, query_id, (int)policy.max_passages, (int)results->count, run->search_ms);
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
        /* Zero turns = single-turn behavior; one generation entry point for CLI and app. */
        char *answer = generation_generate_answer_with_history(question, store, results, NULL, 0, -1);
        clock_gettime(CLOCK_MONOTONIC, &gen_end);
        retrieval_run_free(run);

        if (query_id != -1) {
            query_log_insert_generation_run(store, query_id, model_path, passages_included,
                                             passages_skipped, gen_prompt, answer, answer != NULL,
                                             lexis_elapsed_ms(gen_start, gen_end));
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
        query_log_finish_query(store, query_id, lexis_elapsed_ms(pipeline_start, pipeline_end),
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

    PgStore *store = pg_store_open(g_db_conninfo);
    if (store == NULL) {
        fprintf(stderr, "lexis: failed to open index at %s\n", LEXIS_DB_LABEL);
        stopword_set_free(stopwords);
        wordnet_table_free(wordnet);
        lemmatizer_free(lemmatizer);
        return 1;
    }

    /* Eval scores formulation only, never generation; loads model once up front iff expansion is on. */
    if (use_llm_expansion) {
        char *model_path = config_load_model_path(lexis_paths_config_file());
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
        long ms = lexis_elapsed_ms(start, end);
        printf("\n=== Eval complete ===\n");
        printf("Queries evaluated: %ld (skipped %ld with no qrels judgments)\n",
               metrics.queries_evaluated, metrics.queries_skipped);
        printf("MRR@10:      %.4f\n", metrics.mrr_at_10);
        printf("nDCG@10:     %.4f\n", metrics.ndcg_at_10);
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

    g_db_conninfo = config_load_db_conninfo(lexis_paths_config_file());
    if (g_db_conninfo == NULL) {
        fprintf(stderr,
                "lexis: no database connection configured -- set db_conninfo in %s\n"
                "(copy config/lexis.conf.example and fill in your password)\n",
                lexis_paths_config_file());
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
