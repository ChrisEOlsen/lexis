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
`data/models/bge-small-en-v1.5-f16.gguf` (67MB).

**`thinking`** -- `on` or `off`. Whether the chat model runs a
reasoning pass before answering. On is ~3x slower per answer; measured
quality on our test sets was the same with it off, so the shipped
setting is `off`. Only affects final answers -- routing and synonym
selection never use it.

**`mode`** -- `testing` or `production`. Testing logs every pipeline
step (prompts, responses, timings) to the database for debugging;
production skips that logging.

Settings are read once at startup. Restart the app after changing
them.

## Environment overrides (tuning experiments only)

These exist so retrieval experiments don't need rebuilds. Leave them
unset in normal use.

- `LEXIS_CHUNK_SIZE`, `LEXIS_CHUNK_OVERLAP` -- ingestion chunking
  (defaults 200/40).
- `LEXIS_BM25_K1`, `LEXIS_BM25_B` -- BM25's two ranking constants
  (defaults 1.2/0.75, which beat the common alternatives in our
  sweeps).

## Models and data on disk

- `data/models/` -- the GGUF model files (not in git; run
  `scripts/download_model.sh` and fetch the reranker per
  [building.md](building.md)).
- `data/wordnet/` -- the WordNet dictionary (in git).
- `data/synonyms/` -- the learned synonym table (in git;
  regenerate with `scripts/build_synonym_table.py` if ever needed).
- `data/stopwords/` -- the filler-word list (in git).
