/*
 * Implementation of the WordNet-style morphological lemmatizer.
 * See include/lemmatizer.h for the module's role.
 */

/* See tokenizer.c for why this must come before any #include (strdup and
 * strtok_r are POSIX extensions hidden by glibc under strict -std=c11
 * otherwise). */
#define _POSIX_C_SOURCE 200809L

#include "lemmatizer.h"

#include "ingest.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void lemmatizer_exception_free(LemmatizerException *exception) {
    if (exception == NULL) {
        return;
    }

    free(exception->inflected);
    if (exception->bases != NULL) {
        for (size_t i = 0; i < exception->base_count; i++) {
            free(exception->bases[i]);
        }
        free(exception->bases);
    }
    free(exception);
}

void lemmatizer_free(Lemmatizer *lemmatizer) {
    if (lemmatizer == NULL) {
        return;
    }

    for (size_t i = 0; i < lemmatizer->count; i++) {
        lemmatizer_exception_free(lemmatizer->exceptions[i]);
    }
    free(lemmatizer->exceptions);
    free(lemmatizer);
}

/* Format (WordNet's noun.exc/verb.exc/adj.exc/adv.exc): "inflected base1
 * [base2 ...]" -- one or more base forms per line, space-separated. No
 * header lines in these files, unlike index.<pos>/data.<pos>. */
static LemmatizerException *parse_exception_line(const char *line) {
    char *mutable_line = strdup(line);
    if (mutable_line == NULL) {
        return NULL;
    }

    LemmatizerException *exception = calloc(1, sizeof(LemmatizerException));
    if (exception == NULL) {
        free(mutable_line);
        return NULL;
    }

    char *saveptr;
    char *token = strtok_r(mutable_line, " ", &saveptr);
    if (token == NULL) {
        goto fail;
    }
    exception->inflected = strdup(token);
    if (exception->inflected == NULL) {
        goto fail;
    }

    size_t capacity = 0;
    token = strtok_r(NULL, " ", &saveptr);
    while (token != NULL) {
        if (exception->base_count == capacity) {
            size_t new_capacity = (capacity == 0) ? 2 : capacity * 2;
            char **new_bases = realloc(exception->bases, new_capacity * sizeof(char *));
            if (new_bases == NULL) {
                goto fail;
            }
            exception->bases = new_bases;
            capacity = new_capacity;
        }
        char *base_copy = strdup(token);
        if (base_copy == NULL) {
            goto fail;
        }
        exception->bases[exception->base_count++] = base_copy;

        token = strtok_r(NULL, " ", &saveptr);
    }

    if (exception->base_count == 0) {
        /* An inflected form with no base forms at all -- malformed. */
        goto fail;
    }

    free(mutable_line);
    return exception;

fail:
    free(mutable_line);
    lemmatizer_exception_free(exception);
    return NULL;
}

static int load_exception_file(Lemmatizer *lemmatizer, const char *path, size_t *capacity) {
    char *text = ingest_read_file(path);
    if (text == NULL) {
        return -1;
    }

    char *saveptr;
    char *line = strtok_r(text, "\n", &saveptr);
    while (line != NULL) {
        if (line[0] != '\0') {
            LemmatizerException *exception = parse_exception_line(line);
            if (exception == NULL) {
                fprintf(stderr, "lemmatizer_load: failed to parse a line in %s\n", path);
                free(text);
                return -1;
            }

            if (lemmatizer->count == *capacity) {
                size_t new_capacity = (*capacity == 0) ? 256 : *capacity * 2;
                LemmatizerException **new_exceptions =
                    realloc(lemmatizer->exceptions, new_capacity * sizeof(LemmatizerException *));
                if (new_exceptions == NULL) {
                    lemmatizer_exception_free(exception);
                    free(text);
                    return -1;
                }
                lemmatizer->exceptions = new_exceptions;
                *capacity = new_capacity;
            }

            lemmatizer->exceptions[lemmatizer->count++] = exception;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(text);
    return 0;
}

static int compare_exceptions(const void *a, const void *b) {
    const LemmatizerException *const *exception_a = (const LemmatizerException *const *)a;
    const LemmatizerException *const *exception_b = (const LemmatizerException *const *)b;
    return strcmp((*exception_a)->inflected, (*exception_b)->inflected);
}

Lemmatizer *lemmatizer_load(const char *wordnet_dir) {
    Lemmatizer *lemmatizer = malloc(sizeof(Lemmatizer));
    if (lemmatizer == NULL) {
        return NULL;
    }
    lemmatizer->exceptions = NULL;
    lemmatizer->count = 0;
    size_t capacity = 0;

    static const char *filenames[] = {"noun.exc", "verb.exc", "adj.exc", "adv.exc"};
    for (size_t i = 0; i < sizeof(filenames) / sizeof(filenames[0]); i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", wordnet_dir, filenames[i]);
        if (load_exception_file(lemmatizer, path, &capacity) != 0) {
            lemmatizer_free(lemmatizer);
            return NULL;
        }
    }

    qsort(lemmatizer->exceptions, lemmatizer->count, sizeof(LemmatizerException *), compare_exceptions);
    return lemmatizer;
}

static int compare_word_to_exception(const void *key, const void *element) {
    const char *const *word_ptr = (const char *const *)key;
    const LemmatizerException *const *exception_ptr = (const LemmatizerException *const *)element;
    return strcmp(*word_ptr, (*exception_ptr)->inflected);
}

/* One suffix-stripping rule: replace `suffix` at the end of a word with
 * `replacement` (often empty). Tables below are WordNet's own morphy
 * rules verbatim, not approximated. */
typedef struct {
    const char *suffix;
    const char *replacement;
} SuffixRule;

static const SuffixRule NOUN_RULES[] = {
    {"s", ""}, {"ses", "s"}, {"xes", "x"}, {"zes", "z"},
    {"ches", "ch"}, {"shes", "sh"}, {"men", "man"}, {"ies", "y"},
};

static const SuffixRule VERB_RULES[] = {
    {"s", ""}, {"ies", "y"}, {"es", "e"}, {"es", ""},
    {"ed", "e"}, {"ed", ""}, {"ing", "e"}, {"ing", ""},
};

static const SuffixRule ADJECTIVE_RULES[] = {
    {"er", ""}, {"est", ""}, {"er", "e"}, {"est", "e"},
};

static int ends_with(const char *word, const char *suffix) {
    size_t word_len = strlen(word);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > word_len) {
        return 0;
    }
    return strcmp(word + (word_len - suffix_len), suffix) == 0;
}

static char *strip_and_replace(const char *word, const char *suffix, const char *replacement) {
    size_t word_len = strlen(word);
    size_t suffix_len = strlen(suffix);
    size_t stem_len = word_len - suffix_len;
    size_t replacement_len = strlen(replacement);

    char *candidate = malloc(stem_len + replacement_len + 1);
    if (candidate == NULL) {
        return NULL;
    }

    memcpy(candidate, word, stem_len);
    memcpy(candidate + stem_len, replacement, replacement_len);
    candidate[stem_len + replacement_len] = '\0';
    return candidate;
}

/* Tries every rule in `rules`, in order, returning the first candidate
 * that validates against `wordnet` (i.e. wordnet_lookup() finds it).
 * Returns NULL if no rule produces a valid candidate -- including on a
 * rare allocation failure partway through, which is treated the same as
 * "no match" here rather than a hard error: the caller's fallback
 * (return the original word unchanged) is always safe and correct, so
 * there's nothing gained by propagating a malloc failure from a single
 * rule attempt as distinct from "this rule just didn't apply". */
static char *try_rule_table(const WordNetTable *wordnet, const char *word,
                             const SuffixRule *rules, size_t rule_count) {
    for (size_t i = 0; i < rule_count; i++) {
        if (!ends_with(word, rules[i].suffix)) {
            continue;
        }
        char *candidate = strip_and_replace(word, rules[i].suffix, rules[i].replacement);
        if (candidate == NULL) {
            continue;
        }
        if (wordnet_lookup(wordnet, candidate) != NULL) {
            return candidate;
        }
        free(candidate);
    }
    return NULL;
}

char *lemmatize(const Lemmatizer *lemmatizer, const WordNetTable *wordnet, const char *word) {
    /* 1. Exception list first -- already-curated WordNet data, trusted
     * as-is, no re-validation against wordnet_lookup() needed. */
    LemmatizerException **found =
        bsearch(&word, lemmatizer->exceptions, lemmatizer->count,
                sizeof(LemmatizerException *), compare_word_to_exception);
    if (found != NULL) {
        char *result = strdup((*found)->bases[0]);
        if (result != NULL) {
            return result;
        }
        /* strdup failed -- fall through to the unchanged-word path below
         * rather than returning NULL for what should be a safe lookup. */
    }

    /* 2. The word as given is already a WordNet base form -- return it
     * unchanged before any suffix rule can touch it, as real morphy
     * does. Without this guard, base forms that merely look inflected
     * get mangled whenever the stripped stem happens to validate
     * cross-POS: "king" -> {"ing",""} -> "k" (a real WordNet noun --
     * potassium), "ring" -> {"ing","e"} -> "re", "sing" -> "se".
     * Must stay AFTER the exception list: "saw" is in WordNet as a noun
     * but verb.exc still maps it to "see", and exceptions win. */
    if (wordnet_lookup(wordnet, word) != NULL) {
        return strdup(word);
    }

    /* 3. Suffix rules, validated against the real table. */
    char *candidate = try_rule_table(wordnet, word, NOUN_RULES,
                                      sizeof(NOUN_RULES) / sizeof(NOUN_RULES[0]));
    if (candidate == NULL) {
        candidate = try_rule_table(wordnet, word, VERB_RULES,
                                    sizeof(VERB_RULES) / sizeof(VERB_RULES[0]));
    }
    if (candidate == NULL) {
        candidate = try_rule_table(wordnet, word, ADJECTIVE_RULES,
                                    sizeof(ADJECTIVE_RULES) / sizeof(ADJECTIVE_RULES[0]));
    }
    if (candidate != NULL) {
        return candidate;
    }

    /* 4. Nothing matched -- not a WordNet word at all (a proper noun, a
     * made-up word, etc.); base forms already returned at step 2. */
    return strdup(word);
}
