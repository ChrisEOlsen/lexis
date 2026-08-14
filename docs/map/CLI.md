---
tags: [entry]
---

# CLI

**`./lexis` — the command-line entry point**, older than the app and still the tool for bulk work.

Source: `src/core/main.c`.

Three subcommands:
- `lexis query "<question>"` — the CLI prompt→output path: [[Query Formulation]] (the full WordNet+LLM expansion variant, unlike [[Query Worker]]'s) → [[BM25 Search]] (top-5) → [[Generation]]. Logs every stage via [[Query Log]] in testing mode.
- `lexis bulk-ingest <tsv>` — [[Bulk Ingest]], the three-phase MS MARCO-scale pipeline.
- `lexis eval <queries> <qrels>` — [[Eval Harness]] retrieval metrics.

Reads `mode` and `model_path` from [[Config]]. Hardcodes the rest (conninfo, chunk size 200/overlap 40, top-K 5) — see [[LIMITATIONS]].
