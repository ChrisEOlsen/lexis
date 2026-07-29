/*
 * Tests for src/core/config.c — the testing/production mode reader.
 * Uses throwaway config files under build/, same pattern as
 * test_pg_store.c's throwaway-data setup.
 */

#include "config.h"
#include "test_utils.h"

#include <stdio.h>
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

int main(void) {
    test_missing_file_defaults_to_testing();
    test_explicit_testing_mode();
    test_explicit_production_mode();
    test_tolerates_whitespace_and_comments();
    test_unrecognized_value_defaults_to_testing();
    test_no_mode_line_defaults_to_testing();
    remove(TEST_CONFIG_PATH);
    return test_summary();
}
