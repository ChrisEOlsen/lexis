/*
 * Tests for src/core/tokenizer.c — TokenList lifecycle and tokenize().
 * Standalone executable; exit code reflects pass/fail (spec build order
 * Stage 3 calls for validating correctness before building on top of a
 * module, and that applies just as well here in Stage 1).
 */

#include "tokenizer.h"
#include "test_utils.h"

static void test_basic(void) {
    TokenList *list = tokenize("Hello, World! This is LEXIS.");
    TEST_ASSERT(list->count == 5, "expected 5 tokens, got %zu", list->count);
    TEST_ASSERT_STR_EQ(list->terms[0], "hello");
    TEST_ASSERT_STR_EQ(list->terms[1], "world");
    TEST_ASSERT_STR_EQ(list->terms[2], "this");
    TEST_ASSERT_STR_EQ(list->terms[3], "is");
    TEST_ASSERT_STR_EQ(list->terms[4], "lexis");
    token_list_free(list);
}

static void test_extra_whitespace(void) {
    TokenList *list = tokenize("  multiple   spaces\tand\ttabs  ");
    TEST_ASSERT(list->count == 4, "expected 4 tokens, got %zu", list->count);
    TEST_ASSERT_STR_EQ(list->terms[0], "multiple");
    TEST_ASSERT_STR_EQ(list->terms[1], "spaces");
    TEST_ASSERT_STR_EQ(list->terms[2], "and");
    TEST_ASSERT_STR_EQ(list->terms[3], "tabs");
    token_list_free(list);
}

static void test_empty_string(void) {
    TokenList *list = tokenize("");
    TEST_ASSERT(list->count == 0, "expected 0 tokens, got %zu", list->count);
    token_list_free(list);
}

static void test_only_punctuation(void) {
    TokenList *list = tokenize("!!!...???");
    TEST_ASSERT(list->count == 0, "expected 0 tokens, got %zu", list->count);
    token_list_free(list);
}

static void test_apostrophe_stays_together(void) {
    /* Internal apostrophe (letter on both sides) no longer fragments the
     * word -- "don't" stays one token instead of spilling a stray "t". */
    TokenList *list = tokenize("don't stop");
    TEST_ASSERT(list->count == 2, "expected 2 tokens, got %zu", list->count);
    TEST_ASSERT_STR_EQ(list->terms[0], "don't");
    TEST_ASSERT_STR_EQ(list->terms[1], "stop");
    token_list_free(list);
}

static void test_possessive_apostrophe_stays_together(void) {
    /* Deliberately NOT stripped to "okafor" -- see tokenizer.c's doc
     * comment on why possessive-vs-contraction can't be told apart from
     * spelling alone ("isn't" ends in "'t" too), so both are kept whole
     * rather than risking silently inverting a negation. */
    TokenList *list = tokenize("Okafor's telescope");
    TEST_ASSERT(list->count == 2, "expected 2 tokens, got %zu", list->count);
    TEST_ASSERT_STR_EQ(list->terms[0], "okafor's");
    TEST_ASSERT_STR_EQ(list->terms[1], "telescope");
    token_list_free(list);
}

static void test_hyphen_stays_together(void) {
    TokenList *list = tokenize("state-of-the-art design");
    TEST_ASSERT(list->count == 2, "expected 2 tokens, got %zu", list->count);
    TEST_ASSERT_STR_EQ(list->terms[0], "state-of-the-art");
    TEST_ASSERT_STR_EQ(list->terms[1], "design");
    token_list_free(list);
}

static void test_decimal_number_stays_together(void) {
    /* The real bug that started this: "2.4-meter" used to fragment into
     * "2", "4", "meter", destroying the measurement entirely. */
    TokenList *list = tokenize("a 2.4-meter telescope");
    TEST_ASSERT(list->count == 3, "expected 3 tokens, got %zu", list->count);
    TEST_ASSERT_STR_EQ(list->terms[0], "a");
    TEST_ASSERT_STR_EQ(list->terms[1], "2.4-meter");
    TEST_ASSERT_STR_EQ(list->terms[2], "telescope");
    token_list_free(list);
}

static void test_thousands_separator_stays_together(void) {
    TokenList *list = tokenize("1,000 stars");
    TEST_ASSERT(list->count == 2, "expected 2 tokens, got %zu", list->count);
    TEST_ASSERT_STR_EQ(list->terms[0], "1,000");
    TEST_ASSERT_STR_EQ(list->terms[1], "stars");
    token_list_free(list);
}

static void test_sentence_ending_punctuation_still_splits(void) {
    /* The internal-punctuation rule must not swallow sentence-ending
     * punctuation -- "telescope." still ends the word normally, since the
     * period isn't followed by another alphanumeric byte. */
    TokenList *list = tokenize("The telescope. It worked.");
    TEST_ASSERT(list->count == 4, "expected 4 tokens, got %zu", list->count);
    TEST_ASSERT_STR_EQ(list->terms[0], "the");
    TEST_ASSERT_STR_EQ(list->terms[1], "telescope");
    TEST_ASSERT_STR_EQ(list->terms[2], "it");
    TEST_ASSERT_STR_EQ(list->terms[3], "worked");
    token_list_free(list);
}

static void test_quoted_word_still_strips_quotes(void) {
    /* Leading/trailing single quotes used as quotation marks, not
     * apostrophes, must still be stripped -- neither is "internal" since
     * there's no alphanumeric byte on the outward-facing side. */
    TokenList *list = tokenize("'hello' world");
    TEST_ASSERT(list->count == 2, "expected 2 tokens, got %zu", list->count);
    TEST_ASSERT_STR_EQ(list->terms[0], "hello");
    TEST_ASSERT_STR_EQ(list->terms[1], "world");
    token_list_free(list);
}

/* 12 tokens crosses the initial capacity of 8 (TOKEN_LIST_INITIAL_CAPACITY
 * in tokenizer.c), forcing token_list_append's realloc-doubling path to
 * actually run rather than just being compiled and never exercised. */
static void test_many_tokens_grows_capacity(void) {
    TokenList *list = tokenize(
        "one two three four five six seven eight nine ten eleven twelve");
    TEST_ASSERT(list->count == 12, "expected 12 tokens, got %zu", list->count);
    TEST_ASSERT_STR_EQ(list->terms[0], "one");
    TEST_ASSERT_STR_EQ(list->terms[7], "eight");
    TEST_ASSERT_STR_EQ(list->terms[11], "twelve");
    token_list_free(list);
}

int main(void) {
    test_basic();
    test_extra_whitespace();
    test_empty_string();
    test_only_punctuation();
    test_apostrophe_stays_together();
    test_possessive_apostrophe_stays_together();
    test_hyphen_stays_together();
    test_decimal_number_stays_together();
    test_thousands_separator_stays_together();
    test_sentence_ending_punctuation_still_splits();
    test_quoted_word_still_strips_quotes();
    test_many_tokens_grows_capacity();
    return test_summary();
}
