---
tags: [eval]
---

# Eval Harness

**Retrieval quality, measured**: runs real query formulation + BM25 (never generation — MRR/Recall don't depend on what the model writes) against labeled queries and reports MRR@10, Recall@10, Recall@100.

Source: `src/core/eval.c` (CLI `lexis eval <queries_tsv> <qrels_tsv>`); `app/src/eval_main.cpp` (`lexis_eval`, links the app's own QueryWorker code so measurements exercise exactly what the UI runs); `scripts/depth_ab*` (answer-quality A/B at different retrieval depths).

- `--no-llm-expansion` scores plain lemmatized terms with no model load — the cheap baseline.
- Data: `data/eval/msmarco/queries_dev.tsv` + `qrels_dev.tsv` (6,980 dev queries, via `scripts/export_msmarco.sh`); DelucionQA under `data/eval/delucionqa/` for hands-on QA testing.

Current numbers (2026-08-14, 70 dev queries whose gold passage is inside the ingested 200K slice, post-[[Lemmatizer]]-fix, post-formulation-redesign):
- Plain terms (+coord bonus): MRR@10 0.218 / R@10 0.536 / R@100 0.776
- Sense-filtered weighted expansion: **0.222 / 0.550 / 0.776** — expansion now ahead on every metric (it was 0.142 / 0.414 / 0.690 before the redesign).
- DelucionQA starter run (real app pipeline, re-ingested corpus): 30/30 routed SEARCH, 0 refusals, coverage mean 53.7%, mean 29.3s/answer.

**Uses:** [[Query Formulation]], [[BM25 Search]], [[Postgres Store]], [[Config]].
