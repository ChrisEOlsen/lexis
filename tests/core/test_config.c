/*
 * Tests for src/core/config.c — the testing/production mode reader.
 * Uses throwaway config files under build/, same pattern as
 * test_pg_store.c's throwaway-data setup.
 */

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
    remove(TEST_CONFIG_PATH);
    return test_summary();
}
