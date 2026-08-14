---
tags: [language]
---

# Tokenizer

**First step for all text, both at index time and query time**: lowercase, strip punctuation, split into tokens.

Source: `src/core/tokenizer.c`.

- Also home to `TokenList`, the generic growable string list reused across the whole codebase.
- Model-independent and boring on purpose — when retrieval misbehaves, the interesting bugs are usually one step later in the [[Lemmatizer]].

**Used by:** [[Query Formulation]], [[Ingest Primitives]]. **Feeds:** [[Stopword Filter]].
