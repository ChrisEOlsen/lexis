---
tags: [language]
---

# Stopword Filter

**Drops high-frequency, low-signal words** ("what", "did", "the") from a `TokenList` before they pollute the index or the query.

Source: `src/core/stopwords.c`; word list in `data/stopwords/english.txt` (committed).

**Used by:** [[Query Formulation]], [[Ingest Primitives]] — always between [[Tokenizer]] and [[Lemmatizer]] so both index and query sides agree.
