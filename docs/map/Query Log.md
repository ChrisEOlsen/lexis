---
tags: [storage, config]
---

# Query Log

**Pipeline observability, testing mode only**: every stage's inputs, outputs, and timing, keyed by query — the reason the king→k lemmatizer bug could be diagnosed from recorded prompts instead of guesswork.

Source: `src/core/query_log.c`.

Its tables (`queries`, `query_formulation_runs`, `search_runs`, `search_results`, `generation_runs`) record per query: the formulation prompt and the model's raw response, whether the fallback fired, the selected terms, per-stage latency, every search result with rank and score, and the generation prompt + answer.

- Gated by [[Config]]'s `mode` — production skips all of it (~2.5ms p50 measured overhead).
- Written via [[Postgres Store]]; read by humans (and debugging sessions) with plain SQL.

**Called by:** [[CLI]]'s query path.
