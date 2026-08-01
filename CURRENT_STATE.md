# Current State

A snapshot of what LEXIS actually is right now: every moving piece, how
they connect, and the exact order of operations to go from nothing to a
running, evaluated system. This is the "what" and "in what order" — for
"why" a given design was chosen over the alternatives, see `SPEED.md`
(ingestion performance history) and `LIMITATIONS.md` (known gaps and
their rationale). This file should be kept current as the system changes;
treat a stale entry here as a bug.

`README.md` and `INGESTION.md` are stale (they predate the Postgres
migration and describe a SQLite-backed pipeline that no longer exists) --
this file supersedes them until they're rewritten.

## What LEXIS is

A retrieval-augmented pipeline that answers a question by: expanding it
with an LLM-assisted WordNet lookup, running a BM25 lexical search over a
Postgres-backed inverted index, and asking a local LLM to answer using
only the retrieved passages as context. No vector embeddings, no external
API calls at query time -- retrieval is pure lexical scoring (BM25) and
generation runs a locally hosted GGUF model via llama.cpp, in-process.

## The three Postgres instances

This is the single easiest thing to get confused about -- there are
three separate Postgres servers on this machine, and mixing them up
means either touching real unrelated data or wondering why your changes
don't show up.

| Port | What | Managed by | Used by |
|------|------|------------|---------|
| 5432 | Pre-existing `postgresql@14`, **not part of this project** | Not LEXIS's concern | Other, unrelated projects on this machine (e.g. `pt_website_builder_*`). Never touch this. |
| 5433 | Docker `postgres:18` (`docker-compose.yml`) | `docker compose up -d` / `down` | The test suite exclusively (`make check`, `TEST_CONNINFO` in every `tests/core/test_*.c`, database `lexis_test`). Disposable -- `docker compose down -v` wipes it clean. |
| 5434 | Native Homebrew `postgresql@18` | `make pg-start` / `make pg-stop` | The real CLI (`./lexis ingest`/`bulk-ingest`/`query`/`eval`, database `lexis`). Moved off Docker because Docker Desktop's macOS VM networking layer adds real per-round-trip latency a native install doesn't pay (see `SPEED.md`). Does not auto-start on login. |

Neither 5433 nor 5434 auto-starts. Before running `make check`, `docker
compose up -d` must be running. Before running `./lexis ...` for real,
`make pg-start` must have been run.

## System components

| File | Role |
|------|------|
| `tokenizer.c` | Lowercasing, punctuation-stripping tokenization; `TokenList`, the generic growable string list reused everywhere below. |
| `stopwords.c` | Loads a stopword list, filters a `TokenList` against it. |
| `wordnet.c` | Loads WordNet 3.0's real database files into an in-memory synonym/hypernym/hyponym lookup table. |
| `lemmatizer.c` | Reduces a word to its base form (`"called"` -> `"call"`), WordNet-backed. Used at both index time and query time so the two sides match. |
| `pg_store.c` | All Postgres I/O: schema (`passages`/`terms`/`postings`), passage/term/posting reads and writes, transaction control, and the bulk-ingest staging tables (see below). The only module that issues SQL. |
| `term_cache.c` | Shared, thread-safe in-memory `term -> id` cache used by the directory-ingestion path (`concurrent_ingest.c`) to avoid a Postgres round trip for already-resolved terms. Not used by `bulk_ingest.c` -- see "Two ingestion paths." |
| `ingest.c` | The core per-document pipeline: split into chunks, tokenize, stopword-filter, lemmatize, persist. Used by both ingestion paths below. |
| `concurrent_ingest.c` | `lexis ingest <dir>` -- ingests every file in a directory across worker threads, each processing one whole document per transaction, coordinated through `term_cache.c`. |
| `bulk_ingest.c` | `lexis bulk-ingest <tsv>` -- the three-phase, deferred-term-resolution pipeline for large TSV corpora (MS MARCO scale). See "Two ingestion paths" below. |
| `bm25.c` | BM25 scoring: corpus stats, per-term document frequency/IDF, the result-set hash index, `bm25_search()`. |
| `query_formulation.c` | Turns a question into search terms: tokenize, stopword-filter, lemmatize, look up WordNet candidates, ask the local LLM which candidates to include. |
| `generation.c` | Builds the "answer using only this context" prompt from BM25 results and asks the local LLM for a final answer. |
| `local_llm_client.c` | llama.cpp wrapper -- loads one GGUF model once, serves every chat-completion call (query formulation and generation both use it). |
| `eval.c` | `lexis eval <queries_tsv> <qrels_tsv>` -- runs the real query-formulation + BM25 path against a labeled query set and reports MRR@10/Recall@10/Recall@100. |
| `query_log.c` | Optional pipeline observability (testing mode only) -- records every stage's inputs/outputs/timing to its own tables, keyed by `query_id`. |
| `config.c` | Reads `config/lexis.conf`'s `mode = testing|production` setting. |
| `main.c` | CLI dispatch (`ingest`/`bulk-ingest`/`query`/`eval`) and orchestration of the above. |

## Database schema

Created by `pg_store_open()` on every connection (`IF NOT EXISTS`, safe
to run repeatedly):

```sql
CREATE TABLE passages (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    document_name TEXT NOT NULL,   -- source filename, or a TSV row's own id (e.g. an MS MARCO pid)
    chunk_id INTEGER NOT NULL,
    text TEXT NOT NULL,
    token_count INTEGER NOT NULL
);

CREATE TABLE terms (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    term TEXT NOT NULL UNIQUE
);

CREATE TABLE postings (
    term_id BIGINT NOT NULL REFERENCES terms(id),
    passage_id BIGINT NOT NULL REFERENCES passages(id),
    term_frequency INTEGER NOT NULL,
    token_count INTEGER NOT NULL,  -- denormalized copy of passages.token_count -- see pg_store.c's schema comment for why (avoids a JOIN on every search)
    PRIMARY KEY (term_id, passage_id)
);
```

`query_log.c` owns four more tables (`queries`, `query_formulation_runs`,
`search_runs`, `search_results`, `generation_runs`), only populated in
testing mode.

`bulk_ingest.c` additionally owns two **transient** staging tables,
created fresh and dropped again by every `bulk_ingest_tsv()` call --
they should never be relied on existing outside of one in-progress bulk
ingest:

```sql
CREATE UNLOGGED TABLE documents_raw (
    row_num BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    pid TEXT NOT NULL,
    text TEXT NOT NULL
);

CREATE UNLOGGED TABLE postings_staged (
    passage_id BIGINT NOT NULL,
    term TEXT NOT NULL,
    term_frequency INTEGER NOT NULL,
    token_count INTEGER NOT NULL
);
```

## Two ingestion paths

Both end up calling the same core per-document logic in `ingest.c`
(`ingest_split_words` -> `ingest_chunk_words` -> `tokenize` ->
`stopwords_filter` -> `ingest_lemmatize_terms` -> `ingest_count_distinct_terms`),
but they resolve terms differently, and that difference is the whole
story of this project's ingestion-performance work (`SPEED.md`).

### `lexis ingest <dir>` -- directory ingestion (`concurrent_ingest.c`)

One worker thread per file (work-stealing over a shared file list), each
worker on its own Postgres connection, each document in its own
transaction. Terms are resolved live, per document, through the shared
`TermCache` -- a mutex-guarded hash map that turns "some other thread
already resolved this term" into an in-process lookup instead of a
Postgres round trip. A deadlock on Postgres's `terms.term` unique index
(the only thing that ever contends under concurrency) is retried up to 3
times per document before that document is logged and skipped.

Meant for smaller, directory-of-files corpora (the intended long-term
production ingestion path once documents arrive incrementally, not just
a one-time bulk load).

### `lexis bulk-ingest <tsv>` -- three-phase deferred-term-resolution (`bulk_ingest.c`)

Built specifically for MS MARCO-scale (8.84M row) one-shot loads, where
`concurrent_ingest.c`'s per-document term resolution -- even cached --
still contends on `terms.term`'s unique index badly enough to cap
achievable thread count. This pipeline restructures the work into three
phases so worker threads never touch the `terms` table at all until
every one of them is done:

1. **Phase 1 -- raw append** (`pg_store_copy_documents_raw`). One
   `COPY ... FROM STDIN` streams the entire input file into
   `documents_raw`, client-side, in 64KB chunks -- no per-row parsing,
   no per-row round trip. The input file must be RFC4180 CSV
   (tab-delimited, no header): real MS MARCO passage text contains
   literal, unescaped backslash and double-quote characters, which
   Postgres's default `COPY` TEXT format cannot safely ingest (backslash
   is that format's escape character). **The corpus must be exported
   with `FORMAT CSV` for this to be safe** -- see "Known gaps" below,
   this step is not yet scripted into the repo.

2. **Phase 2 -- parallel, contention-free processing.** Worker threads
   claim independent `row_num` ranges out of `documents_raw` (plain
   `SELECT` range queries -- nothing to lock), run the same
   tokenize/stopword-filter/lemmatize pipeline as the directory path,
   insert real rows into `passages`, and stage each passage's postings
   by raw term *text* (not a resolved `terms.id`) into
   `postings_staged`. Because this phase never touches `terms`, batching
   many documents into one transaction is safe here (`BULK_PHASE2_BATCH_SIZE`
   = 500 docs/transaction) -- unlike an earlier, fully reverted attempt
   to batch the *old* per-document pipeline this way, which caused
   severe lock contention because that pipeline's transactions did touch
   `terms` (see `SPEED.md`).

3. **Phase 3 -- finalize** (`pg_store_finalize_terms_and_postings`). Once
   every Phase 2 worker has joined, one single-threaded, single-writer
   pass resolves every distinct staged term into `terms`
   (`INSERT ... SELECT DISTINCT ... ON CONFLICT (term) DO NOTHING`),
   then writes the real `postings` rows by joining `postings_staged`
   against `terms` on text. `work_mem` is raised to 1GB for this
   connection only. The staging tables are dropped once this succeeds.

`term_cache.c` is not used anywhere in this path -- Phase 2 has nothing
for it to coordinate.

**Failure handling differs meaningfully from the directory path**: because
Phase 1 is one atomic `COPY`, a single malformed row (wrong column count,
bad CSV) fails the *entire* load, not just that row -- there's no
per-row skip for a bulk loader whose whole point is trusting the source
file's format. Phase 2 batch failures, by contrast, are tolerant: a
batch that exhausts its retries is logged and skipped (costing at most
that batch's ~500 documents), not fatal to the run.

**Real measured throughput** (200K-row slice, native Postgres, 6
threads): **3490.9 passages/sec**, verified correct (exact passage/term/
posting counts, zero duplicate postings, a real `lexis query` sanity
check). Full corpus (8,841,823 rows) projected at ~42 minutes. See
`SPEED.md` for the complete performance history this number comes out
of. **Not yet run at full corpus scale** -- see "Where things actually
stand" below.

## Query pipeline (`lexis query "<question>"`)

1. **Query formulation** (`query_formulation.c`): tokenize + stopword-filter
   the question, lemmatize each surviving term, look up WordNet
   synonyms/hypernyms/hyponyms for each, build a prompt listing the
   candidates, ask the local LLM which ones to actually search with.
   Falls back to the plain lemmatized terms if the LLM call fails or
   returns something unparseable.
2. **BM25 search** (`bm25.c`): `bm25_corpus_stats()` once (total passage
   count + average length), then for each search term, look up its
   `terms.id`, pull every matching posting, score via the standard BM25
   formula, accumulate into a hash-indexed result set (O(1) amortized
   per add, not the O(n) linear scan an earlier version used), sort
   descending, truncate to `LEXIS_TOP_K` (5).
3. **Generation** (`generation.c`): build a "use only this context"
   prompt from the top results' actual passage text, ask the local LLM
   for a final answer.

Testing mode (`config/lexis.conf`) logs every stage's inputs/outputs/
timing via `query_log.c`; production mode skips that logging entirely.

## Eval (`lexis eval <queries_tsv> <qrels_tsv>`)

Runs query formulation + BM25 search (never generation -- retrieval
metrics don't depend on what the LLM says) against every row in
`queries_tsv` (`<query_id><TAB><query_text>`, no header), scores the
results against `qrels_tsv` (`query-id<TAB>corpus-id<TAB>score`, header
required, e.g. BeIR/msmarco-qrels' `dev.tsv`), and reports macro-averaged
MRR@10, Recall@10, and Recall@100 -- the standard reporting points for
MS MARCO passage-ranking baselines. Prints running metrics + ETA every
50 queries since a full 6,980-query run is expected to take on the order
of tens of minutes to hours; meant to run as one long-lived process, not
one CLI invocation per query, since loading the local model costs ~9-19s
by itself (see `LIMITATIONS.md`).

## Order of operations, start to finish

**One-time setup:**

```
docker compose up -d          # Postgres for the test suite (port 5433)
make pg-start                 # native Postgres for the real CLI (port 5434)
scripts/download_model.sh     # fetches the local GGUF model once
make check                    # build + run the full test suite against Docker
make lexis                    # build the CLI binary
```

**Corpus prep (currently manual, not yet scripted -- see "Known gaps"):**
export the MS MARCO passage corpus (`BeIR/msmarco` on HuggingFace) as a
tab-delimited, **RFC4180 CSV-quoted** `<pid><TAB><text>` file via duckdb:

```sql
INSTALL httpfs; LOAD httpfs;
COPY (SELECT _id, text FROM 'hf://datasets/BeIR/msmarco/corpus/*.parquet')
TO 'corpus_csv.tsv' (FORMAT CSV, DELIMITER '\t', HEADER false);
```

Plain (non-CSV) TSV is not safe here -- see "Two ingestion paths" above.

**Ingest:**

```
./lexis bulk-ingest corpus_csv.tsv     # ~42 minutes projected for the full 8.84M-row corpus
```

If re-running against a database that already has data in it, truncate
first: `TRUNCATE postings, terms, passages RESTART IDENTITY CASCADE;`

**Use it:**

```
./lexis query "some question"
./lexis eval queries.tsv qrels/dev.tsv
```

## Where things actually stand right now

- Ingestion, search, and generation all work end-to-end and are verified
  correct at up to 200K-row scale (real data, including the specific
  backslash/quote edge cases that motivated the CSV re-export).
- **The full 8.84M-row corpus has not yet been ingested with the current
  (three-phase) pipeline** -- the native database currently holds only
  the 200K-row correctness/benchmark run's data. This is the next step,
  paused on purpose pending this review.
- **The full 6,980-query eval has not yet been run** -- depends on the
  full ingest above.

## Known gaps

- **Corpus export isn't scripted.** The duckdb `FORMAT CSV` export shown
  above was run ad hoc this session and isn't captured anywhere in
  `scripts/` -- `scripts/download_msmarco.py` is currently just a
  docstring stub. Re-deriving `corpus_csv.tsv` from scratch today means
  re-typing that duckdb command by hand.
- **`data/index/lexis.db`** is a leftover SQLite file from before the
  Postgres migration. Nothing reads it anymore; it's dead weight.
- **`README.md`/`INGESTION.md`** describe the pre-Postgres, SQLite-backed
  system and are stale (see this file's intro).
- Everything else known-and-accepted (thread-count uncertainty, WordNet
  candidate ordering, LLM-response ambiguity in logging, etc.) is
  tracked in `LIMITATIONS.md`, not duplicated here.
