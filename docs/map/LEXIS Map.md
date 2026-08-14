---
tags: [hub]
---

# LEXIS Map

Hub note for the architecture graph. Open Obsidian's graph view (or this note's local graph) to see the structure; every node explains one component and links to what it calls.

LEXIS answers questions with **no vector embeddings**: lexical BM25 retrieval over a Postgres inverted index, plus a local GGUF model (in-process llama.cpp) for routing, query help, and grounded answering.

## Prompt → output, the two paths

**Desktop app chat** (the primary path):
[[Qt App UI]] → [[App Controller]] → [[Query Worker]] → [[Tool Router]] picks one of three:
- SEARCH: [[Query Formulation]] (history-aware reformulation + the same sense-filtered WordNet expansion the CLI runs) → [[BM25 Search]] (weighted, coordinated) → [[Generation]] → answer with source citations
- SUMMARY: [[Corpus Summary]] → answer from the cached group overview
- CHAT: [[Local LLM Client]] directly → conversational answer, no retrieval

**CLI** (`./lexis query "..."`):
[[CLI]] → [[Query Formulation]] (same expansion, minus chat history) → [[BM25 Search]] → [[Generation]]
Since 2026-08-14 the two paths run ONE retrieval pipeline — same expansion machinery, same weights, same depth/trim policy (`LEXIS_SEARCH_*` in bm25.h).

## Ingestion paths

- CLI bulk: [[CLI]] → [[Bulk Ingest]] (three-phase, MS MARCO scale) → [[Postgres Store]]
- App drag-and-drop: [[App Controller]] → [[Ingest Worker]] → [[Document Extractors]] → [[Ingest Primitives]] → [[Postgres Store]]

## Supporting cast

[[Tokenizer]], [[Stopword Filter]], [[Lemmatizer]], [[WordNet]] — the language pipeline both sides share.
[[Config]], [[Query Log]], [[Model Loader]], [[Jinja Chat Template]], [[Eval Harness]].

## Deeper prose docs (repo root)

[[CURRENT_STATE]] — the full operational snapshot. [[LIMITATIONS]] — known gaps and their rationale. [[SPEED]] — ingestion performance history. [[APP_SPEC]] — the app's design spec.
