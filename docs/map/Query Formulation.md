---
tags: [query-path]
---

# Query Formulation

**Turns a question into search terms.** Two distinct variants live here — knowing which caller uses which matters:

Source: `src/core/query_formulation.c`.

**Variant 1 — WordNet+LLM expansion** (`query_formulation_formulate_query()` and its decomposed sub-steps). Used by [[CLI]] `query` and [[Eval Harness]]:
1. [[Tokenizer]] → [[Stopword Filter]] → [[Lemmatizer]] on the question.
2. [[WordNet]] lookup per surviving term: synonyms, hypernyms, hyponyms as candidates.
3. One [[Local LLM Client]] call: "select every word likely to appear in a relevant document." Falls back to the plain lemmatized terms if the call fails or returns unparseable JSON.

⚠ Measured 2026-08-13 on the 70 in-slice MS MARCO dev queries: this expansion is **net-negative** (MRR@10 0.142 vs 0.219 plain; selected terms aren't deduped, and generic expansions like `monarch`/`sovereign` outscore rare high-signal terms like `tut`). Redesign pending — see [[LIMITATIONS]].

**Variant 2 — history-aware contextualization** (`query_formulation_contextualize_question()` + `query_formulation_terms_union()`). Used by [[Query Worker]]:
- A plain LLM call rewrites follow-ups ("what about his father?") into standalone queries against chat history, then the raw and reformulated questions' terms are **unioned** — no WordNet expansion at all.

**Next in the flow:** [[BM25 Search]].
