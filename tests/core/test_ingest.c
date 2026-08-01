/*
 * Tests for src/core/ingest.c — reading, splitting, chunking, and
 * lemmatizing a document's text, the primitives bulk_ingest.c's Phase 2
 * worker builds on. No database or WordNet-backed lemmatizer tests here
 * (ingest_lemmatize_terms/ingest_count_distinct_terms are exercised
 * indirectly via test_bulk_ingest.c's real end-to-end tests instead).
 */

#include "ingest.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_FILE_PATH "build/test_ingest_file.txt"

static void write_test_file(const char *contents) {
    FILE *fp = fopen(TEST_FILE_PATH, "wb");
    fwrite(contents, 1, strlen(contents), fp);
    fclose(fp);
}

static void test_read_file_returns_exact_contents(void) {
    write_test_file("hypertension treatment options\nsecond line here");

    char *contents = ingest_read_file(TEST_FILE_PATH);
    TEST_ASSERT(contents != NULL, "expected ingest_read_file to succeed");
    TEST_ASSERT_STR_EQ(contents, "hypertension treatment options\nsecond line here");

    free(contents);
}

static void test_read_file_handles_empty_file(void) {
    write_test_file("");

    char *contents = ingest_read_file(TEST_FILE_PATH);
    TEST_ASSERT(contents != NULL, "expected an empty file to still succeed, not fail");
    TEST_ASSERT_STR_EQ(contents, "");

    free(contents);
}

static void test_read_file_missing_file_returns_null(void) {
    remove(TEST_FILE_PATH);
    char *contents = ingest_read_file(TEST_FILE_PATH);
    TEST_ASSERT(contents == NULL, "expected a missing file to return NULL");
}

static void test_split_words_basic(void) {
    TokenList *words = ingest_split_words("hypertension treatment options");
    TEST_ASSERT(words != NULL, "expected ingest_split_words to succeed");
    TEST_ASSERT(words->count == 3, "expected 3 words, got %zu", words->count);
    TEST_ASSERT_STR_EQ(words->terms[0], "hypertension");
    TEST_ASSERT_STR_EQ(words->terms[1], "treatment");
    TEST_ASSERT_STR_EQ(words->terms[2], "options");

    token_list_free(words);
}

static void test_split_words_keeps_punctuation_attached(void) {
    /* Unlike tokenize(), this must NOT strip punctuation or lowercase --
     * that's the whole point of a separate raw word-splitter for chunking. */
    TokenList *words = ingest_split_words("Hypertension, treatment.");
    TEST_ASSERT(words != NULL, "expected ingest_split_words to succeed");
    TEST_ASSERT(words->count == 2, "expected 2 words, got %zu", words->count);
    TEST_ASSERT_STR_EQ(words->terms[0], "Hypertension,");
    TEST_ASSERT_STR_EQ(words->terms[1], "treatment.");

    token_list_free(words);
}

static void test_split_words_collapses_consecutive_whitespace(void) {
    TokenList *words = ingest_split_words("one   two\n\nthree\t\tfour");
    TEST_ASSERT(words != NULL, "expected ingest_split_words to succeed");
    TEST_ASSERT(words->count == 4, "expected 4 words, got %zu", words->count);
    TEST_ASSERT_STR_EQ(words->terms[0], "one");
    TEST_ASSERT_STR_EQ(words->terms[1], "two");
    TEST_ASSERT_STR_EQ(words->terms[2], "three");
    TEST_ASSERT_STR_EQ(words->terms[3], "four");

    token_list_free(words);
}

static void test_split_words_empty_string(void) {
    TokenList *words = ingest_split_words("");
    TEST_ASSERT(words != NULL, "expected ingest_split_words to succeed on an empty string");
    TEST_ASSERT(words->count == 0, "expected 0 words for an empty string, got %zu", words->count);

    token_list_free(words);
}

static void test_split_words_only_whitespace(void) {
    TokenList *words = ingest_split_words("   \t\n  ");
    TEST_ASSERT(words != NULL, "expected ingest_split_words to succeed on whitespace-only input");
    TEST_ASSERT(words->count == 0, "expected 0 words for whitespace-only input, got %zu", words->count);

    token_list_free(words);
}

static void test_join_words_full_range(void) {
    TokenList *words = ingest_split_words("a bb ccc");
    TEST_ASSERT(words != NULL, "expected ingest_split_words to succeed");

    char *joined = ingest_join_words(words, 0, words->count);
    TEST_ASSERT(joined != NULL, "expected ingest_join_words to succeed");
    TEST_ASSERT_STR_EQ(joined, "a bb ccc");

    free(joined);
    token_list_free(words);
}

static void test_join_words_sub_range(void) {
    TokenList *words = ingest_split_words("one two three four five");
    TEST_ASSERT(words != NULL, "expected ingest_split_words to succeed");

    char *joined = ingest_join_words(words, 1, 3);
    TEST_ASSERT(joined != NULL, "expected ingest_join_words to succeed");
    TEST_ASSERT_STR_EQ(joined, "two three");

    free(joined);
    token_list_free(words);
}

static void test_join_words_single_word(void) {
    TokenList *words = ingest_split_words("solo");
    TEST_ASSERT(words != NULL, "expected ingest_split_words to succeed");

    char *joined = ingest_join_words(words, 0, 1);
    TEST_ASSERT(joined != NULL, "expected ingest_join_words to succeed");
    TEST_ASSERT_STR_EQ(joined, "solo");

    free(joined);
    token_list_free(words);
}

static void test_join_words_empty_range(void) {
    TokenList *words = ingest_split_words("one two three");
    TEST_ASSERT(words != NULL, "expected ingest_split_words to succeed");

    /* start == end, mid-list -- not just the start==end==0 case. */
    char *joined = ingest_join_words(words, 1, 1);
    TEST_ASSERT(joined != NULL, "expected ingest_join_words to succeed on an empty range");
    TEST_ASSERT_STR_EQ(joined, "");

    free(joined);
    token_list_free(words);
}

static void test_chunk_words_overlapping_windows(void) {
    TokenList *words = ingest_split_words("one two three four five six seven eight nine ten");
    TEST_ASSERT(words != NULL, "expected ingest_split_words to succeed");

    /* chunk_size=5, overlap=2 -> step=3. 10 words should produce exactly 3
     * chunks, the last one naturally shorter -- no degenerate 4th chunk
     * covering only already-seen tail words. */
    TokenList *chunks = ingest_chunk_words(words, 5, 2);
    TEST_ASSERT(chunks != NULL, "expected ingest_chunk_words to succeed");
    TEST_ASSERT(chunks->count == 3, "expected 3 chunks, got %zu", chunks->count);
    TEST_ASSERT_STR_EQ(chunks->terms[0], "one two three four five");
    TEST_ASSERT_STR_EQ(chunks->terms[1], "four five six seven eight");
    TEST_ASSERT_STR_EQ(chunks->terms[2], "seven eight nine ten");

    token_list_free(chunks);
    token_list_free(words);
}

static void test_chunk_words_short_document_single_chunk(void) {
    TokenList *words = ingest_split_words("just a few words");
    TEST_ASSERT(words != NULL, "expected ingest_split_words to succeed");

    /* Only 4 words, chunk_size=5 -- should produce exactly 1 chunk
     * containing everything, not an empty or truncated result. */
    TokenList *chunks = ingest_chunk_words(words, 5, 2);
    TEST_ASSERT(chunks != NULL, "expected ingest_chunk_words to succeed");
    TEST_ASSERT(chunks->count == 1, "expected 1 chunk, got %zu", chunks->count);
    TEST_ASSERT_STR_EQ(chunks->terms[0], "just a few words");

    token_list_free(chunks);
    token_list_free(words);
}

static void test_chunk_words_empty_input_returns_empty_list(void) {
    TokenList *words = ingest_split_words("");
    TEST_ASSERT(words != NULL, "expected ingest_split_words to succeed");

    TokenList *chunks = ingest_chunk_words(words, 5, 2);
    TEST_ASSERT(chunks != NULL, "expected an empty document to produce an empty list, not NULL");
    TEST_ASSERT(chunks->count == 0, "expected 0 chunks, got %zu", chunks->count);

    token_list_free(chunks);
    token_list_free(words);
}

static void test_chunk_words_rejects_invalid_params(void) {
    TokenList *words = ingest_split_words("one two three");
    TEST_ASSERT(words != NULL, "expected ingest_split_words to succeed");

    TokenList *zero_chunk_size = ingest_chunk_words(words, 0, 0);
    TEST_ASSERT(zero_chunk_size == NULL, "expected chunk_size 0 to be rejected");

    TokenList *overlap_too_large = ingest_chunk_words(words, 3, 3);
    TEST_ASSERT(overlap_too_large == NULL, "expected overlap >= chunk_size to be rejected");

    token_list_free(words);
}

int main(void) {
    test_read_file_returns_exact_contents();
    test_read_file_handles_empty_file();
    test_read_file_missing_file_returns_null();
    test_split_words_basic();
    test_split_words_keeps_punctuation_attached();
    test_split_words_collapses_consecutive_whitespace();
    test_split_words_empty_string();
    test_split_words_only_whitespace();
    test_join_words_full_range();
    test_join_words_sub_range();
    test_join_words_single_word();
    test_join_words_empty_range();
    test_chunk_words_overlapping_windows();
    test_chunk_words_short_document_single_chunk();
    test_chunk_words_empty_input_returns_empty_list();
    test_chunk_words_rejects_invalid_params();
    remove(TEST_FILE_PATH);
    return test_summary();
}
