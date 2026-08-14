---
tags: [query-path, llm]
---

# Corpus Summary

**The SUMMARY tool's backing store: one cached, model-generated overview per group.** Solves cost, not capability — the old READ tool re-fed whole documents through a 16k context on every broad question, the slowest thing in the app.

Source: `src/core/corpus_summary.c`.

- **Lazy**: built on the first broad question about a group, never during ingestion — keeps every LLM call on the one serialized path ([[App Controller]]'s contract with [[Local LLM Client]]).
- Cached in `public.corpus_summaries`; regenerated when the group's document count changes.
- Coverage is a token-budgeted **sample** (head of each document + evenly spaced excerpts), representative rather than exhaustive — it can miss a topic in an unsampled section. Accepted trade; see [[LIMITATIONS]].

**Called by:** [[Query Worker]] when [[Tool Router]] picks SUMMARY. **Uses:** [[Postgres Store]], [[Local LLM Client]].
