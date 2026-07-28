/*
 * WordNet-style morphological lemmatizer. Reduces an inflected word
 * ("called", "installed") to its WordNet base form ("call", "install")
 * so query terms and indexed terms can be matched/looked-up consistently
 * regardless of grammatical inflection. Implements the same algorithm
 * WordNet's own morphy processor uses: check a curated exception list
 * of irregular forms first, then try a small set of suffix-stripping
 * rules per part of speech, validating each candidate against the real
 * WordNet table before accepting it (a blind suffix strip can produce
 * nonsense -- e.g. stripping "s" from "gas" gives "ga").
 */

#ifndef LEXIS_LEMMATIZER_H
#define LEXIS_LEMMATIZER_H

#include <stddef.h>

#include "wordnet.h"

/* One exception-list entry: an irregular inflected form mapped to one or
 * more base forms (most entries have exactly one; a handful, like
 * "appalled" -> "appal"/"appall", have two). These come directly from
 * WordNet's own noun.exc/verb.exc/adj.exc/adv.exc files -- curated,
 * trusted data, not a guess a suffix rule produced. */
typedef struct {
    char *inflected;
    char **bases;
    size_t base_count;
} LemmatizerException;

/* Every exception entry from all four of WordNet's .exc files, merged
 * into one table sorted by inflected form. Merged rather than kept
 * separate per part of speech because there's no POS tagger in this
 * pipeline (deferred, see LIMITATIONS.md) to pick the "right" file to
 * consult -- all four are searched together. */
typedef struct {
    LemmatizerException **exceptions;
    size_t count;
} Lemmatizer;

/* Loads all four exception files (noun.exc, verb.exc, adj.exc, adv.exc)
 * from `wordnet_dir` (e.g. "data/wordnet"). Returns NULL on any file
 * load or allocation failure. */
Lemmatizer *lemmatizer_load(const char *wordnet_dir);

/* Frees every exception entry, the exceptions array, and the struct
 * itself. Safe to call with lemmatizer == NULL. */
void lemmatizer_free(Lemmatizer *lemmatizer);

/* Reduces `word` to its WordNet base form. Checks the exception list
 * first (trusted as-is, no further validation -- it's already curated
 * WordNet data); if no exception matches, tries WordNet's own suffix-
 * stripping rules (noun/verb/adjective), validating each candidate
 * against `wordnet` via wordnet_lookup() before accepting it. Returns a
 * heap-allocated copy of `word` itself, unchanged, if nothing validates
 * (e.g. a proper noun, or a word already in base form) -- callers never
 * need to special-case "was this actually lemmatized," the return value
 * is always owned and always usable. Caller must free() the result. */
char *lemmatize(const Lemmatizer *lemmatizer, const WordNetTable *wordnet, const char *word);

#endif /* LEXIS_LEMMATIZER_H */
