/*
 * Tests for src/core/wordnet.c — parsing WordNet's data.<pos> line
 * format. Fixtures below are real lines copied verbatim from the
 * official WordNet 3.0 database files (data.noun), not fabricated, so
 * expected values are hand-verified against the actual file content.
 */

#include "wordnet.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_DATA_FILE_PATH "build/test_wordnet_data.noun"
#define TEST_INDEX_FILE_PATH "build/test_wordnet_index.noun"
#define TEST_TABLE_DIR "build/test_wordnet_table_dir"

/* "entity" -- the root of the noun hierarchy. w_cnt=01 (hex) -> 1 word.
 * p_cnt=003, all three pointers are '~' (hyponym), none are '@'. */
static void test_parse_entity_root_synset(void) {
    const char *line =
        "00001740 03 n 01 entity 0 003 ~ 00001930 n 0000 ~ 00002137 n 0000 "
        "~ 04424418 n 0000 | that which is perceived or known or inferred "
        "to have its own distinct existence (living or nonliving)  ";

    WordNetSynset *synset = wordnet_parse_data_line(line, WORDNET_NOUN);
    TEST_ASSERT(synset != NULL, "expected parse to succeed");
    TEST_ASSERT(synset->pos == WORDNET_NOUN, "expected pos WORDNET_NOUN");
    TEST_ASSERT(synset->offset == 1740, "expected offset 1740, got %ld", synset->offset);

    TEST_ASSERT(synset->word_count == 1, "expected word_count 1, got %zu", synset->word_count);
    TEST_ASSERT_STR_EQ(synset->words[0], "entity");

    TEST_ASSERT(synset->hypernym_count == 0, "expected 0 hypernyms (this is the root), got %zu",
                synset->hypernym_count);
    TEST_ASSERT(synset->hyponym_count == 3, "expected 3 hyponyms, got %zu", synset->hyponym_count);
    TEST_ASSERT(synset->hyponym_offsets[0] == 1930, "expected first hyponym offset 1930, got %ld",
                synset->hyponym_offsets[0]);
    TEST_ASSERT(synset->hyponym_offsets[1] == 2137, "expected second hyponym offset 2137, got %ld",
                synset->hyponym_offsets[1]);
    TEST_ASSERT(synset->hyponym_offsets[2] == 4424418, "expected third hyponym offset 4424418, got %ld",
                synset->hyponym_offsets[2]);

    wordnet_synset_free(synset);
}

/* "object, physical_object" -- w_cnt=02 (hex) -> 2 words. p_cnt=039
 * (decimal 39): 1 hypernym ('@'), 1 derivationally-related-form ('+',
 * ignored -- not synonym/hypernym/hyponym), and 37 hyponyms ('~'). This
 * is also the case that proves w_cnt is genuinely read as hex, not
 * decimal -- if it were misread as decimal "02" the result is the same
 * value here (2), so this alone wouldn't catch a hex/decimal mixup; the
 * real proof is that decimal p_cnt=039 correctly yields 39 total
 * pointers (1+1+37), not a different count a base-mismatch would produce. */
static void test_parse_object_synset_realistic_pointer_mix(void) {
    const char *line =
        "00002684 03 n 02 object 0 physical_object 0 039 "
        "@ 00001930 n 0000 + 00532607 v 0105 "
        "~ 00003553 n 0000 ~ 00027167 n 0000 ~ 03009633 n 0000 ~ 03149951 n 0000 "
        "~ 03233423 n 0000 ~ 03338648 n 0000 ~ 03532080 n 0000 ~ 03595179 n 0000 "
        "~ 03610270 n 0000 ~ 03714721 n 0000 ~ 03892891 n 0000 ~ 04012260 n 0000 "
        "~ 04248010 n 0000 ~ 04345288 n 0000 ~ 04486445 n 0000 ~ 07851054 n 0000 "
        "~ 09238143 n 0000 ~ 09251689 n 0000 ~ 09267490 n 0000 ~ 09279458 n 0000 "
        "~ 09281777 n 0000 ~ 09283193 n 0000 ~ 09287968 n 0000 ~ 09295338 n 0000 "
        "~ 09300905 n 0000 ~ 09302031 n 0000 ~ 09308398 n 0000 ~ 09334396 n 0000 "
        "~ 09335240 n 0000 ~ 09358550 n 0000 ~ 09368224 n 0000 ~ 09407346 n 0000 "
        "~ 09409203 n 0000 ~ 09432990 n 0000 ~ 09468237 n 0000 ~ 09474162 n 0000 "
        "~ 09477037 n 0000 | a tangible and visible entity";

    WordNetSynset *synset = wordnet_parse_data_line(line, WORDNET_NOUN);
    TEST_ASSERT(synset != NULL, "expected parse to succeed");
    TEST_ASSERT(synset->offset == 2684, "expected offset 2684, got %ld", synset->offset);

    TEST_ASSERT(synset->word_count == 2, "expected word_count 2, got %zu", synset->word_count);
    TEST_ASSERT_STR_EQ(synset->words[0], "object");
    TEST_ASSERT_STR_EQ(synset->words[1], "physical_object");

    TEST_ASSERT(synset->hypernym_count == 1, "expected 1 hypernym, got %zu", synset->hypernym_count);
    TEST_ASSERT(synset->hypernym_offsets[0] == 1930, "expected hypernym offset 1930, got %ld",
                synset->hypernym_offsets[0]);

    TEST_ASSERT(synset->hyponym_count == 37, "expected 37 hyponyms, got %zu", synset->hyponym_count);
    TEST_ASSERT(synset->hyponym_offsets[0] == 3553, "expected first hyponym offset 3553, got %ld",
                synset->hyponym_offsets[0]);
    TEST_ASSERT(synset->hyponym_offsets[36] == 9477037, "expected last hyponym offset 9477037, got %ld",
                synset->hyponym_offsets[36]);

    wordnet_synset_free(synset);
}

static void test_parse_empty_line_fails_gracefully(void) {
    WordNetSynset *synset = wordnet_parse_data_line("", WORDNET_NOUN);
    TEST_ASSERT(synset == NULL, "expected an empty line to fail to parse, not crash");
}

static void test_parse_truncated_line_fails_gracefully(void) {
    /* Cuts off mid-pointer-list: claims p_cnt=003 but only supplies one
     * full pointer entry before the string ends. */
    WordNetSynset *synset = wordnet_parse_data_line(
        "00001740 03 n 01 entity 0 003 ~ 00001930 n 0000", WORDNET_NOUN);
    TEST_ASSERT(synset == NULL, "expected a truncated pointer list to fail to parse, not crash");
}

static void test_synset_free_null_is_safe(void) {
    wordnet_synset_free(NULL);
}

/* Synthetic but format-valid (not copied from the real file, unlike the
 * fixtures above) -- exercises p_cnt=0, the one path the two real
 * fixtures above never hit: hypernym_offsets/hyponym_offsets are never
 * malloc'd at all when there are zero pointers (see the `pointer_count >
 * 0` guard in wordnet_parse_data_line), so both should come back NULL,
 * not just empty. */
static void test_parse_zero_pointers_synset(void) {
    const char *line = "00000500 03 n 01 testword 0 000 | a test entry with no pointers";

    WordNetSynset *synset = wordnet_parse_data_line(line, WORDNET_NOUN);
    TEST_ASSERT(synset != NULL, "expected parse to succeed");
    TEST_ASSERT(synset->word_count == 1, "expected word_count 1, got %zu", synset->word_count);
    TEST_ASSERT_STR_EQ(synset->words[0], "testword");
    TEST_ASSERT(synset->hypernym_count == 0, "expected 0 hypernyms, got %zu", synset->hypernym_count);
    TEST_ASSERT(synset->hyponym_count == 0, "expected 0 hyponyms, got %zu", synset->hyponym_count);
    TEST_ASSERT(synset->hypernym_offsets == NULL, "expected hypernym_offsets to be NULL, not just empty");
    TEST_ASSERT(synset->hyponym_offsets == NULL, "expected hyponym_offsets to be NULL, not just empty");

    wordnet_synset_free(synset);
}

/* Writes the two real fixture lines (entity=1740, object=2684) plus the
 * synthetic zero-pointer one (testword=500), deliberately out of offset
 * order, with a fake copyright-style header line (leading space) and a
 * blank line mixed in -- to prove wordnet_load_data_file both sorts
 * correctly and skips exactly the lines it should. */
static void write_test_data_file(void) {
    FILE *fp = fopen(TEST_DATA_FILE_PATH, "wb");
    const char *content =
        "  fake copyright header line, should be skipped\n"
        "\n"
        "00002684 03 n 02 object 0 physical_object 0 001 @ 00001930 n 0000 | a tangible entity\n"
        "00000500 03 n 01 testword 0 000 | a test entry with no pointers\n"
        "00001740 03 n 01 entity 0 001 ~ 00001930 n 0000 | that which is perceived\n";
    fwrite(content, 1, strlen(content), fp);
    fclose(fp);
}

static void test_load_data_file_parses_sorts_and_skips(void) {
    write_test_data_file();

    WordNetSynsetIndex *index = wordnet_load_data_file(TEST_DATA_FILE_PATH, WORDNET_NOUN);
    TEST_ASSERT(index != NULL, "expected wordnet_load_data_file to succeed");
    TEST_ASSERT(index->count == 3, "expected 3 synsets (header/blank lines skipped), got %zu",
                index->count);

    TEST_ASSERT(index->synsets[0]->offset == 500, "expected offset 500 first, got %ld",
                index->synsets[0]->offset);
    TEST_ASSERT(index->synsets[1]->offset == 1740, "expected offset 1740 second, got %ld",
                index->synsets[1]->offset);
    TEST_ASSERT(index->synsets[2]->offset == 2684, "expected offset 2684 third, got %ld",
                index->synsets[2]->offset);

    wordnet_synset_index_free(index);
}

static void test_synset_index_find_hit_and_miss(void) {
    write_test_data_file();
    WordNetSynsetIndex *index = wordnet_load_data_file(TEST_DATA_FILE_PATH, WORDNET_NOUN);
    TEST_ASSERT(index != NULL, "expected wordnet_load_data_file to succeed");

    const WordNetSynset *found = wordnet_synset_index_find(index, 1740);
    TEST_ASSERT(found != NULL, "expected to find offset 1740");
    TEST_ASSERT_STR_EQ(found->words[0], "entity");

    const WordNetSynset *missing = wordnet_synset_index_find(index, 999999);
    TEST_ASSERT(missing == NULL, "expected a nonexistent offset to return NULL");

    wordnet_synset_index_free(index);
}

static void test_load_data_file_missing_file_returns_null(void) {
    WordNetSynsetIndex *index = wordnet_load_data_file("build/does_not_exist.noun", WORDNET_NOUN);
    TEST_ASSERT(index == NULL, "expected a missing file to return NULL");
}

static void test_synset_index_free_null_is_safe(void) {
    wordnet_synset_index_free(NULL);
}

/* "entity" -- real line from index.noun. synset_cnt=1, p_cnt=1 ('~'),
 * sense_cnt=1, tagsense_cnt=1, one trailing offset: 1740. Matches the
 * single synset "entity" belongs to in data.noun above. */
static void test_parse_index_entity(void) {
    const char *line = "entity n 1 1 ~ 1 1 00001740";

    WordNetIndexEntry *entry = wordnet_parse_index_line(line);
    TEST_ASSERT(entry != NULL, "expected parse to succeed");
    TEST_ASSERT_STR_EQ(entry->lemma, "entity");
    TEST_ASSERT(entry->synset_count == 1, "expected synset_count 1, got %zu", entry->synset_count);
    TEST_ASSERT(entry->synset_offsets[0] == 1740, "expected offset 1740, got %ld",
                entry->synset_offsets[0]);

    wordnet_index_entry_free(entry);
}

/* "dog" -- real line from index.noun. synset_cnt=7, p_cnt=5 (five
 * distinct pointer symbols used across dog's senses: @ ~ #m #p %p),
 * sense_cnt=7, tagsense_cnt=1, seven trailing offsets. This is the case
 * that actually proves p_cnt pointer symbols get consumed and skipped
 * correctly -- with only one pointer symbol (like "entity" above) a
 * miscounted skip could still coincidentally land on the right field. */
static void test_parse_index_dog_multiple_senses(void) {
    const char *line = "dog n 7 5 @ ~ #m #p %p 7 1 02084071 10114209 10023039 09886220 07676602 03901548 02710044";

    WordNetIndexEntry *entry = wordnet_parse_index_line(line);
    TEST_ASSERT(entry != NULL, "expected parse to succeed");
    TEST_ASSERT_STR_EQ(entry->lemma, "dog");
    TEST_ASSERT(entry->synset_count == 7, "expected synset_count 7, got %zu", entry->synset_count);
    TEST_ASSERT(entry->synset_offsets[0] == 2084071, "expected offset[0] 2084071, got %ld",
                entry->synset_offsets[0]);
    TEST_ASSERT(entry->synset_offsets[1] == 10114209, "expected offset[1] 10114209, got %ld",
                entry->synset_offsets[1]);
    TEST_ASSERT(entry->synset_offsets[6] == 2710044, "expected offset[6] 2710044, got %ld",
                entry->synset_offsets[6]);

    wordnet_index_entry_free(entry);
}

static void test_parse_index_empty_line_fails_gracefully(void) {
    WordNetIndexEntry *entry = wordnet_parse_index_line("");
    TEST_ASSERT(entry == NULL, "expected an empty line to fail to parse, not crash");
}

static void test_parse_index_truncated_line_fails_gracefully(void) {
    /* Claims synset_cnt=7 but supplies no offsets at all. */
    WordNetIndexEntry *entry = wordnet_parse_index_line("dog n 7 5 @ ~ #m #p %p 7 1");
    TEST_ASSERT(entry == NULL, "expected a truncated offset list to fail to parse, not crash");
}

static void test_index_entry_free_null_is_safe(void) {
    wordnet_index_entry_free(NULL);
}

/* Same two real fixtures as above, deliberately out of alphabetical
 * order, plus a fake header line and a blank line -- proves sorting and
 * line-skipping both work for the index file loader too. */
static void write_test_index_file(void) {
    FILE *fp = fopen(TEST_INDEX_FILE_PATH, "wb");
    const char *content =
        "  fake copyright header line, should be skipped\n"
        "\n"
        "entity n 1 1 ~ 1 1 00001740\n"
        "dog n 7 5 @ ~ #m #p %p 7 1 02084071 10114209 10023039 09886220 07676602 03901548 02710044\n";
    fwrite(content, 1, strlen(content), fp);
    fclose(fp);
}

static void test_load_index_file_parses_sorts_and_skips(void) {
    write_test_index_file();

    WordNetWordIndex *index = wordnet_load_index_file(TEST_INDEX_FILE_PATH);
    TEST_ASSERT(index != NULL, "expected wordnet_load_index_file to succeed");
    TEST_ASSERT(index->count == 2, "expected 2 entries (header/blank lines skipped), got %zu",
                index->count);

    /* Alphabetical: "dog" sorts before "entity". */
    TEST_ASSERT_STR_EQ(index->entries[0]->lemma, "dog");
    TEST_ASSERT_STR_EQ(index->entries[1]->lemma, "entity");

    wordnet_word_index_free(index);
}

static void test_word_index_find_hit_and_miss(void) {
    write_test_index_file();
    WordNetWordIndex *index = wordnet_load_index_file(TEST_INDEX_FILE_PATH);
    TEST_ASSERT(index != NULL, "expected wordnet_load_index_file to succeed");

    const WordNetIndexEntry *found = wordnet_word_index_find(index, "entity");
    TEST_ASSERT(found != NULL, "expected to find \"entity\"");
    TEST_ASSERT(found->synset_offsets[0] == 1740, "expected offset 1740, got %ld",
                found->synset_offsets[0]);

    const WordNetIndexEntry *missing = wordnet_word_index_find(index, "nonexistentword");
    TEST_ASSERT(missing == NULL, "expected a nonexistent word to return NULL");

    wordnet_word_index_free(index);
}

static void test_load_index_file_missing_file_returns_null(void) {
    WordNetWordIndex *index = wordnet_load_index_file("build/does_not_exist_index.noun");
    TEST_ASSERT(index == NULL, "expected a missing file to return NULL");
}

static void test_word_index_free_null_is_safe(void) {
    wordnet_word_index_free(NULL);
}

static void write_file_at(const char *path, const char *content) {
    FILE *fp = fopen(path, "wb");
    fwrite(content, 1, strlen(content), fp);
    fclose(fp);
}

/* A minimal, synthetic (not real-data) index/data file pair for one part
 * of speech: "testword" belongs to one synset containing one other word,
 * `synonym`. */
static void write_minimal_pos_fixture(const char *dir, const char *suffix, const char *synonym) {
    char path[512];

    snprintf(path, sizeof(path), "%s/index.%s", dir, suffix);
    write_file_at(path, "testword n 1 0 1 1 00000100\n");

    snprintf(path, sizeof(path), "%s/data.%s", dir, suffix);
    char data_content[256];
    snprintf(data_content, sizeof(data_content), "00000100 03 n 02 testword 0 %s 0 000 | test synset\n",
              synonym);
    write_file_at(path, data_content);
}

/* An empty-but-valid pair (header-only, no real entries) -- needed
 * because wordnet_table_load() always loads all four parts of speech;
 * this fixture is used for the two POS that shouldn't contribute data. */
static void write_empty_pos_fixture(const char *dir, const char *suffix) {
    char path[512];
    snprintf(path, sizeof(path), "%s/index.%s", dir, suffix);
    write_file_at(path, "  header only, no real entries\n");
    snprintf(path, sizeof(path), "%s/data.%s", dir, suffix);
    write_file_at(path, "  header only, no real entries\n");
}

/* Proves cross-POS merging: "testword" appears as both a noun (synonym
 * "nounsynonym") and a verb (synonym "verbsynonym") in this synthetic
 * fixture -- a real lookup should return BOTH, merged into one entry,
 * not two separate results or just the last one loaded. This is the
 * design decision confirmed earlier: flat, merged, deduplicated across
 * every sense AND every part of speech. */
static void test_table_load_merges_across_parts_of_speech(void) {
    mkdir(TEST_TABLE_DIR, 0755);
    write_minimal_pos_fixture(TEST_TABLE_DIR, "noun", "nounsynonym");
    write_minimal_pos_fixture(TEST_TABLE_DIR, "verb", "verbsynonym");
    write_empty_pos_fixture(TEST_TABLE_DIR, "adj");
    write_empty_pos_fixture(TEST_TABLE_DIR, "adv");

    WordNetTable *table = wordnet_table_load(TEST_TABLE_DIR);
    TEST_ASSERT(table != NULL, "expected wordnet_table_load to succeed");

    const WordNetLookupResult *result = wordnet_lookup(table, "testword");
    TEST_ASSERT(result != NULL, "expected to find \"testword\"");
    TEST_ASSERT(result->synonyms->count == 2,
                "expected 2 merged synonyms (noun + verb sense), got %zu", result->synonyms->count);

    int found_noun_synonym = 0, found_verb_synonym = 0;
    for (size_t i = 0; i < result->synonyms->count; i++) {
        if (strcmp(result->synonyms->terms[i], "nounsynonym") == 0) found_noun_synonym = 1;
        if (strcmp(result->synonyms->terms[i], "verbsynonym") == 0) found_verb_synonym = 1;
    }
    TEST_ASSERT(found_noun_synonym, "expected \"nounsynonym\" (from the noun sense) to be present");
    TEST_ASSERT(found_verb_synonym, "expected \"verbsynonym\" (from the verb sense) to be present");

    wordnet_table_free(table);
}

static void test_table_load_missing_directory_returns_null(void) {
    WordNetTable *table = wordnet_table_load("build/does_not_exist_wordnet_dir");
    TEST_ASSERT(table == NULL, "expected a missing directory to return NULL");
}

static void test_lookup_nonexistent_word_returns_null(void) {
    mkdir(TEST_TABLE_DIR, 0755);
    write_minimal_pos_fixture(TEST_TABLE_DIR, "noun", "nounsynonym");
    write_empty_pos_fixture(TEST_TABLE_DIR, "verb");
    write_empty_pos_fixture(TEST_TABLE_DIR, "adj");
    write_empty_pos_fixture(TEST_TABLE_DIR, "adv");

    WordNetTable *table = wordnet_table_load(TEST_TABLE_DIR);
    TEST_ASSERT(table != NULL, "expected wordnet_table_load to succeed");

    const WordNetLookupResult *result = wordnet_lookup(table, "zzznonexistentzzz");
    TEST_ASSERT(result == NULL, "expected a word never seen in WordNet to return NULL");

    wordnet_table_free(table);
}

static void test_table_free_null_is_safe(void) {
    wordnet_table_free(NULL);
}

/* End-to-end against the real, full WordNet data committed in
 * data/wordnet/ -- not a synthetic fixture. "hypertension" is the
 * spec's own justifying example (5.2.3): "'Hypertension' and 'high
 * blood pressure' are semantically identical but lexically different."
 * If this doesn't come back, the whole point of building this module
 * hasn't actually been achieved regardless of what the unit tests above
 * say. */
static void test_table_load_real_data_hypertension_example(void) {
    WordNetTable *table = wordnet_table_load("data/wordnet");
    TEST_ASSERT(table != NULL, "expected wordnet_table_load to succeed against real data");

    const WordNetLookupResult *result = wordnet_lookup(table, "hypertension");
    TEST_ASSERT(result != NULL, "expected to find \"hypertension\"");

    int found_high_blood_pressure = 0;
    for (size_t i = 0; i < result->synonyms->count; i++) {
        if (strcmp(result->synonyms->terms[i], "high_blood_pressure") == 0) {
            found_high_blood_pressure = 1;
        }
    }
    TEST_ASSERT(found_high_blood_pressure,
                "expected \"high_blood_pressure\" among hypertension's synonyms -- the spec's own example");

    /* "entity" is the root of the noun hierarchy -- it must have zero
     * hypernyms (nothing is broader than it) but real hyponyms. */
    const WordNetLookupResult *entity = wordnet_lookup(table, "entity");
    TEST_ASSERT(entity != NULL, "expected to find \"entity\"");
    TEST_ASSERT(entity->hypernyms->count == 0, "expected \"entity\" to have 0 hypernyms (it's the root), got %zu",
                entity->hypernyms->count);
    TEST_ASSERT(entity->hyponyms->count > 0, "expected \"entity\" to have real hyponyms, got 0");

    const WordNetLookupResult *missing = wordnet_lookup(table, "zzznonexistentzzz");
    TEST_ASSERT(missing == NULL, "expected a nonexistent word to return NULL against real data too");

    wordnet_table_free(table);
}

int main(void) {
    test_parse_entity_root_synset();
    test_parse_object_synset_realistic_pointer_mix();
    test_parse_empty_line_fails_gracefully();
    test_parse_truncated_line_fails_gracefully();
    test_synset_free_null_is_safe();
    test_parse_zero_pointers_synset();
    test_load_data_file_parses_sorts_and_skips();
    test_synset_index_find_hit_and_miss();
    test_load_data_file_missing_file_returns_null();
    test_synset_index_free_null_is_safe();
    test_parse_index_entity();
    test_parse_index_dog_multiple_senses();
    test_parse_index_empty_line_fails_gracefully();
    test_parse_index_truncated_line_fails_gracefully();
    test_index_entry_free_null_is_safe();
    test_load_index_file_parses_sorts_and_skips();
    test_word_index_find_hit_and_miss();
    test_load_index_file_missing_file_returns_null();
    test_word_index_free_null_is_safe();
    test_table_load_merges_across_parts_of_speech();
    test_table_load_missing_directory_returns_null();
    test_lookup_nonexistent_word_returns_null();
    test_table_free_null_is_safe();
    test_table_load_real_data_hypertension_example();
    remove(TEST_DATA_FILE_PATH);
    remove(TEST_INDEX_FILE_PATH);
    remove(TEST_TABLE_DIR "/index.noun");
    remove(TEST_TABLE_DIR "/data.noun");
    remove(TEST_TABLE_DIR "/index.verb");
    remove(TEST_TABLE_DIR "/data.verb");
    remove(TEST_TABLE_DIR "/index.adj");
    remove(TEST_TABLE_DIR "/data.adj");
    remove(TEST_TABLE_DIR "/index.adv");
    remove(TEST_TABLE_DIR "/data.adv");
    rmdir(TEST_TABLE_DIR);
    return test_summary();
}
