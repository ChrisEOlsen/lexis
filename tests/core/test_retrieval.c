/*
 * Tests for src/core/retrieval.c — the shared retrieval orchestrator.
 * Uses the real committed WordNet/stopword data and the lexis_test
 * database, like the other DB-backed suites. The local model is never
 * loaded here: expansion's model call returns NULL without
 * local_llm_client_init(), which is exactly the fallback path worth
 * pinning (expansion degrades to plain terms, never blocks retrieval).
 */

#include "retrieval.h"
#include "test_utils.h"

#include <stdio.h>
#include <string.h>

#define TEST_CONNINFO test_conninfo()
#define STOPWORD_FILE "data/stopwords/english.txt"
#define WORDNET_DIR "data/wordnet"

static void test_default_policy_matches_shared_constants(void) {
    RetrievalPolicy policy = retrieval_default_policy();
    TEST_ASSERT(policy.candidate_ceiling == LEXIS_SEARCH_CANDIDATE_CEILING,
                "expected default ceiling to be the shared constant");
    TEST_ASSERT(policy.max_passages == LEXIS_SEARCH_MAX_PASSAGES,
                "expected default trim cap to be the shared constant");
    TEST_ASSERT(policy.use_expansion == 1, "expected expansion on by default");
    TEST_ASSERT(policy.bm25.coord_bonus == BM25_DEFAULT_COORD_BONUS,
                "expected the coordination bonus on by default");
}

static void test_all_stopwords_question_returns_empty_run(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL && store != NULL,
                "expected setup to succeed");

    RetrievalPolicy policy = retrieval_default_policy();
    RetrievalRun *run = retrieval_run(store, "what is the for", NULL, stopwords, wordnet,
                                       lemmatizer, &policy);
    TEST_ASSERT(run != NULL, "expected an empty run, not NULL, for an all-stopwords question");
    TEST_ASSERT(run->terms != NULL && run->terms->count == 0,
                "expected zero terms for an all-stopwords question");
    TEST_ASSERT(run->results == NULL, "expected no search to have happened");
    TEST_ASSERT(run->expansion_prompt == NULL, "expected no expansion attempt with zero terms");

    retrieval_run_free(run);
    pg_store_close(store);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

static void test_no_expansion_policy_searches_plain_terms(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL && store != NULL,
                "expected setup to succeed");

    RetrievalPolicy policy = retrieval_default_policy();
    policy.use_expansion = 0;
    RetrievalRun *run = retrieval_run(store, "What is the treatment for hypertension?", NULL,
                                       stopwords, wordnet, lemmatizer, &policy);
    TEST_ASSERT(run != NULL, "expected retrieval to succeed");
    TEST_ASSERT(run->terms->count == 2, "expected 2 plain terms, got %zu", run->terms->count);
    TEST_ASSERT_STR_EQ(run->terms->terms[0], "treatment");
    TEST_ASSERT_STR_EQ(run->terms->terms[1], "hypertension");
    TEST_ASSERT(run->original_count == 2, "expected every term to be an original");
    TEST_ASSERT(run->expansion_prompt == NULL && run->expansion_response == NULL,
                "expected no expansion artifacts when the policy disables it");
    TEST_ASSERT(run->results != NULL, "expected a (possibly empty) result set, not NULL");

    retrieval_run_free(run);
    pg_store_close(store);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

static void test_expansion_without_model_degrades_to_plain_terms(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL && store != NULL,
                "expected setup to succeed");

    /* No local_llm_client_init() in this binary: the model call inside
     * expansion returns NULL. The run must still search -- on the plain
     * question terms -- and record that the fallback fired. */
    RetrievalPolicy policy = retrieval_default_policy();
    RetrievalRun *run = retrieval_run(store, "What is the treatment for hypertension?", NULL,
                                       stopwords, wordnet, lemmatizer, &policy);
    TEST_ASSERT(run != NULL, "expected retrieval to succeed despite the model being unavailable");
    TEST_ASSERT(run->used_fallback == 1, "expected the fallback flag when the model call fails");
    TEST_ASSERT(run->expansion_prompt != NULL,
                "expected the prompt artifact even when the call failed (query_log wants it)");
    TEST_ASSERT(run->expansion_response == NULL, "expected no response artifact on model failure");
    TEST_ASSERT(run->terms->count == 2 && run->original_count == 2,
                "expected plain original terms only, got %zu terms / %zu originals",
                run->terms->count, run->original_count);
    TEST_ASSERT(run->results != NULL, "expected a result set");

    retrieval_run_free(run);
    pg_store_close(store);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

static void test_rewritten_question_terms_are_unioned(void) {
    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    PgStore *store = pg_store_open(TEST_CONNINFO);
    TEST_ASSERT(stopwords != NULL && wordnet != NULL && lemmatizer != NULL && store != NULL,
                "expected setup to succeed");

    RetrievalPolicy policy = retrieval_default_policy();
    policy.use_expansion = 0; /* isolate the union behavior */
    RetrievalRun *run = retrieval_run(store, "what about the dog", "tell me about the dog breed",
                                       stopwords, wordnet, lemmatizer, &policy);
    TEST_ASSERT(run != NULL, "expected retrieval to succeed");
    /* Union, raw question's terms first, deduplicated: dog (both), then
     * the rewrite-only terms. */
    TEST_ASSERT(run->terms->count == 3, "expected 3 unioned terms, got %zu", run->terms->count);
    TEST_ASSERT_STR_EQ(run->terms->terms[0], "dog");
    TEST_ASSERT(run->original_count == run->terms->count,
                "expected every union term to count as an original");

    retrieval_run_free(run);
    pg_store_close(store);
    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
}

int main(void) {
    test_default_policy_matches_shared_constants();
    test_all_stopwords_question_returns_empty_run();
    test_no_expansion_policy_searches_plain_terms();
    test_expansion_without_model_degrades_to_plain_terms();
    test_rewritten_question_terms_are_unioned();
    return test_summary();
}
