# LEXIS

**Purpose: Chat with very large documents completely offline.**

Drop your documents into a group -- manuals, papers, contracts,
scanned files -- and ask questions in plain language. LEXIS finds the
right passages and writes a grounded answer, with the sources one
click away. Nothing leaves your machine: no API keys, no cloud, no
per-question cost.

## What makes it different

Most document-chat tools embed everything into vectors and search
those. LEXIS uses classic full-text search instead -- the same family
of algorithms behind real search engines -- sharpened with local
models where they help:

- a small model refines your question and picks meaning-appropriate
  synonyms before searching,
- a tiny embedding model re-orders the results so the best passage
  leads,
- a local chat model writes the answer from those passages and
  nothing else.

The payoff is a system you can see into. The Source panel shows the
exact search that ran and the exact passages the answer came from.
Indexing is pure text processing -- a thousand documents take minutes,
not hours -- and the index never needs rebuilding when models change.

## Where it stands

Measured, not estimated (details in [docs/evaluation.md](docs/evaluation.md)):

- On a 913-question benchmark against a 900-page manual: the right
  passage reaches the model 97% of the time, 99% of answers are
  faithful to their sources, ~8.6s per answer, all local.
- On the standard BEIR retrieval benchmark, LEXIS outscores the
  published BM25 baseline and the embedding retrievers most RAG
  systems were built on.

## Documentation

- [docs/overview.md](docs/overview.md) -- what LEXIS is and how the pieces fit
- [docs/pipeline.md](docs/pipeline.md) -- what happens when you ask a question
- [docs/architecture.md](docs/architecture.md) -- the codebase, module by module
- [docs/ingestion.md](docs/ingestion.md) -- how documents become searchable
- [docs/configuration.md](docs/configuration.md) -- settings
- [docs/building.md](docs/building.md) -- build and run from source
- [docs/packaging.md](docs/packaging.md) -- build the one-file installer (DMG)
- [docs/evaluation.md](docs/evaluation.md) -- quality measurement and current numbers

## Status

Working desktop app (macOS, Apple Silicon). Build it from source
([docs/building.md](docs/building.md)) or package it as a one-file
installer with a first-run model download
([docs/packaging.md](docs/packaging.md)).
