/*
 * Tests for src/core/lemmatizer.c — reducing inflected words to their
 * WordNet base form. Uses the real committed WordNet data and exception
 * files, same pattern as wordnet.c's own end-to-end tests, since this
 * module's whole job is validating candidates against the real table.
 */

#include "lemmatizer.h"
#include "wordnet.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>

#define WORDNET_DIR "data/wordnet"

static void test_lemmatize_regular_verb_suffix_rule(void) {
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    /* "called" has no exception entry (confirmed against the real file)
     * -- this exercises the suffix-rule path: strip "ed", validate
     * "call" against real WordNet (28 senses, confirmed earlier). This
     * is the exact case that started this whole investigation. */
    char *result = lemmatize(lemmatizer, wordnet, "called");
    TEST_ASSERT(result != NULL, "expected lemmatize to succeed");
    TEST_ASSERT_STR_EQ(result, "call");

    free(result);
    lemmatizer_free(lemmatizer);
    wordnet_table_free(wordnet);
}

static void test_lemmatize_irregular_exception(void) {
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    /* Classic irregular verb -- no suffix rule could ever derive "go"
     * from "went". Must come from the exception list. */
    char *result = lemmatize(lemmatizer, wordnet, "went");
    TEST_ASSERT(result != NULL, "expected lemmatize to succeed");
    TEST_ASSERT_STR_EQ(result, "go");

    free(result);
    lemmatizer_free(lemmatizer);
    wordnet_table_free(wordnet);
}

static void test_lemmatize_exception_with_spelling_variant(void) {
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    /* "installed" has an exception entry with TWO base forms ("instal",
     * "install" -- British/American spelling, both separately indexed
     * in WordNet pointing at the same synsets). lemmatize() takes the
     * first-listed base form. */
    char *result = lemmatize(lemmatizer, wordnet, "installed");
    TEST_ASSERT(result != NULL, "expected lemmatize to succeed");
    TEST_ASSERT_STR_EQ(result, "instal");

    free(result);
    lemmatizer_free(lemmatizer);
    wordnet_table_free(wordnet);
}

static void test_lemmatize_already_base_form_unchanged(void) {
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    /* "install" doesn't end in any rule's suffix and has no exception
     * entry of its own (it's the target, not the source) -- should pass
     * through completely unchanged. */
    char *result = lemmatize(lemmatizer, wordnet, "install");
    TEST_ASSERT(result != NULL, "expected lemmatize to succeed");
    TEST_ASSERT_STR_EQ(result, "install");

    free(result);
    lemmatizer_free(lemmatizer);
    wordnet_table_free(wordnet);
}

static void test_lemmatize_word_not_in_wordnet_unchanged(void) {
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    /* A fictional/made-up word -- no exception entry, and no
     * rule-derived candidate would validate against real WordNet
     * either. Must fall through to returning the word unchanged, not
     * crash or return NULL. */
    char *result = lemmatize(lemmatizer, wordnet, "windhollow");
    TEST_ASSERT(result != NULL, "expected lemmatize to succeed even for an unrecognized word");
    TEST_ASSERT_STR_EQ(result, "windhollow");

    free(result);
    lemmatizer_free(lemmatizer);
    wordnet_table_free(wordnet);
}

static void test_lemmatizer_load_missing_directory_returns_null(void) {
    Lemmatizer *lemmatizer = lemmatizer_load("build/does_not_exist_wordnet_dir");
    TEST_ASSERT(lemmatizer == NULL, "expected a missing directory to return NULL");
}

static void test_lemmatizer_free_null_is_safe(void) {
    lemmatizer_free(NULL);
}

int main(void) {
    test_lemmatize_regular_verb_suffix_rule();
    test_lemmatize_irregular_exception();
    test_lemmatize_exception_with_spelling_variant();
    test_lemmatize_already_base_form_unchanged();
    test_lemmatize_word_not_in_wordnet_unchanged();
    test_lemmatizer_load_missing_directory_returns_null();
    test_lemmatizer_free_null_is_safe();
    return test_summary();
}
