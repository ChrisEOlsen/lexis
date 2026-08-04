/*
 * Tests for src/core/csv_parse.c — strict RFC4180 CSV parsing. Pure
 * in-memory logic, no database needed.
 */

#include "csv_parse.h"
#include "test_utils.h"

#include <stdio.h>
#include <string.h>

#define TEST_FILE_PATH "build/test_csv_parse_file.csv"

static void write_test_file(const char *contents) {
    FILE *fp = fopen(TEST_FILE_PATH, "wb");
    fwrite(contents, 1, strlen(contents), fp);
    fclose(fp);
}

static void test_simple_rows_joined_with_spaces(void) {
    write_test_file("name,email,note\n"
                     "alice,alice@example.com,first row\n"
                     "bob,bob@example.com,second row\n");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows != NULL, "expected csv_parse_file to succeed");
    TEST_ASSERT(rows->count == 2, "expected 2 data rows, got %zu", rows->count);
    TEST_ASSERT_STR_EQ(rows->terms[0], "alice alice@example.com first row");
    TEST_ASSERT_STR_EQ(rows->terms[1], "bob bob@example.com second row");

    token_list_free(rows);
}

static void test_quoted_field_with_embedded_comma(void) {
    write_test_file("name,note\n"
                     "alice,\"hello, world\"\n");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows != NULL, "expected csv_parse_file to succeed");
    TEST_ASSERT(rows->count == 1, "expected 1 data row, got %zu", rows->count);
    TEST_ASSERT_STR_EQ(rows->terms[0], "alice hello, world");

    token_list_free(rows);
}

static void test_quoted_field_with_embedded_newline(void) {
    write_test_file("name,note\n"
                     "alice,\"line one\nline two\"\n");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows != NULL, "expected csv_parse_file to succeed");
    TEST_ASSERT(rows->count == 1, "expected 1 data row (the embedded newline must not split it), got %zu",
                rows->count);
    TEST_ASSERT_STR_EQ(rows->terms[0], "alice line one\nline two");

    token_list_free(rows);
}

static void test_escaped_quote_doubling(void) {
    write_test_file("name,note\n"
                     "alice,\"she said \"\"hi\"\" to me\"\n");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows != NULL, "expected csv_parse_file to succeed");
    TEST_ASSERT(rows->count == 1, "expected 1 data row, got %zu", rows->count);
    TEST_ASSERT_STR_EQ(rows->terms[0], "alice she said \"hi\" to me");

    token_list_free(rows);
}

static void test_empty_fields_between_commas(void) {
    write_test_file("a,b,c\n"
                     "1,,3\n");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows != NULL, "expected csv_parse_file to succeed");
    TEST_ASSERT(rows->count == 1, "expected 1 data row, got %zu", rows->count);
    TEST_ASSERT_STR_EQ(rows->terms[0], "1  3");

    token_list_free(rows);
}

static void test_header_only_returns_empty_result(void) {
    write_test_file("a,b,c\n");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows != NULL, "expected a header-only file to succeed (empty result), not fail");
    TEST_ASSERT(rows->count == 0, "expected 0 data rows, got %zu", rows->count);

    token_list_free(rows);
}

static void test_completely_empty_file_fails(void) {
    write_test_file("");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows == NULL, "expected a completely empty file (no header at all) to fail");
}

static void test_missing_file_fails(void) {
    TokenList *rows = csv_parse_file("build/does_not_exist.csv");
    TEST_ASSERT(rows == NULL, "expected a missing file to fail");
}

static void test_field_count_mismatch_fails_whole_file(void) {
    write_test_file("a,b,c\n"
                     "1,2,3\n"
                     "4,5\n" /* one field short */
                     "7,8,9\n");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows == NULL,
                "expected a field-count mismatch on one row to fail the WHOLE file, not skip that row");
}

static void test_unterminated_quote_fails(void) {
    write_test_file("a,b\n"
                     "1,\"never closed\n");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows == NULL, "expected an unterminated quoted field to fail");
}

static void test_stray_quote_mid_unquoted_field_fails(void) {
    write_test_file("a,b\n"
                     "abc\"def,2\n");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows == NULL, "expected a stray quote in the middle of an unquoted field to fail");
}

static void test_junk_immediately_after_closing_quote_fails(void) {
    write_test_file("a,b\n"
                     "\"abc\"def,2\n");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows == NULL,
                "expected text immediately after a quoted field's closing quote (no delimiter) to fail");
}

static void test_trailing_newline_produces_no_extra_row(void) {
    write_test_file("a,b\n"
                     "1,2\n"
                     "\n");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows != NULL, "expected csv_parse_file to succeed");
    TEST_ASSERT(rows->count == 1, "expected the trailing blank line to produce no extra row, got %zu", rows->count);
    TEST_ASSERT_STR_EQ(rows->terms[0], "1 2");

    token_list_free(rows);
}

static void test_last_row_without_trailing_newline_is_included(void) {
    write_test_file("a,b\n"
                     "1,2"); /* no trailing newline */

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows != NULL, "expected csv_parse_file to succeed");
    TEST_ASSERT(rows->count == 1, "expected the last, unterminated row to still be included, got %zu", rows->count);
    TEST_ASSERT_STR_EQ(rows->terms[0], "1 2");

    token_list_free(rows);
}

static void test_crlf_line_endings(void) {
    write_test_file("a,b\r\n"
                     "1,2\r\n"
                     "3,4\r\n");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows != NULL, "expected csv_parse_file to succeed on CRLF line endings");
    TEST_ASSERT(rows->count == 2, "expected 2 data rows, got %zu", rows->count);
    TEST_ASSERT_STR_EQ(rows->terms[0], "1 2");
    TEST_ASSERT_STR_EQ(rows->terms[1], "3 4");

    token_list_free(rows);
}

static void test_quoted_field_starting_with_a_digit_matches_unquoted(void) {
    /* Real-world case: a numeric-looking column sometimes gets quoted by
     * spreadsheet exporters even without needing to be -- must parse
     * identically to an unquoted equivalent. */
    write_test_file("id,name\n"
                     "\"123\",alice\n");

    TokenList *rows = csv_parse_file(TEST_FILE_PATH);
    TEST_ASSERT(rows != NULL, "expected csv_parse_file to succeed");
    TEST_ASSERT(rows->count == 1, "expected 1 data row, got %zu", rows->count);
    TEST_ASSERT_STR_EQ(rows->terms[0], "123 alice");

    token_list_free(rows);
}

int main(void) {
    test_simple_rows_joined_with_spaces();
    test_quoted_field_with_embedded_comma();
    test_quoted_field_with_embedded_newline();
    test_escaped_quote_doubling();
    test_empty_fields_between_commas();
    test_header_only_returns_empty_result();
    test_completely_empty_file_fails();
    test_missing_file_fails();
    test_field_count_mismatch_fails_whole_file();
    test_unterminated_quote_fails();
    test_stray_quote_mid_unquoted_field_fails();
    test_junk_immediately_after_closing_quote_fails();
    test_trailing_newline_produces_no_extra_row();
    test_last_row_without_trailing_newline_is_included();
    test_crlf_line_endings();
    test_quoted_field_starting_with_a_digit_matches_unquoted();
    remove(TEST_FILE_PATH);
    return test_summary();
}
