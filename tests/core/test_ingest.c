/*
 * Tests for src/core/ingest.c — reading a document's full contents into
 * memory as the first step of the ingestion pipeline.
 */

#include "ingest.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_FILE_PATH "build/test_ingest_file.txt"
#define TEST_DB_PATH "build/test_ingest_document.db"
#define STOPWORD_FILE "data/stopwords/english.txt"
#define WORDNET_DIR "data/wordnet"
#define TEST_CORPUS_DIR "build/test_corpus"

static void write_file_at(const char *path, const char *contents) {
    FILE *fp = fopen(path, "wb");
    fwrite(contents, 1, strlen(contents), fp);
    fclose(fp);
}

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

static void test_ingest_document_end_to_end(void) {
    write_test_file("The hypertension treatment options are effective.");

    remove(TEST_DB_PATH);
    SqliteStore *store = sqlite_store_open(TEST_DB_PATH);
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    TEST_ASSERT(stopwords != NULL, "expected stopword_set_load to succeed");
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL, "expected wordnet_table_load to succeed");
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(lemmatizer != NULL, "expected lemmatizer_load to succeed");

    /* chunk_size large enough that the whole (short) document is exactly
     * one chunk -- isolates this test to ingest_document's own wiring
     * rather than re-testing windowing, which already has its own tests. */
    long passages = ingest_document(store, stopwords, wordnet, lemmatizer,
                                     TEST_FILE_PATH, "doc1.txt", 100, 0);
    TEST_ASSERT(passages == 1, "expected exactly 1 passage ingested, got %ld", passages);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT document_name, text, token_count FROM passages;";
    TEST_ASSERT(sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK,
                "expected verification query to prepare");
    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected a passage row to exist");
    TEST_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "doc1.txt");
    /* Passage text is the raw chunk -- original casing/punctuation intact,
     * NOT the lowercased/stripped tokenizer output. */
    TEST_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1),
                        "The hypertension treatment options are effective.");
    /* "The" and "are" are stopwords -- token_count reflects the 4 terms
     * that survive tokenize() + stopwords_filter(), not the 6 raw words. */
    TEST_ASSERT(sqlite3_column_int(stmt, 2) == 4,
                "expected token_count 4 after stopword filtering, got %d",
                sqlite3_column_int(stmt, 2));
    sqlite3_finalize(stmt);

    sqlite3_int64 the_id = sqlite_store_lookup_term(store, "the");
    sqlite3_int64 are_id = sqlite_store_lookup_term(store, "are");
    TEST_ASSERT(the_id == -1, "expected \"the\" to be filtered out as a stopword");
    TEST_ASSERT(are_id == -1, "expected \"are\" to be filtered out as a stopword");

    sqlite3_int64 hypertension_id = sqlite_store_lookup_term(store, "hypertension");
    TEST_ASSERT(hypertension_id != -1, "expected \"hypertension\" to have been indexed");

    const char *freq_sql = "SELECT term_frequency FROM postings WHERE term_id = ?;";
    TEST_ASSERT(sqlite3_prepare_v2(store->db, freq_sql, -1, &stmt, NULL) == SQLITE_OK,
                "expected frequency query to prepare");
    sqlite3_bind_int64(stmt, 1, hypertension_id);
    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected a posting row for \"hypertension\"");
    TEST_ASSERT(sqlite3_column_int(stmt, 0) == 1, "expected term_frequency 1, got %d",
                sqlite3_column_int(stmt, 0));
    sqlite3_finalize(stmt);

    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    sqlite_store_close(store);
}

static void test_ingest_document_counts_repeated_terms(void) {
    write_test_file("treatment treatment treatment");

    remove(TEST_DB_PATH);
    SqliteStore *store = sqlite_store_open(TEST_DB_PATH);
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    TEST_ASSERT(stopwords != NULL, "expected stopword_set_load to succeed");
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL, "expected wordnet_table_load to succeed");
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(lemmatizer != NULL, "expected lemmatizer_load to succeed");

    long passages = ingest_document(store, stopwords, wordnet, lemmatizer,
                                     TEST_FILE_PATH, "doc1.txt", 100, 0);
    TEST_ASSERT(passages == 1, "expected exactly 1 passage ingested, got %ld", passages);

    sqlite3_int64 term_id = sqlite_store_lookup_term(store, "treatment");
    TEST_ASSERT(term_id != -1, "expected \"treatment\" to have been indexed");

    /* A repeated term within one chunk must collapse into exactly ONE
     * posting row with the correct total frequency -- not three separate
     * rows (which the postings table's composite primary key would
     * reject as a duplicate insert anyway, but the count should never
     * have been attempted 3 times to begin with). */
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT COUNT(*), term_frequency FROM postings WHERE term_id = ?;";
    TEST_ASSERT(sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK,
                "expected verification query to prepare");
    sqlite3_bind_int64(stmt, 1, term_id);
    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected a result row");
    TEST_ASSERT(sqlite3_column_int(stmt, 0) == 1,
                "expected exactly 1 posting row for a repeated term, got %d",
                sqlite3_column_int(stmt, 0));
    TEST_ASSERT(sqlite3_column_int(stmt, 1) == 3,
                "expected term_frequency 3, got %d", sqlite3_column_int(stmt, 1));
    sqlite3_finalize(stmt);

    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    sqlite_store_close(store);
}

static void reset_test_corpus_dir(void) {
    /* Best-effort cleanup of a previous run -- ignore errors, since these
     * paths may not exist yet on the first run. */
    remove(TEST_CORPUS_DIR "/doc1.txt");
    remove(TEST_CORPUS_DIR "/doc2.txt");
    remove(TEST_CORPUS_DIR "/subdir/nested.txt");
    rmdir(TEST_CORPUS_DIR "/subdir");
    rmdir(TEST_CORPUS_DIR);

    mkdir(TEST_CORPUS_DIR, 0755);
}

static void test_ingest_corpus_ingests_every_top_level_file(void) {
    reset_test_corpus_dir();
    write_file_at(TEST_CORPUS_DIR "/doc1.txt", "hypertension treatment options");
    write_file_at(TEST_CORPUS_DIR "/doc2.txt", "cardiac arrest response plan");

    remove(TEST_DB_PATH);
    SqliteStore *store = sqlite_store_open(TEST_DB_PATH);
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    TEST_ASSERT(stopwords != NULL, "expected stopword_set_load to succeed");
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL, "expected wordnet_table_load to succeed");
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(lemmatizer != NULL, "expected lemmatizer_load to succeed");

    long total = ingest_corpus(store, stopwords, wordnet, lemmatizer, TEST_CORPUS_DIR, 100, 0);
    TEST_ASSERT(total == 2, "expected 2 total passages (1 per file), got %ld", total);

    /* Each passage's document_name should be the bare filename, not the
     * full directory-joined path. */
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT COUNT(*) FROM passages WHERE document_name = 'doc1.txt';";
    TEST_ASSERT(sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) == SQLITE_OK,
                "expected verification query to prepare");
    TEST_ASSERT(sqlite3_step(stmt) == SQLITE_ROW, "expected a result row");
    TEST_ASSERT(sqlite3_column_int(stmt, 0) == 1,
                "expected exactly 1 passage tagged \"doc1.txt\", got %d",
                sqlite3_column_int(stmt, 0));
    sqlite3_finalize(stmt);

    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    sqlite_store_close(store);
}

static void test_ingest_corpus_skips_subdirectories(void) {
    reset_test_corpus_dir();
    write_file_at(TEST_CORPUS_DIR "/doc1.txt", "hypertension treatment");
    mkdir(TEST_CORPUS_DIR "/subdir", 0755);
    write_file_at(TEST_CORPUS_DIR "/subdir/nested.txt", "should not be ingested");

    remove(TEST_DB_PATH);
    SqliteStore *store = sqlite_store_open(TEST_DB_PATH);
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    TEST_ASSERT(stopwords != NULL, "expected stopword_set_load to succeed");
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL, "expected wordnet_table_load to succeed");
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(lemmatizer != NULL, "expected lemmatizer_load to succeed");

    /* Non-recursive: only doc1.txt should be ingested, "subdir" itself
     * (and anything inside it) skipped entirely. */
    long total = ingest_corpus(store, stopwords, wordnet, lemmatizer, TEST_CORPUS_DIR, 100, 0);
    TEST_ASSERT(total == 1, "expected only the top-level file to be ingested, got %ld", total);

    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    sqlite_store_close(store);
}

static void test_ingest_corpus_missing_directory_returns_negative_one(void) {
    remove(TEST_DB_PATH);
    SqliteStore *store = sqlite_store_open(TEST_DB_PATH);
    TEST_ASSERT(store != NULL, "expected sqlite_store_open to succeed");

    StopwordSet *stopwords = stopword_set_load(STOPWORD_FILE);
    TEST_ASSERT(stopwords != NULL, "expected stopword_set_load to succeed");
    WordNetTable *wordnet = wordnet_table_load(WORDNET_DIR);
    TEST_ASSERT(wordnet != NULL, "expected wordnet_table_load to succeed");
    Lemmatizer *lemmatizer = lemmatizer_load(WORDNET_DIR);
    TEST_ASSERT(lemmatizer != NULL, "expected lemmatizer_load to succeed");

    long total = ingest_corpus(store, stopwords, wordnet, lemmatizer, "build/does_not_exist_dir", 100, 0);
    TEST_ASSERT(total == -1, "expected -1 for a missing directory, got %ld", total);

    stopword_set_free(stopwords);
    wordnet_table_free(wordnet);
    lemmatizer_free(lemmatizer);
    sqlite_store_close(store);
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
    test_ingest_document_end_to_end();
    test_ingest_document_counts_repeated_terms();
    test_ingest_corpus_ingests_every_top_level_file();
    test_ingest_corpus_skips_subdirectories();
    test_ingest_corpus_missing_directory_returns_negative_one();
    remove(TEST_FILE_PATH);
    remove(TEST_DB_PATH);
    remove(TEST_CORPUS_DIR "/doc1.txt");
    remove(TEST_CORPUS_DIR "/doc2.txt");
    remove(TEST_CORPUS_DIR "/subdir/nested.txt");
    rmdir(TEST_CORPUS_DIR "/subdir");
    rmdir(TEST_CORPUS_DIR);
    return test_summary();
}
