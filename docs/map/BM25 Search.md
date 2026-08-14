---
tags: [query-path]
---

# BM25 Search

**The retrieval core: pure lexical scoring, no embeddings.** Scores every passage containing at least one query term with the standard BM25 formula and returns the top K.

Source: `src/core/bm25.c`.

Per search:
1. `bm25_corpus_stats()` once — total passage count and average length.
2. For each search term: look up its `terms.id` in [[Postgres Store]], pull every matching posting (term frequency + passage length riding along, denormalized to avoid a per-posting JOIN).
3. Accumulate scores into a hash-indexed result set (O(1) amortized per add — replaced an O(n) linear scan), sort descending, truncate.

Callers differ in K: [[CLI]] takes top-5 straight; [[Query Worker]] over-fetches to a candidate ceiling then `bm25_result_set_trim()` cuts by passage count, token budget, and score-floor ratio.

Two scoring refinements (2026-08-14):
- **Per-term weights** (`bm25_search_weighted()`): original question terms score at 1.0, [[Query Formulation]] expansions at 0.4 — an expansion can assist a passage but never let it outrank one matching the question itself. (Duplicate terms are also gone: the formulation parser dedups.)
- **Coordination bonus** (`BM25Params.coord_bonus`, default 0.25): a passage's score scales up as it matches more *distinct* query terms, so one corpus-frequent topic matching two terms nineteen times can't drown the single passage matching three.

**Reads:** [[Postgres Store]]. **Feeds:** [[Generation]].
