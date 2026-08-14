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

There is exactly one ingestion pipeline (`bulk_ingest.c`'s three-phase
design, see below) -- an earlier, separate directory-based pipeline
(`concurrent_ingest.c`, plus the shared in-memory `term_cache.c` it
depended on) was deleted outright once the three-phase pipeline proved
both faster and simpler to reason about, rather than being kept around
as a second, slower option. See SPEED.md for that pipeline's full
performance history before it was replaced.

## What LEXIS is

A retrieval-augmented pipeline that answers a question by: expanding it
with an LLM-assisted WordNet lookup, running a BM25 lexical search over a
Postgres-backed inverted index, and asking a local LLM to answer using
only the retrieved passages as context. No vector embeddings, no external
API calls at query time -- retrieval is pure lexical scoring (BM25) and
generation runs a locally hosted GGUF model via llama.cpp, in-process.

## Postgres

LEXIS runs on exactly one Postgres instance: native Homebrew
`postgresql@18`, port 5434, managed by `make pg-start`/`make pg-stop`
(does not auto-start on login). It serves everything -- the test suite
(`make check`, `TEST_CONNINFO` in every `tests/core/test_*.c`, database
`lexis_test`) and the real CLI (`./lexis bulk-ingest`/`query`/`eval`,
database `lexis`) -- as two separate databases on the one instance.

A separate Docker Postgres instance (port 5433) served the test suite
earlier in this project's history -- removed once the test suite was
verified passing against native Postgres too (Docker Desktop's macOS VM
networking layer adds real per-round-trip latency a native install
doesn't pay, which was already the reason the real CLI had moved off it;
see `SPEED.md`), so there was no reason left to run two separate
Postgres processes for one project. `docker-compose.yml` and `docker/`
no longer exist in this repo.

*(Port 5432 also has a Postgres server on this machine -- a pre-existing
`postgresql@14` install with other, unrelated projects' real data, e.g.
`pt_website_builder_*`. It has nothing to do with LEXIS; never touch
it.)*

5434 does not auto-start -- `make pg-start` must have been run before
either `make check` or `./lexis ...`.

## System components

| File | Role |
|------|------|
| `tokenizer.c` | Lowercasing, punctuation-stripping tokenization; `TokenList`, the generic growable string list reused everywhere below. |
| `stopwords.c` | Loads a stopword list, filters a `TokenList` against it. |
| `wordnet.c` | Loads WordNet 3.0's real database files into an in-memory synonym/hypernym/hyponym lookup table. |
| `lemmatizer.c` | Reduces a word to its base form (`"called"` -> `"call"`), WordNet-backed. Used at both index time and query time so the two sides match. |
| `pg_store.c` | All Postgres I/O: schema (`passages`/`terms`/`postings`), passage/term/posting reads and writes, transaction control, and the bulk-ingest staging tables (see below). The only module that issues SQL. |
| `ingest.c` | Document-chunking/tokenizing/lemmatizing primitives (split into overlapping word windows, tokenize, stopword-filter, lemmatize, dedup+count terms). Used exclusively by `bulk_ingest.c`'s Phase 2 worker -- see "Ingestion" below. |
| `bulk_ingest.c` | `lexis bulk-ingest <tsv>` -- the three-phase, deferred-term-resolution ingestion pipeline. The only ingestion path in this codebase. |
| `bm25.c` | BM25 scoring: corpus stats, per-term document frequency/IDF, the result-set hash index, `bm25_search()`. |
| `query_formulation.c` | Term/expansion primitives: tokenize/filter/lemmatize, WordNet candidate gathering, the sense-filter LLM prompt/parse, history-aware contextualization, terms union. |
| `retrieval.c` | THE shared retrieval orchestrator: `retrieval_run()` (terms -> sense-filtered expansion -> weighted+coordinated BM25 -> trim), run identically by the CLI, the app's QueryWorker, and eval. Caller differences are `RetrievalPolicy` values; per-stage artifacts come back in `RetrievalRun` for query_log and the app's source inspector. |
| `generation.c` | Builds the "answer using only this context" prompt from BM25 results and asks the local LLM for a final answer. |
| `local_llm_client.c` | llama.cpp wrapper -- loads one GGUF model once, serves every chat-completion call (query formulation and generation both use it). |
| `eval.c` | `lexis eval <queries_tsv> <qrels_tsv>` -- runs the real query-formulation + BM25 path against a labeled query set and reports MRR@10/Recall@10/Recall@100. |
| `query_log.c` | Optional pipeline observability (testing mode only) -- records every stage's inputs/outputs/timing to its own tables, keyed by `query_id`. |
| `config.c` | Reads `config/lexis.conf`: the `mode = testing|production` setting and `model_path` (the local GGUF model every binary loads; falls back to `LEXIS_DEFAULT_MODEL_PATH` in config.h). |
| `main.c` | CLI dispatch (`bulk-ingest`/`query`/`eval`) and orchestration of the above. |

`pg_store_get_or_create_term()`/`pg_store_get_or_create_terms()` (single
and batch term resolution against the real `terms` table) remain in
`pg_store.c` as tested, general-purpose API surface, but currently have
no production caller -- `bulk_ingest.c`'s Phase 2/3 resolve terms through
a different, staging-table-based path instead (see "Ingestion" below).
See `LIMITATIONS.md`.

## Database schema

Created by `pg_store_open()` on every connection (`IF NOT EXISTS`, safe
to run repeatedly):

```sql
CREATE TABLE passages (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    document_name TEXT NOT NULL,   -- a TSV row's own id (e.g. an MS MARCO pid)
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

## Ingestion (`lexis bulk-ingest <tsv>`, `bulk_ingest.c`)

A three-phase, deferred-term-resolution pipeline, built so worker
threads never touch the `terms` table -- the sole source of every
deadlock ever measured in this project (Postgres's `ON CONFLICT`
speculative insertion racing on `terms.term`'s unique index, see
`SPEED.md`) -- until every worker is done:

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
   `SELECT` range queries -- nothing to lock), run `ingest.c`'s
   tokenize/stopword-filter/lemmatize pipeline
   (`ingest_split_words` -> `ingest_chunk_words` -> `tokenize` ->
   `stopwords_filter` -> `ingest_lemmatize_terms` ->
   `ingest_count_distinct_terms`), insert real rows into `passages`,
   and stage each passage's postings by raw term *text* (not a resolved
   `terms.id`) into `postings_staged`. Because this phase never touches
   `terms`, batching many documents into one transaction is safe here
   (`BULK_PHASE2_BATCH_SIZE` = 500 docs/transaction) -- batching a
   transaction that *did* touch `terms` was tried once, on the earlier
   (now-deleted) per-document pipeline, and caused severe lock
   contention; see `SPEED.md` for why that failure mode is structurally
   absent here.

3. **Phase 3 -- finalize** (`pg_store_finalize_terms_and_postings`),
   bracketed by `pg_store_prepare_bulk_load()`/`pg_store_finish_bulk_
   load()`. `prepare` drops `postings`' PRIMARY KEY and both FOREIGN KEY
   constraints and sets `postings`/`terms` `UNLOGGED` (measured as the
   single largest lever in this whole pipeline -- the two foreign keys
   alone accounted for a ~6.5x difference, bigger than the PK, see
   `SPEED.md`). Once every Phase 2 worker has joined, one single-
   threaded, single-writer pass resolves every distinct staged term into
   `terms` (`INSERT ... SELECT DISTINCT ... ON CONFLICT (term) DO
   NOTHING`), then writes the real `postings` rows by joining
   `postings_staged` against `terms` on text. `work_mem` is raised to
   1GB for this connection only. `finish` then rebuilds the PK and both
   FKs (while still `UNLOGGED`, so the build itself generates no WAL)
   and restores `LOGGED` status -- `terms` before `postings`, since
   Postgres refuses to mark a table `LOGGED` while it holds a live FK
   pointing at a still-`UNLOGGED` table. `passages` is deliberately never
   touched by this -- `query_log.c`'s `search_results` table holds a
   `LOGGED` FK referencing it, and `passages` isn't written by Phase 3
   anyway. The staging tables are dropped once this all succeeds.

**Failure handling**: because Phase 1 is one atomic `COPY`, a single
malformed row (wrong column count, bad CSV) fails the *entire* load, not
just that row -- there's no per-row skip for a bulk loader whose whole
point is trusting the source file's format. Phase 2 batch failures, by
contrast, are tolerant: a batch that exhausts its retries is logged and
skipped (costing at most that batch's ~500 documents), not fatal to the
run. A failure between `prepare` and `finish` leaves `postings`/`terms`
without their constraints and `UNLOGGED` until the next successful run
restores them -- an accepted trade-off matching this pipeline's existing
"rebuildable, not crash-safe mid-run" philosophy, not a gap.

**Real measured throughput** (200K-row slice, native Postgres, 6
threads): **7,778.8 passages/sec**, verified correct (exact passage/term/
posting counts, zero duplicate postings, schema fully restored, a real
`lexis query` sanity check). Full corpus (8,841,823 rows) projected at
~18.9 minutes, down from this pipeline's original 42-minute projection
and the pre-redesign 151.5 minutes. See `SPEED.md` for the complete
performance history and the full per-phase breakdown this number comes
out of. **Not yet run at full corpus scale** -- see "Where things
actually stand" below.

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
make pg-start                 # native Postgres, port 5434 -- serves both the test suite and the real CLI
scripts/download_model.sh     # fetches the local GGUF model once
make check                    # build + run the full test suite
make lexis                    # build the CLI binary
```

**Corpus prep:** `scripts/export_msmarco.sh` (requires `brew install
duckdb`) exports the MS MARCO passage corpus (`BeIR/msmarco` on
HuggingFace) as a tab-delimited, **RFC4180 CSV-quoted** `<pid><TAB><text>`
file to `corpus_csv.tsv` at the repo root, and also fetches
`data/eval/msmarco/qrels_dev.tsv` + `queries_dev.tsv` (the 6,980-query
dev set) for `lexis eval`. Plain (non-CSV) TSV is not safe for the
corpus -- see "Ingestion" above.

**Ingest:**

```
./lexis bulk-ingest corpus_csv.tsv     # ~18.9 minutes projected for the full 8.84M-row corpus
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

- **`README.md`/`INGESTION.md`** describe the pre-Postgres, SQLite-backed
  system and are stale (see this file's intro).
- Everything else known-and-accepted (thread-count uncertainty, WordNet
  candidate ordering, LLM-response ambiguity in logging, etc.) is
  tracked in `LIMITATIONS.md`, not duplicated here.
