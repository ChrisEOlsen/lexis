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

Known sharp edge (2026-08-13): duplicate search terms are **not deduped**, so a term selected twice by [[Query Formulation]]'s expansion double-counts its score contribution.

**Reads:** [[Postgres Store]]. **Feeds:** [[Generation]].
