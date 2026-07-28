/*
 * Tests for src/core/query_formulation.c — gathering WordNet candidates
 * for a query's surviving (post-stopword) terms. Uses the real committed
 * stopword list and WordNet data, same pattern as wordnet.c's end-to-end
 * tests, since this module's whole job is orchestrating those two real
 * pieces together.
 */

/* See tokenizer.c for why this must come before any #include (unsetenv is
 * a POSIX extension hidden by glibc under strict -std=c11 otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "query_formulation.h"
#include "lemmatizer.h"
#include "wordnet.h"
#include "stopwords.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STOPWORD_FILE "data/stopwords/english.txt"
#define WORDNET_DIR "data/wordnet"

static void test_gather_candidates_hypertension_example(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    TEST_ASSERT(stopwords != NULL, "expected stopword_set_load to succeed");
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL, "expected wordnet_table_load to succeed");
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(lemmatizer != NULL, "expected lemmatizer_load to succeed");

    QueryFormulationCandidates *candidates =
        query_formulation_gather_candidates("What is the treatment for hypertension?",
                                             stopwords, wordnet, lemmatizer);
    TEST_ASSERT(candidates != NULL, "expected query_formulation_gather_candidates to succeed");

    /* "what", "is", "the", "for" are stopwords -- only "treatment" and
     * "hypertension" should survive. */
    TEST_ASSERT(candidates->count == 2, "expected 2 surviving terms, got %zu", candidates->count);
    TEST_ASSERT_STR_EQ(candidates->terms[0].term, "treatment");
    TEST_ASSERT_STR_EQ(candidates->terms[1].term, "hypertension");

    /* Both are real WordNet words -- neither should have NULL candidates. */
    TEST_ASSERT(candidates->terms[0].candidates != NULL, "expected \"treatment\" to have candidates");
    TEST_ASSERT(candidates->terms[1].candidates != NULL, "expected \"hypertension\" to have candidates");

    /* Same fact confirmed earlier in wordnet.c's own tests -- confirming
     * it survives the tokenize + stopword-filter + lookup chain intact. */
    const WordNetLookupResult *hypertension = candidates->terms[1].candidates;
    int found_high_blood_pressure = 0;
    for (size_t i = 0; i < hypertension->synonyms->count; i++) {
        if (strcmp(hypertension->synonyms->terms[i], "high_blood_pressure") == 0) {
            found_high_blood_pressure = 1;
        }
    }
    TEST_ASSERT(found_high_blood_pressure, "expected \"high_blood_pressure\" among hypertension's synonyms");

    query_formulation_candidates_free(candidates);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    stopword_set_free(stopwords);
}

static void test_gather_candidates_term_not_in_wordnet(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    TEST_ASSERT(stopwords != NULL, "expected stopword_set_load to succeed");
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL, "expected wordnet_table_load to succeed");
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(lemmatizer != NULL, "expected lemmatizer_load to succeed");

    QueryFormulationCandidates *candidates =
        query_formulation_gather_candidates("zzznonexistentzzz", stopwords, wordnet, lemmatizer);
    TEST_ASSERT(candidates != NULL, "expected query_formulation_gather_candidates to succeed");
    TEST_ASSERT(candidates->count == 1, "expected 1 surviving term, got %zu", candidates->count);
    TEST_ASSERT_STR_EQ(candidates->terms[0].term, "zzznonexistentzzz");
    TEST_ASSERT(candidates->terms[0].candidates == NULL,
                "expected a term absent from WordNet to have NULL candidates, not crash");

    query_formulation_candidates_free(candidates);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    stopword_set_free(stopwords);
}

static void test_gather_candidates_all_stopwords_returns_empty(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    TEST_ASSERT(stopwords != NULL, "expected stopword_set_load to succeed");
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL, "expected wordnet_table_load to succeed");
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(lemmatizer != NULL, "expected lemmatizer_load to succeed");

    QueryFormulationCandidates *candidates =
        query_formulation_gather_candidates("what is the for", stopwords, wordnet, lemmatizer);
    TEST_ASSERT(candidates != NULL,
                "expected an all-stopwords query to succeed with an empty result, not fail");
    TEST_ASSERT(candidates->count == 0, "expected 0 surviving terms, got %zu", candidates->count);

    query_formulation_candidates_free(candidates);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    stopword_set_free(stopwords);
}

static void test_candidates_free_null_is_safe(void) {
    query_formulation_candidates_free(NULL);
}

static void test_build_prompt_contains_expected_content(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    const char *query = "What is the treatment for hypertension?";
    QueryFormulationCandidates *candidates =
        query_formulation_gather_candidates(query, stopwords, wordnet, lemmatizer);
    TEST_ASSERT(candidates != NULL, "expected gather_candidates to succeed");

    char *prompt = query_formulation_build_prompt(query, candidates);
    TEST_ASSERT(prompt != NULL, "expected build_prompt to succeed");

    TEST_ASSERT(strstr(prompt, query) != NULL, "expected the prompt to contain the original question");
    TEST_ASSERT(strstr(prompt, "\"treatment\"") != NULL, "expected the prompt to mention \"treatment\"");
    TEST_ASSERT(strstr(prompt, "\"hypertension\"") != NULL, "expected the prompt to mention \"hypertension\"");
    TEST_ASSERT(strstr(prompt, "high_blood_pressure") != NULL,
                "expected the prompt to include hypertension's real synonym");
    TEST_ASSERT(strstr(prompt, "JSON array") != NULL,
                "expected the prompt to instruct the model to respond with a JSON array");

    free(prompt);
    query_formulation_candidates_free(candidates);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    stopword_set_free(stopwords);
}

static size_t count_char(const char *text, char target) {
    size_t count = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == target) {
            count++;
        }
    }
    return count;
}

static void test_build_prompt_caps_candidates(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    /* "dog" has 29 real synonyms (confirmed in wordnet.c's own tests) --
     * well over the 8-per-category cap. Isolate just dog's synonym line
     * and count commas to confirm it was actually capped, not dumped
     * wholesale into the prompt. */
    QueryFormulationCandidates *candidates =
        query_formulation_gather_candidates("dog", stopwords, wordnet, lemmatizer);
    TEST_ASSERT(candidates != NULL, "expected gather_candidates to succeed");
    TEST_ASSERT(candidates->terms[0].candidates->synonyms->count > 8,
                "expected \"dog\" to have more than 8 real synonyms to make this test meaningful, got %zu",
                candidates->terms[0].candidates->synonyms->count);

    char *prompt = query_formulation_build_prompt("dog", candidates);
    TEST_ASSERT(prompt != NULL, "expected build_prompt to succeed");

    const char *synonyms_line = strstr(prompt, "  synonyms: ");
    TEST_ASSERT(synonyms_line != NULL, "expected a synonyms line in the prompt");
    const char *line_end = strchr(synonyms_line, '\n');
    TEST_ASSERT(line_end != NULL, "expected the synonyms line to end with a newline");

    char line_copy[512];
    size_t line_len = (size_t)(line_end - synonyms_line);
    TEST_ASSERT(line_len < sizeof(line_copy), "synonyms line unexpectedly long");
    memcpy(line_copy, synonyms_line, line_len);
    line_copy[line_len] = '\0';

    /* 8 capped entries -> 7 comma separators, not 28. */
    TEST_ASSERT(count_char(line_copy, ',') == 7,
                "expected exactly 7 commas (8 capped entries), got %zu in \"%s\"",
                count_char(line_copy, ','), line_copy);

    free(prompt);
    query_formulation_candidates_free(candidates);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    stopword_set_free(stopwords);
}

static QueryFormulationCandidates *make_two_term_candidates(StopwordSet *stopwords, WordNetTable *wordnet,
                                                             Lemmatizer *lemmatizer) {
    return query_formulation_gather_candidates("dog cat", stopwords, wordnet, lemmatizer);
}

static void test_parse_selected_terms_valid_json(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    QueryFormulationCandidates *fallback = make_two_term_candidates(stopwords, wordnet, lemmatizer);
    TEST_ASSERT(fallback != NULL, "expected setup to succeed");

    TokenList *result = query_formulation_parse_selected_terms(
        "[\"dog\", \"canine\", \"domestic_dog\"]", fallback);
    TEST_ASSERT(result != NULL, "expected parse to succeed");
    TEST_ASSERT(result->count == 3, "expected 3 selected terms, got %zu", result->count);
    TEST_ASSERT_STR_EQ(result->terms[0], "dog");
    TEST_ASSERT_STR_EQ(result->terms[1], "canine");
    TEST_ASSERT_STR_EQ(result->terms[2], "domestic_dog");

    token_list_free(result);
    query_formulation_candidates_free(fallback);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    stopword_set_free(stopwords);
}

static void test_parse_selected_terms_invalid_json_falls_back(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    QueryFormulationCandidates *fallback = make_two_term_candidates(stopwords, wordnet, lemmatizer);
    TEST_ASSERT(fallback != NULL, "expected setup to succeed");

    TokenList *result = query_formulation_parse_selected_terms("this is not json at all {{{", fallback);
    TEST_ASSERT(result != NULL, "expected a fallback result, not NULL, on unparseable JSON");
    TEST_ASSERT(result->count == 2, "expected fallback to the 2 original terms, got %zu", result->count);
    TEST_ASSERT_STR_EQ(result->terms[0], "dog");
    TEST_ASSERT_STR_EQ(result->terms[1], "cat");

    token_list_free(result);
    query_formulation_candidates_free(fallback);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    stopword_set_free(stopwords);
}

static void test_parse_selected_terms_non_array_falls_back(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    QueryFormulationCandidates *fallback = make_two_term_candidates(stopwords, wordnet, lemmatizer);
    TEST_ASSERT(fallback != NULL, "expected setup to succeed");

    /* Valid JSON, but an object, not the array the prompt asked for. */
    TokenList *result = query_formulation_parse_selected_terms("{\"terms\": [\"dog\"]}", fallback);
    TEST_ASSERT(result != NULL, "expected a fallback result, not NULL, on a non-array response");
    TEST_ASSERT(result->count == 2, "expected fallback to the 2 original terms, got %zu", result->count);

    token_list_free(result);
    query_formulation_candidates_free(fallback);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    stopword_set_free(stopwords);
}

static void test_parse_selected_terms_empty_array_falls_back(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    QueryFormulationCandidates *fallback = make_two_term_candidates(stopwords, wordnet, lemmatizer);
    TEST_ASSERT(fallback != NULL, "expected setup to succeed");

    TokenList *result = query_formulation_parse_selected_terms("[]", fallback);
    TEST_ASSERT(result != NULL, "expected a fallback result, not NULL, on an empty array");
    TEST_ASSERT(result->count == 2, "expected fallback to the 2 original terms, got %zu", result->count);

    token_list_free(result);
    query_formulation_candidates_free(fallback);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    stopword_set_free(stopwords);
}

static void test_parse_selected_terms_ignores_non_string_items(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    QueryFormulationCandidates *fallback = make_two_term_candidates(stopwords, wordnet, lemmatizer);
    TEST_ASSERT(fallback != NULL, "expected setup to succeed");

    TokenList *result = query_formulation_parse_selected_terms("[\"dog\", 42, \"canine\", null]", fallback);
    TEST_ASSERT(result != NULL, "expected parse to succeed");
    TEST_ASSERT(result->count == 2, "expected only the 2 string items kept, got %zu", result->count);
    TEST_ASSERT_STR_EQ(result->terms[0], "dog");
    TEST_ASSERT_STR_EQ(result->terms[1], "canine");

    token_list_free(result);
    query_formulation_candidates_free(fallback);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    stopword_set_free(stopwords);
}

static void test_formulate_query_falls_back_without_api_key(void) {
    unsetenv("OPENROUTER_API_KEY");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    /* openrouter_chat_completion() returns NULL immediately with no API
     * key set -- no network call happens. This exercises the real
     * API-failure fallback path without needing network access. */
    TokenList *result = query_formulation_formulate_query(
        "What is the treatment for hypertension?", "openai/gpt-4", stopwords, wordnet, lemmatizer);
    TEST_ASSERT(result != NULL, "expected a fallback result, not NULL, when the API call fails");
    TEST_ASSERT(result->count == 2, "expected fallback to the 2 original terms, got %zu", result->count);
    TEST_ASSERT_STR_EQ(result->terms[0], "treatment");
    TEST_ASSERT_STR_EQ(result->terms[1], "hypertension");

    token_list_free(result);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    stopword_set_free(stopwords);
}

static void test_formulate_query_all_stopwords_returns_empty_not_null(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL, "expected setup to succeed");

    TokenList *result = query_formulation_formulate_query("what is the for", "openai/gpt-4", stopwords,
                                                            wordnet, lemmatizer);
    TEST_ASSERT(result != NULL, "expected an empty result, not NULL, for an all-stopwords query");
    TEST_ASSERT(result->count == 0, "expected 0 terms, got %zu", result->count);

    token_list_free(result);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    stopword_set_free(stopwords);
}

int main(void) {
    test_gather_candidates_hypertension_example();
    test_gather_candidates_term_not_in_wordnet();
    test_gather_candidates_all_stopwords_returns_empty();
    test_candidates_free_null_is_safe();
    test_build_prompt_contains_expected_content();
    test_build_prompt_caps_candidates();
    test_parse_selected_terms_valid_json();
    test_parse_selected_terms_invalid_json_falls_back();
    test_parse_selected_terms_non_array_falls_back();
    test_parse_selected_terms_empty_array_falls_back();
    test_parse_selected_terms_ignores_non_string_items();
    test_formulate_query_falls_back_without_api_key();
    test_formulate_query_all_stopwords_returns_empty_not_null();
    return test_summary();
}
