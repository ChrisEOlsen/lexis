/*
 * Tests for src/core/generation.c — assembling retrieved passages into a
 * grounded generation prompt (spec 5.2.7).
 */

/* See tokenizer.c for why this must come before any #include (unsetenv is
 * a POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "generation.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_DB_PATH "build/test_generation.db"

static SqliteStore *open_fresh_store(void) {
    remove(TEST_DB_PATH);
    return sqlite_store_open(TEST_DB_PATH);
}

static void test_build_prompt_includes_passages_and_question(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 p1 = sqlite_store_insert_passage(store, "doc1.txt", 2,
                                                    "Hypertension is treated with medication.", 5);
    sqlite3_int64 p2 = sqlite_store_insert_passage(store, "doc2.txt", 0,
                                                    "Lifestyle changes also help.", 4);

    BM25ResultSet *results = bm25_result_set_create();
    TEST_ASSERT(results != NULL, "expected bm25_result_set_create to succeed");
    bm25_result_set_add(results, p1, 5.0);
    bm25_result_set_add(results, p2, 3.0);

    char *prompt = generation_build_prompt("How is hypertension treated?", store, results);
    TEST_ASSERT(prompt != NULL, "expected build_prompt to succeed");

    TEST_ASSERT(strstr(prompt, "Hypertension is treated with medication.") != NULL,
                "expected the first passage's text in the prompt");
    TEST_ASSERT(strstr(prompt, "Lifestyle changes also help.") != NULL,
                "expected the second passage's text in the prompt");
    TEST_ASSERT(strstr(prompt, "doc1.txt") != NULL, "expected source attribution for doc1.txt");
    TEST_ASSERT(strstr(prompt, "chunk 2") != NULL, "expected chunk id attribution");
    TEST_ASSERT(strstr(prompt, "doc2.txt") != NULL, "expected source attribution for doc2.txt");
    TEST_ASSERT(strstr(prompt, "How is hypertension treated?") != NULL,
                "expected the original question in the prompt");

    free(prompt);
    bm25_result_set_free(results);
    sqlite_store_close(store);
}

static void test_build_prompt_empty_results_returns_null(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    BM25ResultSet *results = bm25_result_set_create();
    TEST_ASSERT(results != NULL, "expected bm25_result_set_create to succeed");

    char *prompt = generation_build_prompt("anything", store, results);
    TEST_ASSERT(prompt == NULL, "expected NULL when there are no results to build a prompt from");

    bm25_result_set_free(results);
    sqlite_store_close(store);
}

static void test_build_prompt_skips_unloadable_passages(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    sqlite3_int64 real_passage = sqlite_store_insert_passage(store, "doc1.txt", 0, "real content here", 3);

    BM25ResultSet *results = bm25_result_set_create();
    TEST_ASSERT(results != NULL, "expected bm25_result_set_create to succeed");
    /* A passage_id that was never actually inserted -- simulates a stale
     * or inconsistent reference; must be skipped, not fatal. */
    bm25_result_set_add(results, 999999, 10.0);
    bm25_result_set_add(results, real_passage, 5.0);

    char *prompt = generation_build_prompt("a question", store, results);
    TEST_ASSERT(prompt != NULL, "expected build_prompt to succeed using only the loadable passage");
    TEST_ASSERT(strstr(prompt, "real content here") != NULL,
                "expected the real passage's text to still be included");

    free(prompt);
    bm25_result_set_free(results);
    sqlite_store_close(store);
}

static void test_build_prompt_all_passages_unloadable_returns_null(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    BM25ResultSet *results = bm25_result_set_create();
    TEST_ASSERT(results != NULL, "expected bm25_result_set_create to succeed");
    bm25_result_set_add(results, 999999, 10.0);

    char *prompt = generation_build_prompt("a question", store, results);
    TEST_ASSERT(prompt == NULL,
                "expected NULL when every referenced passage fails to load -- nothing to ground an answer in");

    bm25_result_set_free(results);
    sqlite_store_close(store);
}

static void test_generate_answer_returns_null_without_api_key(void) {
    unsetenv("OPENROUTER_API_KEY");

    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");
    sqlite3_int64 p1 = sqlite_store_insert_passage(store, "doc1.txt", 0, "some real content", 3);

    BM25ResultSet *results = bm25_result_set_create();
    bm25_result_set_add(results, p1, 5.0);

    /* Unlike query formulation, generation has no fallback -- an API
     * failure just propagates as NULL, since there's no "lesser" answer
     * to fall back to. */
    char *answer = generation_generate_answer("a question", "openai/gpt-4", store, results);
    TEST_ASSERT(answer == NULL, "expected NULL when the API call fails, no fallback answer");

    bm25_result_set_free(results);
    sqlite_store_close(store);
}

static void test_generate_answer_empty_results_returns_null(void) {
    SqliteStore *store = open_fresh_store();
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    BM25ResultSet *results = bm25_result_set_create();

    char *answer = generation_generate_answer("a question", "openai/gpt-4", store, results);
    TEST_ASSERT(answer == NULL, "expected NULL for empty results, without needing an API key at all");

    bm25_result_set_free(results);
    sqlite_store_close(store);
}

int main(void) {
    test_build_prompt_includes_passages_and_question();
    test_build_prompt_empty_results_returns_null();
    test_build_prompt_skips_unloadable_passages();
    test_build_prompt_all_passages_unloadable_returns_null();
    test_generate_answer_returns_null_without_api_key();
    test_generate_answer_empty_results_returns_null();
    remove(TEST_DB_PATH);
    return test_summary();
}
