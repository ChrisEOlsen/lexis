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

## 1. The 913-question DelucionQA run -- DONE 2026-08-15

Ran in 100 minutes (thinking=off), zero crashes. Headline numbers vs
the Tier 3 targets:

- Routing: 99.1% SEARCH (905/913) -- target >=98% MET. The 8
  non-SEARCH: 6 CHAT misroutes all shaped "can I <use vehicle
  feature>?" ("how can I adjust the volume?", "can I make a phone call
  using Uconnect?"), which trips the router's "questions about YOUR
  capabilities" rule; 2 SUMMARY ("What is the Owner's Manuals?"),
  arguably defensible. The "can I" pattern is the next router-prompt
  fix if 0.7% matters.
- Pipeline failures: 0. Refusal-shaped answers: 19 (2.1%) -- target 0
  NOT met; these are answerable per the dataset, so each is a
  retrieval or formulation miss worth reading.
- Latency: mean 6.6s, median 6.2s, max 23.2s -- target <=10s MET.
- Coverage: mean 54.8%; 155 answers (17%) under 25% coverage -- the
  candidate-failure pile. Too many to hand-read; the known limits of
  lexical coverage (undercounts paraphrase) mean the real failure
  rate is lower, but certifying the >=93% correctness target needs
  automated grounding attribution.

ATTRIBUTION DONE (2026-08-15, second run with passages saved --
lexis_eval column 8 -- scored by scripts/grounding_score.py: gold_sent
via shingle containment against the dataset's per-question gold
documents, answer support via a DeBERTa-v3 NLI judge; RAGBench's own
judge is unpublished, so this is a same-family stand-in, and NLI
support is conservative for multi-passage synthesis):

- gold_sent: 831/905 = 91.8% (target >=95%, just under). The 74
  misses split 40 total (no gold shingle arrived at all -- vocabulary/
  pool problem; only document expansion or embeddings reach these) vs
  34 partial (some gold chunks arrived -- chunk-boundary/ranking/trim
  territory; a reranker and chunk tuning can reach these).
- supported answers (NLI >= 0.5): 852/886 = 96.2% -- ~3.8%
  unsupported-answer rate.
- The 134 low-coverage answers decompose: 45 retrieval-miss, 89 with
  gold present -- and of those 89, only THREE are judged unsupported.
  The other ~86 are coverage-metric artifacts (grounded answers
  phrased differently than the reference), not failures.
- Refusals (19): 11 retrieval-miss, 8 gold-arrived-but-refused.
- Bottom line: genuine failures ~56 retrieval + ~11 generation of 905
  (~7.4%) -> estimated true correctness ~92-93%, at the Tier 3 target
  within measurement error. Retrieval outnumbers generation failures
  ~5:1: retrieval is the bottleneck, generation is nearly solved.

Ops note for re-runs: run the NLI judge on MPS with fixed-length
padding (grounding_score.py does both) -- the first attempt ran on CPU
with variable padding and crawled for 1.5h+; fixed, the full 8,267
pairs take ~35 min end to end.

### Original run plan (kept for re-runs)

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

## Done: lexical hygiene sweep (2026-08-15)

Chunking and BM25 k1/b swept on the two BEIR corpora via env knobs
(LEXIS_CHUNK_SIZE/LEXIS_CHUNK_OVERLAP on `lexis bulk-ingest`,
LEXIS_BM25_K1/LEXIS_BM25_B on retrieval_default_policy()). Findings:

- k1/b: the shipped 1.2/0.75 beat or tied anserini's 0.9/0.4 on both
  corpora. No change.
- Chunking is corpus-dependent: whole-document indexing (huge
  LEXIS_CHUNK_SIZE) is worth +0.017 nDCG@10 on NFCorpus (0.3061 vs
  0.2895) and slightly NEGATIVE on SciFact (0.629-0.631 vs 0.6353).
  The app default stays 200/40 (long documents must chunk under the
  1500-token trim budget); benchmark corpora with abstract-sized
  documents should ingest whole-doc via the env knob.
- Found and fixed along the way: BM25's ranking sort had no tie-break,
  so equal scores ordered by qsort whim -- NFCorpus results wobbled
  ~0.02 between identical configurations. Ties now break on
  passage_id; all six re-runs above reproduce exactly.
- Where this leaves the gap to published BM25: SciFact 0.6353 vs
  0.665, NFCorpus 0.3061 vs 0.325. The remaining delta is
  analyzer-level -- Porter stemming (published baselines) vs WordNet
  lemmatization (ours), stopword list differences, and single-field
  indexing vs weighted title fields. A Porter-stemmer experiment is
  the next hygiene candidate; it touches index AND query vocabulary,
  so it needs re-ingest and its own sweep.

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
