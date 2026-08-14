---
tags: [language]
---

# WordNet

**The real WordNet 3.0 database, loaded into an in-memory lookup table**: synonyms, hypernyms (broader), hyponyms (narrower) per word.

Source: `src/core/wordnet.c`; data files committed under `data/wordnet/` (index.\*, data.\*, \*.exc for noun/verb/adj/adv).

- Powers [[Query Formulation]]'s expansion candidates and validates [[Lemmatizer]] outputs.
- Knows only dictionary words: no proper nouns like "Tut" (only "tutankhamen", spelled -en), which is why expansion candidates can misrepresent a query term's actual sense.

**Used by:** [[Lemmatizer]], [[Query Formulation]].
