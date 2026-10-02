/* Tests for config.c using throwaway files under build/. */

#include "bm25.h"
#include "config.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CONFIG_PATH "build/test_config.conf"

static void write_config(const char *contents) {
    FILE *fp = fopen(TEST_CONFIG_PATH, "wb");
    fwrite(contents, 1, strlen(contents), fp);
    fclose(fp);
}

static void test_missing_file_defaults_to_testing(void) {
    remove(TEST_CONFIG_PATH);
    LexisMode mode = config_load_mode(TEST_CONFIG_PATH);
    TEST_ASSERT(mode == LEXIS_MODE_TESTING,
                "expected a missing config file to quietly default to testing mode");
}

static void test_explicit_testing_mode(void) {
    write_config("mode=testing\n");
    LexisMode mode = config_load_mode(TEST_CONFIG_PATH);
    TEST_ASSERT(mode == LEXIS_MODE_TESTING, "expected explicit mode=testing to be honored");
}

static void test_explicit_production_mode(void) {
    write_config("mode=production\n");
    LexisMode mode = config_load_mode(TEST_CONFIG_PATH);
    TEST_ASSERT(mode == LEXIS_MODE_PRODUCTION, "expected explicit mode=production to be honored");
}

static void test_tolerates_whitespace_and_comments(void) {
    write_config("# a comment line\n\n  mode = production  \n# trailing comment\n");
    LexisMode mode = config_load_mode(TEST_CONFIG_PATH);
    TEST_ASSERT(mode == LEXIS_MODE_PRODUCTION,
                "expected whitespace/comments around the mode line to be tolerated");
}

static void test_unrecognized_value_defaults_to_testing(void) {
    write_config("mode=bogus\n");
    LexisMode mode = config_load_mode(TEST_CONFIG_PATH);
    TEST_ASSERT(mode == LEXIS_MODE_TESTING,
                "expected an unrecognized mode value to default safely to testing");
}

static void test_no_mode_line_defaults_to_testing(void) {
    write_config("# just a comment, no mode line at all\n");
    LexisMode mode = config_load_mode(TEST_CONFIG_PATH);
    TEST_ASSERT(mode == LEXIS_MODE_TESTING,
                "expected a config file with no mode line to default to testing");
}

static void test_missing_file_defaults_model_path(void) {
    remove(TEST_CONFIG_PATH);
    char *path = config_load_model_path(TEST_CONFIG_PATH);
    TEST_ASSERT(path != NULL && strcmp(path, LEXIS_DEFAULT_MODEL_PATH) == 0,
                "expected a missing config file to fall back to LEXIS_DEFAULT_MODEL_PATH");
    free(path);
}

static void test_explicit_model_path(void) {
    write_config("mode=testing\nmodel_path=data/models/other-model-Q4_K_M.gguf\n");
    char *path = config_load_model_path(TEST_CONFIG_PATH);
    TEST_ASSERT(path != NULL && strcmp(path, "data/models/other-model-Q4_K_M.gguf") == 0,
                "expected an explicit model_path to be honored");
    free(path);
}

static void test_model_path_tolerates_whitespace_and_comments(void) {
    write_config("# comment\n  model_path =  data/models/spaced.gguf  \n");
    char *path = config_load_model_path(TEST_CONFIG_PATH);
    TEST_ASSERT(path != NULL && strcmp(path, "data/models/spaced.gguf") == 0,
                "expected whitespace/comments around the model_path line to be tolerated");
    free(path);
}

static void test_empty_model_path_defaults(void) {
    write_config("model_path=\n");
    char *path = config_load_model_path(TEST_CONFIG_PATH);
    TEST_ASSERT(path != NULL && strcmp(path, LEXIS_DEFAULT_MODEL_PATH) == 0,
                "expected an empty model_path value to fall back to the default");
    free(path);
}

static void test_no_model_path_line_defaults(void) {
    write_config("mode=production\n");
    char *path = config_load_model_path(TEST_CONFIG_PATH);
    TEST_ASSERT(path != NULL && strcmp(path, LEXIS_DEFAULT_MODEL_PATH) == 0,
                "expected a config file with no model_path line to fall back to the default");
    free(path);
}

static void test_last_model_path_line_wins(void) {
    write_config("model_path=data/models/first.gguf\nmodel_path=data/models/second.gguf\n");
    char *path = config_load_model_path(TEST_CONFIG_PATH);
    TEST_ASSERT(path != NULL && strcmp(path, "data/models/second.gguf") == 0,
                "expected the last model_path line to win, matching the mode parser");
    free(path);
}

static void test_thinking_defaults_on(void) {
    remove(TEST_CONFIG_PATH);
    TEST_ASSERT(config_load_thinking(TEST_CONFIG_PATH) == 1,
                "expected a missing config file to default thinking on");
    write_config("mode=testing\n");
    TEST_ASSERT(config_load_thinking(TEST_CONFIG_PATH) == 1,
                "expected a config with no thinking line to default on");
    write_config("thinking=bogus\n");
    TEST_ASSERT(config_load_thinking(TEST_CONFIG_PATH) == 1,
                "expected an unrecognized thinking value to default safely on");
}

static void test_thinking_off_honored(void) {
    write_config("thinking=off\n");
    TEST_ASSERT(config_load_thinking(TEST_CONFIG_PATH) == 0,
                "expected explicit thinking=off to be honored");
    write_config("  thinking = off  \n");
    TEST_ASSERT(config_load_thinking(TEST_CONFIG_PATH) == 0,
                "expected whitespace around thinking=off to be tolerated");
}

static void test_missing_file_defaults_new_keys(void) {
    remove(TEST_CONFIG_PATH);
    TEST_ASSERT(config_load_chunk_size(TEST_CONFIG_PATH) == LEXIS_DEFAULT_CHUNK_SIZE,
                "expected a missing config file to default chunk_size");
    TEST_ASSERT(config_load_chunk_overlap(TEST_CONFIG_PATH) == LEXIS_DEFAULT_CHUNK_OVERLAP,
                "expected a missing config file to default chunk_overlap");
    TEST_ASSERT(config_load_ingest_threads(TEST_CONFIG_PATH) == LEXIS_DEFAULT_INGEST_THREADS,
                "expected a missing config file to default ingest_threads");
    TEST_ASSERT(config_load_candidate_ceiling(TEST_CONFIG_PATH) == LEXIS_SEARCH_CANDIDATE_CEILING,
                "expected a missing config file to default candidate_ceiling");
    TEST_ASSERT(config_load_max_passages(TEST_CONFIG_PATH) == LEXIS_SEARCH_MAX_PASSAGES,
                "expected a missing config file to default max_passages");
    TEST_ASSERT(config_load_token_budget(TEST_CONFIG_PATH) == LEXIS_SEARCH_TOKEN_BUDGET,
                "expected a missing config file to default token_budget");
    TEST_ASSERT(config_load_score_floor_ratio(TEST_CONFIG_PATH) == LEXIS_SEARCH_SCORE_FLOOR_RATIO,
                "expected a missing config file to default score_floor_ratio");
    TEST_ASSERT(config_load_bm25_k1(TEST_CONFIG_PATH) == BM25_DEFAULT_K1,
                "expected a missing config file to default bm25_k1");
    TEST_ASSERT(config_load_bm25_b(TEST_CONFIG_PATH) == BM25_DEFAULT_B,
                "expected a missing config file to default bm25_b");
}

static void test_unset_keys_default(void) {
    write_config("mode=testing\n");
    TEST_ASSERT(config_load_chunk_size(TEST_CONFIG_PATH) == LEXIS_DEFAULT_CHUNK_SIZE,
                "expected a config with no chunk_size line to default");
    TEST_ASSERT(config_load_ingest_threads(TEST_CONFIG_PATH) == LEXIS_DEFAULT_INGEST_THREADS,
                "expected a config with no ingest_threads line to default");
    TEST_ASSERT(config_load_max_passages(TEST_CONFIG_PATH) == LEXIS_SEARCH_MAX_PASSAGES,
                "expected a config with no max_passages line to default");
    TEST_ASSERT(config_load_token_budget(TEST_CONFIG_PATH) == LEXIS_SEARCH_TOKEN_BUDGET,
                "expected a config with no token_budget line to default");
    TEST_ASSERT(config_load_bm25_k1(TEST_CONFIG_PATH) == BM25_DEFAULT_K1,
                "expected a config with no bm25_k1 line to default");
}

static void test_explicit_new_keys_honored(void) {
    write_config("chunk_size=500\nchunk_overlap=100\ningest_threads=12\n"
                 "candidate_ceiling=80\nmax_passages=20\ntoken_budget=3000\n"
                 "score_floor_ratio=0.5\nbm25_k1=2.0\nbm25_b=0.9\n");
    TEST_ASSERT(config_load_chunk_size(TEST_CONFIG_PATH) == 500,
                "expected explicit chunk_size to be honored");
    TEST_ASSERT(config_load_chunk_overlap(TEST_CONFIG_PATH) == 100,
                "expected explicit chunk_overlap to be honored");
    TEST_ASSERT(config_load_ingest_threads(TEST_CONFIG_PATH) == 12,
                "expected explicit ingest_threads to be honored");
    TEST_ASSERT(config_load_candidate_ceiling(TEST_CONFIG_PATH) == 80,
                "expected explicit candidate_ceiling to be honored");
    TEST_ASSERT(config_load_max_passages(TEST_CONFIG_PATH) == 20,
                "expected explicit max_passages to be honored");
    TEST_ASSERT(config_load_token_budget(TEST_CONFIG_PATH) == 3000,
                "expected explicit token_budget to be honored");
    TEST_ASSERT(config_load_score_floor_ratio(TEST_CONFIG_PATH) == 0.5,
                "expected explicit score_floor_ratio to be honored");
    TEST_ASSERT(config_load_bm25_k1(TEST_CONFIG_PATH) == 2.0,
                "expected explicit bm25_k1 to be honored");
    TEST_ASSERT(config_load_bm25_b(TEST_CONFIG_PATH) == 0.9,
                "expected explicit bm25_b to be honored");
}

static void test_zero_is_honored_where_it_disables(void) {
    write_config("chunk_overlap=0\nscore_floor_ratio=0.0\nbm25_b=0\n");
    TEST_ASSERT(config_load_chunk_overlap(TEST_CONFIG_PATH) == 0,
                "expected chunk_overlap=0 (no overlap) to be honored");
    TEST_ASSERT(config_load_score_floor_ratio(TEST_CONFIG_PATH) == 0.0,
                "expected score_floor_ratio=0 (floor off) to be honored");
    TEST_ASSERT(config_load_bm25_b(TEST_CONFIG_PATH) == 0.0,
                "expected bm25_b=0 (no length norm) to be honored");
}

static void test_invalid_new_keys_default(void) {
    write_config("chunk_size=0\nchunk_overlap=-5\ningest_threads=0\n"
                 "candidate_ceiling=-1\nmax_passages=0\ntoken_budget=bogus\n"
                 "score_floor_ratio=-0.1\nbm25_k1=0\nbm25_b=bogus\n");
    TEST_ASSERT(config_load_chunk_size(TEST_CONFIG_PATH) == LEXIS_DEFAULT_CHUNK_SIZE,
                "expected chunk_size=0 to fall back to the default");
    TEST_ASSERT(config_load_chunk_overlap(TEST_CONFIG_PATH) == LEXIS_DEFAULT_CHUNK_OVERLAP,
                "expected a negative chunk_overlap to fall back to the default");
    TEST_ASSERT(config_load_ingest_threads(TEST_CONFIG_PATH) == LEXIS_DEFAULT_INGEST_THREADS,
                "expected ingest_threads=0 to fall back to the default");
    TEST_ASSERT(config_load_candidate_ceiling(TEST_CONFIG_PATH) == LEXIS_SEARCH_CANDIDATE_CEILING,
                "expected a negative candidate_ceiling to fall back to the default");
    TEST_ASSERT(config_load_max_passages(TEST_CONFIG_PATH) == LEXIS_SEARCH_MAX_PASSAGES,
                "expected max_passages=0 to fall back to the default");
    TEST_ASSERT(config_load_token_budget(TEST_CONFIG_PATH) == LEXIS_SEARCH_TOKEN_BUDGET,
                "expected a garbage token_budget to fall back to the default");
    TEST_ASSERT(config_load_score_floor_ratio(TEST_CONFIG_PATH) == LEXIS_SEARCH_SCORE_FLOOR_RATIO,
                "expected a negative score_floor_ratio to fall back to the default");
    TEST_ASSERT(config_load_bm25_k1(TEST_CONFIG_PATH) == BM25_DEFAULT_K1,
                "expected bm25_k1=0 to fall back to the default");
    TEST_ASSERT(config_load_bm25_b(TEST_CONFIG_PATH) == BM25_DEFAULT_B,
                "expected a garbage bm25_b to fall back to the default");
}

static void test_partial_number_rejected(void) {
    write_config("chunk_size=200x\ntoken_budget=15x00\n");
    TEST_ASSERT(config_load_chunk_size(TEST_CONFIG_PATH) == LEXIS_DEFAULT_CHUNK_SIZE,
                "expected chunk_size=200x to fall back to the default");
    TEST_ASSERT(config_load_token_budget(TEST_CONFIG_PATH) == LEXIS_SEARCH_TOKEN_BUDGET,
                "expected token_budget=15x00 to fall back to the default");
}

int main(void) {
    test_missing_file_defaults_to_testing();
    test_explicit_testing_mode();
    test_explicit_production_mode();
    test_tolerates_whitespace_and_comments();
    test_unrecognized_value_defaults_to_testing();
    test_no_mode_line_defaults_to_testing();
    test_missing_file_defaults_model_path();
    test_explicit_model_path();
    test_model_path_tolerates_whitespace_and_comments();
    test_empty_model_path_defaults();
    test_no_model_path_line_defaults();
    test_last_model_path_line_wins();
    test_thinking_defaults_on();
    test_thinking_off_honored();
    test_missing_file_defaults_new_keys();
    test_unset_keys_default();
    test_explicit_new_keys_honored();
    test_zero_is_honored_where_it_disables();
    test_invalid_new_keys_default();
    test_partial_number_rejected();
    remove(TEST_CONFIG_PATH);
    return test_summary();
}
