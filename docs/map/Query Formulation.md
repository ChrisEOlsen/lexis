---
tags: [query-path]
---

# Query Formulation

**Turns a question into search terms.** Two distinct variants live here — knowing which caller uses which matters:

Source: `src/core/query_formulation.c`.

**Variant 1 — sense-filtered WordNet expansion** (`query_formulation_formulate_query()` and its decomposed sub-steps). Used by [[CLI]] `query` and [[Eval Harness]]:
1. [[Tokenizer]] → [[Stopword Filter]] → [[Lemmatizer]] on the question.
2. [[WordNet]] lookup per surviving term: synonyms and hypernyms as candidates — **hyponyms deliberately not offered** (they enumerate the answer space: "which dynasty?" → `Bourbon_dynasty` pulls wrong-answer passages).
3. One [[Local LLM Client]] call whose job is **veto, not selection**: keep only candidates matching the question's intended sense. The original question terms are enforced in code — seated first and deduplicated in `parse_selected_terms()`, which also rejects expansions that weren't offered, aren't index-shaped (lowercased single words), or repeat. `original_count` marks the boundary so [[BM25 Search]] can discount expansions (weight 0.4).

Redesigned 2026-08-14 after the original "select every likely word" objective measured net-negative (MRR@10 0.142 vs 0.219 plain). Post-redesign: **0.222 / R@10 0.550 / R@100 0.776 — ahead of plain terms on every metric.** History in [[LIMITATIONS]].

**Variant 2 — history-aware contextualization** (`query_formulation_contextualize_question()` + `query_formulation_terms_union()`). Used by [[Query Worker]] *in front of* Variant 1:
- A plain LLM call rewrites follow-ups ("what about his father?") into standalone queries against chat history, the raw and reformulated questions' terms are **unioned**, and since 2026-08-14 that union feeds the same sense-filtered expansion via `query_formulation_gather_candidates_from_terms()` — one retrieval pipeline for both entry points.

**Next in the flow:** [[BM25 Search]].
