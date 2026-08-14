---
tags: [ingestion]
---

# Bulk Ingest

**`lexis bulk-ingest <tsv>` — the three-phase, deferred-term-resolution pipeline** for MS MARCO-scale loads. Designed so worker threads never touch the `terms` table (the sole source of every deadlock ever measured here) until all workers are done.

Source: `src/core/bulk_ingest.c`.

1. **Phase 1 — raw append**: one `COPY ... FROM STDIN` streams the whole file into an UNLOGGED staging table. Input must be RFC4180 CSV (tab-delimited) — real passage text contains literal backslashes/quotes that Postgres's TEXT COPY format can't ingest safely. `scripts/export_msmarco.sh` produces exactly this.
2. **Phase 2 — parallel processing**: 6 worker threads claim row ranges, run [[Ingest Primitives]] (chunk → tokenize → stopword-filter → lemmatize → count), write real `passages`, stage postings **by term text** into `postings_staged`. 500 docs/transaction is safe because nothing here touches `terms`.
3. **Phase 3 — finalize**: drop `postings`' PK + both FKs and go UNLOGGED (the single largest speed lever measured — the FKs alone were ~6.5x), then one single-writer pass resolves distinct terms and joins staged postings into place; rebuild constraints, restore LOGGED.

Measured on the M5 (200K slice): **9,062 passages/sec** post-[[Lemmatizer]]-fix (13,595 before the fix's extra lookup; 7,779 on the old M2). Full 8.84M corpus ≈ 16 min, **not yet run**.

**Called by:** [[CLI]]. **Writes via:** [[Postgres Store]]. History: [[SPEED]].
