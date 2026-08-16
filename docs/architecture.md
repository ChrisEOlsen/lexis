# Architecture

Two builds share one core:

- The **Makefile** builds the CLI (`lexis`) and the test suite from
  `src/core/` -- plain C11.
- The **CMake project** in `app/` builds the Qt desktop app and links
  the same core files as a static library.

## Core modules (`src/core/`)

**Text processing**
- `tokenizer.c` -- lowercases and splits text into words.
- `stopwords.c` -- drops filler words ("the", "how", "is").
- `lemmatizer.c` -- reduces words to their base form ("called" ->
  "call") using WordNet's rules, so a question and a document match
  even when their word forms differ. Runs at both index time and
  question time.
- `wordnet.c` -- loads the WordNet dictionary (shipped in
  `data/wordnet/`) for synonym lookups.
- `synonym_table.c` -- a second synonym source, learned from word
  embeddings and shipped as a data file (`data/synonyms/`). Covers
  related words a dictionary doesn't link, like "functions" and
  "controls".

**Retrieval**
- `query_formulation.c` -- turns a question into search terms and
  offers synonym candidates to the model, which keeps only the ones
  that fit the question's meaning.
- `bm25.c` -- the search itself: scores every passage that shares
  words with the query, with per-term weights and a bonus for
  passages matching more distinct terms.
- `reranker.c` -- embeds the query and the top 40 candidates with a
  small model and re-orders them by meaning. Optional; enabled by a
  config line.
- `retrieval.c` -- the orchestrator. One function, `retrieval_run()`,
  runs the whole sequence above. The app, the CLI, and the evaluation
  harness all call this same function -- differences between them are
  policy values (search depth, whether to expand), never separate
  code paths.

**Answering**
- `generation.c` -- builds the "answer only from these passages"
  prompt and gets the answer from the chat model.
- `tool_router.c` -- app only: decides per message whether to search,
  summarize the whole group, or just reply conversationally.
- `corpus_summary.c` -- builds and caches a one-time overview of a
  group, used for "what is this collection about?" questions.
- `local_llm_client.c` -- the llama.cpp wrapper. One chat model per
  process, loaded once, all layers on the GPU.

**Storage and ingestion**
- `pg_store.c` -- all PostgreSQL access. The index is three tables:
  passages, terms, and postings (which term appears in which passage,
  how often). Each app group gets its own schema; chat history lives
  in shared tables.
- `ingest.c` -- splits documents into ~200-word overlapping chunks and
  runs the text processing above on each.
- `bulk_ingest.c` -- the high-throughput path for large corpora:
  a three-phase design that keeps worker threads from ever contending
  on the terms table. Roughly 9,000 passages/second.
- `config.c` -- reads `config/lexis.conf`.

## The app (`app/src/`)

- `AppController` -- the object QML talks to; owns models and worker
  threads, and makes sure only one thing talks to the LLM at a time.
- `QueryWorker` -- runs one chat question end to end on a background
  thread (see [pipeline.md](pipeline.md)).
- `IngestWorker` -- turns dropped files into indexed passages.
- `PdfExtractor` / `DocxExtractor` / `OcrExtractor` -- file format
  adapters (poppler, pugixml+libzip, tesseract).
- `qml/` -- the interface: group sidebar, chat panel, source
  inspector.

## Design rules that keep this maintainable

1. **One retrieval pipeline.** Anything that changes how search works
   is one edit in `retrieval.c` and applies to the app, the CLI, and
   every measurement at once.
2. **Observers read, they don't re-run.** The retrieval call returns
   its artifacts (search terms, prompts, timings); logging and the
   Source panel read that record instead of re-deriving it.
3. **Optional features fail quiet and off.** No reranker model
   configured? No reranking. Synonym file missing? No learned
   synonyms. The pipeline never breaks because an extra is absent.
