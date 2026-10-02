/* Tests for lemmatizer.c against real WordNet data and exception files. */

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

    /* "called" has no exception entry; suffix rule must yield "call". */
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

    /* Irregular verb; must come from the exception list. */
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

    /* "installed" maps to two bases ("instal", "install"); first one wins. */
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

    /* Base form with no rule or exception; passes through unchanged. */
    char *result = lemmatize(lemmatizer, wordnet, "install");
    TEST_ASSERT(result != NULL, "expected lemmatize to succeed");
    TEST_ASSERT_STR_EQ(result, "install");

    free(result);
    lemmatizer_free(lemmatizer);
    wordnet_table_free(wordnet);
}

static void test_lemmatize_base_form_ending_in_rule_suffix_unchanged(void) {
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    /* "king"/"ring"/"sing" look inflected but are base forms; must not strip to letters. */
    char *result = lemmatize(lemmatizer, wordnet, "king");
    TEST_ASSERT(result != NULL, "expected lemmatize to succeed");
    TEST_ASSERT_STR_EQ(result, "king");
    free(result);

    result = lemmatize(lemmatizer, wordnet, "ring");
    TEST_ASSERT(result != NULL, "expected lemmatize to succeed");
    TEST_ASSERT_STR_EQ(result, "ring");
    free(result);

    result = lemmatize(lemmatizer, wordnet, "sing");
    TEST_ASSERT(result != NULL, "expected lemmatize to succeed");
    TEST_ASSERT_STR_EQ(result, "sing");
    free(result);

    lemmatizer_free(lemmatizer);
    wordnet_table_free(wordnet);
}

static void test_lemmatize_plural_of_ing_base_still_stripped(void) {
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    /* Genuinely inflected "kings" must still strip to "king". */
    char *result = lemmatize(lemmatizer, wordnet, "kings");
    TEST_ASSERT(result != NULL, "expected lemmatize to succeed");
    TEST_ASSERT_STR_EQ(result, "king");

    free(result);
    lemmatizer_free(lemmatizer);
    wordnet_table_free(wordnet);
}

static void test_lemmatize_exception_beats_base_form_guard(void) {
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    /* Exception must beat the already-in-WordNet guard ("saw" -> "see"). */
    char *result = lemmatize(lemmatizer, wordnet, "saw");
    TEST_ASSERT(result != NULL, "expected lemmatize to succeed");
    TEST_ASSERT_STR_EQ(result, "see");

    free(result);
    lemmatizer_free(lemmatizer);
    wordnet_table_free(wordnet);
}

static void test_lemmatize_word_not_in_wordnet_unchanged(void) {
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    /* Unknown words return unchanged, not NULL. */
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
    test_lemmatize_base_form_ending_in_rule_suffix_unchanged();
    test_lemmatize_plural_of_ing_base_still_stripped();
    test_lemmatize_exception_beats_base_form_guard();
    test_lemmatize_word_not_in_wordnet_unchanged();
    test_lemmatizer_load_missing_directory_returns_null();
    test_lemmatizer_free_null_is_safe();
    return test_summary();
}
