/*
 * Tests for src/core/stopwords.c — StopwordSet lifecycle and lookup.
 * Standalone executable; exit code reflects pass/fail. Run from the repo
 * root so the relative path to data/stopwords/english.txt resolves.
 */

#include "stopwords.h"
#include "test_utils.h"

#define STOPWORD_FILE "data/stopwords/english.txt"

static void test_load_succeeds(void) {
    StopwordSet *set = stopword_set_load(STOPWORD_FILE);
    TEST_ASSERT(set != NULL, "expected stopword_set_load to succeed");
    TEST_ASSERT(set->count == 198, "expected 198 words, got %zu", set->count);
    stopword_set_free(set);
}

static void test_load_missing_file_returns_null(void) {
    StopwordSet *set = stopword_set_load("data/stopwords/does_not_exist.txt");
    TEST_ASSERT(set == NULL, "expected NULL for a missing file");
}

static void test_contains_known_stopwords(void) {
    StopwordSet *set = stopword_set_load(STOPWORD_FILE);
    TEST_ASSERT(set != NULL, "expected stopword_set_load to succeed");

    /* Spans the sorted range: first entry, last entry, and a couple from
     * the middle — exercises bsearch() across the whole array, not just
     * whatever happens to land near the midpoint. */
    TEST_ASSERT(stopword_set_contains(set, "a"), "expected 'a' to be a stopword");
    TEST_ASSERT(stopword_set_contains(set, "you've"), "expected \"you've\" to be a stopword");
    TEST_ASSERT(stopword_set_contains(set, "the"), "expected 'the' to be a stopword");
    TEST_ASSERT(stopword_set_contains(set, "is"), "expected 'is' to be a stopword");

    /* Contraction-split fragments (see LIMITATIONS.md) that tokenizer.c
     * actually produces from input like "don't". */
    TEST_ASSERT(stopword_set_contains(set, "t"), "expected 't' to be a stopword");
    TEST_ASSERT(stopword_set_contains(set, "don"), "expected 'don' to be a stopword");

    stopword_set_free(set);
}

static void test_contains_rejects_content_words(void) {
    StopwordSet *set = stopword_set_load(STOPWORD_FILE);
    TEST_ASSERT(set != NULL, "expected stopword_set_load to succeed");

    TEST_ASSERT(!stopword_set_contains(set, "lexis"), "expected 'lexis' to not be a stopword");
    TEST_ASSERT(!stopword_set_contains(set, "bm25"), "expected 'bm25' to not be a stopword");
    TEST_ASSERT(!stopword_set_contains(set, "tokenizer"), "expected 'tokenizer' to not be a stopword");

    stopword_set_free(set);
}

static void test_free_null_is_safe(void) {
    stopword_set_free(NULL);
    TEST_ASSERT(1, "stopword_set_free(NULL) should not crash");
}

static void test_filter_removes_stopwords(void) {
    StopwordSet *set = stopword_set_load(STOPWORD_FILE);
    TEST_ASSERT(set != NULL, "expected stopword_set_load to succeed");

    TokenList *list = tokenize("This is a test of the LEXIS tokenizer");
    stopwords_filter(list, set);

    TEST_ASSERT(list->count == 3, "expected 3 tokens, got %zu", list->count);
    TEST_ASSERT_STR_EQ(list->terms[0], "test");
    TEST_ASSERT_STR_EQ(list->terms[1], "lexis");
    TEST_ASSERT_STR_EQ(list->terms[2], "tokenizer");

    token_list_free(list);
    stopword_set_free(set);
}

static void test_filter_null_args_is_safe(void) {
    TokenList *list = tokenize("the quick fox");
    stopwords_filter(list, NULL);
    TEST_ASSERT(list->count == 3, "NULL set should leave list untouched, got %zu", list->count);

    stopwords_filter(NULL, NULL);
    TEST_ASSERT(1, "stopwords_filter(NULL, NULL) should not crash");

    token_list_free(list);
}

int main(void) {
    test_load_succeeds();
    test_load_missing_file_returns_null();
    test_contains_known_stopwords();
    test_contains_rejects_content_words();
    test_free_null_is_safe();
    test_filter_removes_stopwords();
    test_filter_null_args_is_safe();
    return test_summary();
}
