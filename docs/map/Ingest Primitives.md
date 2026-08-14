---
tags: [ingestion]
---

# Ingest Primitives

**The document-to-index-rows toolkit** shared by every ingestion path: split text into overlapping word windows (chunk size 200, overlap 40), then per chunk run [[Tokenizer]] → [[Stopword Filter]] → [[Lemmatizer]], then dedup and count distinct terms.

Source: `src/core/ingest.c`.

- The same language pipeline [[Query Formulation]] runs at query time — index and query vocabulary must match by construction.
- Pure primitives: no I/O decisions of its own; callers own threading and storage.

**Used by:** [[Bulk Ingest]] (Phase 2 workers), [[Ingest Worker]] (app drag-and-drop).
