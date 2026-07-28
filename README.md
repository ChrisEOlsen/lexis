# LEXIS — Lexical Extraction and Inference System

A retrieval-augmented AI pipeline that replaces the vector embedding layer of
traditional RAG with a fast, lexical retrieval core written in C, backed by a
BM25-scored inverted index and an AI-powered query rewriting step.

See `LEXIS_Project_Specification.docx` for the full project specification.

## Layout

- `src/core/` — C retrieval/indexing core: tokenizer, stopword filter,
  SQLite-backed inverted index, BM25 scorer, ingestion pipeline, OpenRouter
  HTTP client, WordNet synonym/hypernym/hyponym lookup
- `src/sidecar/` — Python sidecar, currently scoped to just the optional
  cross-encoder reranker (Stage 9). POS tagging is deferred and synonym
  expansion is implemented natively in C — see LIMITATIONS.md
- `include/` — public headers for the C core
- `config/` — pipeline configuration templates
- `data/` — corpus, WordNet 3.0 database files (committed, real data),
  stopword lists, generated index
- `scripts/` — benchmarking and baseline comparison scripts
- `tests/` — C and Python test suites

## Build

```
make check
```

## Status

Per the spec's build order (section 7): Stages 1-5 are implemented and
tested — tokenizer, stopword filter, SQLite inverted index, BM25 scorer,
ingestion pipeline (single documents and whole directories), an OpenRouter
HTTP client, and a WordNet-backed synonym/hypernym/hyponym lookup table
built from the real WordNet 3.0 database. Stage 6 (Python sidecar POS
tagging) is deliberately deferred — see LIMITATIONS.md. Stages 7-9 (query
formulation wiring, end-to-end integration test, optional reranker) are not
yet started.
