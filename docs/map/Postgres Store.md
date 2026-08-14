---
tags: [storage]
---

# Postgres Store

**All Postgres I/O lives here — the only module that issues SQL.** One native Homebrew postgresql@18 instance, port 5434, managed by `make pg-start`/`pg-stop`.

Source: `src/core/pg_store.c`.

The inverted index schema (created idempotently by `pg_store_open()`):
- `passages` — id, document_name (e.g. an MS MARCO pid), chunk_id, text, token_count.
- `terms` — id, unique term text.
- `postings` — (term_id, passage_id) → term_frequency, plus a denormalized token_count so [[BM25 Search]] never JOINs per posting.

Also owns:
- Transaction control and the transient staging tables `documents_raw`/`postings_staged` used by [[Bulk Ingest]] (dropped after every run).
- Multi-corpus scoping for the app: `pg_store_use_corpus()` points a connection at one group's schema, while `public` holds cross-group tables (`chat_sessions`, `chat_messages`, `corpus_summaries`, [[Query Log]] tables).
- Two databases on the one instance: `lexis` (real CLI + app) and `lexis_test` (the whole test suite).

**Used by:** everything that touches data — [[BM25 Search]], [[Generation]], [[Bulk Ingest]], [[Ingest Worker]], [[Query Worker]], [[Corpus Summary]], [[Query Log]], [[Eval Harness]].
