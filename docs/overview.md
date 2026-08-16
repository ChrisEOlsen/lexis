# Overview

LEXIS is a desktop app for asking questions about your own documents.
Everything runs on your machine: the search index, the language model,
and your files. Nothing is sent anywhere.

## The idea

Most document-chat tools work by converting text into embeddings
(vectors) and searching those. LEXIS takes a different route: classic
full-text search (BM25, the algorithm behind most search engines),
sharpened with a few modern additions:

- A small language model cleans up your question and picks good
  synonyms before searching.
- A tiny embedding model re-orders the search results by meaning, so
  the best passage ends up on top.
- A larger language model reads the top passages and writes the answer.

This keeps the whole system fast, inspectable, and cheap to run. You
can always open the Source panel and see exactly which search terms
ran and which passages the answer came from -- there is no black box
between your question and the result.

## The pieces

- **The app** (`app/`) -- a Qt desktop app. Create groups, drop
  documents in (PDF, DOCX, plain text, images via OCR), and chat.
- **The core** (`src/core/`) -- a C library that does everything:
  tokenizing, indexing, search, ranking, and talking to the models.
  The app and the command line share this exact code.
- **The CLI** (`lexis`) -- a command-line tool for bulk work: indexing
  large corpora and running quality measurements.
- **Storage** -- PostgreSQL holds the search index and chat history.
- **Models** -- two local GGUF files: a ~5GB chat model for answers and
  a ~67MB embedding model for re-ranking. Both run through llama.cpp
  on the GPU.

## Where to go next

- [architecture.md](architecture.md) -- what each module does
- [pipeline.md](pipeline.md) -- what happens when you ask a question
- [ingestion.md](ingestion.md) -- how documents become searchable
- [configuration.md](configuration.md) -- settings
- [building.md](building.md) -- build and run from source
- [evaluation.md](evaluation.md) -- how quality is measured, and the
  current numbers
