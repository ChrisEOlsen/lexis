# Testing Strategy

The shelved measurement plan, in execution order. Each step gates the
next: the point is to make the embeddings decision (step 4) from real
failure data, not from anecdotes. Written 2026-08-14, right after the
formulation redesign landed (sense-filtered expansion + weighted BM25 +
coordination bonus -- see LIMITATIONS.md for the numbers each change
was verified against). Update this file as steps complete; like
CURRENT_STATE.md, a stale entry here is a bug.

Prerequisites for everything below: `make pg-start`, the model on disk
(`scripts/download_model.sh`), binaries built (`make lexis`, cmake build
in `app/`).

## 1. The 913-question DelucionQA overnight run

Every unique DelucionQA question through the real app pipeline
(QueryWorker: routing -> reformulation -> BM25 -> generation), against
the re-ingested corpus (id 2, built by `scripts/ingest_group.c` with
the fixed lemmatizer).

```
# questions file: every unique question, extracted the same way
# scripts/phase0_run.sh step 1 does (from data/eval/delucionqa/raw/)
./app/build/lexis_eval 2 <questions_file> > run_913.tsv
```

Score with `scripts/pipeline_eval_score.py <raw_dir> <passages_tsv>
run_913.tsv data/stopwords/english.txt` (passages_tsv is a psql dump of
corpus_2's passages -- see phase0_run.sh step 2 for the exact query).

- Expected duration: ~29s/question x 913 ≈ **7.5 hours** -- genuinely
  overnight. The 30-question starter subset (already run: 30/30 routed
  SEARCH, 0 refusals, ~90% correct by reading) took 15 minutes.
- What to read out of it: the **failure distribution** -- routing
  misses vs retrieval misses (gold passage never reached the model) vs
  generation errors (gold passage present, answer still wrong). This
  distribution is the evidence step 4 consumes.

## 2. Full 8.84M-row MS MARCO ingest

The corpus export already exists (`scripts/export_msmarco.sh` ->
`corpus_csv.tsv`); the full ingest has never been run -- the `lexis`
database's public schema holds a 200K-row slice.

```
# public schema only -- does NOT touch corpus_* schemas (DelucionQA
# lives in corpus_2 and survives this untouched)
psql -h 127.0.0.1 -p 5434 -U lexis -d lexis \
  -c "TRUNCATE postings, terms, passages RESTART IDENTITY CASCADE;"
./lexis bulk-ingest corpus_csv.tsv
```

- Expected duration: **~16 minutes** at the measured post-lemmatizer-fix
  rate (9,062 passages/sec on this M5).
- Note: this obsoletes the 70-query in-slice eval subset -- at full
  corpus, all 6,980 dev queries have their gold passages present, so
  the full eval below replaces it as the reference measurement.

## 3. Full 6,980-query MS MARCO eval

Both configurations, so expansion's value is re-verified at full scale:

```
./lexis eval data/eval/msmarco/queries_dev.tsv data/eval/msmarco/qrels_dev.tsv --no-llm-expansion
./lexis eval data/eval/msmarco/queries_dev.tsv data/eval/msmarco/qrels_dev.tsv
```

- Expected duration: the no-expansion run is minutes; the expansion run
  makes one LLM call per query -- extrapolating the 70-query timing,
  on the order of **2-3 hours**.
- This is the first number directly comparable to published baselines:
  classical BM25 on MS MARCO dev is MRR@10 ≈ 0.18-0.19. At or above
  that with expansion ahead of plain = the lexical core is pulling its
  weight.

## Done: BEIR comparison against published retriever scores (2026-08-14)

The "compare against traditional RAG without running a baseline
locally" measurement: two small BEIR corpora (exported by
`scripts/export_beir.sh`, scored by `lexis eval`'s nDCG@10 -- linear
gains, trec_eval convention, matching what BEIR reports through
pytrec_eval), lined up against published zero-shot nDCG@10 tables.

| nDCG@10        | SciFact | NFCorpus |
|----------------|---------|----------|
| DPR (dense, 2020)        | 0.318 | 0.189 |
| ANCE (dense, 2021)       | 0.507 | 0.237 |
| TAS-B (dense, 2021)      | 0.643 | 0.319 |
| published BM25 (anserini) | 0.665 | 0.325 |
| **LEXIS (expansion)**    | **0.6395** | **0.2834** |
| **LEXIS (plain terms)**  | **0.6354** | **0.2802** |
| ColBERTv2 (late-int.)    | 0.693 | 0.338 |
| BGE-Base (modern embed.) | 0.743 | 0.360 |

Reading: LEXIS lands in the classical-BM25 band (0.03-0.04 below
anserini's tuning -- plausibly chunking 200/40 over whole-doc indexing,
WordNet lemmatization vs Porter stemming, k1/b defaults), decisively
above the first-generation dense retrievers, at TAS-B's level on
SciFact, and 0.05-0.10 below modern embedding models. Expansion is
mildly positive on both. Run it again after retrieval changes:
`scripts/export_beir.sh <ds> test` then ingest + eval; ~15 min total
for both corpora on the M5.

## 4. The embeddings decision -- from data, not priors

Hybrid retrieval (an embedding model via llama.cpp, an ingest-time
embedding pass, pgvector or brute-force cosine, score fusion) is the
only fix for the one failure mechanism nothing above addresses: a query
term that matches meaning but not words (zero postings for "functions",
answer passage says "controls"). It is also a substantial subsystem and
a philosophical break with this project's "no vector embeddings"
identity -- so it must be justified by the step 1 failure distribution:

- Retrieval misses in low single digits percent -> not justified; the
  coordination bonus and expansion already cover the proportionate
  share.
- Retrieval misses at 10%+ and predominantly meaning-vs-words shaped ->
  that is the evidence hybrid retrieval needs before it gets built.

Whichever way it goes, record the decision and its evidence in
LIMITATIONS.md.
