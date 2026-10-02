# Configuration

One file: `config/lexis.conf` (copy `config/lexis.conf.example` to
start). Simple `key=value` lines; `#` comments. Every setting has a
safe default when missing -- an absent config file runs fine.

## Settings

**`model_path`** -- the chat model (GGUF file) every answer comes
from. Default: `data/models/gemma-4-E4B-it-Q4_K_M.gguf`.
`scripts/download_model.sh` reads this line and fetches the right
file, so the script and the code can't disagree.

**`reranker_model_path`** -- the small embedding model that re-orders
search results by meaning. Unset = re-ranking off. Shipped setting:
`data/models/bge-small-en-v1.5-f16.gguf` (67MB). Also settable in the
app's Settings panel (live, no restart); disabling comments the line
out so the path survives for re-enabling. With no path in the file at
all there is nothing to enable, and the panel says so rather than
turning a switch on that would do nothing.

**`thinking`** -- `on` or `off`. Whether the chat model runs a
reasoning pass before answering. On is ~3x slower per answer; measured
quality on our test sets was the same with it off, so the shipped
setting is `off`. Only affects final answers -- routing and synonym
selection never use it. Settable in the app's Settings panel, where it
applies immediately (no restart) and is persisted back to this file.

**`db_conninfo`** -- the PostgreSQL connection string (host, port,
database, user, password). Required; there is no built-in default,
because the password belongs on your machine, not in the repo. This is
why `config/lexis.conf` itself is untracked and only the example file
is in git.

**`mode`** -- `testing` or `production`. Testing logs every pipeline
step (prompts, responses, timings) to the database for debugging;
production skips that logging.

**`chunk_size`**, **`chunk_overlap`** -- ingestion chunking in words
(defaults 200/40). Overlap 0 means no overlap; a huge chunk size
yields one passage per document. Changing these only affects the next
`bulk-ingest` (already-indexed passages keep their old chunking).

**`ingest_threads`** -- bulk-ingest worker threads (default 6).

**`candidate_ceiling`** -- how deep BM25 ranks before the trim
(default 40).

**`max_passages`** -- trimmed passages handed to the answer step
(default 12).

**`token_budget`** -- trim cap on passage tokens sent to the model
(default 1500).

**`score_floor_ratio`** -- drop passages scoring below this fraction
of the top score (default 0.6; 0.0 disables the floor).

**`bm25_k1`**, **`bm25_b`** -- BM25's ranking constants, TF
saturation and length norm (defaults 1.2/0.75, which beat the common
alternatives in our sweeps).

`thinking` and the reranker are settable from the app's Settings panel
and apply immediately -- except while a question is being answered, when
both switches are disabled: the reranker's live gate is read by the
query thread mid-search, so it is only changed between queries.
Everything else is read once at startup -- restart the app after
changing those by hand.

## Environment overrides (tuning experiments only)

Precedence everywhere: env var wins over this file, this file wins
over the compiled default. These exist so experiments don't need
rebuilds (or config edits). Leave them unset in normal use.

- `LEXIS_CHUNK_SIZE`, `LEXIS_CHUNK_OVERLAP` -- ingestion chunking
  (over `chunk_size` / `chunk_overlap`).
- `LEXIS_BM25_K1`, `LEXIS_BM25_B` -- BM25's two ranking constants
  (over `bm25_k1` / `bm25_b`).

## Models and data on disk

- `data/models/` -- the GGUF model files (not in git; run
  `scripts/download_model.sh` and fetch the reranker per
  [building.md](building.md)).
- `data/wordnet/` -- the WordNet dictionary (in git).
- `data/synonyms/` -- the learned synonym table (in git;
  regenerate with `scripts/build_synonym_table.py` if ever needed).
- `data/stopwords/` -- the filler-word list (in git).
