---
tags: [eval]
---

# Eval Harness

**Retrieval quality, measured**: runs real query formulation + BM25 (never generation — MRR/Recall don't depend on what the model writes) against labeled queries and reports MRR@10, Recall@10, Recall@100.

Source: `src/core/eval.c` (CLI `lexis eval <queries_tsv> <qrels_tsv>`); `app/src/eval_main.cpp` (`lexis_eval`, links the app's own QueryWorker code so measurements exercise exactly what the UI runs); `scripts/depth_ab*` (answer-quality A/B at different retrieval depths).

- `--no-llm-expansion` scores plain lemmatized terms with no model load — the cheap baseline.
- Data: `data/eval/msmarco/queries_dev.tsv` + `qrels_dev.tsv` (6,980 dev queries, via `scripts/export_msmarco.sh`); DelucionQA under `data/eval/delucionqa/` for hands-on QA testing.

Current numbers (2026-08-13, 70 dev queries whose gold passage is inside the ingested 200K slice, post-[[Lemmatizer]]-fix):
- Plain terms: **MRR@10 0.219 / R@10 0.536 / R@100 0.769**
- With [[Query Formulation]]'s LLM+WordNet expansion: **0.142 / 0.414 / 0.690** — expansion currently hurts on every metric; redesign is the open question.

**Uses:** [[Query Formulation]], [[BM25 Search]], [[Postgres Store]], [[Config]].
